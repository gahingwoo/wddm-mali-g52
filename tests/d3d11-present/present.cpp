// SPDX-License-Identifier: GPL-2.0
/*
 * present: a swap chain on the software driver DLL, cleared and presented a
 * few times into a small window. Exercises d3d10umd's Present path, which
 * for Panfrost is the GDI copy in mesa-patches/0014.
 *
 *   present.exe <path to libgallium_d3d10.dll>   exit 0 = every call succeeded
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

#define CHECK(hr, what)                                                       \
    do {                                                                      \
        if (FAILED(hr)) {                                                     \
            printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)(hr));       \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    /* present.exe <software DLL> | present.exe hw (the DXGI adapter named
     * Mali, a hardware device: the runtime loads maliwddm's UMD). */
    const char *dll = argc > 1 ? argv[1] : "libgallium_d3d10.dll";
    bool hw = _stricmp(dll, "hw") == 0;
    HMODULE sw = NULL;
    IDXGIAdapter1 *pick = NULL;
    if (hw) {
        IDXGIFactory1 *fac = NULL;
        IDXGIAdapter1 *a = NULL;
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
            return 1;
        }
    } else {
        sw = LoadLibraryA(dll);
        if (!sw) {
            printf("FAIL LoadLibrary(%s): %lu\n", dll, GetLastError());
            return 1;
        }
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "mali-present";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("mali-present", "mali-present", WS_OVERLAPPEDWINDOW,
                              0, 0, 160, 160, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("FAIL CreateWindow: %lu\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_10_0, got;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *swap = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(pick, hw ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_SOFTWARE,
                                               sw, 0, &want, 1, D3D11_SDK_VERSION, &sd, &swap, &dev,
                                               &got, &ctx);
    CHECK(hr, "D3D11CreateDeviceAndSwapChain");
    printf("swap chain created (%s), feature level 0x%x\n", hw ? "hardware" : "software", got);

    ID3D11Texture2D *back = NULL;
    CHECK(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back), "GetBuffer");
    ID3D11RenderTargetView *rtv = NULL;
    CHECK(dev->CreateRenderTargetView(back, NULL, &rtv), "CreateRenderTargetView");

    const float colours[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (int i = 0; i < 3; i++) {
        ctx->OMSetRenderTargets(1, &rtv, NULL);
        ctx->ClearRenderTargetView(rtv, colours[i]);
        CHECK(swap->Present(0, 0), "Present");
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&m);
    }
    printf("presented 3 frames\n");

    rtv->Release();
    back->Release();
    swap->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    printf("PASS\n");
    return 0;
}
