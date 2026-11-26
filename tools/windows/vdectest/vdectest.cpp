/*
 * vdectest - H.264, HEVC, VP9 and AV1 through ID3D11VideoDevice, the way a
 * player does it, checked frame by frame.
 *
 *   vdectest <umd.dll> <stream> [options]       the D3D10UMD software driver (CI)
 *   vdectest hw <stream> [options]              the Mali adapter (the board)
 *   vdectest hw --probe                         what the adapter offers, no decode
 *
 * Options:
 *   --ref <file.yuv>   I420 reference (the ITU conformance .yuv, or
 *                      ffmpeg -i x.264 -f rawvideo -pix_fmt yuv420p); MATCH or MISMATCH
 *   --fake             the UMD runs with D3D10UMD_VDEC_FAKE=1: check its pattern instead
 *   --chromium         what Chromium's D3D11VideoDecoder asks of the device, step by step
 *                      (chromeprobe.cpp); exit 0 when all of it is there, 4 otherwise
 *   --fl11             ask for feature level 11_0 first (with UmdReportFL11, as msedge.exe
 *                      --type=gpu-process: what ANGLE gets in the Chromium GPU process)
 *   --type=...         ignored (the argument Chromium's child processes have)
 *   --vp               the video processor and NV12 plane view tests (vptest.cpp); with
 *                      a stream, the decoder texture is also a shader resource and the
 *                      first frame goes through the video processor (with --fake)
 *   --srv              the decoder texture is also a shader resource (as FFmpeg makes
 *                      it, BIND_DECODER | BIND_SHADER_RESOURCE), with R8_UNORM and
 *                      R8G8_UNORM views of each slice made before the first decode;
 *                      every frame is also read through them (with --fake)
 *   --srv-late         the same, the views made at the first frame out, after decodes
 *   --chromium-tex     H.264: the decoder texture as Chromium 158 makes it here (an array,
 *                      BIND_DECODER | BIND_SHADER_RESOURCE, MiscFlags SHARED; MiscFlags 0
 *                      when the runtime refuses a shared texture, as without a KMD in CI),
 *                      the decoder described at the visible size as Chromium's container
 *                      size is, R8/R8G8 Texture2DArray views of every slice made before
 *                      decoding as ANGLE makes them; every frame is read through them and
 *                      compared with the frame read back through a staging copy
 *   --nthandle         with --chromium-tex: MiscFlags SHARED | SHARED_NTHANDLE (Chromium's
 *                      choice when the decoder asks for single textures, or with D3D12)
 *   --open             with --chromium-tex on a shared texture: also open it in a second
 *                      device (its NT or legacy handle) and read every frame through plane
 *                      views made there
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
 * The stream: H.264 or HEVC Annex B (told apart by the first NAL unit), or
 * IVF with VP9 (profile 0) or AV1 (Main, 8 bit). HEVC, VP9 and AV1 are in
 * codecs.cpp; --ref there is the I420 output as vdpu383test writes it
 * (VP9, AV1: each frame at its own size), and "output md5" is the kits'
 * FFmpeg reference MD5.
 *
 * Exit: 0 every frame matched (or --probe succeeded), 2 mismatch or decode
 * error, 1 setup failed, 3 the adapter offers no H.264 decoder.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "h264parse.h"
#include "md5.h"
}
#include "vptest.h"
#include "chromeprobe.h"
#include "codecs.h"

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
   int srv_mode;                           /* 1 --srv, 2 --srv-late */
   ID3D11ShaderResourceView *srv[H264_MAX_SLOTS][2];
   unsigned srv_frames, srv_bad, srv_failed;
   /* --chromium-tex: MiscFlags asked for and got; --open: the texture as a
    * second device opened it, and its plane views. */
   int chromium_tex, nthandle, open;
   UINT misc_asked, misc_got;
   ID3D11Device *dev2;
   ID3D11DeviceContext *ctx2;
   ID3D11Texture2D *tex2;
   ID3D11ShaderResourceView *srv2[H264_MAX_SLOTS][2];
   unsigned srv2_frames, srv2_bad, srv2_failed;
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

