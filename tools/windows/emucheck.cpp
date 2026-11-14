// SPDX-License-Identifier: GPL-2.0
//
// emucheck: which user-mode driver an x64 or x86 process on Windows on Arm
// gets for the Mali, and whether it draws. The same source is built for
// ARM64, x64 and x86; run each build on the board.
//
//   emucheck.exe [hw | warp | sw <dll>]        (default hw)
//
// sw <dll> makes a D3D_DRIVER_TYPE_SOFTWARE device on that DLL (CI uses it
// to check the ARM64X forwarder: which maliumd_*.dll it ends up loading).
//
// It prints, in order:
//   - what the process is: the architecture it was built for and what
//     IsWow64Process2 says (x86 under WOW64 shows process 0x14c);
//   - every DXGI adapter;
//   - for the Mali adapter, the UMD file names the kernel hands this
//     process (D3DKMTQueryAdapterInfo, KMTQAITYPE_UMDRIVERNAME, for DX9,
//     DX10, DX11 and DX12): the names the D3D runtime loads, chosen by the
//     kernel by process type, so this shows what the INF registered for it;
//   - D3D11CreateDevice on that adapter at feature level 10_0, and which
//     adapter the device really ended up on (description, software flag);
//   - one red triangle on blue, 64x64, and the centre pixel read back;
//   - every loaded module whose name contains "mali" or "d3d10warp", with
//     the machine type in its in-memory PE header.
//
// Exit code: 0 the device is on the Mali (not software) and the centre
// pixel is red; 1 the device is on the Mali but the pixel is wrong; 2
// D3D11CreateDevice failed; 3 a device was made, but not on the Mali; 4
// there is no adapter named Mali; 5 the shaders did not compile; 100 any
// other failure. In warp and sw mode, 0 means the triangle was drawn.
//
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d11.h>
#include <dxgi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef EMUCHECK_PRECOMPILED
#include "emucheck_shaders.h"   // g_vs, g_ps: fxc /Fh, made by CI
#else
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")

#define CHECK(hr, what)                                                       \
    do {                                                                      \
        HRESULT hr_ = (hr);                                                   \
        if (FAILED(hr_)) {                                                    \
            printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr_);        \
            return 100;                                                       \
        }                                                                     \
    } while (0)

#ifndef EMUCHECK_PRECOMPILED
static const char kHlsl[] =
    "float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }\n"
    "float4 ps() : SV_Target { return float4(1, 0, 0, 1); }\n";
#endif

static const char *built_for(void)
{
#if defined(_M_ARM64EC)
    return "ARM64EC";
#elif defined(_M_ARM64)
    return "ARM64";
#elif defined(_M_X64)
    return "x64";
#elif defined(_M_IX86)
    return "x86";
#else
    return "?";
#endif
}

static const char *machine_name(USHORT m)
{
    switch (m) {
    case IMAGE_FILE_MACHINE_UNKNOWN: return "none";
    case IMAGE_FILE_MACHINE_I386:    return "x86";
    case IMAGE_FILE_MACHINE_AMD64:   return "x64";
    case IMAGE_FILE_MACHINE_ARM64:   return "ARM64";
    case 0xA641:                     return "ARM64EC";
    case 0xA64E:                     return "ARM64X";
    case IMAGE_FILE_MACHINE_ARMNT:   return "ARM32";
    case 0x3A64:                     return "CHPE x86";
    default:                         return "other";
    }
}

static void print_process(void)
{
    typedef BOOL(WINAPI * WOW2)(HANDLE, USHORT *, USHORT *);
    WOW2 w2 = (WOW2)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    USHORT pm = 0, nm = 0;
    printf("process: built for %s", built_for());
    if (w2 && w2(GetCurrentProcess(), &pm, &nm))
        printf(", IsWow64Process2 process 0x%04x (%s) native 0x%04x (%s)", pm,
               pm ? machine_name(pm) : "not WOW64", nm, machine_name(nm));
    printf(", pointer %u bytes\n", (unsigned)sizeof(void *));
}

