// SPDX-License-Identifier: GPL-2.0
/*
 * d3dprobe: what a browser finds when it asks Direct3D for a GPU.
 *
 * Edge (Chromium, through ANGLE) renders with Direct3D 11 on the adapter
 * DXGI lists first, and uses hardware video decode only through an
 * ID3D11VideoDevice on that same device. This prints, for every adapter
 * and for the default one a plain D3D11CreateDevice picks:
 *
 *   - name, vendor and device id, LUID, flags (software or not);
 *   - the feature level the device comes up at (maliwddm's UMD offers
 *     10_0 at most);
 *   - whether the device is a D3D11.1 one (ID3D11Device1: the UMD would
 *     need the D3D11.1 DDI) and has a video device (ID3D11VideoDevice:
 *     the video DDI), which hardware decode in Edge needs;
 *   - format support a video path uses (NV12, R8, R8G8, B8G8R8A8);
 *   - whether DXGI makes a flip-model swap chain on it (FLIP_SEQUENTIAL),
 *     which lets DWM compose an application's buffers without a copy.
 *
 * Exit status 0 when it ran; it changes nothing on the system.
 *
 * Build: cl /W4 /EHsc tools\windows\d3dprobe.cpp d3d11.lib dxgi.lib user32.lib
 */
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>

static const char *fl_name(D3D_FEATURE_LEVEL f)
{
    switch (f) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    default: return "?";
    }
}

