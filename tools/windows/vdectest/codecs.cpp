/*
 * codecs.cpp - vdectest for HEVC Main, VP9 profile 0 and AV1 Main: the
 * stream through ID3D11VideoDevice the way FFmpeg's d3d11va hands it over,
 * each shown frame copied to a staging texture and checked.
 *
 * The streams are parsed by vdpu383test's parsers (woa-rk3576
 * tools/vdpu383test, copied here), which produce the DXVA parameters
 * vdpu383test's --dxva-only runs build their registers from; the UMD,
 * given the same parameters through the D3D11 runtime, must build the
 * same registers (D3D10UMD_VDEC_REGLOG, tests/vdec-host/reglog.md5).
 *
 * What goes in each frame, as FFmpeg's dxva2_hevc.c, dxva2_vp9.c and
 * dxva2_av1.c fill it:
 *   HEVC  DXVA_PicParams_HEVC, DXVA_Qmatrix_HEVC (with scaling lists only),
 *         DXVA_Slice_HEVC_Short per slice, the slices behind 00 00 01
 *   VP9   DXVA_PicParams_VP9, one DXVA_Slice_VPx_Short, the whole frame
 *   AV1   DXVA_PicParams_AV1, DXVA_Tile_AV1 per tile, the tile data
 * The bitstream is padded with zeros to 128 bytes.
 *
 * Output as vdpu383test writes it, so that "output md5" can be compared
 * with the kits' FFmpeg references: I420, each frame at its own size
 * (VP9, AV1) or cropped (HEVC).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "hevcparse.h"
#include "vp9parse.h"
#include "av1parse.h"
#include "md5.h"
}
#include "codecs.h"

static const GUID ProfileH264 =
   { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
static const GUID ProfileHevcMain =
   { 0x5b11d51b, 0x2f4c, 0x4452, { 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0 } };
static const GUID ProfileHevcMain10 =
   { 0x107af0e0, 0xef1a, 0x4d19, { 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13 } };
static const GUID ProfileVp9Profile0 =
   { 0x463707f8, 0xa1d0, 0x4585, { 0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e } };
static const GUID ProfileVp9Profile2 =
   { 0xa4c749ef, 0x6ecf, 0x48aa, { 0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7 } };
static const GUID ProfileAv1Profile0 =
   { 0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } };

const char *
codec_profile_name(const GUID &g)
{
   if (IsEqualGUID(g, ProfileH264)) return "H.264 VLD no FGT";
   if (IsEqualGUID(g, ProfileHevcMain)) return "HEVC Main";
   if (IsEqualGUID(g, ProfileHevcMain10)) return "HEVC Main 10";
   if (IsEqualGUID(g, ProfileVp9Profile0)) return "VP9 profile 0";
   if (IsEqualGUID(g, ProfileVp9Profile2)) return "VP9 profile 2";
   if (IsEqualGUID(g, ProfileAv1Profile0)) return "AV1 Main";
   return "";
}

int
codec_kind(const uint8_t *data, size_t len)
{
   if (av1_probe(data, len))
      return CODEC_AV1;
   if (vp9_probe(data, len))
      return CODEC_VP9;
   if (hevc_probe(data, len))
      return CODEC_HEVC;
   return CODEC_H264;
}

#define MAX_SLOTS 20            /* HEVC_MAX_SLOTS, more than VP9's and AV1's 10 */

#pragma pack(push, 1)
struct SliceShort {             /* DXVA_Slice_HEVC_Short, DXVA_Slice_VPx_Short */
   UINT BSNALunitDataLocation;
   UINT SliceBytesInBuffer;
   USHORT wBadSliceChopping;
};
#pragma pack(pop)

struct CTest {
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   ID3D11VideoDevice *vdev;
   ID3D11VideoContext *vctx;
   ID3D11VideoDecoder *dec;
   ID3D11Texture2D *tex, *staging;
   ID3D11VideoDecoderOutputView *views[MAX_SLOTS];
   const GUID *guid;
   const char *name;
   unsigned slots, tex_w, tex_h;
   CodecOptions opt;

   unsigned slot_status[MAX_SLOTS];
   unsigned crop_l, crop_r, crop_t, crop_b, pic_w, pic_h;     /* HEVC */

   uint8_t *frame;
   size_t frame_size, ref_off;
   uint8_t *ref;
   size_t ref_len;
   FILE *out;
   Md5 total;

