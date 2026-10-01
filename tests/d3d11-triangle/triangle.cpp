// SPDX-License-Identifier: GPL-2.0
//
// d3d11-triangle: render one triangle through a D3D10-DDI software driver DLL
// (Mesa's d3d10umd target) and check the pixels.
//
//   triangle.exe [path\to\libgallium_d3d10.dll]
//
// Clears a 64x64 RGBA8 target to blue, draws a red triangle covering the
// centre, copies to a staging texture and checks the centre is red and a
// corner is blue. Exit code 0 means pass.
//
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char kHlsl[] =
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p[3] = { float2(0.0, 0.8), float2(0.8, -0.8), float2(-0.8, -0.8) };\n"
    "    return float4(p[id], 0.0, 1.0);\n"
    "}\n"
    "float4 ps() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

#define CHECK(hr, what)                                                       \
    do {                                                                      \
        if (FAILED(hr)) {                                                     \
            printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)(hr));       \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(int argc, char **argv)
{
    const char *dll = argc > 1 ? argv[1] : "libgallium_d3d10.dll";
    HMODULE sw = LoadLibraryA(dll);
    if (!sw) {
        printf("FAIL LoadLibrary(%s): %lu\n", dll, GetLastError());
        return 1;
    }

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_SOFTWARE, sw, 0, &fl, 1,
                            D3D11_SDK_VERSION, &dev, &got, &ctx),
          "D3D11CreateDevice");
    printf("device created, feature level 0x%x\n", got);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &rt), "CreateTexture2D(rt)");
    ID3D11RenderTargetView *rtv = nullptr;
    CHECK(dev->CreateRenderTargetView(rt, nullptr, &rtv), "CreateRenderTargetView");

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "tri", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { printf("FAIL vs compile: %s\n", err ? (char *)err->GetBufferPointer() : "?"); return 1; }
    hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "tri", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { printf("FAIL ps compile: %s\n", err ? (char *)err->GetBufferPointer() : "?"); return 1; }
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    CHECK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs), "CreateVertexShader");
    CHECK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps), "CreatePixelShader");

    const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    ctx->ClearRenderTargetView(rtv, blue);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &st), "CreateTexture2D(staging)");
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "Map");
    auto px = [&](int x, int y) { return *(const UINT *)((const BYTE *)m.pData + y * m.RowPitch + x * 4); };
    UINT centre = px(32, 32), corner = px(1, 1);
    ctx->Unmap(st, 0);

    // RGBA8 little-endian: red = 0xff0000ff, blue = 0xffff0000
    bool ok = centre == 0xff0000ffu && corner == 0xffff0000u;
    printf("centre=0x%08x (want 0xff0000ff) corner=0x%08x (want 0xffff0000) -> %s\n",
           centre, corner, ok ? "PASS" : "FAIL");
    return ok ? 0 : 2;
}