/* --fl11: ask for 11_0 first, as ANGLE does (UmdReportFL11 gives it to
 * Chromium's GPU process only: run as msedge.exe with --type=gpu-process). */
static int g_fl11;

static HRESULT
create_device(const char *what, Test *t)
{
   static const D3D_FEATURE_LEVEL levels10[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
   static const D3D_FEATURE_LEVEL levels11[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                 D3D_FEATURE_LEVEL_10_0 };
   const D3D_FEATURE_LEVEL *levels = g_fl11 ? levels11 : levels10;
   UINT nlevels = g_fl11 ? 3 : 2;
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
      hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, levels, nlevels, D3D11_SDK_VERSION,
                             &t->dev, &got, &t->ctx);
      pick->Release();
   } else {
      HMODULE m = LoadLibraryA(what);
      if (!m) {
         printf("cannot load %s (%lu)\n", what, GetLastError());
         return E_FAIL;
      }
      hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_SOFTWARE, m, flags, levels, nlevels, D3D11_SDK_VERSION,
                             &t->dev, &got, &t->ctx);
   }
   printf("D3D11CreateDevice with VIDEO_SUPPORT: 0x%08lx, feature level %u_%u\n", (unsigned long)hr,
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
      printf("  {%08lx-%04x-%04x-...} %s NV12 %s\n", g.Data1, g.Data2, g.Data3, codec_profile_name(g),
             nv12 ? "yes" : "no");
      if (IsEqualGUID(g, ProfileH264) && nv12)
         found = 1;
   }
   return found ? 0 : 3;
}

/* --open: the shared decoder texture in a second device of the same
 * adapter, as another D3D11 device opens Chromium's: by its NT handle
 * (SHARED_NTHANDLE) or its legacy handle (SHARED). */
static const char *g_device_arg;