   unsigned pictures, decode_errors, frames_out, mismatches, first_mismatch;
   int failed;
};

static int
setup_decoder(CTest *t, unsigned sample_w, unsigned sample_h)
{
   D3D11_VIDEO_DECODER_DESC desc = {};
   D3D11_VIDEO_DECODER_CONFIG cfg = {}, pick = {};
   D3D11_TEXTURE2D_DESC td = {};
   UINT nc = 0, have = 0;
   BOOL ok = FALSE;
   HRESULT hr;

   if (FAILED(t->vdev->CheckVideoDecoderFormat(t->guid, DXGI_FORMAT_NV12, &ok)) || !ok) {
      printf("%s: the adapter offers no decoder with NV12 output\n", t->name);
      return 3;
   }
   t->tex_w = (sample_w + 15) & ~15u;
   t->tex_h = (sample_h + 15) & ~15u;
   desc.Guid = *t->guid;
   desc.SampleWidth = sample_w;
   desc.SampleHeight = sample_h;
   desc.OutputFormat = DXGI_FORMAT_NV12;
   hr = t->vdev->GetVideoDecoderConfigCount(&desc, &nc);
   printf("%s decoder configs for %ux%u: %u (0x%08lx)\n", t->name, sample_w, sample_h, nc, (unsigned long)hr);
   for (UINT i = 0; i < nc; i++) {
      if (FAILED(t->vdev->GetVideoDecoderConfig(&desc, i, &cfg)))
         continue;
      printf("  config %u: ConfigBitstreamRaw %u\n", i, cfg.ConfigBitstreamRaw);
      if (cfg.ConfigBitstreamRaw == 1 && !have) {
         pick = cfg;
         have = 1;
      }
   }
   if (!have) {
      printf("no configuration with ConfigBitstreamRaw 1\n");
      return 1;
   }
   td.Width = t->tex_w;
   td.Height = t->tex_h;
   td.MipLevels = 1;
   td.ArraySize = t->slots;
   td.Format = DXGI_FORMAT_NV12;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_DEFAULT;
   td.BindFlags = D3D11_BIND_DECODER;
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
   if (FAILED(hr)) {
      printf("NV12 staging texture: 0x%08lx\n", (unsigned long)hr);
      return 1;
   }
   for (UINT i = 0; i < t->slots; i++) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {};
      vd.DecodeProfile = *t->guid;
      vd.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      vd.Texture2D.ArraySlice = i;
      hr = t->vdev->CreateVideoDecoderOutputView(t->tex, &vd, &t->views[i]);
      if (FAILED(hr)) {
         printf("output view %u: 0x%08lx\n", i, (unsigned long)hr);
         return 1;
      }
   }
   hr = t->vdev->CreateVideoDecoder(&desc, &pick, &t->dec);
   printf("CreateVideoDecoder %s: 0x%08lx\n", t->name, (unsigned long)hr);
   return FAILED(hr) ? 1 : 0;
}

static HRESULT
fill_buffer(CTest *t, D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void *data, UINT size, UINT pad, UINT *written)
{
   void *p = NULL;
   UINT cap = 0;
   HRESULT hr = t->vctx->GetDecoderBuffer(t->dec, type, &cap, &p);
   if (FAILED(hr))
      return hr;
   if (size + pad > cap) {
      t->vctx->ReleaseDecoderBuffer(t->dec, type);
      printf("buffer type %u: %u bytes do not fit in %u\n", (unsigned)type, size + pad, cap);
      return E_OUTOFMEMORY;
   }
   memcpy(p, data, size);
   memset((uint8_t *)p + size, 0, pad);
   *written = size + pad;
   return t->vctx->ReleaseDecoderBuffer(t->dec, type);
}

