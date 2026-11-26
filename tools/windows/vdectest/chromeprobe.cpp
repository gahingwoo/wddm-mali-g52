/*
 * chromeprobe - what Chromium's D3D11VideoDecoder asks of a D3D11 device,
 * step by step, for each codec (vdectest --chromium). Read from Chromium
 * 158 (branch 8127) media/gpu/windows and gpu/command_buffer/service/
 * shared_image:
 *
 *   d3d11_video_decoder_backend.cc  the ANGLE device, GetFeatureLevel() >= 11_0
 *                                   or no decoder at all; As(ID3D11VideoDevice1)
 *   supported_profile_helpers.cc    GetVideoDecoderProfileCount/Profile, then
 *                                   GetVideoDecoderConfigCount at 1920x1080,
 *                                   2560x1440, 3840x2160, 4096x2160, 4096x2304,
 *                                   4096x4096, ... (the largest with a config is
 *                                   the limit; H.264 is listed up to 1920x1088
 *                                   when its GUID is missing or no size has one)
 *   d3d11_video_decoder_wrapper.cc  configs at the container's size (1920x1080):
 *                                   ConfigBitstreamRaw 2 for H.264, 1 for HEVC,
 *                                   VP9 and AV1; CreateVideoDecoder;
 *                                   ConfigDecoderSpecific bit 14: single textures
 *   d3d_video_decoder.cc            bit 14 set: one texture per picture, shared
 *                                   by NT handle; clear: one array
 *   d3d_decoder_configurator.cc     the output texture: NV12, BIND_DECODER |
 *                                   BIND_SHADER_RESOURCE, MiscFlags SHARED |
 *                                   SHARED_NTHANDLE (NT handle), else 0 with a
 *                                   decode swap chain, else SHARED (the case
 *                                   with --disable-direct-composition)
 *   d3d11_texture_selector.cc,      NV12 out: the decoder's texture itself
 *   d3d_texture_wrapper.cc          (DefaultD3DTextureWrapper), no copy
 *   d3d_image_backing.cc,           the shared image: an EGL image per plane
 *   ANGLE ExternalImageSiblingImpl11  (EGL_D3D11_TEXTURE_PLANE_ANGLE, ARRAY_SLICE),
 *                                   which ANGLE samples through an R8_UNORM or
 *                                   R8G8_UNORM Texture2DArray view of that one
 *                                   slice, on the same device; another device
 *                                   (Dawn) opens the NT handle
 *
 * Nothing is decoded (vdectest --chromium-tex decodes through such a
 * texture). Every step prints its result; the exit code is 0 when
 * everything Chromium needs is there, 4 otherwise (the first missing piece
 * is named as "chromium: blocked by").
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <stdio.h>

#include "chromeprobe.h"

static const GUID kH264 = { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
static const GUID kHevcMain = { 0x5b11d51b, 0x2f4c, 0x4452, { 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0 } };
static const GUID kHevcMain10 = { 0x107af0e0, 0xef1a, 0x4d19, { 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13 } };
static const GUID kVp9P0 = { 0x463707f8, 0xa1d0, 0x4585, { 0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e } };
static const GUID kVp9P2 = { 0xa4c749ef, 0x6ecf, 0x48aa, { 0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7 } };
static const GUID kAv1P0 = { 0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } };

static const char *
guid_name(const GUID &g)
{
   if (IsEqualGUID(g, kH264)) return "H.264 VLD no FGT";
   if (IsEqualGUID(g, kHevcMain)) return "HEVC Main";
   if (IsEqualGUID(g, kHevcMain10)) return "HEVC Main10";
   if (IsEqualGUID(g, kVp9P0)) return "VP9 profile 0";
   if (IsEqualGUID(g, kVp9P2)) return "VP9 10-bit profile 2";
   if (IsEqualGUID(g, kAv1P0)) return "AV1 profile 0";
   return "other";
}

static const char *g_blocked;

static void
need(bool ok, const char *what)
{
   if (!ok && !g_blocked)
      g_blocked = what;
}

/* GetResolutionsForGUID: the largest size of Chromium's list with a config. */
static void
resolutions(ID3D11VideoDevice1 *vdev, const GUID &g)
{
   static const UINT sizes[][2] = { { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 4096, 2160 },
                                    { 4096, 2304 }, { 4096, 4096 }, { 7680, 4320 }, { 8192, 4352 },
                                    { 8192, 8192 }, { 16384, 16384 } };
   UINT w = 0, h = 0;

   for (UINT k = 0; k < ARRAYSIZE(sizes); k++) {
      D3D11_VIDEO_DECODER_DESC d = { g, sizes[k][0], sizes[k][1], DXGI_FORMAT_NV12 };
      UINT c = 0;
      if (FAILED(vdev->GetVideoDecoderConfigCount(&d, &c)) || !c)
         break;
      w = sizes[k][0];
      h = sizes[k][1];
   }
   if (w)
      printf("      Chromium's size probe: up to %ux%u\n", w, h);
   else
      printf("      Chromium's size probe: no size%s\n",
             IsEqualGUID(g, kH264) ? " (Chromium then lists H.264 up to 1920x1088 anyway)" : "");
}

