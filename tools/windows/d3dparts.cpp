// SPDX-License-Identifier: GPL-2.0
//
// d3dparts: the pieces Direct2D's text shader is made of, one per draw, with
// the pixels checked, to find which one the driver gets wrong.
//
//   d3dparts.exe hw | warp
//
// A 320x64 RGBA8 target in five 64x64 cells:
//   1  Texture2D.Load in the pixel shader (a 4x4 texture)
//   2  Buffer<uint>.Load in the pixel shader (a 16 byte R8_UINT view)
//   3  an integer passed from the vertex to the pixel shader, nointerpolation
//   4  DrawInstanced: four 16x16 squares placed and coloured by per-instance
//      SINT elements, corners from a per-vertex SINT element
//   5  the same with DrawIndexedInstanced
// Exit code: the number of cells that failed.
//
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

#define W 320
#define H 64

static const char kHlsl[] =
    "struct V { float4 p : SV_Position; nointerpolation uint k : K; };\n"
    "Texture2D<float4> t2 : register(t0);\n"
    "Buffer<uint> tb : register(t1);\n"
    "V vs_plain(float2 p : POSITION) { V o; o.p = float4(p, 0, 1); o.k = 0x00C0FFEEu; return o; }\n"
    "V vs_inst(int2 corner : CORNER, int2 org : ORG, int key : KEY) {\n"
    "  V o; float2 px = float2(org + corner * 16);\n"
    "  o.p = float4(px.x / 160.0 - 1.0, 1.0 - px.y / 32.0, 0, 1); o.k = (uint)key; return o; }\n"
    "float4 ps_load2d(V i) : SV_Target { return t2.Load(int3(int(i.p.x) & 3, int(i.p.y) & 3, 0)); }\n"
    "float4 ps_buf(V i) : SV_Target { uint v = tb.Load((int(i.p.x) & 3) + 4 * (int(i.p.y) & 3));\n"
    "  return float4(v / 255.0, 0, 0.5, 1); }\n"
    "float4 ps_flat(V i) : SV_Target { return i.k == 0x00C0FFEEu ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1); }\n"
    "float4 ps_key(V i) : SV_Target { return float4((i.k & 0xff) / 255.0, ((i.k >> 8) & 0xff) / 255.0,\n"
    "  ((i.k >> 16) & 0xff) / 255.0, 1); }\n";

#define CHECK(hr, what)                                                       \
    do {                                                                      \
        HRESULT hr_ = (hr);                                                   \
        if (FAILED(hr_)) {                                                    \
            printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr_);        \
            return 100;                                                       \
        }                                                                     \
    } while (0)

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "d3dparts", nullptr, nullptr, entry, target, 0, 0,
                            &code, &err);
    if (FAILED(hr)) {
        printf("FAIL compile %s: 0x%08lx %s\n", entry, (unsigned long)hr,
               err ? (const char *)err->GetBufferPointer() : "");
        exit(100);
    }
    return code;
}

static int near_(unsigned got, unsigned want)
{
    for (int c = 0; c < 3; c++) {
        int g = (got >> (8 * c)) & 0xff, w = (want >> (8 * c)) & 0xff;
        if (abs(g - w) > 2)
            return 0;
    }
    return 1;
}

/* RGBA8 in memory is R,G,B,A: as a little-endian word 0xAABBGGRR. */
#define RGB(r, g, b) ((unsigned)(r) | ((unsigned)(g) << 8) | ((unsigned)(b) << 16))

static const unsigned char *g_px;
static unsigned g_pitch;

