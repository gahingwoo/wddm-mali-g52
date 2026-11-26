/*
 * vptest - the D3D11 video processor and NV12 plane views (vdectest --vp).
 *
 * What Media Foundation's Media Engine and Edge's copy path do with a
 * decoded NV12 picture, on a picture vdectest makes itself:
 *
 *   1. NV12 (BIND_DECODER | BIND_SHADER_RESOURCE), filled by
 *      UpdateSubresource with a pattern: luma changing at every texel,
 *      steep chroma ramps.
 *   2. VideoProcessorBlt to B8G8R8A8 at the same size, BT.709 studio range
 *      in, full range out; then 2x up and 0.6x down with a source
 *      rectangle; then a destination rectangle inside a target rectangle
 *      with a background colour, on a surface cleared to a third colour.
 *      Each read back and compared with a model of the sampling the UMD
 *      does (bilinear luma, bilinear chroma at centred chroma positions).
 *   3. B8G8R8A8 in, scaled.
 *   4. The NV12 texture as an R8_UNORM and an R8G8_UNORM shader resource
 *      view, read by a pixel shader with Load into an RGBA texture: luma,
 *      Cb and Cr exactly as written.
 *
 * Tolerance: 2 per channel at the same size, 3 scaled (filter precision).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vptest.h"

#pragma comment(lib, "d3dcompiler.lib")

static inline int imax(int a, int b) { return a > b ? a : b; }

#define W 128
#define H 96

static uint8_t g_y[W * H], g_uv[W * H / 2];

static void
make_pattern(void)
{
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
         g_y[y * W + x] = (uint8_t)(16 + (x * 37 + y * 11) % 220);
   for (int y = 0; y < H / 2; y++)
      for (int x = 0; x < W / 2; x++) {
         g_uv[y * W + 2 * x] = (uint8_t)(40 + y * 7 / 2);         /* Cb: down the rows */
         g_uv[y * W + 2 * x + 1] = (uint8_t)(220 - x * 5 / 2);    /* Cr: across */
      }
}

/* ---------------------------------------------------- the model */

static float
texel(const uint8_t *p, int pitch, int w, int h, int x, int y)
{
   x = x < 0 ? 0 : x >= w ? w - 1 : x;
   y = y < 0 ? 0 : y >= h ? h - 1 : y;
   return p[y * pitch + x] / 255.0f;
}

static float
bgra_texel(const uint8_t *p, int w, int h, int x, int y, int ch)
{
   x = x < 0 ? 0 : x >= w ? w - 1 : x;
   y = y < 0 ? 0 : y >= h ? h - 1 : y;
   return p[((size_t)y * w + x) * 4 + ch] / 255.0f;
}

/* Luma as the sampler does it: bilinear at (sx, clamped sy), over the
 * whole NV12 texture (H * 3 / 2 rows, the chroma rows below). */
