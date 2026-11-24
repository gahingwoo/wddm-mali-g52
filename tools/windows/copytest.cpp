// SPDX-License-Identifier: GPL-2.0
//
// copytest: GPU copies (CopySubresourceRegion, CopyResource) in the orders
// DWM and applications mix them with draws, with the pixels checked; and a
// timing loop shaped like DWM's frame while a window is dragged.
//
//   copytest.exe [hw | warp | path\to\libgallium_d3d10.dll] [bench [frames]] [nocheck]
//
// hw (the default) is the adapter whose description names the Mali; a DLL
// path is a D3D10-DDI software driver (CI: softpipe, or Panfrost on the
// emulated driver, where nothing runs the jobs: "nocheck" then reports the
// pixels without failing on them).
//
// The checks, each on 64x64 RGBA8 textures:
//   1  copy, then the source drawn over: the copy keeps the old pixels
//   2  into the bound render target between two draws: draw, copy, draw
//      keep their order where they overlap
//   3  a texture sampled, then copied into, then sampled again: the first
//      draw sees the old pixels, the second the new
//   4  a chain A -> B -> C -> D of region copies
//   5  copy, draw over the source, copy another region of it
//   6  from a texture filled by UpdateSubresource (no render target bind)
//   7  between two mip levels of one texture
//   8  a copy into a staging texture straight after the draw, then Map
//
// bench: per frame, 4 draws into a 2560x1440 BGRA8 back buffer and 2
// CopySubresourceRegion of 600x400 from the previous frame's buffer into
// it, then Flush, then the buffers swap; at most two frames in flight (an
// event query), as DXGI allows. Prints the CPU time of this process per
// frame. With maliwddm's UmdCopyBatch, or D3D10UMD_COPYBATCH=0 and =1 in
// the environment of this process alone, the two ways of copying can be
// compared without touching DWM.
//
// Exit code: the number of checks that failed (0 with nocheck), 100 on an
// error before any check.
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

static const char kHlsl[] =
    "cbuffer c : register(b0) { float4 rect; float4 colour; float4 size; };\n"
    "Texture2D<float4> t : register(t0);\n"
    "float4 vs(float2 u : POSITION) : SV_Position {\n"
    "  float2 px = rect.xy + u * rect.zw;\n"
    "  return float4(px.x / size.x * 2.0 - 1.0, 1.0 - px.y / size.y * 2.0, 0, 1); }\n"
    "float4 ps(float4 p : SV_Position) : SV_Target { return colour; }\n"
    "float4 pst(float4 p : SV_Position) : SV_Target { return t.Load(int3(int2(p.xy), 0)); }\n";

#define CHECK(hr, what)                                                       \
    do {                                                                      \
        HRESULT hr_ = (hr);                                                   \
        if (FAILED(hr_)) {                                                    \
            printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr_);        \
            exit(100);                                                        \
        }                                                                     \
    } while (0)

// RGBA8 in memory, little endian: R in the low byte.
enum : UINT {
    RED = 0xff0000ffu, GREEN = 0xff00ff00u, BLUE = 0xffff0000u, YELLOW = 0xff00ffffu,
    MAGENTA = 0xffff00ffu, CYAN = 0xffffff00u, WHITE = 0xffffffffu, GREY = 0xff808080u,
};

struct Cb {
    float rect[4], colour[4], size[4];
};

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps, *pst;
static ID3D11Buffer *cb, *vb;
static ID3D11InputLayout *il;
static ID3D11RasterizerState *rs;
static bool nocheck;
static int failed;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "copytest", nullptr, nullptr, entry, target, 0, 0,
                            &code, &err);
    if (FAILED(hr)) {
        printf("FAIL compile %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        exit(100);
    }
    return code;
}

static void colour_of(UINT c, float *f)
{
    for (int i = 0; i < 4; i++)
        f[i] = ((c >> (8 * i)) & 0xff) / 255.0f;
}

static ID3D11Texture2D *tex(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, UINT levels = 1,
                            const void *init = nullptr, UINT pitch = 0)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = levels;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd = {init, pitch, 0};
    ID3D11Texture2D *t = nullptr;
    CHECK(dev->CreateTexture2D(&td, init ? &sd : nullptr, &t), "CreateTexture2D");
    return t;
}

