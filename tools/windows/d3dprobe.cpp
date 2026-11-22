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
 *     which lets DWM compose an application's buffers without a copy;
 *   - when the device does not come up with the full list of feature
 *     levels, the result at 10_1 and at 10_0 alone;
 *   - every format's support bits and quality levels at 1x, 2x, 4x and 8x
 *     ("format table"), and where they disagree with each other or within
 *     a typeless family ("MSAA CONSISTENCY"), which the runtime may refuse
 *     a device for.
 *
 * The UMD gives its multisample answers only to some process names
 * (msedge.exe, chrome.exe, msedgewebview2.exe, d3dprobe.exe): run this
 * binary under such a name and under another one to see both sets.
 *
 *   d3dprobe            every adapter DXGI lists, then the default device
 *   d3dprobe <dll>      only <dll>, as a D3D_DRIVER_TYPE_SOFTWARE driver
 *                       (Mesa's d3d10umd on a CI runner); exit status 3
 *                       when the runtime does not create that device.
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
        { DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8" },
        { DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2" },
    };
    for (const auto &e : list) {
        UINT s = 0;
        HRESULT hr = dev->CheckFormatSupport(e.f, &s);
        printf("    format %-12s %s (0x%08x)%s%s%s%s\n", e.n, SUCCEEDED(hr) ? "supported" : "not supported",
               s, (s & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? " texture2d" : "",
               (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? " render-target" : "",
               (s & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? " sample" : "",
               (s & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) ? " ms-render-target" : "");
        /* What ANGLE builds its sample counts from (and Skia, through it,
         * whether RGBA8 is renderable at all). */
        if (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET) {
            printf("      quality levels:");
            for (UINT n = 1; n <= 8; n *= 2) {
                UINT q = 0;
                HRESULT qh = dev->CheckMultisampleQualityLevels(e.f, n, &q);
                printf(" %ux %u%s", n, q, SUCCEEDED(qh) ? "" : "(failed)");
            }
            printf("\n");
        }
    }
}

/* The DXGI formats up to B8G8R8X8_UNORM_SRGB, B4G4R4A4 and the video ones
 * a browser uses; the number is the DXGI_FORMAT value. */
static const struct { DXGI_FORMAT f; const char *n; } all_formats[] = {
#define F(x) { DXGI_FORMAT_##x, #x }
    F(R32G32B32A32_TYPELESS), F(R32G32B32A32_FLOAT), F(R32G32B32A32_UINT), F(R32G32B32A32_SINT),
    F(R32G32B32_TYPELESS), F(R32G32B32_FLOAT), F(R32G32B32_UINT), F(R32G32B32_SINT),
    F(R16G16B16A16_TYPELESS), F(R16G16B16A16_FLOAT), F(R16G16B16A16_UNORM), F(R16G16B16A16_UINT),
    F(R16G16B16A16_SNORM), F(R16G16B16A16_SINT),
    F(R32G32_TYPELESS), F(R32G32_FLOAT), F(R32G32_UINT), F(R32G32_SINT),
    F(R32G8X24_TYPELESS), F(D32_FLOAT_S8X24_UINT), F(R32_FLOAT_X8X24_TYPELESS), F(X32_TYPELESS_G8X24_UINT),
    F(R10G10B10A2_TYPELESS), F(R10G10B10A2_UNORM), F(R10G10B10A2_UINT), F(R11G11B10_FLOAT),
    F(R8G8B8A8_TYPELESS), F(R8G8B8A8_UNORM), F(R8G8B8A8_UNORM_SRGB), F(R8G8B8A8_UINT),
    F(R8G8B8A8_SNORM), F(R8G8B8A8_SINT),
    F(R16G16_TYPELESS), F(R16G16_FLOAT), F(R16G16_UNORM), F(R16G16_UINT), F(R16G16_SNORM), F(R16G16_SINT),
    F(R32_TYPELESS), F(D32_FLOAT), F(R32_FLOAT), F(R32_UINT), F(R32_SINT),
    F(R24G8_TYPELESS), F(D24_UNORM_S8_UINT), F(R24_UNORM_X8_TYPELESS), F(X24_TYPELESS_G8_UINT),
    F(R8G8_TYPELESS), F(R8G8_UNORM), F(R8G8_UINT), F(R8G8_SNORM), F(R8G8_SINT),
    F(R16_TYPELESS), F(R16_FLOAT), F(D16_UNORM), F(R16_UNORM), F(R16_UINT), F(R16_SNORM), F(R16_SINT),
    F(R8_TYPELESS), F(R8_UNORM), F(R8_UINT), F(R8_SNORM), F(R8_SINT), F(A8_UNORM), F(R1_UNORM),
    F(R9G9B9E5_SHAREDEXP), F(R8G8_B8G8_UNORM), F(G8R8_G8B8_UNORM),
    F(BC1_TYPELESS), F(BC1_UNORM), F(BC1_UNORM_SRGB), F(BC2_TYPELESS), F(BC2_UNORM), F(BC2_UNORM_SRGB),
    F(BC3_TYPELESS), F(BC3_UNORM), F(BC3_UNORM_SRGB), F(BC4_TYPELESS), F(BC4_UNORM), F(BC4_SNORM),
    F(BC5_TYPELESS), F(BC5_UNORM), F(BC5_SNORM), F(B5G6R5_UNORM), F(B5G5R5A1_UNORM),
    F(B8G8R8A8_UNORM), F(B8G8R8X8_UNORM), F(R10G10B10_XR_BIAS_A2_UNORM), F(B8G8R8A8_TYPELESS),
    F(B8G8R8A8_UNORM_SRGB), F(B8G8R8X8_TYPELESS), F(B8G8R8X8_UNORM_SRGB), F(B4G4R4A4_UNORM),
    F(NV12), F(P010), F(YUY2),
#undef F
};

/* The typeless family (cast set) a format belongs to, by its first member;
 * 0 for a format that is alone in its own. */
static UINT family(DXGI_FORMAT f)
{
    static const UINT first[] = { 1, 5, 9, 15, 19, 23, 27, 33, 39, 44, 48, 53, 60 };
    static const UINT last[] = { 4, 8, 14, 18, 22, 25, 32, 38, 43, 47, 52, 59, 64 };
    for (UINT i = 0; i < ARRAYSIZE(first); i++)
        if ((UINT)f >= first[i] && (UINT)f <= last[i])
            return first[i];
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default:
        return 0;
    }
}

/*
 * The whole table, and what in it does not agree: on a render or depth
 * target format, a sample count above one with quality levels but no
 * multisample render target bit, or the bit with no such count; and any
 * member of a typeless family whose quality levels above 1x are not the
 * typeless format's. The second is what the D3D11 runtime refuses a device for
 * (0x887A0020): it asks the typeless format first and stops at the first
 * member that answers otherwise (mesa-patches/0068). Returns the number of
 * disagreements.
 */
static unsigned format_table(ID3D11Device *dev)
{
    static const UINT counts[] = { 1, 2, 4, 8 };
    UINT sup[ARRAYSIZE(all_formats)], q[ARRAYSIZE(all_formats)][ARRAYSIZE(counts)];
    unsigned bad = 0;

    printf("    format table (support bits; quality levels at 1x 2x 4x 8x):\n");
    for (UINT i = 0; i < ARRAYSIZE(all_formats); i++) {
        DXGI_FORMAT f = all_formats[i].f;
        sup[i] = 0;
        if (FAILED(dev->CheckFormatSupport(f, &sup[i])))
            sup[i] = 0;
        for (UINT c = 0; c < ARRAYSIZE(counts); c++) {
            q[i][c] = 0;
            if (FAILED(dev->CheckMultisampleQualityLevels(f, counts[c], &q[i][c])))
                q[i][c] = 0;
        }
        printf("      %3u %-26s 0x%08x %s%s%s%s%s%s q %u %u %u %u\n", (UINT)f, all_formats[i].n, sup[i],
               (sup[i] & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? " rt" : "",
               (sup[i] & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) ? " ds" : "",
               (sup[i] & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? " sample" : "",
               (sup[i] & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) ? " ms-rt" : "",
               (sup[i] & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) ? " ms-resolve" : "",
               (sup[i] & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD) ? " ms-load" : "",
               q[i][0], q[i][1], q[i][2], q[i][3]);
    }
    for (UINT i = 0; i < ARRAYSIZE(all_formats); i++) {
        bool above = q[i][1] || q[i][2] || q[i][3];
        bool msrt = (sup[i] & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) != 0;
        UINT kind = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
        if ((sup[i] & kind) && above != msrt) {
            printf("    MSAA CONSISTENCY: %s: %s\n", all_formats[i].n,
                   msrt ? "multisample render target bit, but no sample count above 1"
                        : "quality levels above 1x, but no multisample render target bit");
            bad++;
        }
        UINT fam = family(all_formats[i].f);
        if (!fam || (UINT)all_formats[i].f == fam)
            continue;
        for (UINT j = 0; j < ARRAYSIZE(all_formats); j++) {
            if ((UINT)all_formats[j].f != fam)
                continue;
            /* 1x is the runtime's own answer; above it the driver's. */
            for (UINT c = 1; c < ARRAYSIZE(counts); c++)
                if (q[i][c] != q[j][c]) {
                    printf("    MSAA CONSISTENCY: %ux: %s %u, its typeless %s %u\n", counts[c],
                           all_formats[i].n, q[i][c], all_formats[j].n, q[j][c]);
                    bad++;
                }
        }
    }
    printf("    MSAA consistency: %u disagreement(s)\n", bad);
    return bad;
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

/* D3D11CreateDevice on an adapter, the default hardware device (adapter
 * null, sw null), or a software driver DLL (sw). */
static HRESULT create(IDXGIAdapter1 *adapter, HMODULE sw, const D3D_FEATURE_LEVEL *levels, UINT n,
                      ID3D11Device **dev, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx)
{
    D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : sw ? D3D_DRIVER_TYPE_SOFTWARE
                                                                  : D3D_DRIVER_TYPE_HARDWARE;
    return D3D11CreateDevice(adapter, type, sw, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, n,
                             D3D11_SDK_VERSION, dev, got, ctx);
}

/* Returns the D3D11CreateDevice result with every feature level offered. */
static HRESULT probe(IDXGIAdapter1 *adapter, const char *label, HMODULE sw = nullptr)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1,
    };
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    HRESULT hr;

    hr = create(adapter, sw, levels, ARRAYSIZE(levels), &dev, &got, &ctx);
    if (hr == E_INVALIDARG) /* a runtime that does not know 11_1 */
        hr = create(adapter, sw, levels + 1, ARRAYSIZE(levels) - 1, &dev, &got, &ctx);
    printf("  %s: D3D11CreateDevice 0x%08lx, feature level %s\n", label, (unsigned long)hr,
           SUCCEEDED(hr) ? fl_name(got) : "-");
    if (FAILED(hr)) {
        /* Which feature level the runtime refuses: each alone. */
        for (UINT i = 2; i < 4; i++) {
            ID3D11Device *d = nullptr;
            ID3D11DeviceContext *c = nullptr;
            D3D_FEATURE_LEVEL g;
            HRESULT h = create(adapter, sw, &levels[i], 1, &d, &g, &c);
            printf("    feature level %s alone: 0x%08lx\n", fl_name(levels[i]), (unsigned long)h);
            if (c) c->Release();
            if (d) d->Release();
        }
        return hr;
    }
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
    format_table(dev);
    flip_model(dev);
    ctx->Release();
    dev->Release();
    return hr;
}

int main(int argc, char **argv)
{
    IDXGIFactory1 *f = nullptr;
    setvbuf(stdout, nullptr, _IONBF, 0);
    {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
        printf("running as %s\n", n && n < sizeof exe ? exe : "?");
    }
    if (argc > 1) {
        HMODULE sw = LoadLibraryA(argv[1]);
        if (!sw) {
            printf("LoadLibrary(%s): %lu\n", argv[1], GetLastError());
            return 2;
        }
        printf("software driver %s:\n", argv[1]);
        return FAILED(probe(nullptr, "software", sw)) ? 3 : 0;
    }
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
