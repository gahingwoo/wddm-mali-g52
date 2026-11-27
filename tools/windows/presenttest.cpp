// SPDX-License-Identifier: GPL-2.0
//
// presenttest: which D3D11 present models reach the screen. One window per
// run, cleared to a colour of the variant's own and presented for a few
// seconds, every HRESULT logged.
//
//   presenttest.exe <variant> [seconds] [log|-] [warp] [nodup]
//
// Variants (colour in brackets):
//   gdi          a GDI window, FillRect in WM_PAINT, no D3D (yellow)
//   legacy       D3D11CreateDeviceAndSwapChain, DISCARD (orange)
//   discard      CreateSwapChainForHwnd, DISCARD, blt model (red)
//   sequential   CreateSwapChainForHwnd, SEQUENTIAL, blt model (green)
//   flipseq      CreateSwapChainForHwnd, FLIP_SEQUENTIAL (blue)
//   flipdiscard  CreateSwapChainForHwnd, FLIP_DISCARD (cyan)
//   nr-flipseq   flipseq on a WS_EX_NOREDIRECTIONBITMAP window (purple)
//   nr-flipdiscard flipdiscard on a WS_EX_NOREDIRECTIONBITMAP window (teal)
//
// While it presents, the window's centre pixel is read three ways:
//   wdc  GetPixel on the window's DC (the redirection bitmap, if any);
//   sdc  GetPixel on the screen DC;
//   dup  DXGI Desktop Duplication, the composed output DWM scans out
//        (left out with nodup).
// With a maliwddm adapter it also logs the KMD's present counters
// (MW_ESCAPE_STATS) before and after.
//
// Exit code: 0 when every create and present call succeeded.
//
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "maliwddm_abi.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#ifndef DXGI_SWAP_EFFECT_FLIP_DISCARD
#define DXGI_SWAP_EFFECT_FLIP_DISCARD ((DXGI_SWAP_EFFECT)4)
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static FILE *g_log;
static int g_fails;