static void formats(ID3D11Device *dev)
{
    static const struct { DXGI_FORMAT f; const char *n; } list[] = {
        { DXGI_FORMAT_NV12, "NV12" },
        { DXGI_FORMAT_R8_UNORM, "R8" },
        { DXGI_FORMAT_R8G8_UNORM, "R8G8" },
        { DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8" },
        { DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2" },
    };
    for (const auto &e : list) {
        UINT s = 0;
        HRESULT hr = dev->CheckFormatSupport(e.f, &s);
        printf("    format %-12s %s (0x%08x)%s%s%s\n", e.n, SUCCEEDED(hr) ? "supported" : "not supported",
               s, (s & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? " texture2d" : "",
               (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? " render-target" : "",
               (s & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? " sample" : "");
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

/* A flip-model swap chain on this device, in a small window of our own. */
static void flip_model(ID3D11Device *dev)
{
    IDXGIDevice *xd = nullptr;
    IDXGIAdapter *ad = nullptr;
    IDXGIFactory2 *f2 = nullptr;
    HWND wnd;
    WNDCLASSW wc = {};

    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd)) ||
        FAILED(xd->GetAdapter(&ad)) || FAILED(ad->GetParent(__uuidof(IDXGIFactory2), (void **)&f2))) {
        printf("    flip model: no IDXGIFactory2 (DXGI 1.2)\n");
        goto out;
    }
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"d3dprobe";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(0, L"d3dprobe", L"d3dprobe", WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, nullptr,
                          nullptr, wc.hInstance, nullptr);
    if (!wnd) {
        printf("    flip model: no window (0x%08lx)\n", GetLastError());
        goto out;
    }
    for (int effect = 0; effect < 2; effect++) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        IDXGISwapChain1 *sc = nullptr;
        d.Width = 128;
        d.Height = 96;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = effect ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_DISCARD;
        HRESULT hr = f2->CreateSwapChainForHwnd(dev, wnd, &d, nullptr, nullptr, &sc);
        HRESULT pr = E_FAIL;
        if (SUCCEEDED(hr))
            pr = sc->Present(0, 0);
        printf("    swap chain %-15s create 0x%08lx, present 0x%08lx%s\n",
               effect ? "FLIP_SEQUENTIAL" : "DISCARD (blt)", (unsigned long)hr, (unsigned long)pr,
               hr == (HRESULT)0x887A0022 ? " (no interactive desktop: run it in a user's session)" : "");
        if (sc)
            sc->Release();
    }
    DestroyWindow(wnd);
out:
    if (f2) f2->Release();
    if (ad) ad->Release();
    if (xd) xd->Release();
}

static void probe(IDXGIAdapter1 *adapter, const char *label)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1,
    };
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    HRESULT hr;

    hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                           &dev, &got, &ctx);
    if (hr == E_INVALIDARG) /* a runtime that does not know 11_1 */
        hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, ARRAYSIZE(levels) - 1,
                               D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("  %s: D3D11CreateDevice 0x%08lx, feature level %s\n", label, (unsigned long)hr,
           SUCCEEDED(hr) ? fl_name(got) : "-");
    if (FAILED(hr))
        return;
    if (!adapter) {
        /* Which adapter the default device landed on. */
        IDXGIDevice *xd = nullptr;
        IDXGIAdapter *ad = nullptr;
        DXGI_ADAPTER_DESC d = {};
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)) &&
            SUCCEEDED(ad->GetDesc(&d)))
            printf("    on: %ls (vendor 0x%04x device 0x%04x)\n", d.Description, d.VendorId, d.DeviceId);
        if (ad) ad->Release();
        if (xd) xd->Release();
    }
    {
        ID3D11Device1 *d1 = nullptr;
        ID3D11VideoDevice *vd = nullptr;
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS x = {};
        D3D11_FEATURE_DATA_THREADING t = {};
        HRESULT h1 = dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1);
        HRESULT hv = dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&vd);
        printf("    ID3D11Device1 (D3D11.1 DDI): %s (0x%08lx)\n", SUCCEEDED(h1) ? "yes" : "no", (unsigned long)h1);
        printf("    ID3D11VideoDevice (video DDI, hardware decode): %s (0x%08lx)\n",
               SUCCEEDED(hv) ? "yes" : "no", (unsigned long)hv);
        if (vd) {
            printf("    video decoder profiles: %u\n", vd->GetVideoDecoderProfileCount());
            vd->Release();
        }
        if (d1) d1->Release();
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &x, sizeof(x))))
            printf("    compute on 4_x: %u\n", x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x);
        /* ID3D11Device1 exists on any device of the D3D11.1 runtime, even
         * over a D3D10.1-DDI driver; what the driver's DDI decides shows
         * here. The first two are TRUE only for a D3D11.1-DDI driver. */
        D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o))))
            printf("    D3D11.1 DDI seen: discard %u, copy/update flags %u; clear view %u, copy with overlap %u,"
                   " constant buffer offsets %u, logic op %u\n", o.DiscardAPIsSeenByDriver,
                   o.FlagsForUpdateAndCopySeenByDriver, o.ClearView, o.CopyWithOverlap,
                   o.ConstantBufferOffsetting, o.OutputMergerLogicOp);
        else
            printf("    D3D11_OPTIONS: not answered\n");
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &t, sizeof(t))))
            printf("    threading: concurrent creates %u, command lists %u\n", t.DriverConcurrentCreates,
                   t.DriverCommandLists);
    }
    formats(dev);
    flip_model(dev);
    ctx->Release();
    dev->Release();
}

int main()
{
    IDXGIFactory1 *f = nullptr;
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
        printf("no DXGI factory\n");
        return 2;
    }
    printf("adapters, in the order DXGI lists them (the first is what Edge and ANGLE use by default):\n");
    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *a = nullptr;
        DXGI_ADAPTER_DESC1 d = {};
        char label[32];
        if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        a->GetDesc1(&d);
        printf("%u: %ls, vendor 0x%04x device 0x%04x rev %u, LUID %08lx:%08lx, %s, dedicated %zu MB,"
               " shared %zu MB\n", i, d.Description, d.VendorId, d.DeviceId, d.Revision,
               (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart,
               (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "software" : "hardware",
               d.DedicatedVideoMemory >> 20, d.SharedSystemMemory >> 20);
        snprintf(label, sizeof label, "adapter %u", i);
        probe(a, label);
        a->Release();
    }
    printf("default hardware device (no adapter given):\n");
    probe(nullptr, "default");
    f->Release();
    return 0;
}
