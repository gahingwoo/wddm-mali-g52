// SPDX-License-Identifier: GPL-2.0
//
// gsso: geometry shaders and stream output, each step on its own, so that a
// driver without them fails the creates cleanly and nothing after that
// crashes.
//
//   gsso.exe hw | warp | <software DLL>  [gs | nogs]
//
// hw: the adapter whose description names the Mali. warp: WARP. Anything
// else is loaded as a D3D_DRIVER_TYPE_SOFTWARE DLL (libgallium_d3d10.dll).
// The second argument says what to expect: gs, geometry shaders and stream
// output work; nogs, every create of one fails. Without it either is
// accepted. A create that fails is UNSUPPORTED, unless "gs" was given.
//
// Steps (one PASS / FAIL / UNSUPPORTED line each):
//   1  CreateGeometryShader (a triangle GS that swaps red and blue, no
//      loop and no indexing of its inputs by a variable)
//   2  CreateGeometryShaderWithStreamOutput, with that GS
//   3  CreateGeometryShaderWithStreamOutput, vertex shader bytecode
//      (stream output without a GS)
//   4  GSSetShader + Draw: a red triangle comes out blue
//   4b the same with a GS that loops over its inputs (i[k], k a variable)
//   5  SOSetTargets + Draw with the pass-through SO shader, SO_STATISTICS
//      and SO_OVERFLOW_PREDICATE around it; the buffer holds the triangle
//   6  DrawAuto from that buffer: a red triangle
//   7  the same Draw with no GS: red (the device still draws)
//   8  release everything, ClearState, Flush; the device is not removed
// GSSO_SKIP=4b leaves step 4b out (SKIPPED), for a known hang.
// Exit code: the number of FAIL lines; 200 when a step made no progress for
// 30 s (the hung step is named). "done" is the last line printed; a run that
// prints neither crashed.
//
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

#define W 64
#define H 64

static const char kHlsl[] =
    "struct V { float4 p : SV_Position; float4 c : COLOR; };\n"
    "V vs(float4 p : POSITION, float4 c : COLOR) { V o; o.p = p; o.c = c; return o; }\n"
    "[maxvertexcount(3)] void gs(triangle V i[3], inout TriangleStream<V> s) {\n"
    "  V o; o = i[0]; o.c = i[0].c.bgra; s.Append(o);\n"
    "  o = i[1]; o.c = i[1].c.bgra; s.Append(o);\n"
    "  o = i[2]; o.c = i[2].c.bgra; s.Append(o);\n"
    "  s.RestartStrip(); }\n"
    "[maxvertexcount(3)] void gs_loop(triangle V i[3], inout TriangleStream<V> s) {\n"
    "  for (int k = 0; k < 3; k++) { V o = i[k]; o.c = i[k].c.bgra; s.Append(o); }\n"
    "  s.RestartStrip(); }\n"
    "float4 ps(V i) : SV_Target { return i.c; }\n";

struct Vtx {
    float p[4];
    float c[4];
};

/* One triangle over the middle of the target, red. */
static const Vtx kTri[3] = {
    {{-0.8f, -0.8f, 0, 1}, {1, 0, 0, 1}},
    {{0.0f, 0.8f, 0, 1}, {1, 0, 0, 1}},
    {{0.8f, -0.8f, 0, 1}, {1, 0, 0, 1}},
};

static int g_fail;
static volatile LONG g_tick;
static const char *volatile g_label = "setup";

static void step(const char *label)
{
    g_label = label;
    InterlockedIncrement(&g_tick);
}

/* A step that does not return within 30 s is a FAIL too; say which one and
 * end the process rather than leave CI (or the board) waiting. */
static DWORD WINAPI watchdog(void *)
{
    LONG last = -1;
    int same = 0;
    for (;;) {
        Sleep(1000);
        LONG now = g_tick;
        same = now == last ? same + 1 : 0;
        last = now;
        if (same >= 30) {
            printf("FAIL        %s hung: no progress for 30 s\n", g_label);
            fflush(stdout);
            ExitProcess(200);
        }
    }
}
static int g_expect; /* 1 gs, -1 nogs, 0 either */

static void line(const char *verdict, const char *step, const char *fmt, ...)
{
    va_list ap;
    printf("%-11s %-46s ", verdict, step);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (strcmp(verdict, "FAIL") == 0)
        g_fail++;
}