static ID3D11Texture2D *rt64(void)
{
    return tex(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
}

static ID3D11RenderTargetView *rtv(ID3D11Texture2D *t, UINT level = 0)
{
    D3D11_TEXTURE2D_DESC td;
    t->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC d = {};
    d.Format = td.Format;
    d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice = level;
    ID3D11RenderTargetView *v = nullptr;
    CHECK(dev->CreateRenderTargetView(t, &d, &v), "CreateRenderTargetView");
    return v;
}

static ID3D11ShaderResourceView *srv(ID3D11Texture2D *t)
{
    ID3D11ShaderResourceView *v = nullptr;
    CHECK(dev->CreateShaderResourceView(t, nullptr, &v), "CreateShaderResourceView");
    return v;
}

static void clear(ID3D11RenderTargetView *v, UINT c)
{
    float f[4];
    colour_of(c, f);
    ctx->ClearRenderTargetView(v, f);
}

// A rectangle of one colour, or of the texture's texels at the same pixels.
static void draw(ID3D11RenderTargetView *target, UINT w, UINT h, int x, int y, int rw, int rh, UINT c,
                 ID3D11ShaderResourceView *from = nullptr)
{
    Cb d = {{(float)x, (float)y, (float)rw, (float)rh}, {}, {(float)w, (float)h, 0, 0}};
    colour_of(c, d.colour);
    ctx->UpdateSubresource(cb, 0, nullptr, &d, 0, 0);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs);
    UINT stride = 8, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(from ? pst : ps, nullptr, 0);
    if (from)
        ctx->PSSetShaderResources(0, 1, &from);
    ctx->Draw(4, 0);
    // Unbind, so the next use of the texture as a target or a copy
    // destination is not a hazard the runtime resolves for us.
    ID3D11ShaderResourceView *none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView *nort = nullptr;
    ctx->OMSetRenderTargets(1, &nort, nullptr);
}

static void copy(ID3D11Texture2D *dst, UINT dl, int dx, int dy, ID3D11Texture2D *src, UINT sl, int x, int y,
                 int w, int h)
{
    D3D11_BOX b = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
    ctx->CopySubresourceRegion(dst, dl, dx, dy, 0, src, sl, &b);
}

// Reads subresource Level of T back through a staging texture.
struct Readback {
    UINT w = 0, h = 0;
    UINT px[64 * 64] = {};
    UINT at(int x, int y) const { return px[y * w + x]; }
};

static void readback(ID3D11Texture2D *t, UINT level, Readback &r)
{
    D3D11_TEXTURE2D_DESC td;
    t->GetDesc(&td);
    r.w = td.Width >> level;
    r.h = td.Height >> level;
    td.Width = r.w;
    td.Height = r.h;
    td.MipLevels = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &st), "CreateTexture2D(staging)");
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, level, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "Map");
    for (UINT y = 0; y < r.h; y++)
        memcpy(&r.px[y * r.w], (const BYTE *)m.pData + y * m.RowPitch, r.w * 4);
    ctx->Unmap(st, 0);
    st->Release();
}

struct Want {
    int x, y;
    UINT c;
};

static void expect(int n, const char *what, const Readback &r, const Want *w, int count)
{
    int bad = 0;
    for (int i = 0; i < count; i++) {
        UINT got = r.at(w[i].x, w[i].y);
        if (got != w[i].c) {
            printf("    check %d at %d,%d: 0x%08x, want 0x%08x\n", n, w[i].x, w[i].y, got, w[i].c);
            bad++;
        }
    }
    printf("%s %d %s\n", bad ? (nocheck ? "DIFF" : "FAIL") : "PASS", n, what);
    if (bad && !nocheck)
        failed++;
}