// The UMD names the kernel gives this process for the adapter: what the
// runtime will LoadLibrary. A WOW64 process is given the Wow names.
static void print_umd_names(LUID luid)
{
    D3DKMT_OPENADAPTERFROMLUID o = {};
    o.AdapterLuid = luid;
    NTSTATUS st = D3DKMTOpenAdapterFromLuid(&o);
    if (st < 0) {
        printf("  D3DKMTOpenAdapterFromLuid: 0x%08lx\n", (unsigned long)st);
        return;
    }
    static const struct { KMTUMDVERSION v; const char *name; } kinds[] = {
        {KMTUMDVERSION_DX9, "DX9"}, {KMTUMDVERSION_DX10, "DX10"},
        {KMTUMDVERSION_DX11, "DX11"}, {KMTUMDVERSION_DX12, "DX12"},
    };
    for (const auto &k : kinds) {
        D3DKMT_UMDFILENAMEINFO u = {};
        u.Version = k.v;
        D3DKMT_QUERYADAPTERINFO q = {};
        q.hAdapter = o.hAdapter;
        q.Type = KMTQAITYPE_UMDRIVERNAME;
        q.pPrivateDriverData = &u;
        q.PrivateDriverDataSize = sizeof(u);
        st = D3DKMTQueryAdapterInfo(&q);
        if (st < 0)
            printf("  UMD name %-4s: status 0x%08lx\n", k.name, (unsigned long)st);
        else
            printf("  UMD name %-4s: %ls\n", k.name, u.UmdFileName);
    }
    D3DKMT_CLOSEADAPTER c = {};
    c.hAdapter = o.hAdapter;
    D3DKMTCloseAdapter(&c);
}

static void print_modules(void)
{
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        printf("modules: EnumProcessModules failed (%lu)\n", GetLastError());
        return;
    }
    unsigned n = need / sizeof(HMODULE), shown = 0;
    if (n > 1024)
        n = 1024;
    for (unsigned i = 0; i < n; i++) {
        wchar_t path[MAX_PATH * 2];
        if (!GetModuleFileNameExW(GetCurrentProcess(), mods[i], path, MAX_PATH * 2))
            continue;
        wchar_t lower[MAX_PATH * 2];
        wcsncpy_s(lower, path, _TRUNCATE);
        _wcslwr_s(lower);
        const wchar_t *base = wcsrchr(lower, L'\\');
        base = base ? base + 1 : lower;
        if (!wcsstr(base, L"mali") && !wcsstr(base, L"d3d10warp"))
            continue;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mods[i];
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const BYTE *)mods[i] + dos->e_lfanew);
        printf("module: %ls (header machine %s)\n", path, machine_name(nt->FileHeader.Machine));
        shown++;
    }
    printf("modules matching mali or d3d10warp: %u of %u\n", shown, n);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "hw";
    bool warp = _stricmp(mode, "warp") == 0;
    const char *swdll = _stricmp(mode, "sw") == 0 ? (argc > 2 ? argv[2] : "maliumd.dll") : nullptr;
    bool hw = !warp && !swdll;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got = (D3D_FEATURE_LEVEL)0;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;

    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    print_process();

    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *pick = nullptr, *a = nullptr;
    CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac), "CreateDXGIFactory1");
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        printf("adapter %u: %ls (vendor 0x%04x device 0x%04x, luid %08lx:%08lx, flags 0x%x)\n", i,
               d.Description, d.VendorId, d.DeviceId, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart,
               d.Flags);
        if (!pick && wcsstr(d.Description, L"Mali")) {
            pick = a;
            print_umd_names(d.AdapterLuid);
        } else {
            a->Release();
        }
    }

    HRESULT hr;
    if (hw) {
        if (!pick) {
            printf("FAIL no adapter named Mali\n");
            print_modules();
            return 4;
        }
        hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                               &dev, &got, &ctx);
    } else if (swdll) {
        HMODULE sw = LoadLibraryA(swdll);
        if (!sw) {
            printf("FAIL LoadLibrary(%s): error %lu\n", swdll, GetLastError());
            print_modules();
            return 2;
        }
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_SOFTWARE, sw, 0, &fl, 1, D3D11_SDK_VERSION,
                               &dev, &got, &ctx);
    } else {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                               &dev, &got, &ctx);
    }
    printf("D3D11CreateDevice (%s, feature level 10_0): hr=0x%08lx\n",
           hw ? "Mali adapter" : swdll ? swdll : "WARP", (unsigned long)hr);
    if (FAILED(hr)) {
        print_modules();
        printf("RESULT no device\n");
        return 2;
    }

    // Which adapter the device is really on.
    bool on_mali = false, software = false;
    {
        IDXGIDevice *xd = nullptr;
        IDXGIAdapter *xa = nullptr;
        IDXGIAdapter1 *xa1 = nullptr;
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd)) &&
            SUCCEEDED(xd->GetAdapter(&xa)) &&
            SUCCEEDED(xa->QueryInterface(__uuidof(IDXGIAdapter1), (void **)&xa1)) &&
            SUCCEEDED(xa1->GetDesc1(&d))) {
            on_mali = wcsstr(d.Description, L"Mali") != nullptr;
            software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            printf("device: feature level 0x%x on \"%ls\", %s\n", got, d.Description,
                   software ? "software adapter" : "hardware adapter");
        } else {
            printf("device: feature level 0x%x, adapter unknown\n", got);
        }
        if (xa1) xa1->Release();
        if (xa) xa->Release();
        if (xd) xd->Release();
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &rt), "CreateTexture2D (target)");
    ID3D11RenderTargetView *rtv = nullptr;
    CHECK(dev->CreateRenderTargetView(rt, nullptr, &rtv), "CreateRenderTargetView");

    const void *vsc, *psc;
    SIZE_T vsn, psn;