static int expect(const char *what, int x, int y, unsigned want)
{
    unsigned got = *(const unsigned *)(g_px + (size_t)y * g_pitch + x * 4) & 0xffffff;
    int ok = near_(got, want);
    printf("  %-28s (%3d,%2d) rgb %3u %3u %3u, want %3u %3u %3u  %s\n", what, x, y, got & 0xff,
           (got >> 8) & 0xff, (got >> 16) & 0xff, want & 0xff, (want >> 8) & 0xff, (want >> 16) & 0xff,
           ok ? "ok" : "WRONG");
    return ok;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "hw";
    bool hw = _stricmp(mode, "hw") == 0;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;

    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (hw) {
        IDXGIFactory1 *fac = nullptr;
        IDXGIAdapter1 *pick = nullptr, *a = nullptr;
        CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac), "CreateDXGIFactory1");
        for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (!pick && wcsstr(d.Description, L"Mali"))
                pick = a;
            else
                a->Release();
        }
        if (!pick) {
            printf("FAIL no adapter named Mali\n");
            return 100;
        }
        CHECK(D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                &dev, &got, &ctx),
              "D3D11CreateDevice (hardware)");
    } else {
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                &dev, &got, &ctx),
              "D3D11CreateDevice (WARP)");
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &tex), "CreateTexture2D (target)");
    CHECK(dev->CreateRenderTargetView(tex, nullptr, &rtv), "CreateRenderTargetView");

    /* The 4x4 texture: R = 50 + 50x, G = 50 + 50y, B = 0. */
    unsigned texels[16];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            texels[y * 4 + x] = RGB(50 + 50 * x, 50 + 50 * y, 0) | 0xff000000u;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Width = sd.Height = 4;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = texels;
    init.SysMemPitch = 16;
    ID3D11Texture2D *small = nullptr;
    ID3D11ShaderResourceView *srv2d = nullptr;
    CHECK(dev->CreateTexture2D(&sd, &init, &small), "CreateTexture2D (4x4)");
    CHECK(dev->CreateShaderResourceView(small, nullptr, &srv2d), "CreateShaderResourceView (4x4)");

    /* The buffer: 16 bytes, value 100 + 10 i. */
    unsigned char bytes[16];
    for (int i = 0; i < 16; i++)
        bytes[i] = (unsigned char)(100 + 10 * i);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = bytes;
    ID3D11Buffer *buf = nullptr;
    ID3D11ShaderResourceView *srvbuf = nullptr;
    CHECK(dev->CreateBuffer(&bd, &init, &buf), "CreateBuffer (R8_UINT data)");
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = DXGI_FORMAT_R8_UINT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.FirstElement = 0;
    vd.Buffer.NumElements = 16;
    CHECK(dev->CreateShaderResourceView(buf, &vd, &srvbuf), "CreateShaderResourceView (buffer)");

    /* Three quads in clip space for cells 1 to 3. */
    float quads[3 * 6 * 2];
    for (int c = 0; c < 3; c++) {
        float x0 = -1.0f + 0.4f * c, x1 = x0 + 0.4f;
        float q[12] = {x0, 1, x1, 1, x0, -1, x0, -1, x1, 1, x1, -1};
        memcpy(quads + c * 12, q, sizeof(q));
    }
    bd.ByteWidth = sizeof(quads);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    init.pSysMem = quads;
    ID3D11Buffer *vbquads = nullptr;
    CHECK(dev->CreateBuffer(&bd, &init, &vbquads), "CreateBuffer (quads)");

    /* Corners, as a list and as four vertices with an index buffer. */
    short corners6[12] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
    short corners4[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    unsigned short indices[6] = {0, 1, 2, 2, 1, 3};
    ID3D11Buffer *vb6 = nullptr, *vb4 = nullptr, *ib = nullptr, *vbinst = nullptr;
    bd.ByteWidth = sizeof(corners6);
    init.pSysMem = corners6;
    CHECK(dev->CreateBuffer(&bd, &init, &vb6), "CreateBuffer (corners, list)");
    bd.ByteWidth = sizeof(corners4);
    init.pSysMem = corners4;
    CHECK(dev->CreateBuffer(&bd, &init, &vb4), "CreateBuffer (corners, indexed)");
    bd.ByteWidth = sizeof(indices);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = indices;
    CHECK(dev->CreateBuffer(&bd, &init, &ib), "CreateBuffer (indices)");

    /* Per instance: origin (two shorts), key (an int). Instances 0-3 are
     * cell 4, instances 4-7 the same squares in cell 5. */
    struct Inst { short x, y; int key; } inst[8];
    for (int i = 0; i < 8; i++) {
        int n = i & 3;
        inst[i].x = (short)((i < 4 ? 192 : 256) + 16 * n);
        inst[i].y = (short)(16 * n);
        inst[i].key = (40 * (n + 1)) | ((20 * (n + 1)) << 8) | ((10 * (n + 1)) << 16);
    }
    bd.ByteWidth = sizeof(inst);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    init.pSysMem = inst;
    CHECK(dev->CreateBuffer(&bd, &init, &vbinst), "CreateBuffer (instances)");

    ID3DBlob *bvs_plain = compile("vs_plain", "vs_4_0"), *bvs_inst = compile("vs_inst", "vs_4_0");
    ID3DBlob *bps[4] = {compile("ps_load2d", "ps_4_0"), compile("ps_buf", "ps_4_0"),
                        compile("ps_flat", "ps_4_0"), compile("ps_key", "ps_4_0")};
    ID3D11VertexShader *vs_plain = nullptr, *vs_inst = nullptr;
    ID3D11PixelShader *ps[4] = {};
    CHECK(dev->CreateVertexShader(bvs_plain->GetBufferPointer(), bvs_plain->GetBufferSize(), nullptr,
                                  &vs_plain), "CreateVertexShader (plain)");
    CHECK(dev->CreateVertexShader(bvs_inst->GetBufferPointer(), bvs_inst->GetBufferSize(), nullptr,
                                  &vs_inst), "CreateVertexShader (instanced)");
    for (int i = 0; i < 4; i++)
        CHECK(dev->CreatePixelShader(bps[i]->GetBufferPointer(), bps[i]->GetBufferSize(), nullptr,
                                     &ps[i]), "CreatePixelShader");

    D3D11_INPUT_ELEMENT_DESC el_plain[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    D3D11_INPUT_ELEMENT_DESC el_inst[] = {
        {"CORNER", 0, DXGI_FORMAT_R16G16_SINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"ORG", 0, DXGI_FORMAT_R16G16_SINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"KEY", 0, DXGI_FORMAT_R32_SINT, 1, 4, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    ID3D11InputLayout *il_plain = nullptr, *il_inst = nullptr;
    CHECK(dev->CreateInputLayout(el_plain, 1, bvs_plain->GetBufferPointer(), bvs_plain->GetBufferSize(),
                                 &il_plain), "CreateInputLayout (plain)");
    CHECK(dev->CreateInputLayout(el_inst, 3, bvs_inst->GetBufferPointer(), bvs_inst->GetBufferSize(),
                                 &il_inst), "CreateInputLayout (instanced)");

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = nullptr;
    CHECK(dev->CreateRasterizerState(&rd, &rs), "CreateRasterizerState");

    float grey[4] = {0.25f, 0.25f, 0.25f, 1.0f};
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    ID3D11ShaderResourceView *srvs[2] = {srv2d, srvbuf};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs);
    ctx->ClearRenderTargetView(rtv, grey);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = 8, zero = 0;
    ctx->IASetInputLayout(il_plain);
    ctx->IASetVertexBuffers(0, 1, &vbquads, &stride, &zero);
    ctx->VSSetShader(vs_plain, nullptr, 0);
    for (int c = 0; c < 3; c++) {
        ctx->PSSetShader(ps[c], nullptr, 0);
        ctx->Draw(6, 6 * c);
    }

    ctx->IASetInputLayout(il_inst);
    ctx->VSSetShader(vs_inst, nullptr, 0);
    ctx->PSSetShader(ps[3], nullptr, 0);
    ID3D11Buffer *vbs[2] = {vb6, vbinst};
    UINT strides[2] = {4, 8}, offsets[2] = {0, 0};
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->DrawInstanced(6, 4, 0, 0);
    vbs[0] = vb4;
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
    ctx->DrawIndexedInstanced(6, 4, 0, 0, 4);
    ctx->Flush();

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &stage), "CreateTexture2D (staging)");
    ctx->CopyResource(stage, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m), "Map (staging)");
    g_px = (const unsigned char *)m.pData;
    g_pitch = m.RowPitch;

    int failed = 0, ok;

    printf("1 Texture2D.Load\n");
    ok = expect("texel (1,2)", 5, 6, RGB(100, 150, 0));
    ok &= expect("texel (3,0)", 43, 32, RGB(200, 50, 0));
    failed += !ok;

    printf("2 Buffer<uint>.Load\n");
    ok = expect("element 9", 64 + 5, 6, RGB(190, 0, 128));
    ok &= expect("element 3", 64 + 43, 32, RGB(130, 0, 128));
    failed += !ok;

    printf("3 integer, vertex to pixel shader\n");
    ok = expect("0x00C0FFEE arrives", 160, 32, RGB(0, 255, 0));
    failed += !ok;

    for (int cell = 0; cell < 2; cell++) {
        printf(cell == 0 ? "4 DrawInstanced, SINT elements\n"
                         : "5 DrawIndexedInstanced, SINT elements, start instance 4\n");
        ok = 1;
        for (int n = 0; n < 4; n++) {
            char what[32];
            snprintf(what, sizeof(what), "instance %d", n);
            ok &= expect(what, 192 + 64 * cell + 16 * n + 8, 16 * n + 8,
                         RGB(40 * (n + 1), 20 * (n + 1), 10 * (n + 1)));
        }
        ok &= expect("between the squares", 192 + 64 * cell + 40, 8, RGB(64, 64, 64));
        failed += !ok;
    }

    printf(failed ? "%d of 5 FAILED\n" : "all 5 pass\n", failed);
    ctx->Unmap(stage, 0);
    return failed;
}