static void say(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    if (g_log) {
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

static HRESULT logged(HRESULT hr, const char *what)
{
    say("%s: hr=0x%08lx%s", what, (unsigned long)hr, FAILED(hr) ? "  <== FAILED" : "");
    if (FAILED(hr))
        g_fails++;
    return hr;
}

// ---- KMD counters (maliwddm only) ----

typedef NTSTATUS(APIENTRY *KMT_FN)(void *);
static KMT_FN pEscape, pEnumAdapters2;
static D3DKMT_HANDLE g_mw;

static NTSTATUS escape(D3DKMT_HANDLE a, void *data, UINT size)
{
    D3DKMT_ESCAPE e = {};
    e.hAdapter = a;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = data;
    e.PrivateDriverDataSize = size;
    return pEscape(&e);
}

static void find_mw(void)
{
    HMODULE g = LoadLibraryA("gdi32.dll");
    if (!g)
        return;
    pEscape = (KMT_FN)(void *)GetProcAddress(g, "D3DKMTEscape");
    pEnumAdapters2 = (KMT_FN)(void *)GetProcAddress(g, "D3DKMTEnumAdapters2");
    if (!pEscape || !pEnumAdapters2)
        return;
    D3DKMT_ADAPTERINFO info[16];
    D3DKMT_ENUMADAPTERS2 en = {};
    en.NumAdapters = 16;
    en.pAdapters = info;
    if (pEnumAdapters2(&en) < 0)
        return;
    for (ULONG i = 0; i < en.NumAdapters; i++) {
        MW_ESCAPE_GPU_INFO_DATA gi = {};
        gi.Code = MW_ESCAPE_GPU_INFO;
        if (escape(info[i].hAdapter, &gi, sizeof(gi)) >= 0 && gi.Count != 0) {
            g_mw = info[i].hAdapter;
            return;
        }
    }
}

static void kmd_stats(const char *when, MW_ESCAPE_STATS_DATA *out)
{
    MW_ESCAPE_STATS_DATA s = {};
    s.Code = MW_ESCAPE_STATS;
    if (!g_mw) {
        say("kmd %s: no maliwddm adapter", when);
        return;
    }
    NTSTATUS st = escape(g_mw, &s, sizeof(s));
    if (st < 0) {
        say("kmd %s: escape 0x%08lx", when, (unsigned long)st);
        return;
    }
    say("kmd %s: presents %u blts %u fills %u skipped %u flips %u flipcopies %u flipcopyskips %u renders %u",
         when, s.Presents, s.PresentBlts, s.PresentFills, s.PresentSkips, s.Flips, s.FlipCopies,
         s.FlipCopySkips, s.Renders);
    if (out)
        *out = s;
}

// ---- the window ----

struct Variant {
    const char *name;
    int kind;               // 0 gdi, 1 legacy, 2 for-hwnd
    DXGI_SWAP_EFFECT effect;
    BOOL noredir;
    float rgb[3];
};

static const Variant kVariants[] = {
    {"gdi", 0, DXGI_SWAP_EFFECT_DISCARD, FALSE, {1, 1, 0}},
    {"legacy", 1, DXGI_SWAP_EFFECT_DISCARD, FALSE, {1, 0.5f, 0}},
    {"discard", 2, DXGI_SWAP_EFFECT_DISCARD, FALSE, {1, 0, 0}},
    {"sequential", 2, DXGI_SWAP_EFFECT_SEQUENTIAL, FALSE, {0, 1, 0}},
    {"flipseq", 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, FALSE, {0, 0, 1}},
    {"flipdiscard", 2, DXGI_SWAP_EFFECT_FLIP_DISCARD, FALSE, {0, 1, 1}},
    {"nr-flipseq", 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, TRUE, {0.5f, 0, 1}},
    {"nr-flipdiscard", 2, DXGI_SWAP_EFFECT_FLIP_DISCARD, TRUE, {0, 0.5f, 0.5f}},
};

static const Variant *g_v;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_v && g_v->kind == 0) {
            HBRUSH b = CreateSolidBrush(RGB((int)(g_v->rgb[0] * 255), (int)(g_v->rgb[1] * 255),
                                            (int)(g_v->rgb[2] * 255)));
            FillRect(dc, &ps.rcPaint, b);
            DeleteObject(b);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND && g_v && g_v->kind != 0)
        return 1;
    return DefWindowProcW(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static const char *pix(COLORREF c, char *buf, size_t n)
{
    if (c == CLR_INVALID)
        snprintf(buf, n, "invalid");
    else
        snprintf(buf, n, "%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

// ---- Desktop Duplication of the output holding the point ----

static void dup_sample(POINT p, const char *when)
{
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
        say("dup %s: no factory", when);
        return;
    }
    IDXGIAdapter1 *ad = nullptr;
    bool done = false;
    for (UINT i = 0; !done && f->EnumAdapters1(i, &ad) == S_OK; i++) {
        IDXGIOutput *o = nullptr;
        for (UINT j = 0; !done && ad->EnumOutputs(j, &o) == S_OK; j++) {
            DXGI_OUTPUT_DESC od;
            o->GetDesc(&od);
            RECT r = od.DesktopCoordinates;
            if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) {
                done = true;
                IDXGIOutput1 *o1 = nullptr;
                ID3D11Device *d = nullptr;
                ID3D11DeviceContext *c = nullptr;
                IDXGIOutputDuplication *dup = nullptr;
                HRESULT hr = o->QueryInterface(__uuidof(IDXGIOutput1), (void **)&o1);
                if (SUCCEEDED(hr))
                    hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d,
                                           NULL, &c);
                if (SUCCEEDED(hr))
                    hr = o1->DuplicateOutput(d, &dup);
                if (FAILED(hr)) {
                    say("dup %s: adapter %u output %u: setup hr=0x%08lx", when, i, j, (unsigned long)hr);
                } else {
                    DXGI_OUTDUPL_FRAME_INFO fi;
                    IDXGIResource *res = nullptr;
                    // The first frame may be empty (no image yet); try a few.
                    for (int k = 0; k < 6; k++) {
                        hr = dup->AcquireNextFrame(500, &fi, &res);
                        if (SUCCEEDED(hr) && fi.LastPresentTime.QuadPart != 0)
                            break;
                        if (SUCCEEDED(hr)) {
                            res->Release();
                            res = nullptr;
                            dup->ReleaseFrame();
                        }
                    }
                    if (FAILED(hr) || !res) {
                        say("dup %s: AcquireNextFrame hr=0x%08lx", when, (unsigned long)hr);
                    } else {
                        ID3D11Texture2D *t = nullptr, *st = nullptr;
                        res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
                        D3D11_TEXTURE2D_DESC td;
                        t->GetDesc(&td);
                        td.Usage = D3D11_USAGE_STAGING;
                        td.BindFlags = 0;
                        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        td.MiscFlags = 0;
                        hr = d->CreateTexture2D(&td, NULL, &st);
                        if (SUCCEEDED(hr)) {
                            c->CopyResource(st, t);
                            D3D11_MAPPED_SUBRESOURCE m;
                            hr = c->Map(st, 0, D3D11_MAP_READ, 0, &m);
                            if (SUCCEEDED(hr)) {
                                LONG x = p.x - r.left, y = p.y - r.top;
                                const BYTE *px = (const BYTE *)m.pData + (SIZE_T)y * m.RowPitch + (SIZE_T)x * 4;
                                say("dup %s: adapter %u output %u %ux%u fmt %u rot %u: centre %02x%02x%02x",
                                     when, i, j, td.Width, td.Height, td.Format, od.Rotation, px[2], px[1], px[0]);
                                c->Unmap(st, 0);
                            }
                            st->Release();
                        }
                        if (FAILED(hr))
                            say("dup %s: readback hr=0x%08lx", when, (unsigned long)hr);
                        t->Release();
                        res->Release();
                        dup->ReleaseFrame();
                    }
                }
                if (dup) dup->Release();
                if (c) c->Release();
                if (d) d->Release();
                if (o1) o1->Release();
            }
            o->Release();
        }
        ad->Release();
    }
    if (!done)
        say("dup %s: no output holds the point", when);
    f->Release();
}

int main(int argc, char **argv)
{
    const char *vname = argc > 1 ? argv[1] : "discard";
    int seconds = argc > 2 ? atoi(argv[2]) : 8;
    bool warp = false, nodup = false;
    for (int i = 4; i < argc; i++) {
        warp |= !strcmp(argv[i], "warp");
        nodup |= !strcmp(argv[i], "nodup");
    }
    if (argc > 3 && strcmp(argv[3], "-"))
        fopen_s(&g_log, argv[3], "w");
    for (const Variant &v : kVariants)
        if (!strcmp(v.name, vname))
            g_v = &v;
    if (!g_v) {
        printf("unknown variant %s\n", vname);
        return 100;
    }
    SetProcessDPIAware();
    find_mw();
    say("presenttest %s, %d s, %s%s", g_v->name, seconds, warp ? "WARP" : "hardware",
        nodup ? ", no duplication" : "");
    MW_ESCAPE_STATS_DATA k0 = {}, k1 = {};
    kmd_stats("before", &k0);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"presenttest";
    RegisterClassW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int W = 640, H = 480;
    DWORD ex = WS_EX_TOPMOST | (g_v->noredir ? WS_EX_NOREDIRECTIONBITMAP : 0);
    HWND hwnd = CreateWindowExW(ex, L"presenttest", L"presenttest", WS_POPUP | WS_VISIBLE, (sw - W) / 2,
                                (sh - H) / 2, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        say("CreateWindowEx failed: %lu", GetLastError());
        return 101;
    }
    SetForegroundWindow(hwnd);
    pump();
    RECT wr;
    GetWindowRect(hwnd, &wr);
    POINT centre = {(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
    say("window %p at %ld,%ld-%ld,%ld (screen %dx%d), exstyle 0x%lx", (void *)hwnd, wr.left, wr.top, wr.right,
         wr.bottom, sw, sh, (unsigned long)ex);

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    IDXGISwapChain *sc = nullptr;
    IDXGISwapChain1 *sc1 = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    D3D_DRIVER_TYPE dt = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;

    if (g_v->kind == 1) {
        DXGI_SWAP_CHAIN_DESC d = {};
        d.BufferDesc.Width = W;
        d.BufferDesc.Height = H;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 1;
        d.OutputWindow = hwnd;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        logged(D3D11CreateDeviceAndSwapChain(NULL, dt, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d, &sc, &dev, &fl,
                                             &ctx),
               "D3D11CreateDeviceAndSwapChain(DISCARD)");
    } else if (g_v->kind == 2) {
        if (SUCCEEDED(logged(D3D11CreateDevice(NULL, dt, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx),
                             "D3D11CreateDevice"))) {
            IDXGIDevice *dd = nullptr;
            IDXGIAdapter *da = nullptr;
            IDXGIFactory2 *f2 = nullptr;
            dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd);
            dd->GetAdapter(&da);
            DXGI_ADAPTER_DESC ad;
            da->GetDesc(&ad);
            say("adapter: %ls (vendor 0x%x device 0x%x), FL 0x%x", ad.Description, ad.VendorId, ad.DeviceId, fl);
            logged(da->GetParent(__uuidof(IDXGIFactory2), (void **)&f2), "GetParent(IDXGIFactory2)");
            DXGI_SWAP_CHAIN_DESC1 d = {};
            d.Width = W;
            d.Height = H;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.SwapEffect = g_v->effect;
            d.BufferCount = (g_v->effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                             g_v->effect == DXGI_SWAP_EFFECT_FLIP_DISCARD) ? 2 : 1;
            d.Scaling = DXGI_SCALING_STRETCH;
            d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            char what[96];
            snprintf(what, sizeof(what), "CreateSwapChainForHwnd(effect %d, %u buffers)", (int)g_v->effect,
                     d.BufferCount);
            if (f2 && SUCCEEDED(logged(f2->CreateSwapChainForHwnd(dev, hwnd, &d, NULL, NULL, &sc1), what)))
                sc1->QueryInterface(__uuidof(IDXGISwapChain), (void **)&sc);
            if (f2) f2->Release();
            da->Release();
            dd->Release();
        }
    }
    if (dev && g_v->kind == 1)
        say("FL 0x%x", fl);

    ULONG64 t0 = GetTickCount64();
    unsigned frames = 0, bad = 0, sampled = 0;
    HRESULT lasthr = S_OK;
    while (GetTickCount64() - t0 < (ULONG64)seconds * 1000) {
        pump();
        if (sc) {
            ID3D11Texture2D *bb = nullptr;
            if (!rtv || g_v->effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                g_v->effect == DXGI_SWAP_EFFECT_FLIP_DISCARD) {
                // Flip model: the current back buffer changes each frame.
                if (rtv) {
                    rtv->Release();
                    rtv = nullptr;
                }
                HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
                if (SUCCEEDED(hr))
                    hr = dev->CreateRenderTargetView(bb, NULL, &rtv);
                if (bb) bb->Release();
                if (FAILED(hr)) {
                    logged(hr, "GetBuffer/CreateRenderTargetView");
                    break;
                }
            }
            ctx->OMSetRenderTargets(1, &rtv, NULL);
            float c[4] = {g_v->rgb[0], g_v->rgb[1], g_v->rgb[2], 1};
            ctx->ClearRenderTargetView(rtv, c);
            HRESULT hr = sc->Present(1, 0);
            frames++;
            if (hr != lasthr || frames == 1) {
                char what[64];
                snprintf(what, sizeof(what), "Present #%u", frames);
                logged(hr, what);
                lasthr = hr;
            }
            if (FAILED(hr)) {
                bad++;
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                    say("removed reason 0x%08lx", (unsigned long)dev->GetDeviceRemovedReason());
                    break;
                }
            }
        } else {
            Sleep(16);
            frames++;
        }
        ULONG64 el = GetTickCount64() - t0;
        if ((sampled == 0 && el > 1500) || (sampled == 1 && el > (ULONG64)seconds * 600)) {
            char a[16], b[16];
            HDC wdc = GetDC(hwnd), sdc = GetDC(NULL);
            COLORREF cw = GetPixel(wdc, W / 2, H / 2), cs = GetPixel(sdc, centre.x, centre.y);
            ReleaseDC(hwnd, wdc);
            ReleaseDC(NULL, sdc);
            say("sample %u at %llu ms, frame %u: wdc %s sdc %s", sampled, el, frames, pix(cw, a, sizeof(a)),
                 pix(cs, b, sizeof(b)));
            if (!nodup)
                dup_sample(centre, sampled ? "late" : "early");
            sampled++;
        }
    }
    say("presented %u frames in %d s, %u failed", sc ? frames : 0, seconds, bad);
    if (bad)
        g_fails++;
    kmd_stats("after", &k1);
    if (g_mw)
        say("kmd delta: presents %u blts %u fills %u skipped %u flips %u flipcopies %u",
             k1.Presents - k0.Presents, k1.PresentBlts - k0.PresentBlts, k1.PresentFills - k0.PresentFills,
             k1.PresentSkips - k0.PresentSkips, k1.Flips - k0.Flips, k1.FlipCopies - k0.FlipCopies);
    if (rtv) rtv->Release();
    if (sc1) sc1->Release();
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    say("exit %d", g_fails);
    if (g_log)
        fclose(g_log);
    return g_fails;
}