/* One frame: picture parameters, matrix (or none), slice control, bitstream. */
static int
submit(CTest *t, unsigned slot, unsigned index, const void *pp, UINT pp_size, const void *qm, UINT qm_size,
       const void *slices, UINT slices_size, const uint8_t *bits, UINT bits_len)
{
   D3D11_VIDEO_DECODER_BUFFER_DESC bd[4] = {};
   UINT sz = 0, n = 0;
   HRESULT hr;

   if (slot >= t->slots)
      return -1;
   for (int tries = 0;; tries++) {
      hr = t->vctx->DecoderBeginFrame(t->dec, t->views[slot], 0, NULL);
      if (hr != E_PENDING || tries > 1000)
         break;
      Sleep(1);
   }
   if (FAILED(hr)) {
      printf("pic %u: DecoderBeginFrame 0x%08lx\n", index, (unsigned long)hr);
      return -1;
   }
   hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, pp, pp_size, 0, &sz);
   bd[n].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
   bd[n++].DataSize = sz;
   if (SUCCEEDED(hr) && qm) {
      hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, qm, qm_size, 0, &sz);
      bd[n].BufferType = D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
      bd[n++].DataSize = sz;
   }
   if (SUCCEEDED(hr)) {
      hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, slices, slices_size, 0, &sz);
      bd[n].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
      bd[n++].DataSize = sz;
   }
   if (SUCCEEDED(hr)) {
      UINT pad = (128 - (bits_len & 127)) & 127;
      hr = fill_buffer(t, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, bits, bits_len, pad, &sz);
      bd[n].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
      bd[n++].DataSize = sz;
   }
   if (SUCCEEDED(hr))
      hr = t->vctx->SubmitDecoderBuffers(t->dec, n, bd);
   HRESULT end = t->vctx->DecoderEndFrame(t->dec);
   t->pictures++;
   if (FAILED(hr) || FAILED(end)) {
      t->decode_errors++;
      printf("pic %u: submit 0x%08lx, end 0x%08lx\n", index, (unsigned long)hr, (unsigned long)end);
      return -1;
   }
   return 0;
}

/* A shown frame: the w x h part at (x0, y0) of slice slot, as I420. */
static void
output(CTest *t, int slot, unsigned x0, unsigned y0, unsigned w, unsigned h, unsigned index)
{
   D3D11_MAPPED_SUBRESOURCE m;
   unsigned cw = (w + 1) / 2, ch = (h + 1) / 2;
   size_t fsize = (size_t)w * h + 2 * (size_t)cw * ch;
   HRESULT hr;

   if (!t->dec || slot < 0 || (unsigned)slot >= t->slots)
      return;
   t->ctx->CopySubresourceRegion(t->staging, 0, 0, 0, 0, t->tex, (UINT)slot, NULL);
   hr = t->ctx->Map(t->staging, 0, D3D11_MAP_READ, 0, &m);
   if (FAILED(hr)) {
      printf("frame %u: Map 0x%08lx\n", t->frames_out, (unsigned long)hr);
      t->mismatches++;
      t->frames_out++;
      return;
   }
   if (t->frame_size < fsize) {
      free(t->frame);
      t->frame = (uint8_t *)malloc(fsize);
      t->frame_size = fsize;
   }
   const uint8_t *s = (const uint8_t *)m.pData;
   const uint8_t *c = s + (size_t)m.RowPitch * t->tex_h;
   uint8_t *p = t->frame;
   for (unsigned y = 0; y < h; y++, p += w)
      memcpy(p, s + (size_t)(y + y0) * m.RowPitch + x0, w);
   uint8_t *u = p, *v = p + (size_t)cw * ch;
   for (unsigned y = 0; y < ch; y++)
      for (unsigned x = 0; x < cw; x++) {
         const uint8_t *q = c + (size_t)(y + y0 / 2) * m.RowPitch + (x + x0 / 2) * 2;
         u[y * cw + x] = q[0];
         v[y * cw + x] = q[1];
      }
   t->ctx->Unmap(t->staging, 0);

   md5_update(&t->total, t->frame, fsize);
   if (t->out)
      fwrite(t->frame, 1, fsize, t->out);
   if (t->opt.fake) {
      uint8_t ly = (uint8_t)t->slot_status[slot], lc = (uint8_t)(0x80 ^ slot);
      size_t bad = 0, ysz = (size_t)w * h;
      for (size_t i = 0; i < fsize; i++)
         if (t->frame[i] != (i < ysz ? ly : lc))
            bad++;
      if (bad) {
         if (!t->mismatches++)
            t->first_mismatch = t->frames_out;
         printf("frame %4u (pic %u, slot %d): %zu bytes not the fake decoder's pattern\n", t->frames_out,
                index, slot, bad);
      }
   } else if (t->ref) {
      if (t->ref_off + fsize > t->ref_len || memcmp(t->ref + t->ref_off, t->frame, fsize)) {
         if (!t->mismatches++)
            t->first_mismatch = t->frames_out;
         printf("frame %4u (pic %u): MISMATCH\n", t->frames_out, index);
      }
      t->ref_off += fsize;
   }
   t->frames_out++;
}

