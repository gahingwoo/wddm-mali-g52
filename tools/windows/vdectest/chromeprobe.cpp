/*
 * chromeprobe - what Chromium's D3D11VideoDecoder asks of a D3D11 device,
 * step by step (vdectest --chromium). Read from Chromium's
 * media/gpu/windows (main, 2026-11-25):
 *
 *   d3d11_video_decoder_backend.cc  the ANGLE device, GetFeatureLevel() >= 11_0
 *                                   or no decoder at all; As(ID3D11VideoDevice1)
 *   supported_profile_helpers.cc    GetVideoDecoderProfileCount/Profile, then
 *                                   GetVideoDecoderConfigCount per resolution
 *                                   (the largest one with a config is the limit)
 *   d3d11_video_decoder_wrapper.cc  ID3D11VideoContext1; ConfigBitstreamRaw 2 for
 *                                   H.264, 1 for HEVC, VP9 and AV1
 *   d3d_decoder_configurator.cc     the output texture: NV12 (P010 for 10 bit), an
 *                                   array, BIND_DECODER | BIND_SHADER_RESOURCE,
 *                                   MiscFlags SHARED | SHARED_NTHANDLE (the shared
 *                                   image path), else SHARED, else 0
 *   d3d11_texture_selector.cc       output views per slice; the planes as R8 and
 *                                   R8G8 shader resource views, also of the
 *                                   texture opened from its NT handle
 *
 * Nothing is decoded. Every step prints its result; the exit code is 0 when
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

int
chrome_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
   ID3D11VideoDevice1 *vdev1 = NULL;
   ID3D11VideoContext1 *vctx1 = NULL;
   ID3D11Device5 *dev5 = NULL;
   ID3D11Texture2D *tex = NULL;
   HRESULT hr;
   D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
   UINT n, i, h264_max = 0;

   g_blocked = NULL;
   printf("chromium probe:\n");
   printf("  feature level %x_%x (D3D11VideoDecoder needs 11_0)\n", ((unsigned)fl >> 12) & 0xF,
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
         if (IsEqualGUID(g, kH264)) {
            /* Chromium's resolution probe, the sizes it tries */
            static const UINT sizes[][2] = { { 1920, 1088 }, { 2560, 1440 }, { 3840, 2160 }, { 4096, 2160 },
                                             { 4096, 2304 }, { 7680, 4320 }, { 8192, 4320 } };
            for (UINT k = 0; k < ARRAYSIZE(sizes); k++) {
               D3D11_VIDEO_DECODER_DESC d = { g, sizes[k][0], sizes[k][1], DXGI_FORMAT_NV12 };
               UINT c = 0;
               if (SUCCEEDED(vdev1->GetVideoDecoderConfigCount(&d, &c)) && c) {
                  h264_max = sizes[k][0] * 10000 + sizes[k][1];
                  for (UINT j = 0; j < c; j++) {
                     D3D11_VIDEO_DECODER_CONFIG cfg;
                     if (k == 0 && SUCCEEDED(vdev1->GetVideoDecoderConfig(&d, j, &cfg)))
                        printf("      config %u at 1920x1088: ConfigBitstreamRaw %u%s\n", j, cfg.ConfigBitstreamRaw,
                               cfg.ConfigBitstreamRaw == 2 ? " (what Chromium takes for H.264)" : "");
                  }
               }
            }
            printf("      largest size with a config: %ux%u\n", h264_max / 10000, h264_max % 10000);
         }
      }
      need(n > 0, "no decoder profile");
   }

   /* The output texture, as the configurator makes it (an array of 8 here). */
   {
      static const struct { UINT misc; const char *name; } tries[] = {
         { D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, "SHARED | SHARED_NTHANDLE" },
         { D3D11_RESOURCE_MISC_SHARED, "SHARED" },
         { 0, "none" },
      };
      for (i = 0; i < ARRAYSIZE(tries) && !tex; i++) {
         D3D11_TEXTURE2D_DESC td = {};
         td.Width = 1920;
         td.Height = 1088;
         td.MipLevels = 1;
         td.ArraySize = 8;
         td.Format = DXGI_FORMAT_NV12;
         td.SampleDesc.Count = 1;
         td.Usage = D3D11_USAGE_DEFAULT;
         td.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
         td.MiscFlags = tries[i].misc;
         hr = dev->CreateTexture2D(&td, NULL, &tex);
         printf("  NV12 1920x1088 x8, DECODER|SHADER_RESOURCE, misc %s: 0x%08lx\n", tries[i].name,
                (unsigned long)hr);
         if (i == 0)
            need(SUCCEEDED(hr), "no NV12 decoder texture with SHARED_NTHANDLE (the shared image path)");
      }
   }

   if (tex && vdev1) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC od = {};
      ID3D11VideoDecoderOutputView *ov = NULL;
      od.DecodeProfile = kH264;
      od.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      od.Texture2D.ArraySlice = 3;
      hr = vdev1->CreateVideoDecoderOutputView(tex, &od, &ov);
      printf("  decoder output view, slice 3: 0x%08lx\n", (unsigned long)hr);
      need(SUCCEEDED(hr), "no decoder output view on that texture");
      if (ov)
         ov->Release();
   }

   if (tex) {
      for (i = 0; i < 2; i++) {
         D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
         ID3D11ShaderResourceView *srv = NULL;
         sd.Format = i ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
         sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
         sd.Texture2DArray.MipLevels = 1;
         sd.Texture2DArray.FirstArraySlice = 3;
         sd.Texture2DArray.ArraySize = 1;
         hr = dev->CreateShaderResourceView(tex, &sd, &srv);
         printf("  %s view of slice 3: 0x%08lx\n", i ? "R8G8 (chroma)" : "R8 (luma)", (unsigned long)hr);
         need(SUCCEEDED(hr), "no plane view of a decoder texture slice");
         if (srv)
            srv->Release();
      }

      IDXGIResource1 *r1 = NULL;
      HANDLE h = NULL;
      if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1))) {
         hr = r1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h);
         printf("  IDXGIResource1::CreateSharedHandle: 0x%08lx\n", (unsigned long)hr);
         r1->Release();
      }
      if (h) {
         ID3D11Device1 *d1 = NULL;
         ID3D11Texture2D *opened = NULL;
         if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1))) {
            hr = d1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void **)&opened);
            printf("  OpenSharedResource1: 0x%08lx\n", (unsigned long)hr);
            need(SUCCEEDED(hr), "the decoder texture cannot be opened from its NT handle");
            if (opened) {
               D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
               ID3D11ShaderResourceView *srv = NULL;
               sd.Format = DXGI_FORMAT_R8G8_UNORM;
               sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
               sd.Texture2DArray.MipLevels = 1;
               sd.Texture2DArray.FirstArraySlice = 3;
               sd.Texture2DArray.ArraySize = 1;
               hr = dev->CreateShaderResourceView(opened, &sd, &srv);
               printf("  R8G8 view of slice 3 of the opened texture: 0x%08lx\n", (unsigned long)hr);
               need(SUCCEEDED(hr), "no plane view of the opened decoder texture");
               if (srv)
                  srv->Release();
               opened->Release();
            }
            d1->Release();
         }
         CloseHandle(h);
      } else {
         need(false, "no NT handle for the decoder texture");
      }
      tex->Release();
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
