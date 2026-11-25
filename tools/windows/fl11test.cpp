/*
 * fl11test - feature level 11_0 the way Chromium's GPU process gets it
 * (maliwddm UmdReportFL11, Mesa patch 0080, docs/CHROMIUM-VIDEO.md C1).
 *
 *   fl11test <umd.dll | hw> [--type=gpu-process]
 *
 * The UMD gives 11_0 only to msedge.exe, chrome.exe or msedgewebview2.exe
 * started with --type=gpu-process, so this is run as a copy named
 * msedge.exe with that argument (which is otherwise ignored here), and as
 * itself or without it to see that nothing else changes.
 *
 * Creates a device asking for 11_0, 10_1, 10_0 and prints what it got.
 * Below 11_0: exit 2. At 11_0:
 *   - vs_5_0/ps_5_0 shaders of the kind FXC makes from ANGLE's OpenGL ES
 *     3.0 code (their disassembly is printed, so the log shows which shader
 *     model 5 instructions FXC chose): rcp, ddx/ddy coarse and fine, a
 *     texture sample (sample_indexable: chained extended opcode tokens),
 *     shifts and masks FXC may turn into ubfe/ibfe/bfi, countbits,
 *     firstbitlow, firstbithigh (unsigned and signed), reversebits; one
 *     draw, the pixel compared with the values computed here;
 *   - a cs_5_0 compute shader must fail to create, and the device must
 *     live on (GetDeviceRemovedReason S_OK).
 * Exit 0 when all of that holds, 1 otherwise.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

static const char kHlsl[] =
   "cbuffer C : register(b0) { float4 f; uint4 u; };\n"
   "Texture2D<float4> t : register(t0);\n"
   "SamplerState s : register(s0);\n"
   "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
   "V vs(uint id : SV_VertexID) {\n"
   "   V o;\n"
   "   float2 p = float2((id << 1) & 2, id & 2);\n"
   "   o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
   "   o.uv = p;\n"
   "   return o;\n"
   "}\n"
   "struct P { float4 a : SV_Target0; uint4 b : SV_Target1; uint4 c : SV_Target2; };\n"
   "P ps(V i) {\n"
   "   P o;\n"
   "   o.a = float4(1.0 / f.x, ddx(i.pos.x) + ddy(i.pos.y), ddx_fine(i.pos.x) + ddy_fine(i.pos.y),\n"
   "                t.Sample(s, f.zw).r);\n"
   "   o.b = uint4((u.x >> 8) & 0xffu, countbits(u.x), firstbitlow(u.x), firstbithigh(u.x));\n"
   "   o.c = uint4(reversebits(u.x), (u.y & ~0xff0u) | ((u.z << 4) & 0xff0u),\n"
   "               (uint)(((int)(u.w << 16)) >> 24), (uint)firstbithigh((int)((u.w | 0xffff0000u) & ~0xffffu)));\n"
   "   return o;\n"
   "}\n";

static const char kCs[] =
   "RWBuffer<uint> b : register(u0);\n"
   "[numthreads(1, 1, 1)] void cs(uint3 i : SV_DispatchThreadID) { b[i.x] = i.x; }\n";

#define CHECK(hr, what)                                                       \
   do {                                                                       \
      if (FAILED(hr)) {                                                       \
         printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)(hr));          \
         return 1;                                                            \
      }                                                                       \
   } while (0)

static ID3DBlob *
compile(const char *src, size_t len, const char *entry, const char *target)
{
   ID3DBlob *code = NULL, *err = NULL, *dis = NULL;
   HRESULT hr = D3DCompile(src, len, entry, NULL, NULL, entry, target, 0, 0, &code, &err);
   if (FAILED(hr)) {
      printf("FAIL compile %s %s: %s\n", entry, target, err ? (const char *)err->GetBufferPointer() : "?");
      return NULL;
   }
   if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(),
                                D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, NULL, &dis))) {
      printf("---- %s (%s)\n%s----\n", entry, target, (const char *)dis->GetBufferPointer());
      dis->Release();
   }
   return code;
}

static HRESULT
create_device(const char *what, ID3D11Device **dev, ID3D11DeviceContext **ctx, D3D_FEATURE_LEVEL *got)
{
   static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0 };
   HRESULT hr;

   if (_stricmp(what, "hw") == 0) {
      IDXGIFactory1 *fac = NULL;
      IDXGIAdapter1 *a = NULL, *pick = NULL;
      hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
      if (FAILED(hr))
         return hr;
      for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
         DXGI_ADAPTER_DESC1 d;
         a->GetDesc1(&d);
         printf("adapter %u: %ls (vendor 0x%04x)\n", i, d.Description, d.VendorId);
         if (!pick && wcsstr(d.Description, L"Mali"))
            pick = a;
         else
            a->Release();
      }
      fac->Release();
      if (!pick) {
         printf("FAIL no adapter named Mali\n");
         return E_FAIL;
      }
      hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, levels, ARRAYSIZE(levels),
                             D3D11_SDK_VERSION, dev, got, ctx);
      pick->Release();
   } else {
      HMODULE m = LoadLibraryA(what);
      if (!m) {
         printf("FAIL cannot load %s (%lu)\n", what, GetLastError());
         return E_FAIL;
      }
      hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_SOFTWARE, m, 0, levels, ARRAYSIZE(levels),
                             D3D11_SDK_VERSION, dev, got, ctx);
   }
   return hr;
}

int
main(int argc, char **argv)
{
   _set_error_mode(_OUT_TO_STDERR);
   _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

   const char *what = argc > 1 ? argv[1] : "libgallium_d3d10.dll";
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
   int fails = 0;

   printf("command line: %s\n", GetCommandLineA());
   HRESULT hr = create_device(what, &dev, &ctx, &fl);
   printf("D3D11CreateDevice(11_0, 10_1, 10_0): 0x%08lx, feature level %u_%u\n", (unsigned long)hr,
          ((unsigned)fl >> 12) & 0xF, ((unsigned)fl >> 8) & 0xF);
   CHECK(hr, "D3D11CreateDevice");
   if (fl < D3D_FEATURE_LEVEL_11_0) {
      printf("feature level below 11_0\n");
      return 2;
   }

   ID3DBlob *vsb = compile(kHlsl, sizeof kHlsl - 1, "vs", "vs_5_0");
   ID3DBlob *psb = compile(kHlsl, sizeof kHlsl - 1, "ps", "ps_5_0");
   if (!vsb || !psb)
      return 1;
   ID3D11VertexShader *vs = NULL;
   ID3D11PixelShader *ps = NULL;
   CHECK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs), "CreateVertexShader vs_5_0");
   CHECK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps), "CreatePixelShader ps_5_0");

   /* Three 4x4 targets. */
   static const DXGI_FORMAT fmts[3] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT,
                                        DXGI_FORMAT_R32G32B32A32_UINT };
   ID3D11Texture2D *rt[3] = {}, *st[3] = {};
   ID3D11RenderTargetView *rtv[3] = {};
   for (int i = 0; i < 3; i++) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = td.Height = 4;
      td.MipLevels = td.ArraySize = 1;
      td.Format = fmts[i];
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      CHECK(dev->CreateTexture2D(&td, NULL, &rt[i]), "CreateTexture2D target");
      CHECK(dev->CreateRenderTargetView(rt[i], NULL, &rtv[i]), "CreateRenderTargetView");
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(dev->CreateTexture2D(&td, NULL, &st[i]), "CreateTexture2D staging");
   }

   /* A 2x2 texture, red 0x40 0x80 / 0xc0 0xff, point sampled at texel (1, 0). */
   static const UINT texels[4] = { 0xff000040, 0xff000080, 0xff0000c0, 0xff0000ff };
   D3D11_TEXTURE2D_DESC xd = {};
   xd.Width = xd.Height = 2;
   xd.MipLevels = xd.ArraySize = 1;
   xd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   xd.SampleDesc.Count = 1;
   xd.Usage = D3D11_USAGE_DEFAULT;
   xd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   D3D11_SUBRESOURCE_DATA xinit = { texels, 8, 0 };
   ID3D11Texture2D *tex = NULL;
   ID3D11ShaderResourceView *srv = NULL;
   CHECK(dev->CreateTexture2D(&xd, &xinit, &tex), "CreateTexture2D sampled");
   CHECK(dev->CreateShaderResourceView(tex, NULL, &srv), "CreateShaderResourceView");
   D3D11_SAMPLER_DESC sd = {};
   sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
   sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
   sd.MaxLOD = D3D11_FLOAT32_MAX;
   ID3D11SamplerState *smp = NULL;
   CHECK(dev->CreateSamplerState(&sd, &smp), "CreateSamplerState");

   struct { float f[4]; UINT u[4]; } cb = { { 4.0f, 0.0f, 0.75f, 0.25f },
                                           { 0x12345678u, 0xAAAAAAAAu, 0x5Bu, 0x0000C300u } };
   D3D11_BUFFER_DESC bd = {};
   bd.ByteWidth = sizeof cb;
   bd.Usage = D3D11_USAGE_DEFAULT;
   bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   D3D11_SUBRESOURCE_DATA binit = { &cb, 0, 0 };
   ID3D11Buffer *cbuf = NULL;
   CHECK(dev->CreateBuffer(&bd, &binit, &cbuf), "CreateBuffer");

   D3D11_VIEWPORT vp = { 0, 0, 4, 4, 0, 1 };
   ctx->OMSetRenderTargets(3, rtv, NULL);
   ctx->RSSetViewports(1, &vp);
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ctx->VSSetShader(vs, NULL, 0);
   ctx->PSSetShader(ps, NULL, 0);
   ctx->PSSetConstantBuffers(0, 1, &cbuf);
   ctx->PSSetShaderResources(0, 1, &srv);
   ctx->PSSetSamplers(0, 1, &smp);
   ctx->Draw(3, 0);
   for (int i = 0; i < 3; i++)
      ctx->CopyResource(st[i], rt[i]);

   UINT px[3][4];
   for (int i = 0; i < 3; i++) {
      D3D11_MAPPED_SUBRESOURCE m;
      CHECK(ctx->Map(st[i], 0, D3D11_MAP_READ, 0, &m), "Map");
      memcpy(px[i], (const BYTE *)m.pData + m.RowPitch + 16, 16); /* pixel (1, 1) */
      ctx->Unmap(st[i], 0);
   }
   float a[4];
   memcpy(a, px[0], sizeof a);
   const float want_a[4] = { 0.25f, 2.0f, 2.0f, 128.0f / 255.0f };
   const char *name_a[4] = { "rcp", "ddx+ddy (coarse)", "ddx+ddy (fine)", "sample (sample_indexable)" };
   for (int k = 0; k < 4; k++) {
      bool ok = fabsf(a[k] - want_a[k]) <= 1.0f / 512.0f;
      printf("%-28s %f (want %f) %s\n", name_a[k], a[k], want_a[k], ok ? "MATCH" : "DIFFER");
      fails += !ok;
   }
   const UINT want_b[4] = { 0x56, 13, 3, 28 };
   const char *name_b[4] = { "(x >> 8) & 0xff (ubfe?)", "countbits", "firstbitlow", "firstbithigh" };
   const UINT want_c[4] = { 0x1e6a2c48, 0xaaaaa5ba, 0xffffffc3, 15 };
   const char *name_c[4] = { "reversebits", "masked insert (bfi?)", "signed field (ibfe?)",
                             "firstbithigh (signed)" };
   for (int k = 0; k < 4; k++) {
      bool ok = px[1][k] == want_b[k];
      printf("%-28s 0x%08x (want 0x%08x) %s\n", name_b[k], px[1][k], want_b[k], ok ? "MATCH" : "DIFFER");
      fails += !ok;
   }
   for (int k = 0; k < 4; k++) {
      bool ok = px[2][k] == want_c[k];
      printf("%-28s 0x%08x (want 0x%08x) %s\n", name_c[k], px[2][k], want_c[k], ok ? "MATCH" : "DIFFER");
      fails += !ok;
   }

   /* Compute: must fail to create, and the device must live on. */
   ID3DBlob *csb = compile(kCs, sizeof kCs - 1, "cs", "cs_5_0");
   if (csb) {
      ID3D11ComputeShader *cs = NULL;
      hr = dev->CreateComputeShader(csb->GetBufferPointer(), csb->GetBufferSize(), NULL, &cs);
      printf("CreateComputeShader: 0x%08lx (want a failure)\n", (unsigned long)hr);
      if (SUCCEEDED(hr)) {
         fails++;
         cs->Release();
      }
   } else {
      fails++;
   }
   hr = dev->GetDeviceRemovedReason();
   printf("GetDeviceRemovedReason: 0x%08lx\n", (unsigned long)hr);
   if (hr != S_OK)
      fails++;

   printf("%s: %d difference(s)\n", fails ? "FAIL" : "PASS", fails);
   return fails ? 1 : 0;
}