/* GSSO_SKIP, a comma-separated list of steps to leave out (only 4b looks at
 * it), a SKIPPED line each: for a known
 * hang that would otherwise stop every later step from running. */
static bool skipped(const char *label)
{
    const char *list = getenv("GSSO_SKIP");
    size_t n = strlen(label);
    for (const char *p = list; p && *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, label, n) == 0)
            return true;
        p = end ? end + 1 : nullptr;
    }
    return false;
}

/* A create that failed: UNSUPPORTED, or FAIL when it should have worked. */
static void created(const char *step, HRESULT hr)
{
    if (SUCCEEDED(hr))
        line(g_expect < 0 ? "FAIL" : "PASS", step, "hr=0x%08lx%s", (unsigned long)hr,
             g_expect < 0 ? " (expected to fail)" : "");
    else
        line(g_expect > 0 ? "FAIL" : "UNSUPPORTED", step, "hr=0x%08lx", (unsigned long)hr);
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "gsso", nullptr, nullptr, entry, target, 0, 0,
                            &code, &err);
    if (FAILED(hr)) {
        printf("FAIL compile %s: 0x%08lx %s\n", entry, (unsigned long)hr,
               err ? (const char *)err->GetBufferPointer() : "");
        exit(100);
    }
    return code;
}

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *rt, *rtStage;
static ID3D11RenderTargetView *rtv;

static void clear()
{
    const float grey[4] = {0.25f, 0.25f, 0.25f, 1};
    ctx->ClearRenderTargetView(rtv, grey);
}

/* The pixel in the middle of the target as 0xBBGGRR. */
static unsigned centre()
{
    D3D11_MAPPED_SUBRESOURCE m;
    ctx->CopyResource(rtStage, rt);
    if (FAILED(ctx->Map(rtStage, 0, D3D11_MAP_READ, 0, &m)))
        return 0xffffffff;
    unsigned v = *(const unsigned *)((const char *)m.pData + (H / 2) * m.RowPitch + (W / 2) * 4) & 0xffffff;
    ctx->Unmap(rtStage, 0);
    return v;
}

#define RED 0x0000ffu
#define BLUE 0xff0000u
#define GREY 0x404040u

static bool near_(unsigned got, unsigned want)
{
    for (int c = 0; c < 3; c++) {
        int g = (got >> (8 * c)) & 0xff, w = (want >> (8 * c)) & 0xff;
        if (abs(g - w) > 2)
            return false;
    }
    return true;
}

