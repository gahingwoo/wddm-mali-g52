/*
 * vdectest - H.264 through ID3D11VideoDevice, the way a player does it,
 * checked frame by frame.
 *
 *   vdectest <umd.dll> <stream.264> [options]   the D3D10UMD software driver (CI)
 *   vdectest hw <stream.264> [options]          the Mali adapter (the board)
 *   vdectest hw --probe                         what the adapter offers, no decode
 *
 * Options:
 *   --ref <file.yuv>   I420 reference (the ITU conformance .yuv, or
 *                      ffmpeg -i x.264 -f rawvideo -pix_fmt yuv420p); MATCH or MISMATCH
 *   --fake             the UMD runs with D3D10UMD_VDEC_FAKE=1: check its pattern instead
 *   --vp               the video processor and NV12 plane view tests (vptest.cpp); with
 *                      a stream, the decoder texture is also a shader resource and the
 *                      first frame goes through the video processor (with --fake)
 *   -n <pictures>      stop after that many
 *   -o <file.yuv>      write the frames (I420)
 *
 * The stream is parsed by vdpu383test's parser (woa-rk3576
 * tools/vdpu383test), which produced the DXVA parameters vdpu383test's
 * --dxva-only run decoded bit-exact on the TaishanPi 3M; here they go
 * through the D3D11 runtime and the UMD instead of straight to the
 * driver: one NV12 texture array with BIND_DECODER, an output view per
 * slice, DecoderBeginFrame, the picture parameters, the scaling matrix, the
 * short slice control and the bitstream, SubmitDecoderBuffers,
 * DecoderEndFrame, as FFmpeg's d3d11va does, and each frame copied to a
 * staging NV12 texture and mapped, as FFmpeg's d3d11va-copy does.
 *
 * Exit: 0 every frame matched (or --probe succeeded), 2 mismatch or decode
 * error, 1 setup failed, 3 the adapter offers no H.264 decoder.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "h264parse.h"
#include "md5.h"
}
#include "vptest.h"

#include "../../../video/vdpu383/vdpu383_ioctl.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static const GUID ProfileH264 =
   { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };

#pragma pack(push, 1)
struct SliceShort {             /* DXVA_Slice_H264_Short */
   UINT BSNALunitDataLocation;
   UINT SliceBytesInBuffer;
   USHORT wBadSliceChopping;
};
#pragma pack(pop)

struct Test {
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   ID3D11VideoDevice *vdev;
   ID3D11VideoContext *vctx;
   ID3D11VideoDecoder *dec;
   ID3D11Texture2D *tex, *staging;
   ID3D11VideoDecoderOutputView *views[H264_MAX_SLOTS];
   unsigned tex_w, tex_h;

   int fake, vp, vp_done, vp_rc;
   unsigned slot_status[H264_MAX_SLOTS];   /* StatusReportFeedbackNumber last decoded there */

   unsigned width, height, crop_l, crop_r, crop_t, crop_b;
   uint8_t *frame;
   size_t frame_size;
   uint8_t *ref;
   size_t ref_len;
   FILE *out;
   Md5 total;

   unsigned pictures, decode_errors, frames_out, mismatches, first_mismatch;
   double decode_ms, copy_ms;
};

static double
now_ms(void)
{
   LARGE_INTEGER f, c;
   QueryPerformanceFrequency(&f);
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint8_t *
read_file(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   uint8_t *d;
   long n;

   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   d = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
   if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
      free(d);
      d = NULL;
   }
   fclose(f);
   *len = (size_t)n;
   return d;
}