static void checks(void)
{
    Readback r;

    {   // 1: copy, then the source drawn over.
        ID3D11Texture2D *a = rt64(), *b = rt64();
        ID3D11RenderTargetView *av = rtv(a), *bv = rtv(b);
        clear(bv, BLUE);
        clear(av, RED);
        draw(av, 64, 64, 0, 0, 64, 64, RED);
        copy(b, 0, 8, 8, a, 0, 0, 0, 32, 32);
        draw(av, 64, 64, 0, 0, 64, 64, GREEN);
        readback(b, 0, r);
        Want w[] = {{8, 8, RED}, {39, 39, RED}, {4, 4, BLUE}, {48, 48, BLUE}};
        expect(1, "copy keeps the source as it was at the copy", r, w, 4);
        readback(a, 0, r);
        Want w2[] = {{0, 0, GREEN}, {63, 63, GREEN}};
        expect(1, "the source after the copy has the later draw", r, w2, 2);
        av->Release(); bv->Release(); a->Release(); b->Release();
    }

    {   // 2: into the bound render target between two draws.
        ID3D11Texture2D *d = rt64(), *e = rt64();
        ID3D11RenderTargetView *dv = rtv(d), *ev = rtv(e);
        clear(ev, MAGENTA);
        clear(dv, BLUE);
        draw(dv, 64, 64, 0, 0, 32, 64, YELLOW);
        ctx->OMSetRenderTargets(1, &dv, nullptr);   // still bound at the copy
        copy(d, 0, 32, 0, e, 0, 0, 0, 32, 64);
        draw(dv, 64, 64, 24, 24, 16, 16, CYAN);
        readback(d, 0, r);
        Want w[] = {{8, 8, YELLOW}, {56, 8, MAGENTA}, {32, 32, CYAN}, {8, 60, YELLOW}, {56, 60, MAGENTA},
                    {26, 26, CYAN}, {38, 38, CYAN}};
        expect(2, "draw, copy, draw into one target keep their order", r, w, 7);
        dv->Release(); ev->Release(); d->Release(); e->Release();
    }

    {   // 3: sampled, copied into, sampled again.
        ID3D11Texture2D *t = rt64(), *g = rt64(), *x = rt64(), *y = rt64();
        ID3D11RenderTargetView *tv = rtv(t), *gv = rtv(g), *xv = rtv(x), *yv = rtv(y);
        ID3D11ShaderResourceView *ts = srv(t);
        clear(tv, RED);
        clear(gv, GREEN);
        draw(xv, 64, 64, 0, 0, 64, 64, 0, ts);
        copy(t, 0, 0, 0, g, 0, 0, 0, 64, 64);
        draw(yv, 64, 64, 0, 0, 64, 64, 0, ts);
        readback(x, 0, r);
        Want w[] = {{1, 1, RED}, {62, 62, RED}};
        expect(3, "a draw before the copy samples the old texels", r, w, 2);
        readback(y, 0, r);
        Want w2[] = {{1, 1, GREEN}, {62, 62, GREEN}};
        expect(3, "a draw after the copy samples the new texels", r, w2, 2);
        ts->Release(); tv->Release(); gv->Release(); xv->Release(); yv->Release();
        t->Release(); g->Release(); x->Release(); y->Release();
    }

    {   // 4: a chain of region copies.
        ID3D11Texture2D *t[4];
        ID3D11RenderTargetView *v[4];
        for (int i = 0; i < 4; i++) {
            t[i] = rt64();
            v[i] = rtv(t[i]);
            clear(v[i], GREY);
        }
        draw(v[0], 64, 64, 0, 0, 16, 16, WHITE);
        copy(t[1], 0, 16, 0, t[0], 0, 0, 0, 16, 16);
        copy(t[2], 0, 16, 16, t[1], 0, 16, 0, 16, 16);
        copy(t[3], 0, 40, 40, t[2], 0, 16, 16, 16, 16);
        readback(t[3], 0, r);
        Want w[] = {{40, 40, WHITE}, {55, 55, WHITE}, {39, 39, GREY}, {0, 0, GREY}};
        expect(4, "A -> B -> C -> D", r, w, 4);
        for (int i = 0; i < 4; i++) {
            v[i]->Release();
            t[i]->Release();
        }
    }

    {   // 5: copy, draw over the source, copy another region.
        ID3D11Texture2D *a = rt64(), *b = rt64();
        ID3D11RenderTargetView *av = rtv(a), *bv = rtv(b);
        clear(bv, BLUE);
        clear(av, RED);
        copy(b, 0, 0, 0, a, 0, 0, 0, 32, 64);
        draw(av, 64, 64, 0, 0, 64, 64, GREEN);
        copy(b, 0, 32, 0, a, 0, 32, 0, 32, 64);
        readback(b, 0, r);
        Want w[] = {{4, 4, RED}, {28, 60, RED}, {36, 4, GREEN}, {60, 60, GREEN}};
        expect(5, "two copies with a draw into the source between them", r, w, 4);
        av->Release(); bv->Release(); a->Release(); b->Release();
    }

    {   // 6: from a texture filled by UpdateSubresource.
        static UINT pat[64 * 64];
        for (int i = 0; i < 64 * 64; i++)
            pat[i] = ((i % 64) < 32) ? YELLOW : CYAN;
        ID3D11Texture2D *u = tex(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        ID3D11Texture2D *v = rt64();
        ID3D11RenderTargetView *vv = rtv(v);
        clear(vv, BLUE);
        D3D11_BOX b = {0, 0, 0, 64, 64, 1};
        ctx->UpdateSubresource(u, 0, &b, pat, 64 * 4, 0);
        copy(v, 0, 16, 16, u, 0, 16, 0, 32, 32);
        for (int i = 0; i < 64 * 64; i++)
            pat[i] = MAGENTA;
        ctx->UpdateSubresource(u, 0, &b, pat, 64 * 4, 0);
        readback(v, 0, r);
        Want w[] = {{16, 16, YELLOW}, {31, 47, YELLOW}, {32, 16, CYAN}, {47, 47, CYAN}, {8, 8, BLUE}};
        expect(6, "from an uploaded texture, uploaded again after the copy", r, w, 5);
        readback(u, 0, r);
        Want w2[] = {{0, 0, MAGENTA}, {63, 63, MAGENTA}};
        expect(6, "the second upload", r, w2, 2);
        vv->Release(); u->Release(); v->Release();
    }

    {   // 7: between two levels of one texture.
        ID3D11Texture2D *m = tex(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 2);
        ID3D11RenderTargetView *l0 = rtv(m, 0), *l1 = rtv(m, 1);
        clear(l0, RED);
        clear(l1, BLUE);
        copy(m, 1, 8, 8, m, 0, 0, 0, 16, 16);
        readback(m, 1, r);
        Want w[] = {{8, 8, RED}, {23, 23, RED}, {4, 4, BLUE}, {28, 28, BLUE}};
        expect(7, "level 0 into level 1 of the same texture", r, w, 4);
        l0->Release(); l1->Release(); m->Release();
    }

    {   // 8: straight into staging after the draw, then Map.
        ID3D11Texture2D *a = rt64();
        ID3D11RenderTargetView *av = rtv(a);
        clear(av, BLUE);
        draw(av, 64, 64, 16, 16, 32, 32, RED);
        readback(a, 0, r);
        Want w[] = {{16, 16, RED}, {47, 47, RED}, {8, 8, BLUE}, {56, 56, BLUE}};
        expect(8, "a draw, then a copy into staging and Map", r, w, 4);
        av->Release(); a->Release();
    }
}

static double cpu_ms(void)
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULONGLONG t = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                  ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
    return (double)t / 10000.0;
}