/* GetData until it answers, or give up after two seconds. */
static HRESULT wait_data(ID3D11Asynchronous *q, void *data, UINT size)
{
    HRESULT hr = S_FALSE;
    for (int i = 0; i < 2000; i++) {
        hr = ctx->GetData(q, data, size, 0);
        if (hr != S_FALSE)
            break;
        Sleep(1);
    }
    return hr;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "hw";
    bool hw = _stricmp(mode, "hw") == 0, warp = _stricmp(mode, "warp") == 0;
    if (argc > 2)
        g_expect = _stricmp(argv[2], "gs") == 0 ? 1 : _stricmp(argv[2], "nogs") == 0 ? -1 : 0;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    HRESULT hr;

    setvbuf(stdout, nullptr, _IONBF, 0);
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (hw) {
        IDXGIFactory1 *fac = nullptr;
        IDXGIAdapter1 *pick = nullptr, *a = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) {
            printf("FAIL CreateDXGIFactory1\n");
            return 100;
        }
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
        hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               &got, &ctx);
    } else if (warp) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               &got, &ctx);
    } else {
        HMODULE sw = LoadLibraryA(mode);
        if (!sw) {
            printf("FAIL LoadLibrary(%s): %lu\n", mode, GetLastError());
            return 100;
        }
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_SOFTWARE, sw, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               &got, &ctx);
    }
    if (FAILED(hr)) {
        printf("FAIL D3D11CreateDevice (%s): hr=0x%08lx\n", mode, (unsigned long)hr);
        return 100;
    }
    printf("device created (%s), feature level 0x%x\n", mode, got);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt)) || FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
        printf("FAIL render target\n");
        return 100;
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &rtStage))) {
        printf("FAIL staging texture\n");
        return 100;
    }

    ID3DBlob *vsb = compile("vs", "vs_4_0"), *gsb = compile("gs", "gs_4_0"), *psb = compile("ps", "ps_4_0");
    ID3DBlob *gslb = compile("gs_loop", "gs_4_0");
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
        FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps))) {
        printf("FAIL vertex or pixel shader\n");
        return 100;
    }

    const D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout *il = nullptr;
    if (FAILED(dev->CreateInputLayout(el, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il))) {
        printf("FAIL input layout\n");
        return 100;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(kTri);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {kTri, 0, 0};
    ID3D11Buffer *vb = nullptr;
    if (FAILED(dev->CreateBuffer(&bd, &init, &vb))) {
        printf("FAIL vertex buffer\n");
        return 100;
    }

    /* Room for four triangles; stream output and then a vertex buffer. */
    bd.ByteWidth = 4 * sizeof(kTri);
    bd.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER;
    ID3D11Buffer *sob = nullptr;
    hr = dev->CreateBuffer(&bd, nullptr, &sob);
    if (FAILED(hr)) {
        printf("FAIL stream output buffer: hr=0x%08lx\n", (unsigned long)hr);
        return 100;
    }
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *soStage = nullptr;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &soStage))) {
        printf("FAIL staging buffer\n");
        return 100;
    }

    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    UINT stride = sizeof(Vtx), offset = 0;
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(il);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);

    const D3D11_SO_DECLARATION_ENTRY so[] = {
        {0, "SV_Position", 0, 0, 4, 0},
        {0, "COLOR", 0, 0, 4, 0},
    };
    UINT soStride = sizeof(Vtx);

    /* 1 */
    step("1");
    ID3D11GeometryShader *gs = nullptr;
    hr = dev->CreateGeometryShader(gsb->GetBufferPointer(), gsb->GetBufferSize(), nullptr, &gs);
    created("1 CreateGeometryShader", hr);
    if (FAILED(hr))
        gs = nullptr;

    /* 2 */
    step("2");
    ID3D11GeometryShader *gsSo = nullptr;
    hr = dev->CreateGeometryShaderWithStreamOutput(gsb->GetBufferPointer(), gsb->GetBufferSize(), so, 2,
                                                   &soStride, 1, 0, nullptr, &gsSo);
    created("2 CreateGeometryShaderWithStreamOutput (GS)", hr);
    if (FAILED(hr))
        gsSo = nullptr;

    /* 3 */
    step("3");
    ID3D11GeometryShader *passSo = nullptr;
    hr = dev->CreateGeometryShaderWithStreamOutput(vsb->GetBufferPointer(), vsb->GetBufferSize(), so, 2,
                                                   &soStride, 1, 0, nullptr, &passSo);
    created("3 CreateGeometryShaderWithStreamOutput (VS)", hr);
    if (FAILED(hr))
        passSo = nullptr;

    /* 4 */
    step("4");
    if (gs) {
        clear();
        ctx->GSSetShader(gs, nullptr, 0);
        ctx->Draw(3, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        unsigned c = centre();
        line(near_(c, BLUE) ? "PASS" : "FAIL", "4 GSSetShader + Draw", "centre 0x%06x, want 0x%06x", c, BLUE);
    } else {
        line("UNSUPPORTED", "4 GSSetShader + Draw", "no geometry shader to bind");
    }

    /* 4b: the same through a loop: the GS reads its input vertices with a
     * variable index (an indirect second subscript). */
    step("4b");
    {
        ID3D11GeometryShader *gsl = nullptr;
        hr = skipped("4b") ? S_FALSE
                           : dev->CreateGeometryShader(gslb->GetBufferPointer(), gslb->GetBufferSize(), nullptr, &gsl);
        if (hr == S_FALSE) {
            line("SKIPPED", "4b GS indexing its inputs + Draw", "GSSO_SKIP");
        } else if (FAILED(hr)) {
            line(g_expect > 0 ? "FAIL" : "UNSUPPORTED", "4b GS indexing its inputs + Draw", "create hr=0x%08lx",
                 (unsigned long)hr);
        } else {
            clear();
            ctx->GSSetShader(gsl, nullptr, 0);
            ctx->Draw(3, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            unsigned c = centre();
            line(near_(c, BLUE) ? "PASS" : "FAIL", "4b GS indexing its inputs + Draw", "centre 0x%06x, want 0x%06x",
                 c, BLUE);
            gsl->Release();
        }
    }

    step("5");
    /* 5: with no SO shader the same calls still run (targets, queries, a
     * draw); nothing is written and the counts are zero. */
    {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_SO_STATISTICS, 0};
        ID3D11Query *qs = nullptr;
        ID3D11Predicate *qo = nullptr;
        HRESULT hs = dev->CreateQuery(&qd, &qs);
        qd.Query = D3D11_QUERY_SO_OVERFLOW_PREDICATE;
        HRESULT ho = dev->CreatePredicate(&qd, &qo);
        if (FAILED(hs) || FAILED(ho)) {
            line("FAIL", "5 SO queries", "CreateQuery hr=0x%08lx, CreatePredicate hr=0x%08lx",
                 (unsigned long)hs, (unsigned long)ho);
        } else {
            clear();
            ctx->SOSetTargets(1, &sob, &offset);
            ctx->GSSetShader(passSo, nullptr, 0);
            ctx->Begin(qs);
            ctx->Begin(qo);
            ctx->Draw(3, 0);
            ctx->End(qo);
            ctx->End(qs);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer *none = nullptr;
            ctx->SOSetTargets(1, &none, &offset);

            D3D11_QUERY_DATA_SO_STATISTICS st = {};
            BOOL overflow = TRUE;
            HRESULT rs = wait_data(qs, &st, sizeof st), ro = wait_data(qo, &overflow, sizeof overflow);
            if (rs != S_OK || ro != S_OK) {
                line("FAIL", "5 SO queries", "GetData hr=0x%08lx / 0x%08lx", (unsigned long)rs,
                     (unsigned long)ro);
            } else {
                UINT64 want = passSo ? 1 : 0;
                bool ok = st.NumPrimitivesWritten == want && !overflow;
                line(ok ? "PASS" : "FAIL", "5 SO queries", "written %llu (want %llu), storage needed %llu, overflow %d",
                     (unsigned long long)st.NumPrimitivesWritten, (unsigned long long)want,
                     (unsigned long long)st.PrimitivesStorageNeeded, overflow);
            }

            if (passSo) {
                D3D11_MAPPED_SUBRESOURCE m;
                ctx->CopyResource(soStage, sob);
                if (FAILED(ctx->Map(soStage, 0, D3D11_MAP_READ, 0, &m))) {
                    line("FAIL", "5 SO buffer contents", "Map failed");
                } else {
                    bool same = memcmp(m.pData, kTri, sizeof(kTri)) == 0;
                    const float *f = (const float *)m.pData;
                    line(same ? "PASS" : "FAIL", "5 SO buffer contents", "first vertex %g %g %g %g / %g %g %g %g",
                         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                    ctx->Unmap(soStage, 0);
                }
            } else {
                line("UNSUPPORTED", "5 SO buffer contents", "no stream output shader");
            }
            qs->Release();
            qo->Release();
        }
    }

    /* 6 */
    step("6");
    {
        clear();
        ctx->IASetVertexBuffers(0, 1, &sob, &stride, &offset);
        ctx->DrawAuto();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        unsigned c = centre();
        if (passSo)
            line(near_(c, RED) ? "PASS" : "FAIL", "6 DrawAuto", "centre 0x%06x, want 0x%06x", c, RED);
        else
            line(near_(c, GREY) ? "UNSUPPORTED" : "FAIL", "6 DrawAuto",
                 "centre 0x%06x, nothing was streamed out so nothing is drawn (want 0x%06x)", c, GREY);
    }

    /* 7 */
    step("7");
    {
        clear();
        ctx->Draw(3, 0);
        unsigned c = centre();
        line(near_(c, RED) ? "PASS" : "FAIL", "7 Draw, no GS", "centre 0x%06x, want 0x%06x", c, RED);
    }

    /* 8 */
    step("8");
    {
        if (gs)
            gs->Release();
        if (gsSo)
            gsSo->Release();
        if (passSo)
            passSo->Release();
        sob->Release();
        soStage->Release();
        ctx->ClearState();
        ctx->Flush();
        hr = dev->GetDeviceRemovedReason();
        line(hr == S_OK ? "PASS" : "FAIL", "8 release, ClearState, Flush", "GetDeviceRemovedReason 0x%08lx",
             (unsigned long)hr);
    }

    printf("done, %d FAIL\n", g_fail);
    return g_fail;
}