/* vdpu383.sys's run counter, if this process may open it (UserAccess). */
static int
driver_runs(unsigned *runs)
{
   HANDLE h = CreateFileW(VDPU383_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
   VDPU383_INFO info;
   DWORD got = 0;
   BOOL ok;

   if (h == INVALID_HANDLE_VALUE)
      return -1;
   memset(&info, 0, sizeof info);
   ok = DeviceIoControl(h, IOCTL_VDPU383_GET_INFO, NULL, 0, &info, sizeof info, &got, NULL);
   CloseHandle(h);
   if (!ok)
      return -1;
   *runs = info.runs;
   return 0;
}

static HRESULT
create_device(const char *what, Test *t)
{
   static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
   D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
   UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
   HRESULT hr;

   if (_stricmp(what, "hw") == 0) {
      IDXGIFactory1 *f = NULL;
      IDXGIAdapter1 *a = NULL, *pick = NULL;
      hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f);
      if (FAILED(hr))
         return hr;
      for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++) {
         DXGI_ADAPTER_DESC1 d;
         a->GetDesc1(&d);
         printf("adapter %u: %ls (vendor 0x%04x)\n", i, d.Description, d.VendorId);
         if (!pick && wcsstr(d.Description, L"Mali")) {
            pick = a;
            continue;
         }
         a->Release();
      }
      f->Release();
      if (!pick) {
         printf("no adapter named Mali\n");
         return E_FAIL;
      }
      hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, levels, 2, D3D11_SDK_VERSION,
                             &t->dev, &got, &t->ctx);
      pick->Release();
   } else {
      HMODULE m = LoadLibraryA(what);
      if (!m) {
         printf("cannot load %s (%lu)\n", what, GetLastError());
         return E_FAIL;
      }
      hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_SOFTWARE, m, flags, levels, 2, D3D11_SDK_VERSION,
                             &t->dev, &got, &t->ctx);
   }
   printf("D3D11CreateDevice with VIDEO_SUPPORT: 0x%08lx, feature level %x_%x\n", (unsigned long)hr,
          ((unsigned)got >> 12) & 0xF, ((unsigned)got >> 8) & 0xF);
   return hr;
}

static int
probe(Test *t)
{
   UINT n = 0, fs = 0, found = 0;
   HRESULT hr = t->dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&t->vdev);

   printf("ID3D11VideoDevice: %s (0x%08lx)\n", SUCCEEDED(hr) ? "yes" : "no", (unsigned long)hr);
   if (FAILED(hr))
      return 3;
   t->ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&t->vctx);
   hr = t->dev->CheckFormatSupport(DXGI_FORMAT_NV12, &fs);
   printf("NV12 format support: 0x%08lx 0x%08x%s%s\n", (unsigned long)hr, fs,
          (fs & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT) ? " decoder-output" : "",
          (fs & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? " texture2d" : "");
   n = t->vdev->GetVideoDecoderProfileCount();
   printf("decoder profiles: %u\n", n);
   for (UINT i = 0; i < n; i++) {
      GUID g;
      BOOL nv12 = FALSE;
      if (FAILED(t->vdev->GetVideoDecoderProfile(i, &g)))
         continue;
      t->vdev->CheckVideoDecoderFormat(&g, DXGI_FORMAT_NV12, &nv12);
      printf("  {%08lx-%04x-%04x-...}%s NV12 %s\n", g.Data1, g.Data2, g.Data3,
             IsEqualGUID(g, ProfileH264) ? " H.264 VLD no FGT" : "", nv12 ? "yes" : "no");
      if (IsEqualGUID(g, ProfileH264) && nv12)
         found = 1;
   }
   return found ? 0 : 3;
}

static int
setup_decoder(Test *t, unsigned width, unsigned height)
{
   D3D11_VIDEO_DECODER_DESC desc = {};
   D3D11_VIDEO_DECODER_CONFIG cfg = {}, pick = {};
   D3D11_TEXTURE2D_DESC td = {};
   UINT nc = 0, have = 0;
   HRESULT hr;

   t->tex_w = (width + 15) & ~15u;
   t->tex_h = (height + 15) & ~15u;
   desc.Guid = ProfileH264;
   desc.SampleWidth = t->tex_w;
   desc.SampleHeight = t->tex_h;
   desc.OutputFormat = DXGI_FORMAT_NV12;
   hr = t->vdev->GetVideoDecoderConfigCount(&desc, &nc);
   printf("decoder configs for %ux%u: %u (0x%08lx)\n", t->tex_w, t->tex_h, nc, (unsigned long)hr);
   for (UINT i = 0; i < nc; i++) {
      if (FAILED(t->vdev->GetVideoDecoderConfig(&desc, i, &cfg)))
         continue;
      printf("  config %u: ConfigBitstreamRaw %u\n", i, cfg.ConfigBitstreamRaw);
      if (cfg.ConfigBitstreamRaw == 2 && !have) {
         pick = cfg;
         have = 1;
      }
   }
   if (!have) {
      printf("no short-slice configuration\n");
      return 1;
   }

   td.Width = t->tex_w;
   td.Height = t->tex_h;
   td.MipLevels = 1;
   td.ArraySize = H264_MAX_SLOTS;
   td.Format = DXGI_FORMAT_NV12;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_DEFAULT;
   td.BindFlags = D3D11_BIND_DECODER | (t->vp ? D3D11_BIND_SHADER_RESOURCE : 0);
   hr = t->dev->CreateTexture2D(&td, NULL, &t->tex);
   printf("NV12 decoder texture array %ux%u x %u: 0x%08lx\n", td.Width, td.Height, td.ArraySize,
          (unsigned long)hr);
   if (FAILED(hr))
      return 1;
   td.ArraySize = 1;
   td.Usage = D3D11_USAGE_STAGING;
   td.BindFlags = 0;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   hr = t->dev->CreateTexture2D(&td, NULL, &t->staging);
   printf("NV12 staging texture: 0x%08lx\n", (unsigned long)hr);
   if (FAILED(hr))
      return 1;
   for (UINT i = 0; i < H264_MAX_SLOTS; i++) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {};
      vd.DecodeProfile = ProfileH264;
      vd.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      vd.Texture2D.ArraySlice = i;
      hr = t->vdev->CreateVideoDecoderOutputView(t->tex, &vd, &t->views[i]);
      if (FAILED(hr)) {
         printf("output view %u: 0x%08lx\n", i, (unsigned long)hr);
         return 1;
      }
   }
   hr = t->vdev->CreateVideoDecoder(&desc, &pick, &t->dec);
   printf("CreateVideoDecoder: 0x%08lx\n", (unsigned long)hr);
   return FAILED(hr) ? 1 : 0;
}