// GetProcessTimes counts in clock ticks and read 0 on the CI runner for
// half a second of WARP; the cycle count is exact, for comparing two runs.
static double mcycles(void)
{
    ULONG64 c = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &c);
    return (double)c / 1e6;
}

static void bench(int frames)
{
    const UINT W = 2560, H = 1440;
    ID3D11Texture2D *buf[2];
    ID3D11RenderTargetView *bv[2];
    ID3D11Query *q[2] = {};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};

    for (int i = 0; i < 2; i++) {
        buf[i] = tex(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        bv[i] = rtv(buf[i]);
        clear(bv[i], GREY);
        CHECK(dev->CreateQuery(&qd, &q[i]), "CreateQuery");
    }
    ctx->Flush();
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    double c0 = 0, m0 = 0;
    for (int n = -30; n < frames; n++) {
        if (n == 0) {
            c0 = cpu_ms();
            m0 = mcycles();
            QueryPerformanceCounter(&t0);
        }
        int b = n & 1;
        // At most two frames in flight: wait for the one that used this buffer.
        if (n >= -28)
            while (ctx->GetData(q[b], nullptr, 0, 0) == S_FALSE)
                Sleep(0);
        // Inside the buffer with the copies to the right of and below it.
        int x = 200 + (n * 7 % 900 + 900) % 900, y = 100 + (n * 5 % 300 + 300) % 300;
        // The window at its new place, then its shadow and the title bar.
        draw(bv[b], W, H, x, y, 800, 600, WHITE);
        draw(bv[b], W, H, x + 10, y + 600, 800, 12, GREY);
        draw(bv[b], W, H, x, y, 800, 32, BLUE);
        draw(bv[b], W, H, x + 20, y + 60, 400, 300, CYAN);
        // What changed in the other buffer since this one was drawn.
        ctx->OMSetRenderTargets(1, &bv[b], nullptr);
        copy(buf[b], 0, x + 800, y, buf[b ^ 1], 0, x + 800, y, 600, 400);
        copy(buf[b], 0, x, y + 612, buf[b ^ 1], 0, x, y + 612, 600, 400);
        ctx->End(q[b]);
        ctx->Flush();
    }
    for (int i = 0; i < 2; i++)
        while (ctx->GetData(q[i], nullptr, 0, 0) == S_FALSE)
            Sleep(0);
    QueryPerformanceCounter(&t1);
    double wall = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    double cpu = cpu_ms() - c0, mc = mcycles() - m0;
    const char *env = getenv("D3D10UMD_COPYBATCH");
    printf("bench: %d frames (4 draws, 2 copies of 600x400 each) in %.0f ms: %.2f ms a frame,"
           " CPU %.2f ms a frame (%.0f%% of one core), %.2f Mcycles a frame; D3D10UMD_COPYBATCH=%s\n",
           frames, wall, wall / frames, cpu / frames, wall > 0 ? cpu * 100.0 / wall : 0.0, mc / frames,
           env ? env : "(unset, the KMD's UmdCopyBatch)");
    for (int i = 0; i < 2; i++) {
        q[i]->Release();
        bv[i]->Release();
        buf[i]->Release();
    }
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char *how = "hw";
    int frames = 0;
    for (int i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "bench") == 0) {
            frames = 600;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0)
                frames = atoi(argv[++i]);
        } else if (_stricmp(argv[i], "nocheck") == 0) {
            nocheck = true;
        } else {
            how = argv[i];
        }
    }

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    if (_stricmp(how, "hw") == 0) {
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
        CHECK(D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                &got, &ctx),
              "D3D11CreateDevice (hardware)");
    } else if (_stricmp(how, "warp") == 0) {
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                &got, &ctx),
              "D3D11CreateDevice (WARP)");
    } else {
        HMODULE sw = LoadLibraryA(how);
        if (!sw) {
            printf("FAIL LoadLibrary(%s): %lu\n", how, GetLastError());
            return 100;
        }
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_SOFTWARE, sw, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                &got, &ctx),
              "D3D11CreateDevice (software)");
    }
    const char *env = getenv("D3D10UMD_COPYBATCH");
    printf("device: %s, feature level 0x%x, D3D10UMD_COPYBATCH=%s\n", how, got,
           env ? env : "(unset, the KMD's UmdCopyBatch)");

    ID3DBlob *vsb = compile("vs", "vs_4_0"), *psb = compile("ps", "ps_4_0"), *ptb = compile("pst", "ps_4_0");
    CHECK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs), "CreateVertexShader");
    CHECK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps), "CreatePixelShader");
    CHECK(dev->CreatePixelShader(ptb->GetBufferPointer(), ptb->GetBufferSize(), nullptr, &pst), "CreatePixelShader");
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {quad, 0, 0};
    CHECK(dev->CreateBuffer(&bd, &init, &vb), "CreateBuffer(vb)");
    bd.ByteWidth = sizeof(Cb);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CHECK(dev->CreateBuffer(&bd, nullptr, &cb), "CreateBuffer(cb)");
    D3D11_INPUT_ELEMENT_DESC ied = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    CHECK(dev->CreateInputLayout(&ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il), "CreateInputLayout");
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    CHECK(dev->CreateRasterizerState(&rd, &rs), "CreateRasterizerState");

    checks();
    if (frames)
        bench(frames);
    printf("%s: %d check(s) failed%s\n", failed ? "FAIL" : "done", failed, nocheck ? " (nocheck)" : "");
    return nocheck ? 0 : failed;
}