/*
 * One codec as D3DVideoDecoder sets it up for a 1080p stream: the decoder
 * at the container's size, the output texture at the pictures' size with
 * the MiscFlags Chromium picks, the output view and the plane views ANGLE
 * makes of a slice, and the texture opened by its handle.
 */
static void
codec(ID3D11Device *dev, ID3D11VideoDevice1 *vdev, const GUID &g, const char *name, UINT raw, UINT pic_h)
{
   D3D11_VIDEO_DECODER_DESC desc = { g, 1920, 1080, DXGI_FORMAT_NV12 };
   D3D11_VIDEO_DECODER_CONFIG cfg = {}, pick = {};
   ID3D11VideoDecoder *dec = NULL;
   ID3D11Texture2D *tex = NULL;
   UINT n = 0, found = 0, single = 0, slice;
   HRESULT hr;
   char what[160];

   printf("  %s, 1080p as Chromium sets it up:\n", name);
   hr = vdev->GetVideoDecoderConfigCount(&desc, &n);
   printf("    GetVideoDecoderConfigCount at 1920x1080: %u (0x%08lx)\n", n, (unsigned long)hr);
   for (UINT i = 0; SUCCEEDED(hr) && i < n; i++) {
      if (FAILED(vdev->GetVideoDecoderConfig(&desc, i, &cfg)))
         continue;
      printf("    config %u: ConfigBitstreamRaw %u, ConfigDecoderSpecific 0x%x\n", i, cfg.ConfigBitstreamRaw,
             cfg.ConfigDecoderSpecific);
      if (cfg.ConfigBitstreamRaw == raw && !found) {
         pick = cfg;
         found = 1;
      }
   }
   snprintf(what, sizeof what, "%s: no config with ConfigBitstreamRaw %u at 1920x1080", name, raw);
   need(found, what);
   if (!found)
      return;
   hr = vdev->CreateVideoDecoder(&desc, &pick, &dec);
   printf("    CreateVideoDecoder 1920x1080, ConfigBitstreamRaw %u: 0x%08lx\n", raw, (unsigned long)hr);
   snprintf(what, sizeof what, "%s: CreateVideoDecoder at 1920x1080 failed", name);
   need(SUCCEEDED(hr), what);
   if (dec) {
      D3D11_VIDEO_DECODER_DESC d2;
      D3D11_VIDEO_DECODER_CONFIG c2;
      if (SUCCEEDED(dec->GetCreationParameters(&d2, &c2)))
         single = (c2.ConfigDecoderSpecific >> 14) & 1;
      printf("    ConfigDecoderSpecific bit 14 %u: %s\n", single,
             single ? "one texture per picture, shared by NT handle" : "one texture array");
   }

   /* The texture: 18 pictures of a 1080p stream (DPB + 1), or single. */
   {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = 1920;
      td.Height = pic_h;
      td.MipLevels = 1;
      td.ArraySize = single ? 1 : 18;
      td.Format = DXGI_FORMAT_NV12;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
      td.MiscFlags = single ? (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
                            : D3D11_RESOURCE_MISC_SHARED;
      hr = dev->CreateTexture2D(&td, NULL, &tex);
      printf("    NV12 1920x%u x%u, DECODER|SHADER_RESOURCE, MiscFlags 0x%x: 0x%08lx\n", pic_h, td.ArraySize,
             td.MiscFlags, (unsigned long)hr);
      snprintf(what, sizeof what, "%s: no shared NV12 decoder texture (MiscFlags 0x%x; without a KMD, as in "
               "CI, none is made)", name, td.MiscFlags);
      need(SUCCEEDED(hr), what);
      if (!tex) {
         td.MiscFlags = 0;
         hr = dev->CreateTexture2D(&td, NULL, &tex);
         printf("    the same with MiscFlags 0 (a decode swap chain's): 0x%08lx\n", (unsigned long)hr);
      }
   }
   slice = single ? 0 : 3;
   if (tex) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC od = {};
      ID3D11VideoDecoderOutputView *ov = NULL;
      od.DecodeProfile = g;
      od.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      od.Texture2D.ArraySlice = slice;
      hr = vdev->CreateVideoDecoderOutputView(tex, &od, &ov);
      printf("    decoder output view, slice %u: 0x%08lx\n", slice, (unsigned long)hr);
      snprintf(what, sizeof what, "%s: no decoder output view on that texture", name);
      need(SUCCEEDED(hr), what);
      if (ov)
         ov->Release();
      for (UINT i = 0; i < 2; i++) {
         D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
         ID3D11ShaderResourceView *srv = NULL;
         sd.Format = i ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
         sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
         sd.Texture2DArray.MipLevels = 1;
         sd.Texture2DArray.FirstArraySlice = slice;
         sd.Texture2DArray.ArraySize = 1;
         hr = dev->CreateShaderResourceView(tex, &sd, &srv);
         printf("    %s view of slice %u (ANGLE's EGL image of plane %u): 0x%08lx\n",
                i ? "R8G8 (chroma)" : "R8 (luma)", slice, i, (unsigned long)hr);
         snprintf(what, sizeof what, "%s: no plane view of a decoder texture slice", name);
         need(SUCCEEDED(hr), what);
         if (srv)
            srv->Release();
      }
      /* Another device (Dawn) opens it by its NT handle; ANGLE, on the
       * decoding device, does not. Legacy SHARED: the old handle. */
      HANDLE h = NULL;
      D3D11_TEXTURE2D_DESC got;
      tex->GetDesc(&got);
      if (got.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
         IDXGIResource1 *r1 = NULL;
         if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1))) {
            hr = r1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h);
            printf("    CreateSharedHandle: 0x%08lx\n", (unsigned long)hr);
            r1->Release();
         }
      } else if (got.MiscFlags & D3D11_RESOURCE_MISC_SHARED) {
         IDXGIResource *r = NULL;
         if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource), (void **)&r))) {
            hr = r->GetSharedHandle(&h);
            printf("    GetSharedHandle: 0x%08lx\n", (unsigned long)hr);
            r->Release();
         }
      }
      if (h) {
         ID3D11Texture2D *opened = NULL;
         if (got.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
            ID3D11Device1 *d1 = NULL;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1))) {
               hr = d1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void **)&opened);
               d1->Release();
            }
            CloseHandle(h);
         } else {
            hr = dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void **)&opened);
         }
         printf("    the texture opened by its handle: 0x%08lx\n", (unsigned long)hr);
         if (opened) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            ID3D11ShaderResourceView *srv = NULL;
            sd.Format = DXGI_FORMAT_R8G8_UNORM;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.FirstArraySlice = slice;
            sd.Texture2DArray.ArraySize = 1;
            hr = dev->CreateShaderResourceView(opened, &sd, &srv);
            printf("    R8G8 view of slice %u of the opened texture: 0x%08lx\n", slice, (unsigned long)hr);
            if (srv)
               srv->Release();
            opened->Release();
         }
      }
      tex->Release();
   }
   if (dec)
      dec->Release();
}