static HRESULT
fill_buffer(Test *t, D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void *data, UINT size, UINT pad,
            UINT *written)
{
   void *p = NULL;
   UINT cap = 0;
   HRESULT hr = t->vctx->GetDecoderBuffer(t->dec, type, &cap, &p);
   if (FAILED(hr))
      return hr;
   if (size + pad > cap) {
      t->vctx->ReleaseDecoderBuffer(t->dec, type);
      printf("buffer type %u: %u bytes do not fit in %u\n", (unsigned)type, size, cap);
      return E_OUTOFMEMORY;
   }
   memcpy(p, data, size);
   memset((uint8_t *)p + size, 0, pad);
   *written = size + pad;
   return t->vctx->ReleaseDecoderBuffer(t->dec, type);
}

static int
decode_cb(void *ctx, const H264Picture *pic)
{
   Test *t = (Test *)ctx;
   unsigned slot = V383_PIC_INDEX(pic->pp.CurrPic);
   V383_DXVA_PicParams_H264 pp = pic->pp;
   SliceShort slices[256];
   unsigned ns = 0;
   D3D11_VIDEO_DECODER_BUFFER_DESC bd[4] = {};
   UINT sz[4] = {};
   HRESULT hr;
   double t0;

   if (!t->dec) {
      t->width = pic->width;
      t->height = pic->height;
      if (setup_decoder(t, pic->width, pic->height))
         return -1;
   }
   if (slot >= H264_MAX_SLOTS)
      return -1;
   t->crop_l = pic->crop_left;
   t->crop_r = pic->crop_right;
   t->crop_t = pic->crop_top;
   t->crop_b = pic->crop_bottom;

   /* The short slice control: where each NAL unit starts (at its start
    * code) and how long it is. */
   for (size_t i = 0; i + 3 <= pic->stream_len && ns < 256; i++) {
      if (pic->stream[i] == 0 && pic->stream[i + 1] == 0 && pic->stream[i + 2] == 1) {
         if (ns)
            slices[ns - 1].SliceBytesInBuffer = (UINT)i - slices[ns - 1].BSNALunitDataLocation;
         slices[ns].BSNALunitDataLocation = (UINT)i;
         slices[ns].SliceBytesInBuffer = 0;
         slices[ns].wBadSliceChopping = 0;
         ns++;
         i += 2;
      }
   }
   if (ns)
      slices[ns - 1].SliceBytesInBuffer = (UINT)pic->stream_len - slices[ns - 1].BSNALunitDataLocation;

   pp.StatusReportFeedbackNumber = pic->decode_index + 1;
   t->slot_status[slot] = pp.StatusReportFeedbackNumber;

   t0 = now_ms();
   for (int tries = 0;; tries++) {
      hr = t->vctx->DecoderBeginFrame(t->dec, t->views[slot], 0, NULL);
      if (hr != E_PENDING || tries > 1000)
         break;
      Sleep(1);
   }
   if (FAILED(hr)) {
      printf("pic %u: DecoderBeginFrame 0x%08lx\n", pic->decode_index, (unsigned long)hr);
      return -1;
   }
   hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &pp, sizeof pp, 0, &sz[0]);
   if (SUCCEEDED(hr))
      hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &pic->qm,
                       sizeof pic->qm, 0, &sz[1]);
   if (SUCCEEDED(hr))
      hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, slices, ns * sizeof slices[0], 0,
                       &sz[2]);
   if (SUCCEEDED(hr)) {
      /* FFmpeg pads the bitstream to 128 bytes with zeros. */
      UINT pad = (UINT)((128 - (pic->stream_len & 127)) & 127);
      hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, pic->stream, (UINT)pic->stream_len,
                       pad, &sz[3]);
   }
   if (SUCCEEDED(hr)) {
      bd[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
      bd[0].DataSize = sz[0];
      bd[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
      bd[1].DataSize = sz[1];
      bd[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
      bd[2].DataSize = sz[2];
      bd[3].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
      bd[3].DataSize = sz[3];
      hr = t->vctx->SubmitDecoderBuffers(t->dec, 4, bd);
   }
   HRESULT end = t->vctx->DecoderEndFrame(t->dec);
   t->decode_ms += now_ms() - t0;
   t->pictures++;
   if (FAILED(hr) || FAILED(end)) {
      t->decode_errors++;
      printf("pic %u: submit 0x%08lx, end 0x%08lx\n", pic->decode_index, (unsigned long)hr,
             (unsigned long)end);
      return -1;
   }
   return 0;
}

static void
output_cb(void *ctx, int slot, unsigned decode_index, int poc)
{
   Test *t = (Test *)ctx;
   D3D11_MAPPED_SUBRESOURCE m;
   unsigned w = t->width - t->crop_l - t->crop_r, h = t->height - t->crop_t - t->crop_b;
   size_t fsize = (size_t)w * h * 3 / 2;
   double t0 = now_ms();
   HRESULT hr;

   if (!t->dec || slot < 0 || slot >= H264_MAX_SLOTS)
      return;
   t->ctx->CopySubresourceRegion(t->staging, 0, 0, 0, 0, t->tex, (UINT)slot, NULL);
   hr = t->ctx->Map(t->staging, 0, D3D11_MAP_READ, 0, &m);
   if (FAILED(hr)) {
      printf("frame %u: Map 0x%08lx\n", t->frames_out, (unsigned long)hr);
      t->mismatches++;
      t->frames_out++;
      return;
   }
   if (t->frame_size != fsize) {
      free(t->frame);
      t->frame = (uint8_t *)malloc(fsize);
      t->frame_size = fsize;
   }
   const uint8_t *s = (const uint8_t *)m.pData;
   const uint8_t *c = s + (size_t)m.RowPitch * t->tex_h;
   uint8_t *p = t->frame;
   for (unsigned y = 0; y < h; y++, p += w)
      memcpy(p, s + (size_t)(y + t->crop_t) * m.RowPitch + t->crop_l, w);
   uint8_t *u = p, *v = p + (w / 2) * (h / 2);
   for (unsigned y = 0; y < h / 2; y++)
      for (unsigned x = 0; x < w / 2; x++) {
         const uint8_t *q = c + (size_t)(y + t->crop_t / 2) * m.RowPitch + (x + t->crop_l / 2) * 2;
         u[y * (w / 2) + x] = q[0];
         v[y * (w / 2) + x] = q[1];
      }
   t->ctx->Unmap(t->staging, 0);
   t->copy_ms += now_ms() - t0;

   md5_update(&t->total, t->frame, fsize);
   if (t->out)
      fwrite(t->frame, 1, fsize, t->out);

   if (t->fake) {
      uint8_t ly = (uint8_t)t->slot_status[slot], lc = (uint8_t)(0x80 ^ slot);
      size_t bad = 0, ysz = (size_t)w * h;
      for (size_t i = 0; i < fsize; i++)
         if (t->frame[i] != (i < ysz ? ly : lc))
            bad++;
      if (bad) {
         if (!t->mismatches++)
            t->first_mismatch = t->frames_out;
         printf("frame %4u (pic %u, slot %d): %zu bytes not the fake decoder's pattern\n",
                t->frames_out, decode_index, slot, bad);
      }
      if (t->vp && !t->vp_done) {
         t->vp_done = 1;
         t->vp_rc = vp_check_decoded(t->dev, t->ctx, t->vdev, t->vctx, t->tex, (UINT)slot, t->tex_w,
                                     t->tex_h, ly, lc);
      }
   } else if (t->ref) {
      size_t off = (size_t)t->frames_out * fsize;
      if (off + fsize > t->ref_len || memcmp(t->ref + off, t->frame, fsize)) {
         if (!t->mismatches++)
            t->first_mismatch = t->frames_out;
         printf("frame %4u (pic %u, poc %d): MISMATCH\n", t->frames_out, decode_index, poc);
      }
   }
   t->frames_out++;
}

int
main(int argc, char **argv)
{
   Test t;
   const char *stream = NULL, *ref = NULL, *outp = NULL;
   unsigned max_pictures = 0, runs0 = 0, runs1 = 0;
   int probe_only = 0, have_runs, rc;

   memset(&t, 0, sizeof t);
   if (argc < 3) {
      fprintf(stderr, "usage: vdectest <umd.dll|hw> <stream.264|--probe> [--ref f.yuv] [--fake] [-n N] [-o f.yuv]\n");
      return 1;
   }
   for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--probe")) probe_only = 1;
      else if (!strcmp(argv[i], "--fake")) t.fake = 1;
      else if (!strcmp(argv[i], "--vp")) t.vp = 1;
      else if (!strcmp(argv[i], "--ref") && i + 1 < argc) ref = argv[++i];
      else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
      else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_pictures = (unsigned)atoi(argv[++i]);
      else if (argv[i][0] != '-' && !stream) stream = argv[i];
      else {
         fprintf(stderr, "unknown option %s\n", argv[i]);
         return 1;
      }
   }

   if (FAILED(create_device(argv[1], &t)))
      return 1;
   rc = probe(&t);
   if (t.vp && t.vdev && !stream && !probe_only)
      return vp_run_tests(t.dev, t.ctx, t.vdev, t.vctx);
   if (probe_only || rc)
      return rc;
   if (!stream) {
      fprintf(stderr, "no stream\n");
      return 1;
   }

   size_t len = 0;
   uint8_t *data = read_file(stream, &len);
   if (!data) {
      fprintf(stderr, "cannot read %s\n", stream);
      return 1;
   }
   if (ref && !(t.ref = read_file(ref, &t.ref_len))) {
      fprintf(stderr, "cannot read %s\n", ref);
      return 1;
   }
   if (outp && !(t.out = fopen(outp, "wb"))) {
      fprintf(stderr, "cannot write %s\n", outp);
      return 1;
   }

   have_runs = driver_runs(&runs0) == 0;
   md5_init(&t.total);
   H264Parser *p = (H264Parser *)calloc(1, sizeof *p);
   h264_init(p, output_cb, &t);
   double t0 = now_ms();
   int prc = h264_run(p, data, len, max_pictures, decode_cb, &t);
   double total_ms = now_ms() - t0;
   if (prc)
      printf("stream error: %s\n", p->err);
   h264_free(p);
   free(p);
   if (have_runs)
      have_runs = driver_runs(&runs1) == 0;

   char hex[33];
   md5_final(&t.total, hex);
   printf("pictures %u, frames out %u, decode errors %u\n", t.pictures, t.frames_out, t.decode_errors);
   printf("time: %.1f ms total, %.2f ms per picture in Begin..EndFrame, %.2f ms per frame copied out\n",
          total_ms, t.pictures ? t.decode_ms / t.pictures : 0.0,
          t.frames_out ? t.copy_ms / t.frames_out : 0.0);
   if (have_runs)
      printf("vdpu383.sys runs: %u -> %u (+%u)\n", runs0, runs1, runs1 - runs0);
   else
      printf("vdpu383.sys runs: not readable from this process\n");
   printf("output md5 %s\n", hex);
   if (t.fake || t.ref) {
      size_t fs = t.frame_size ? t.frame_size : 1;
      printf("%s: %s", t.fake ? "fake pattern" : "reference", t.mismatches ? "MISMATCH" : "MATCH");
      if (t.mismatches)
         printf(", %u frames differ, first at output frame %u", t.mismatches, t.first_mismatch);
      else if (t.ref && t.frames_out != t.ref_len / fs)
         printf(" (but %u frames out, %zu in the reference)", t.frames_out, t.ref_len / fs);
      printf("\n");
   }
   if (t.out)
      fclose(t.out);
   if (t.vp && !t.vp_done)
      printf("decoded picture through the video processor: not run (needs --fake)\n");
   return (prc || t.decode_errors || t.mismatches || !t.frames_out || t.vp_rc) ? 2 : 0;
}