#ifdef EMUCHECK_PRECOMPILED
    vsc = g_vs; vsn = sizeof(g_vs);
    psc = g_ps; psn = sizeof(g_ps);
#else
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "emucheck", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "emucheck", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        printf("FAIL shader compile 0x%08lx: %s\n", (unsigned long)hr,
               err ? (const char *)err->GetBufferPointer() : "");
        return 5;
    }
    vsc = vsb->GetBufferPointer(); vsn = vsb->GetBufferSize();
    psc = psb->GetBufferPointer(); psn = psb->GetBufferSize();
#endif
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    CHECK(dev->CreateVertexShader(vsc, vsn, nullptr, &vs), "CreateVertexShader");
    CHECK(dev->CreatePixelShader(psc, psn, nullptr, &ps), "CreatePixelShader");

    const float verts[] = {0.0f, 0.8f, 0.8f, -0.8f, -0.8f, -0.8f};
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {verts, 0, 0};
    ID3D11Buffer *vb = nullptr;
    CHECK(dev->CreateBuffer(&bd, &init, &vb), "CreateBuffer");
    D3D11_INPUT_ELEMENT_DESC ied = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ID3D11InputLayout *il = nullptr;
    CHECK(dev->CreateInputLayout(&ied, 1, vsc, vsn, &il), "CreateInputLayout");
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = nullptr;
    CHECK(dev->CreateRasterizerState(&rd, &rs), "CreateRasterizerState");

    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, blue);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs);
    UINT stride = 8, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &st), "CreateTexture2D (staging)");
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "Map");
    UINT centre = *(const UINT *)((const BYTE *)m.pData + 32 * m.RowPitch + 32 * 4);
    UINT corner = *(const UINT *)((const BYTE *)m.pData + 1 * m.RowPitch + 1 * 4);
    ctx->Unmap(st, 0);

    // RGBA8 little-endian: red 0xff0000ff, blue 0xffff0000.
    bool red = centre == 0xff0000ffu;
    printf("pixels: centre 0x%08x (want 0xff0000ff), corner 0x%08x (want 0xffff0000)\n", centre, corner);
    print_modules();

    int code;
    if (!hw)
        code = red ? 0 : 1;
    else if (!on_mali || software)
        code = 3;
    else
        code = red ? 0 : 1;
    printf("RESULT %s\n", code == 0 ? (hw ? "PASS: hardware device on the Mali, triangle drawn"
                                          : "PASS: the triangle was drawn")
                        : code == 1 ? "FAIL: device made, centre pixel wrong"
                                    : "FAIL: the device is not on the Mali");
    return code;
}