static float
model_luma(float sx, float sy)
{
   static uint8_t whole[W * H * 3 / 2];
   static int made;
   if (!made) {
      memcpy(whole, g_y, sizeof g_y);
      memcpy(whole + sizeof g_y, g_uv, sizeof g_uv);
      made = 1;
   }
   sy = sy < 0.5f ? 0.5f : sy > H - 0.5f ? H - 0.5f : sy;
   float u = sx - 0.5f, v = sy - 0.5f;
   int x0 = (int)floorf(u), y0 = (int)floorf(v);
   float fx = u - x0, fy = v - y0;
   float a = texel(whole, W, W, H * 3 / 2, x0, y0), b = texel(whole, W, W, H * 3 / 2, x0 + 1, y0);
   float c = texel(whole, W, W, H * 3 / 2, x0, y0 + 1), d = texel(whole, W, W, H * 3 / 2, x0 + 1, y0 + 1);
   return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

static void
model_chroma(float sx, float sy, float *cb, float *cr)
{
   float u = sx * 0.5f - 0.5f, v = sy * 0.5f - 0.5f;
   int fx0 = (int)floorf(u), fy0 = (int)floorf(v);
   float fx = u - fx0, fy = v - fy0;
   int x0 = fx0 < 0 ? 0 : fx0 > W / 2 - 1 ? W / 2 - 1 : fx0;
   int x1 = fx0 + 1 < 0 ? 0 : fx0 + 1 > W / 2 - 1 ? W / 2 - 1 : fx0 + 1;
   int y0 = fy0 < 0 ? 0 : fy0 > H / 2 - 1 ? H / 2 - 1 : fy0;
   int y1 = fy0 + 1 < 0 ? 0 : fy0 + 1 > H / 2 - 1 ? H / 2 - 1 : fy0 + 1;
   for (int k = 0; k < 2; k++) {
      float a = g_uv[y0 * W + 2 * x0 + k] / 255.0f, b = g_uv[y0 * W + 2 * x1 + k] / 255.0f;
      float c = g_uv[y1 * W + 2 * x0 + k] / 255.0f, d = g_uv[y1 * W + 2 * x1 + k] / 255.0f;
      float r = (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
      *(k ? cr : cb) = r;
   }
}

/* BT.709 studio range YCbCr to full range RGB. */
static void
model_rgb(float y, float cb, float cr, float rgb[3])
{
   const float kr = 0.2126f, kb = 0.0722f, kg = 1 - kr - kb;
   float yy = (y * 255 - 16) / 219, pb = (cb * 255 - 128) / 224, pr = (cr * 255 - 128) / 224;
   rgb[0] = yy + (2 - 2 * kr) * pr;
   rgb[1] = yy - 2 * kb * (1 - kb) / kg * pb - 2 * kr * (1 - kr) / kg * pr;
   rgb[2] = yy + (2 - 2 * kb) * pb;
   for (int i = 0; i < 3; i++)
      rgb[i] = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
}

/* ----------------------------------------------------- D3D plumbing */

struct Vp {
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   ID3D11VideoDevice *vdev;
   ID3D11VideoContext *vctx;
   int failures;
};

static ID3D11Texture2D *
make_texture(Vp *v, DXGI_FORMAT f, UINT w, UINT h, UINT bind, D3D11_USAGE usage, UINT cpu)
{
   D3D11_TEXTURE2D_DESC d = {};
   ID3D11Texture2D *t = NULL;
   d.Width = w;
   d.Height = h;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = f;
   d.SampleDesc.Count = 1;
   d.Usage = usage;
   d.BindFlags = bind;
   d.CPUAccessFlags = cpu;
   HRESULT hr = v->dev->CreateTexture2D(&d, NULL, &t);
   if (FAILED(hr))
      printf("  CreateTexture2D %ux%u format %u bind 0x%x: 0x%08lx\n", w, h, (unsigned)f, bind,
             (unsigned long)hr);
   return t;
}

/* The texture's pixels, 4 bytes each, tightly packed. */
static uint8_t *
read_back(Vp *v, ID3D11Texture2D *t, UINT w, UINT h, DXGI_FORMAT f)
{
   ID3D11Texture2D *s = make_texture(v, f, w, h, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
   D3D11_MAPPED_SUBRESOURCE m;
   uint8_t *out = NULL;
   if (!s)
      return NULL;
   v->ctx->CopyResource(s, t);
   if (SUCCEEDED(v->ctx->Map(s, 0, D3D11_MAP_READ, 0, &m))) {
      out = (uint8_t *)malloc((size_t)w * h * 4);
      for (UINT y = 0; y < h; y++)
         memcpy(out + (size_t)y * w * 4, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
      v->ctx->Unmap(s, 0);
   }
   s->Release();
   return out;
}

static void
result(Vp *v, const char *what, unsigned bad, unsigned total, int maxdiff, int x, int y)
{
   printf("  %-44s %s (%u of %u pixels off, largest difference %d", what, bad ? "MISMATCH" : "MATCH",
          bad, total, maxdiff);
   if (bad)
      printf(", first at %d,%d", x, y);
   printf(")\n");
   if (bad)
      v->failures++;
}

/* Compare a BGRA read back against the model over dst (in the output),
 * from src (in the source); outside dst inside target: bg; outside
 * target: keep. */
static void
check_blit(Vp *v, const char *what, const uint8_t *px, UINT ow, UINT oh, RECT src, RECT dst, RECT target,
           const uint8_t bg[3], const uint8_t keep[3], int tol, bool rgb_source, const uint8_t *rgb_src,
           UINT rw, UINT rh)
{
   unsigned bad = 0;
   int maxdiff = 0, fx = -1, fy = -1;
   for (UINT y = 0; y < oh; y++)
      for (UINT x = 0; x < ow; x++) {
         const uint8_t *p = px + ((size_t)y * ow + x) * 4;   /* B G R A */
         float want[3];
         bool in_target = (LONG)x >= target.left && (LONG)x < target.right && (LONG)y >= target.top &&
                          (LONG)y < target.bottom;
         bool in_dst = (LONG)x >= dst.left && (LONG)x < dst.right && (LONG)y >= dst.top &&
                       (LONG)y < dst.bottom;
         if (!in_target) {
            for (int i = 0; i < 3; i++)
               want[i] = keep[i] / 255.0f;
         } else if (!in_dst) {
            for (int i = 0; i < 3; i++)
               want[i] = bg[i] / 255.0f;
         } else {
            float sx = src.left + (x + 0.5f - dst.left) * (src.right - src.left) / (dst.right - dst.left);
            float sy = src.top + (y + 0.5f - dst.top) * (src.bottom - src.top) / (dst.bottom - dst.top);
            if (rgb_source) {
               /* bilinear over the BGRA source */
               float u = sx - 0.5f, vv = sy - 0.5f;
               int x0 = (int)floorf(u), y0 = (int)floorf(vv);
               float ax = u - x0, ay = vv - y0;
               for (int c = 0; c < 3; c++) {
                  int ch = 2 - c;   /* R G B from B G R A */
                  float a = bgra_texel(rgb_src, rw, rh, x0, y0, ch);
                  float b = bgra_texel(rgb_src, rw, rh, x0 + 1, y0, ch);
                  float cc = bgra_texel(rgb_src, rw, rh, x0, y0 + 1, ch);
                  float d = bgra_texel(rgb_src, rw, rh, x0 + 1, y0 + 1, ch);
                  want[c] = (a * (1 - ax) + b * ax) * (1 - ay) + (cc * (1 - ax) + d * ax) * ay;
               }
            } else {
               float cb, cr;
               model_chroma(sx, sy, &cb, &cr);
               model_rgb(model_luma(sx, sy), cb, cr, want);
            }
         }
         int r = (int)lrintf(want[0] * 255), g = (int)lrintf(want[1] * 255), b = (int)lrintf(want[2] * 255);
         int d = abs(p[2] - r);
         d = imax(d, abs(p[1] - g));
         d = imax(d, abs(p[0] - b));
         if (d > maxdiff)
            maxdiff = d;
         if (d > tol) {
            if (!bad++) {
               fx = x;
               fy = y;
               printf("    at %u,%u: got %u %u %u, want %d %d %d\n", x, y, p[2], p[1], p[0], r, g, b);
            }
         }
      }
   result(v, what, bad, ow * oh, maxdiff, fx, fy);
}

/* ------------------------------------------------------------- tests */

static ID3D11VideoProcessor *
make_vp(Vp *v, UINT iw, UINT ih, UINT ow, UINT oh, ID3D11VideoProcessorEnumerator **pe)
{
   D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
   ID3D11VideoProcessor *vp = NULL;
   cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
   cd.InputWidth = iw;
   cd.InputHeight = ih;
   cd.OutputWidth = ow;
   cd.OutputHeight = oh;
   cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
   HRESULT hr = v->vdev->CreateVideoProcessorEnumerator(&cd, pe);
   if (FAILED(hr)) {
      printf("  CreateVideoProcessorEnumerator: 0x%08lx\n", (unsigned long)hr);
      return NULL;
   }
   hr = v->vdev->CreateVideoProcessor(*pe, 0, &vp);
   if (FAILED(hr))
      printf("  CreateVideoProcessor: 0x%08lx\n", (unsigned long)hr);
   return vp;
}

static HRESULT
blit(Vp *v, ID3D11VideoProcessorEnumerator *e, ID3D11VideoProcessor *vp, ID3D11Texture2D *in,
     ID3D11Texture2D *out, const RECT *src, const RECT *dst, const RECT *target, const float *bg,
     bool ycbcr_in)
{
   D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
   D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
   ID3D11VideoProcessorInputView *iv = NULL;
   ID3D11VideoProcessorOutputView *ov = NULL;
   HRESULT hr;

   ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
   ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
   hr = v->vdev->CreateVideoProcessorInputView(in, e, &ivd, &iv);
   if (FAILED(hr)) {
      printf("  CreateVideoProcessorInputView: 0x%08lx\n", (unsigned long)hr);
      return hr;
   }
   hr = v->vdev->CreateVideoProcessorOutputView(out, e, &ovd, &ov);
   if (FAILED(hr)) {
      printf("  CreateVideoProcessorOutputView: 0x%08lx\n", (unsigned long)hr);
      iv->Release();
      return hr;
   }

   D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
   cs.YCbCr_Matrix = ycbcr_in ? 1 : 0;       /* BT.709 */
   cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
   cs.RGB_Range = 0;
   v->vctx->VideoProcessorSetStreamColorSpace(vp, 0, &cs);
   D3D11_VIDEO_PROCESSOR_COLOR_SPACE ocs = {};
   ocs.RGB_Range = 0;                         /* 0-255 */
   ocs.YCbCr_Matrix = 1;
   v->vctx->VideoProcessorSetOutputColorSpace(vp, &ocs);
   v->vctx->VideoProcessorSetStreamSourceRect(vp, 0, src != NULL, src);
   v->vctx->VideoProcessorSetStreamDestRect(vp, 0, dst != NULL, dst);
   v->vctx->VideoProcessorSetOutputTargetRect(vp, target != NULL, target);
   D3D11_VIDEO_COLOR c = {};
   c.RGBA.R = bg ? bg[0] : 0;
   c.RGBA.G = bg ? bg[1] : 0;
   c.RGBA.B = bg ? bg[2] : 0;
   c.RGBA.A = 1;
   v->vctx->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &c);

   D3D11_VIDEO_PROCESSOR_STREAM s = {};
   s.Enable = TRUE;
   s.pInputSurface = iv;
   hr = v->vctx->VideoProcessorBlt(vp, ov, 0, 1, &s);
   if (FAILED(hr))
      printf("  VideoProcessorBlt: 0x%08lx\n", (unsigned long)hr);
   iv->Release();
   ov->Release();
   return hr;
}

static void
clear(Vp *v, ID3D11Texture2D *t, const uint8_t rgb[3])
{
   ID3D11RenderTargetView *rtv = NULL;
   if (FAILED(v->dev->CreateRenderTargetView(t, NULL, &rtv)))
      return;
   float c[4] = { rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1 };
   v->ctx->ClearRenderTargetView(rtv, c);
   rtv->Release();
}

static const char vs_src[] =
   "float4 main(uint id : SV_VertexID) : SV_Position {\n"
   "   float2 t = float2((id << 1) & 2, id & 2);\n"
   "   return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
   "}\n";
static const char ps_src[] =
   "Texture2D<float> ty : register(t0);\n"
   "Texture2D<float2> tc : register(t1);\n"
   "float4 main(float4 pos : SV_Position) : SV_Target {\n"
   "   int2 p = int2(pos.xy);\n"
   "   return float4(ty.Load(int3(p, 0)), tc.Load(int3(p / 2, 0)), 1);\n"
   "}\n";

static void
test_plane_views(Vp *v, ID3D11Texture2D *nv12)
{
   ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
   ID3D11VertexShader *vs = NULL;
   ID3D11PixelShader *ps = NULL;
   ID3D11ShaderResourceView *srv[2] = {};
   ID3D11RenderTargetView *rtv = NULL;
   ID3D11Texture2D *rt = NULL;
   HRESULT hr;

   printf("NV12 as two shader resource views:\n");
   for (int i = 0; i < 2; i++) {
      D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
      d.Format = i ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
      d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      d.Texture2D.MipLevels = 1;
      hr = v->dev->CreateShaderResourceView(nv12, &d, &srv[i]);
      printf("  CreateShaderResourceView %s: 0x%08lx\n", i ? "R8G8_UNORM (chroma)" : "R8_UNORM (luma)",
             (unsigned long)hr);
      if (FAILED(hr)) {
         v->failures++;
         goto out;
      }
   }
   if (FAILED(D3DCompile(vs_src, sizeof vs_src - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err)) ||
       FAILED(D3DCompile(ps_src, sizeof ps_src - 1, "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &psb, &err))) {
      printf("  D3DCompile: %s\n", err ? (const char *)err->GetBufferPointer() : "failed");
      v->failures++;
      goto out;
   }
   v->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs);
   v->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps);
   rt = make_texture(v, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
   if (!vs || !ps || !rt || FAILED(v->dev->CreateRenderTargetView(rt, NULL, &rtv))) {
      printf("  shaders or render target: failed\n");
      v->failures++;
      goto out;
   }
   {
      D3D11_VIEWPORT vpt = { 0, 0, (float)W, (float)H, 0, 1 };
      v->ctx->OMSetRenderTargets(1, &rtv, NULL);
      v->ctx->RSSetViewports(1, &vpt);
      v->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      v->ctx->IASetInputLayout(NULL);
      v->ctx->VSSetShader(vs, NULL, 0);
      v->ctx->PSSetShader(ps, NULL, 0);
      v->ctx->PSSetShaderResources(0, 2, srv);
      v->ctx->Draw(3, 0);
      ID3D11ShaderResourceView *none[2] = {};
      v->ctx->PSSetShaderResources(0, 2, none);
      v->ctx->OMSetRenderTargets(0, NULL, NULL);
   }
   {
      uint8_t *px = read_back(v, rt, W, H, DXGI_FORMAT_R8G8B8A8_UNORM);
      unsigned bad = 0;
      int maxd = 0, fx = -1, fy = -1;
      for (int y = 0; px && y < H; y++)
         for (int x = 0; x < W; x++) {
            const uint8_t *p = px + ((size_t)y * W + x) * 4;
            int want[3] = { g_y[y * W + x], g_uv[(y / 2) * W + (x / 2) * 2], g_uv[(y / 2) * W + (x / 2) * 2 + 1] };
            int d = 0;
            for (int c = 0; c < 3; c++)
               d = imax(d, abs(p[c] - want[c]));
            maxd = imax(maxd, d);
            if (d && !bad++) {
               fx = x;
               fy = y;
               printf("    at %d,%d: got %u %u %u, want %d %d %d\n", x, y, p[0], p[1], p[2], want[0],
                      want[1], want[2]);
            }
         }
      if (!px)
         bad = W * H;
      result(v, "luma, Cb, Cr through the plane views", bad, W * H, maxd, fx, fy);
      free(px);
   }
out:
   for (int i = 0; i < 2; i++)
      if (srv[i])
         srv[i]->Release();
   if (rtv) rtv->Release();
   if (rt) rt->Release();
   if (vs) vs->Release();
   if (ps) ps->Release();
   if (vsb) vsb->Release();
   if (psb) psb->Release();
   if (err) err->Release();
}

int
vp_run_tests(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *vdev,
             ID3D11VideoContext *vctx)
{
   Vp v = { dev, ctx, vdev, vctx, 0 };
   ID3D11VideoProcessorEnumerator *e = NULL;
   ID3D11VideoProcessor *vp = NULL;
   ID3D11Texture2D *nv12 = NULL, *out = NULL, *big = NULL, *shrunk = NULL, *bgra_in = NULL;
   UINT f = 0, fs = 0;

   make_pattern();
   printf("video processor:\n");
   dev->CheckFormatSupport(DXGI_FORMAT_NV12, &fs);
   printf("  NV12 format support 0x%08x%s%s\n", fs,
          (fs & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) ? " vp-input" : "",
          (fs & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? " sample" : "");
   dev->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &fs);
   printf("  B8G8R8A8 format support 0x%08x%s\n", fs,
          (fs & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) ? " vp-output" : "");

   vp = make_vp(&v, W, H, W * 2, H * 2, &e);
   if (!vp)
      return 1;
   e->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &f);
   printf("  enumerator: NV12 0x%x", f);
   e->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &f);
   printf(", B8G8R8A8 0x%x", f);
   {
      D3D11_VIDEO_PROCESSOR_CAPS caps = {};
      e->GetVideoProcessorCaps(&caps);
      printf(", device caps 0x%x, feature caps 0x%x, %u input stream(s)\n", caps.DeviceCaps,
             caps.FeatureCaps, caps.MaxInputStreams);
   }

   nv12 = make_texture(&v, DXGI_FORMAT_NV12, W, H, D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE,
                       D3D11_USAGE_DEFAULT, 0);
   out = make_texture(&v, DXGI_FORMAT_B8G8R8A8_UNORM, W, H, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
   big = make_texture(&v, DXGI_FORMAT_B8G8R8A8_UNORM, W * 2, H * 2, D3D11_BIND_RENDER_TARGET,
                      D3D11_USAGE_DEFAULT, 0);
   shrunk = make_texture(&v, DXGI_FORMAT_B8G8R8A8_UNORM, 77, 58, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
   if (!nv12 || !out || !big || !shrunk)
      return 1;
   {
      /* NV12 in one go, as D3D lays it out: luma rows, then chroma rows. */
      static uint8_t whole[W * H * 3 / 2];
      memcpy(whole, g_y, sizeof g_y);
      memcpy(whole + sizeof g_y, g_uv, sizeof g_uv);
      ctx->UpdateSubresource(nv12, 0, NULL, whole, W, 0);
   }

   const uint8_t black[3] = { 0, 0, 0 }, keep[3] = { 10, 200, 30 }, bgc[3] = { 51, 102, 204 };
   const float bgf[3] = { 0.2f, 0.4f, 0.8f };
   RECT full = { 0, 0, W, H };

   if (SUCCEEDED(blit(&v, e, vp, nv12, out, NULL, NULL, NULL, NULL, true))) {
      uint8_t *px = read_back(&v, out, W, H, DXGI_FORMAT_B8G8R8A8_UNORM);
      if (px)
         check_blit(&v, "NV12 -> BGRA, same size", px, W, H, full, full, full, black, black, 2, false, NULL, 0, 0);
      free(px);
   } else {
      v.failures++;
   }
   {
      RECT dst = { 0, 0, W * 2, H * 2 };
      if (SUCCEEDED(blit(&v, e, vp, nv12, big, NULL, NULL, NULL, NULL, true))) {
         uint8_t *px = read_back(&v, big, W * 2, H * 2, DXGI_FORMAT_B8G8R8A8_UNORM);
         if (px)
            check_blit(&v, "NV12 -> BGRA, 2x", px, W * 2, H * 2, full, dst, dst, black, black, 3, false, NULL, 0, 0);
         free(px);
      } else {
         v.failures++;
      }
   }
   {
      RECT src = { 10, 6, 116, 86 }, dst = { 0, 0, 77, 58 };
      if (SUCCEEDED(blit(&v, e, vp, nv12, shrunk, &src, NULL, NULL, NULL, true))) {
         uint8_t *px = read_back(&v, shrunk, 77, 58, DXGI_FORMAT_B8G8R8A8_UNORM);
         if (px)
            check_blit(&v, "NV12 -> BGRA, source rect, 0.73x", px, 77, 58, src, dst, dst, black, black, 3,
                       false, NULL, 0, 0);
         free(px);
      } else {
         v.failures++;
      }
   }
   {
      RECT dst = { 40, 30, 200, 150 }, target = { 20, 10, 230, 180 };
      clear(&v, big, keep);
      if (SUCCEEDED(blit(&v, e, vp, nv12, big, NULL, &dst, &target, bgf, true))) {
         uint8_t *px = read_back(&v, big, W * 2, H * 2, DXGI_FORMAT_B8G8R8A8_UNORM);
         if (px)
            check_blit(&v, "dest rect, target rect, background", px, W * 2, H * 2, full, dst, target, bgc,
                       keep, 3, false, NULL, 0, 0);
         free(px);
      } else {
         v.failures++;
      }
   }
   {
      /* BGRA in: the 1:1 result above, scaled to 77x58. */
      uint8_t *src_px = read_back(&v, out, W, H, DXGI_FORMAT_B8G8R8A8_UNORM);
      bgra_in = make_texture(&v, DXGI_FORMAT_B8G8R8A8_UNORM, W, H, D3D11_BIND_SHADER_RESOURCE,
                             D3D11_USAGE_DEFAULT, 0);
      RECT dst = { 0, 0, 77, 58 };
      if (src_px && bgra_in) {
         HRESULT hr;
         ctx->UpdateSubresource(bgra_in, 0, NULL, src_px, W * 4, 0);
         hr = blit(&v, e, vp, bgra_in, shrunk, NULL, NULL, NULL, NULL, false);
         if (FAILED(hr)) {
            /* Which binding the runtime wants for an RGB input: say so, and
             * go on with a render target one. */
            printf("  BGRA input with BIND_SHADER_RESOURCE only: 0x%08lx; again with RENDER_TARGET too\n",
                   (unsigned long)hr);
            bgra_in->Release();
            bgra_in = make_texture(&v, DXGI_FORMAT_B8G8R8A8_UNORM, W, H,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
            if (bgra_in)
               ctx->UpdateSubresource(bgra_in, 0, NULL, src_px, W * 4, 0);
         }
         if (bgra_in && SUCCEEDED(blit(&v, e, vp, bgra_in, shrunk, NULL, NULL, NULL, NULL, false))) {
            uint8_t *px = read_back(&v, shrunk, 77, 58, DXGI_FORMAT_B8G8R8A8_UNORM);
            if (px)
               check_blit(&v, "BGRA -> BGRA, 0.6x", px, 77, 58, full, dst, dst, black, black, 3, true, src_px,
                          W, H);
            free(px);
         } else {
            v.failures++;
         }
      } else {
         v.failures++;
      }
      free(src_px);
   }

   test_plane_views(&v, nv12);

   if (bgra_in) bgra_in->Release();
   shrunk->Release();
   big->Release();
   out->Release();
   nv12->Release();
   vp->Release();
   e->Release();
   printf("video processor tests: %s (%d failed)\n", v.failures ? "FAIL" : "PASS", v.failures);
   return v.failures ? 2 : 0;
}

/* One decoded slice of the decoder's texture through the video processor
 * (vdectest after the decode): the fake decoder's flat picture, converted. */
int
vp_check_decoded(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *vdev,
                 ID3D11VideoContext *vctx, ID3D11Texture2D *tex, UINT slice, UINT w, UINT h,
                 uint8_t luma, uint8_t cb, uint8_t cr)
{
   Vp v = { dev, ctx, vdev, vctx, 0 };
   ID3D11VideoProcessorEnumerator *e = NULL;
   ID3D11VideoProcessor *vp = make_vp(&v, w, h, w, h, &e);
   ID3D11Texture2D *out = make_texture(&v, DXGI_FORMAT_B8G8R8A8_UNORM, w, h, D3D11_BIND_RENDER_TARGET,
                                       D3D11_USAGE_DEFAULT, 0);
   D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
   D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
   ID3D11VideoProcessorInputView *iv = NULL;
   ID3D11VideoProcessorOutputView *ov = NULL;
   int rc = 2;

   if (!vp || !out)
      goto done;
   ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
   ivd.Texture2D.ArraySlice = slice;
   ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
   if (FAILED(vdev->CreateVideoProcessorInputView(tex, e, &ivd, &iv)) ||
       FAILED(vdev->CreateVideoProcessorOutputView(out, e, &ovd, &ov))) {
      printf("decoded picture through the video processor: views failed\n");
      goto done;
   }
   {
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
      cs.YCbCr_Matrix = 1;
      cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
      vctx->VideoProcessorSetStreamColorSpace(vp, 0, &cs);
      D3D11_VIDEO_PROCESSOR_STREAM s = {};
      s.Enable = TRUE;
      s.pInputSurface = iv;
      if (FAILED(vctx->VideoProcessorBlt(vp, ov, 0, 1, &s)))
         goto done;
      uint8_t *px = read_back(&v, out, w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
      float want[3];
      model_rgb(luma / 255.0f, cb / 255.0f, cr / 255.0f, want);
      unsigned bad = 0;
      for (UINT i = 0; px && i < w * h; i++) {
         const uint8_t *p = px + (size_t)i * 4;
         if (abs(p[2] - (int)lrintf(want[0] * 255)) > 2 || abs(p[1] - (int)lrintf(want[1] * 255)) > 2 ||
             abs(p[0] - (int)lrintf(want[2] * 255)) > 2)
            bad++;
      }
      printf("decoded picture (slice %u) through the video processor: %s (%u of %u pixels off)\n", slice,
             (px && !bad) ? "MATCH" : "MISMATCH", px ? bad : w * h, w * h);
      rc = (px && !bad) ? 0 : 2;
      free(px);
   }
done:
   if (iv) iv->Release();
   if (ov) ov->Release();
   if (out) out->Release();
   if (vp) vp->Release();
   if (e) e->Release();
   return rc;
}

/* ------------------------------------------- plane views of the decoder's texture */

/* vdectest --srv / --srv-late: a slice of the decoder's texture array as
 * an R8_UNORM and an R8G8_UNORM shader resource view (Texture2DArray, one
 * slice: the runtime refuses more for NV12), as Chromium and mpv sample a
 * decoded picture; read through them by a pixel shader (Load at the view's
 * slice a constant buffer names, 0). */
/* The chroma is read swizzled (.yx, then put back): fxc folds that into
 * the resource operand's swizzle, which Chromium's shaders also have
 * (sample r1.yz, ..., t1.xxyx) and which tgsi_to_nir ignored before Mesa
 * 0086: on Panfrost Cb and Cr come out swapped without it. softpipe runs
 * TGSI and always applied it. */
static const char ps_array_src[] =
   "Texture2DArray<float> ty : register(t0);\n"
   "Texture2DArray<float2> tc : register(t1);\n"
   "cbuffer c : register(b0) { uint slice; };\n"
   "float4 main(float4 pos : SV_Position) : SV_Target {\n"
   "   int2 p = int2(pos.xy);\n"
   "   float2 c = tc.Load(int4(p / 2, slice, 0)).yx;\n"
   "   return float4(ty.Load(int4(p, slice, 0)), c.y, c.x, 1);\n"
   "}\n";

int
vp_make_plane_views(ID3D11Device *dev, ID3D11Texture2D *tex, UINT slice, ID3D11ShaderResourceView *srv[2])
{
   for (int i = 0; i < 2; i++) {
      D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
      d.Format = i ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
      d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      d.Texture2DArray.MipLevels = 1;
      d.Texture2DArray.FirstArraySlice = slice;
      d.Texture2DArray.ArraySize = 1;
      HRESULT hr = dev->CreateShaderResourceView(tex, &d, &srv[i]);
      if (FAILED(hr)) {
         printf("decoder texture slice %u as %s shader resource view: 0x%08lx\n", slice,
                i ? "R8G8_UNORM" : "R8_UNORM", (unsigned long)hr);
         return 1;
      }
   }
   return 0;
}

int
vp_check_plane_views(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *srv[2],
                     UINT slice, UINT tex_w, UINT tex_h, UINT x0, UINT y0, UINT w, UINT h,
                     const uint8_t *i420)
{
   /* The shaders of each device this has run on. */
   static struct { ID3D11Device *dev; ID3D11VertexShader *vs; ID3D11PixelShader *ps; } cache[4];
   ID3D11VertexShader *vs = NULL;
   ID3D11PixelShader *ps = NULL;
   Vp v = { dev, ctx, NULL, NULL, 0 };
   ID3D11Texture2D *rt = NULL;
   ID3D11RenderTargetView *rtv = NULL;
   ID3D11Buffer *cb = NULL;
   int rc = 2;
   unsigned k;

   for (k = 0; k < 4 && cache[k].dev; k++)
      if (cache[k].dev == dev) {
         vs = cache[k].vs;
         ps = cache[k].ps;
         break;
      }
   if (!vs || !ps) {
      ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
      if (k >= 4)
         return 1;
      if (FAILED(D3DCompile(vs_src, sizeof vs_src - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err)) ||
          FAILED(D3DCompile(ps_array_src, sizeof ps_array_src - 1, "ps", NULL, NULL, "main", "ps_4_0", 0, 0,
                            &psb, &err))) {
         printf("plane views: D3DCompile: %s\n", err ? (const char *)err->GetBufferPointer() : "failed");
         return 1;
      }
      dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs);
      dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps);
      vsb->Release();
      psb->Release();
      if (!vs || !ps)
         return 1;
      cache[k].dev = dev;
      cache[k].vs = vs;
      cache[k].ps = ps;
   }
   {
      D3D11_BUFFER_DESC bd = {};
      UINT data[4] = { slice, 0, 0, 0 };
      D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
      bd.ByteWidth = sizeof data;
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (FAILED(dev->CreateBuffer(&bd, &init, &cb)))
         return 1;
   }
   rt = make_texture(&v, DXGI_FORMAT_R8G8B8A8_UNORM, tex_w, tex_h, D3D11_BIND_RENDER_TARGET,
                     D3D11_USAGE_DEFAULT, 0);
   if (!rt || FAILED(dev->CreateRenderTargetView(rt, NULL, &rtv)))
      goto done;
   {
      D3D11_VIEWPORT vpt = { 0, 0, (float)tex_w, (float)tex_h, 0, 1 };
      ctx->OMSetRenderTargets(1, &rtv, NULL);
      ctx->RSSetViewports(1, &vpt);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->IASetInputLayout(NULL);
      ctx->VSSetShader(vs, NULL, 0);
      ctx->PSSetShader(ps, NULL, 0);
      ctx->PSSetConstantBuffers(0, 1, &cb);
      ctx->PSSetShaderResources(0, 2, srv);
      ctx->Draw(3, 0);
      ID3D11ShaderResourceView *none[2] = {};
      ctx->PSSetShaderResources(0, 2, none);
      ctx->OMSetRenderTargets(0, NULL, NULL);
   }
   {
      uint8_t *px = read_back(&v, rt, tex_w, tex_h, DXGI_FORMAT_R8G8B8A8_UNORM);
      const uint8_t *fu = i420 + (size_t)w * h, *fv = fu + (size_t)(w / 2) * (h / 2);
      unsigned bad = 0;
      for (UINT y = 0; px && y < (h & ~1u); y++)
         for (UINT x = 0; x < (w & ~1u); x++) {
            const uint8_t *p = px + ((size_t)(y + y0) * tex_w + x + x0) * 4;
            const uint8_t wy = i420[(size_t)y * w + x];
            const uint8_t wu = fu[(size_t)(y / 2) * (w / 2) + x / 2], wv = fv[(size_t)(y / 2) * (w / 2) + x / 2];
            if (p[0] != wy || p[1] != wu || p[2] != wv) {
               if (!bad++)
                  printf("plane views, slice %u at %u,%u: got Y %u Cb %u Cr %u, want %u %u %u\n", slice,
                         x + x0, y + y0, p[0], p[1], p[2], wy, wu, wv);
            }
         }
      rc = (px && !bad) ? 0 : 2;
      free(px);
   }
done:
   if (rtv) rtv->Release();
   if (rt) rt->Release();
   if (cb) cb->Release();
   return rc;
}