static int
open_second(Test *t)
{
   Test t2;
   HANDLE h = NULL;
   HRESULT hr;
   int nt = (t->misc_got & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0;

   memset(&t2, 0, sizeof t2);
   if (FAILED(create_device(g_device_arg, &t2)))
      return 1;
   t->dev2 = t2.dev;
   t->ctx2 = t2.ctx;
   if (nt) {
      IDXGIResource1 *r1 = NULL;
      hr = t->tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
      if (SUCCEEDED(hr)) {
         hr = r1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h);
         r1->Release();
      }
      printf("second device: CreateSharedHandle 0x%08lx\n", (unsigned long)hr);
      ID3D11Device1 *d1 = NULL;
      if (SUCCEEDED(hr) && SUCCEEDED(hr = t->dev2->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1))) {
         hr = d1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void **)&t->tex2);
         d1->Release();
      }
      if (h)
         CloseHandle(h);
   } else {
      IDXGIResource *r = NULL;
      hr = t->tex->QueryInterface(__uuidof(IDXGIResource), (void **)&r);
      if (SUCCEEDED(hr)) {
         hr = r->GetSharedHandle(&h);
         r->Release();
      }
      printf("second device: GetSharedHandle 0x%08lx\n", (unsigned long)hr);
      if (SUCCEEDED(hr))
         hr = t->dev2->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void **)&t->tex2);
   }
   printf("second device: the decoder texture opened: 0x%08lx\n", (unsigned long)hr);
   return FAILED(hr) ? 1 : 0;
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
   if (t->chromium_tex) {
      /* Chromium: the container's size (1920x1080), not the pictures'. */
      desc.SampleWidth = width - t->crop_l - t->crop_r;
      desc.SampleHeight = height - t->crop_t - t->crop_b;
   }
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
   td.BindFlags = D3D11_BIND_DECODER | ((t->vp || t->srv_mode) ? D3D11_BIND_SHADER_RESOURCE : 0);
   td.MiscFlags = t->misc_asked;
   hr = t->dev->CreateTexture2D(&td, NULL, &t->tex);
   printf("NV12 decoder texture array %ux%u x %u, BindFlags 0x%x, MiscFlags 0x%x: 0x%08lx\n", td.Width,
          td.Height, td.ArraySize, td.BindFlags, td.MiscFlags, (unsigned long)hr);
   if (FAILED(hr) && t->misc_asked) {
      /* The software runtime with no KMD has no shared allocations (CI). */
      td.MiscFlags = 0;
      hr = t->dev->CreateTexture2D(&td, NULL, &t->tex);
      printf("  shared refused; MiscFlags 0 instead: 0x%08lx\n", (unsigned long)hr);
   }
   if (FAILED(hr))
      return 1;
   t->misc_got = td.MiscFlags;
   if (t->open && t->misc_got && open_second(t))
      return 1;
   for (UINT i = 0; t->srv_mode == 1 && i < H264_MAX_SLOTS; i++)
      if (vp_make_plane_views(t->dev, t->tex, i, t->srv[i]))
         return 1;
   if (t->srv_mode == 1)
      printf("R8_UNORM and R8G8_UNORM views of the %u slices: made\n", H264_MAX_SLOTS);
   for (UINT i = 0; t->tex2 && i < H264_MAX_SLOTS; i++)
      if (vp_make_plane_views(t->dev2, t->tex2, i, t->srv2[i]))
         return 1;
   if (t->tex2)
      printf("R8_UNORM and R8G8_UNORM views of the %u slices in the second device: made\n", H264_MAX_SLOTS);
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

   t->crop_l = pic->crop_left;
   t->crop_r = pic->crop_right;
   t->crop_t = pic->crop_top;
   t->crop_b = pic->crop_bottom;
   if (!t->dec) {
      t->width = pic->width;
      t->height = pic->height;
      if (setup_decoder(t, pic->width, pic->height))
         return -1;
   }
   if (slot >= H264_MAX_SLOTS)
      return -1;

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

   /* Every frame through the plane views, against what the staging copy
    * read: the luma and both chroma channels at every pixel. */
   if (t->srv_mode == 2 && !t->srv[slot][0] && vp_make_plane_views(t->dev, t->tex, (UINT)slot, t->srv[slot]))
      t->srv_failed = 1;
   if (t->srv[slot][0] && t->srv[slot][1]) {
      int r = vp_check_plane_views(t->dev, t->ctx, t->srv[slot], 0, t->tex_w, t->tex_h, t->crop_l,
                                   t->crop_t, w, h, t->frame);
      t->srv_frames++;
      if (r == 1)
         t->srv_failed = 1;
      else if (r)
         t->srv_bad++;
   }
   if (t->srv2[slot][0] && t->srv2[slot][1]) {
      /* The decoding device's work is out before the other device reads. */
      t->ctx->Flush();
      int r = vp_check_plane_views(t->dev2, t->ctx2, t->srv2[slot], 0, t->tex_w, t->tex_h, t->crop_l,
                                   t->crop_t, w, h, t->frame);
      t->srv2_frames++;
      if (r == 1)
         t->srv2_failed = 1;
      else if (r)
         t->srv2_bad++;
   }

   if (t->fake) {
      uint8_t ly = (uint8_t)t->slot_status[slot], lu = vdec_fake_cb((unsigned)slot),
              lv = vdec_fake_cr((unsigned)slot);
      size_t bad = 0, ysz = (size_t)w * h, usz = (size_t)(w / 2) * (h / 2);
      for (size_t i = 0; i < fsize; i++)
         if (t->frame[i] != (i < ysz ? ly : i < ysz + usz ? lu : lv))
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
                                     t->tex_h, ly, lu, lv);
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
   int probe_only = 0, have_runs, rc, chromium = 0;

   memset(&t, 0, sizeof t);
   if (argc < 3) {
      fprintf(stderr, "usage: vdectest <umd.dll|hw> <stream.264|--probe> [--ref f.yuv] [--fake] [-n N] [-o f.yuv]\n");
      return 1;
   }
   for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--probe")) probe_only = 1;
      else if (!strcmp(argv[i], "--fake")) t.fake = 1;
      else if (!strcmp(argv[i], "--vp")) t.vp = 1;
      else if (!strcmp(argv[i], "--srv")) t.srv_mode = 1;
      else if (!strcmp(argv[i], "--srv-late")) t.srv_mode = 2;
      else if (!strcmp(argv[i], "--chromium-tex")) t.chromium_tex = 1;
      else if (!strcmp(argv[i], "--nthandle")) t.nthandle = 1;
      else if (!strcmp(argv[i], "--open")) t.open = 1;
      else if (!strcmp(argv[i], "--chromium")) chromium = 1;
      else if (!strcmp(argv[i], "--fl11")) g_fl11 = 1;
      else if (!strncmp(argv[i], "--type=", 7)) { /* as Chromium's child processes have; ignored */ }
      else if (!strcmp(argv[i], "--ref") && i + 1 < argc) ref = argv[++i];
      else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
      else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_pictures = (unsigned)atoi(argv[++i]);
      else if (argv[i][0] != '-' && !stream) stream = argv[i];
      else {
         fprintf(stderr, "unknown option %s\n", argv[i]);
         return 1;
      }
   }

   g_device_arg = argv[1];
   if (t.chromium_tex) {
      /* What D3DDecoderConfigurator asks for with this UMD's decoder
       * (ConfigDecoderSpecific bit 14 clear: one array) and no decode swap
       * chain (--disable-direct-composition): SHARED. */
      t.srv_mode = 1;
      t.misc_asked = D3D11_RESOURCE_MISC_SHARED |
                     (t.nthandle ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
   }
   if (FAILED(create_device(argv[1], &t)))
      return 1;
   rc = probe(&t);
   if (chromium)
      return chrome_probe(t.dev, t.ctx);
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

   int kind = codec_kind(data, len);
   if (kind != CODEC_H264) {
      CodecOptions co = {};
      co.fake = t.fake;
      co.max_pictures = max_pictures;
      co.ref = ref;
      co.out = outp;
      if (t.out)
         fclose(t.out);
      free(t.ref);
      have_runs = driver_runs(&runs0) == 0;
      rc = codec_run(t.dev, t.ctx, t.vdev, t.vctx, kind, data, len, &co);
      if (have_runs && driver_runs(&runs1) == 0)
         printf("vdpu383.sys runs: %u -> %u (+%u)\n", runs0, runs1, runs1 - runs0);
      free(data);
      return rc;
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
   int srv_rc = 0;
   if (t.srv_mode) {
      printf("decoded pictures through the plane views (%s%s): %s, %u frames, %u differ\n",
             t.srv_mode == 1 ? "made before decoding" : "made after decoding",
             t.chromium_tex ? (t.misc_got ? ", Chromium's shared texture" : ", Chromium's texture unshared") : "",
             (t.srv_failed || !t.srv_frames) ? "FAILED" : t.srv_bad ? "MISMATCH" : "MATCH", t.srv_frames,
             t.srv_bad);
      srv_rc = t.srv_failed || !t.srv_frames || t.srv_bad;
      for (UINT j = 0; j < H264_MAX_SLOTS; j++)
         for (int i = 0; i < 2; i++)
            if (t.srv[j][i])
               t.srv[j][i]->Release();
   }
   if (t.open && t.misc_got) {
      printf("decoded pictures through the plane views of the texture opened in a second device: %s, "
             "%u frames, %u differ\n",
             (t.srv2_failed || !t.srv2_frames) ? "FAILED" : t.srv2_bad ? "MISMATCH" : "MATCH", t.srv2_frames,
             t.srv2_bad);
      srv_rc |= t.srv2_failed || !t.srv2_frames || t.srv2_bad;
   } else if (t.open) {
      printf("decoded pictures through a second device: not run (the texture is not shared)\n");
   }
   /* The decoder goes, so the UMD logs its last line (Video.cpp). */
   for (UINT i = 0; i < H264_MAX_SLOTS; i++)
      if (t.views[i])
         t.views[i]->Release();
   if (t.dec)
      t.dec->Release();
   t.dec = NULL;
   t.ctx->ClearState();
   t.ctx->Flush();
   return (prc || t.decode_errors || t.mismatches || !t.frames_out || t.vp_rc || srv_rc) ? 2 : 0;
}