/* ----------------------------------------------------------------- HEVC */

static int
hevc_decode(void *ctx, const HevcPicture *pic)
{
   CTest *t = (CTest *)ctx;
   V383_DXVA_PicParams_HEVC pp = pic->pp;
   SliceShort slices[600];
   unsigned ns = 0, slot = V383_PIC_INDEX(pp.CurrPic);

   if (!t->dec) {
      int rc = setup_decoder(t, pic->width, pic->height);
      if (rc) {
         t->failed = rc;
         return -1;
      }
   }
   t->crop_l = pic->crop_left;
   t->crop_r = pic->crop_right;
   t->crop_t = pic->crop_top;
   t->crop_b = pic->crop_bottom;
   t->pic_w = pic->width;
   t->pic_h = pic->height;
   for (size_t i = 0; i + 3 <= pic->stream_len && ns < 600; i++) {
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
   if (slot < MAX_SLOTS)
      t->slot_status[slot] = pp.StatusReportFeedbackNumber;
   return submit(t, slot, pic->decode_index, &pp, sizeof pp,
                 V383_HEVC_SCALING_LIST_ENABLED(&pp) ? &pic->qm : NULL, sizeof pic->qm, slices,
                 ns * sizeof slices[0], pic->stream, (UINT)pic->stream_len);
}

static void
hevc_output(void *ctx, int slot, unsigned decode_index, int poc)
{
   CTest *t = (CTest *)ctx;
   (void)poc;
   output(t, slot, t->crop_l, t->crop_t, t->pic_w - t->crop_l - t->crop_r, t->pic_h - t->crop_t - t->crop_b,
          decode_index);
}

/* ------------------------------------------------------------------ VP9 */

static int
vp9_decode(void *ctx, const Vp9Picture *pic)
{
   CTest *t = (CTest *)ctx;
   V383_DXVA_PicParams_VP9 pp = pic->pp;
   SliceShort sl = { 0, (UINT)pic->len, 0 };
   unsigned slot = V383_PIC_INDEX(pp.CurrPic);

   pp.StatusReportFeedbackNumber = pic->decode_index + 1;
   if (slot < MAX_SLOTS)
      t->slot_status[slot] = pp.StatusReportFeedbackNumber;
   return submit(t, slot, pic->decode_index, &pp, sizeof pp, NULL, 0, &sl, sizeof sl, pic->data, (UINT)pic->len);
}

static void
vpx_output(void *ctx, int slot, unsigned w, unsigned h, unsigned decode_index, int existing)
{
   (void)existing;
   output((CTest *)ctx, slot, 0, 0, w, h, decode_index);
}

/* ------------------------------------------------------------------ AV1 */

static int
av1_decode(void *ctx, const Av1Picture *pic)
{
   CTest *t = (CTest *)ctx;
   V383_DXVA_PicParams_AV1 pp = pic->pp;
   unsigned slot = pp.CurrPicTextureIndex, end = 0;

   for (unsigned i = 0; i < pic->num_tiles; i++)
      if (pic->tiles[i].DataOffset + pic->tiles[i].DataSize > end)
         end = pic->tiles[i].DataOffset + pic->tiles[i].DataSize;
   pp.StatusReportFeedbackNumber = pic->decode_index + 1;
   if (slot < MAX_SLOTS)
      t->slot_status[slot] = pp.StatusReportFeedbackNumber;
   return submit(t, slot, pic->decode_index, &pp, sizeof pp, NULL, 0, pic->tiles,
                 pic->num_tiles * sizeof pic->tiles[0], pic->tile_data, end);
}

/* ----------------------------------------------------------------- run */

static uint8_t *
read_whole(const char *path, size_t *len)
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

int
codec_run(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *vdev, ID3D11VideoContext *vctx,
          int kind, const uint8_t *data, size_t len, const CodecOptions *opt)
{
   CTest *t = (CTest *)calloc(1, sizeof *t);
   int prc = 0, rc = 0;
   char hex[33];

   t->dev = dev;
   t->ctx = ctx;
   t->vdev = vdev;
   t->vctx = vctx;
   t->opt = *opt;
   if (opt->ref && !(t->ref = read_whole(opt->ref, &t->ref_len))) {
      fprintf(stderr, "cannot read %s\n", opt->ref);
      return 1;
   }
   if (opt->out && !(t->out = fopen(opt->out, "wb"))) {
      fprintf(stderr, "cannot write %s\n", opt->out);
      return 1;
   }
   md5_init(&t->total);

   if (kind == CODEC_HEVC) {
      HevcParser *p = (HevcParser *)calloc(1, sizeof *p);
      t->guid = &ProfileHevcMain;
      t->name = "HEVC Main";
      t->slots = HEVC_MAX_SLOTS;
      hevc_init(p, hevc_output, t);
      prc = hevc_run(p, data, len, opt->max_pictures, hevc_decode, t);
      if (prc)
         printf("stream error: %s\n", p->err);
      hevc_free(p);
      free(p);
   } else if (kind == CODEC_VP9) {
      Vp9Parser *p = (Vp9Parser *)calloc(1, sizeof *p);
      t->guid = &ProfileVp9Profile0;
      t->name = "VP9 profile 0";
      t->slots = VP9_MAX_SLOTS;
      vp9_init(p, NULL, NULL);
      prc = vp9_run(p, data, len, opt->max_pictures, NULL, NULL);
      if (!prc && p->max_bit_depth > 8) {
         printf("VP9: %u-bit stream; this decoder takes profile 0 (8 bit) only\n", p->max_bit_depth);
         free(p);
         rc = 3;
         goto done;
      }
      /* The decoder for the stream's largest frame, as FFmpeg sizes it from
       * the first, and vdpu383test from the largest (one size here). */
      if (!prc && (rc = setup_decoder(t, p->max_width, p->max_height)) != 0) {
         free(p);
         goto done;
      }
      vp9_init(p, vpx_output, t);
      p->dxva_only = 1;         /* frame_context_idx 0 on intra frames, as FFmpeg and Chromium */
      if (!prc)
         prc = vp9_run(p, data, len, opt->max_pictures, vp9_decode, t);
      if (prc)
         printf("stream error: %s\n", p->err);
      free(p);
   } else {
      Av1Parser *p = (Av1Parser *)calloc(1, sizeof *p);
      t->guid = &ProfileAv1Profile0;
      t->name = "AV1 Main";
      t->slots = AV1_MAX_SLOTS;
      av1_init(p, NULL, NULL);
      prc = av1_run(p, data, len, opt->max_pictures, NULL, NULL);
      if (!prc && p->max_bit_depth > 8) {
         printf("AV1: %u-bit stream; this decoder takes 8 bit only\n", p->max_bit_depth);
         av1_free(p);
         free(p);
         rc = 3;
         goto done;
      }
      if (!prc && (rc = setup_decoder(t, p->max_width, p->max_height)) != 0) {
         av1_free(p);
         free(p);
         goto done;
      }
      av1_init(p, vpx_output, t);
      if (!prc)
         prc = av1_run(p, data, len, opt->max_pictures, av1_decode, t);
      if (prc)
         printf("stream error: %s\n", p->err);
      av1_free(p);
      free(p);
   }
   if (t->failed) {
      rc = t->failed;
      goto done;
   }

   md5_final(&t->total, hex);
   printf("%s: pictures %u, frames out %u, decode errors %u\n", t->name, t->pictures, t->frames_out,
          t->decode_errors);
   printf("output md5 %s\n", hex);
   if (t->opt.fake || t->ref) {
      printf("%s: %s", t->opt.fake ? "fake pattern" : "reference",
             (t->mismatches || (t->ref && t->ref_off != t->ref_len)) ? "MISMATCH" : "MATCH");
      if (t->mismatches)
         printf(", %u frames differ, first at output frame %u", t->mismatches, t->first_mismatch);
      else if (t->ref && t->ref_off != t->ref_len)
         printf(" (the reference has %zu bytes more)", t->ref_len - t->ref_off);
      printf("\n");
   }
   rc = (prc || t->decode_errors || t->mismatches || !t->frames_out ||
         (t->ref && t->ref_off != t->ref_len)) ? 2 : 0;

done:
   if (t->out)
      fclose(t->out);
   for (unsigned i = 0; i < MAX_SLOTS; i++)
      if (t->views[i])
         t->views[i]->Release();
   if (t->dec)
      t->dec->Release();
   if (t->tex)
      t->tex->Release();
   if (t->staging)
      t->staging->Release();
   free(t->frame);
   free(t->ref);
   free(t);
   return rc;
}