int
chrome_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
   ID3D11VideoDevice1 *vdev1 = NULL;
   ID3D11VideoContext1 *vctx1 = NULL;
   ID3D11Device5 *dev5 = NULL;
   HRESULT hr;
   D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
   UINT n, i;
   bool have[4] = {};

   g_blocked = NULL;
   printf("chromium probe:\n");
   printf("  feature level %u_%u (D3D11VideoDecoder needs 11_0)\n", ((unsigned)fl >> 12) & 0xF,
          ((unsigned)fl >> 8) & 0xF);
   need(fl >= D3D_FEATURE_LEVEL_11_0, "feature level below 11_0");

   hr = dev->QueryInterface(__uuidof(ID3D11VideoDevice1), (void **)&vdev1);
   printf("  ID3D11VideoDevice1: 0x%08lx\n", (unsigned long)hr);
   need(SUCCEEDED(hr), "no ID3D11VideoDevice1");
   hr = ctx->QueryInterface(__uuidof(ID3D11VideoContext1), (void **)&vctx1);
   printf("  ID3D11VideoContext1: 0x%08lx\n", (unsigned long)hr);
   need(SUCCEEDED(hr), "no ID3D11VideoContext1");
   hr = dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5);
   printf("  ID3D11Device5 (fences; the D3D12 decoder path only): 0x%08lx\n", (unsigned long)hr);

   if (vdev1) {
      n = vdev1->GetVideoDecoderProfileCount();
      printf("  decoder profiles: %u\n", n);
      for (i = 0; i < n; i++) {
         GUID g;
         if (FAILED(vdev1->GetVideoDecoderProfile(i, &g)))
            continue;
         printf("    {%08lx-...} %s\n", g.Data1, guid_name(g));
         resolutions(vdev1, g);
         have[0] |= IsEqualGUID(g, kH264) != 0;
         have[1] |= IsEqualGUID(g, kHevcMain) != 0;
         have[2] |= IsEqualGUID(g, kVp9P0) != 0;
         have[3] |= IsEqualGUID(g, kAv1P0) != 0;
      }
      need(n > 0, "no decoder profile");
      /* H.264 1080p: pictures of 1088 rows (macroblocks); the others 1080. */
      if (have[0])
         codec(dev, vdev1, kH264, "H.264", 2, 1088);
      if (have[1])
         codec(dev, vdev1, kHevcMain, "HEVC Main", 1, 1080);
      if (have[2])
         codec(dev, vdev1, kVp9P0, "VP9 profile 0", 1, 1080);
      if (have[3])
         codec(dev, vdev1, kAv1P0, "AV1 Main", 1, 1080);
   }

   if (vdev1) vdev1->Release();
   if (vctx1) vctx1->Release();
   if (dev5) dev5->Release();
   if (g_blocked)
      printf("chromium: blocked by: %s\n", g_blocked);
   else
      printf("chromium: everything D3D11VideoDecoder asks for is there\n");
   return g_blocked ? 4 : 0;
}
