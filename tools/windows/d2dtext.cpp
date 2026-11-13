// SPDX-License-Identifier: GPL-2.0
//
// d2dtext: draw what a XAML application draws, with Direct2D on a Direct3D 11
// device, and save the result, so that what the driver makes of Direct2D's
// shaders can be looked at without a monitor.
//
//   d2dtext.exe hw   out.bmp     the adapter whose description names the Mali
//   d2dtext.exe warp out.bmp     Microsoft's software rasteriser, for reference
//
// Draws on a 512x256 BGRA target cleared to blue: a white rectangle, a white
// ellipse, two lines of text (default and greyscale antialiasing), and a
// 64x64 red/green checker bitmap, once stretched to 128x128 and once at half
// opacity. Prints how many pixels of each region differ from the background,
// and writes the frame as a 32-bit BMP. Run it both ways and compare.
//
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define W 512
#define H 256

#define CHECK(hr, what)                                                       \
    do {                                                                      \
        HRESULT hr_ = (hr);                                                   \
        if (FAILED(hr_)) {                                                    \
            printf("FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr_);        \
            return 1;                                                         \
        }                                                                     \
        printf("ok   %s\n", what);                                            \
    } while (0)

static unsigned count_region(const unsigned char *px, unsigned pitch, int x0, int y0, int x1, int y1,
                             unsigned bg)
{
    unsigned n = 0;
    for (int y = y0; y < y1; y++) {
        const unsigned *row = (const unsigned *)(px + (size_t)y * pitch);
        for (int x = x0; x < x1; x++)
            if ((row[x] & 0xffffff) != (bg & 0xffffff))
                n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "hw";
    const char *out = argc > 2 ? argv[2] : "d2dtext.bmp";
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
            printf("adapter %u: %ls\n", i, d.Description);
            if (!pick && wcsstr(d.Description, L"Mali"))
                pick = a;
            else
                a->Release();
        }
        if (!pick) {
            printf("FAIL no adapter named Mali\n");
            return 1;
        }
        CHECK(D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &dev,
                                &got, &ctx),
              "D3D11CreateDevice (hardware)");
    } else {
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &dev,
                                &got, &ctx),
              "D3D11CreateDevice (WARP)");
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &tex), "CreateTexture2D (target)");

    IDXGISurface *surf = nullptr;
    CHECK(tex->QueryInterface(__uuidof(IDXGISurface), (void **)&surf), "target as IDXGISurface");
    IDXGIDevice *dxgidev = nullptr;
    CHECK(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgidev), "device as IDXGIDevice");

    D2D1_FACTORY_OPTIONS fo = {};
    ID2D1Factory1 *d2d = nullptr;
    CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                            (void **)&d2d),
          "D2D1CreateFactory");
    ID2D1Device *d2ddev = nullptr;
    CHECK(d2d->CreateDevice(dxgidev, &d2ddev), "ID2D1Factory1::CreateDevice");
    ID2D1DeviceContext *dc = nullptr;
    CHECK(d2ddev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc), "CreateDeviceContext");

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ID2D1Bitmap1 *target = nullptr;
    CHECK(dc->CreateBitmapFromDxgiSurface(surf, &bp, &target), "CreateBitmapFromDxgiSurface");
    dc->SetTarget(target);

    IDWriteFactory *dw = nullptr;
    CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              (IUnknown **)&dw),
          "DWriteCreateFactory");
    IDWriteTextFormat *fmt = nullptr;
    CHECK(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 40.0f, L"en-us",
                               &fmt),
          "CreateTextFormat");

    ID2D1SolidColorBrush *white = nullptr;
    CHECK(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &white), "CreateSolidColorBrush");

    /* The checker: 8x8 squares, red and green, opaque. */
    static unsigned checker[64 * 64];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            checker[y * 64 + x] = (((x >> 3) ^ (y >> 3)) & 1) ? 0xffff0000u : 0xff00ff00u;
    D2D1_BITMAP_PROPERTIES1 cp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ID2D1Bitmap1 *bmp = nullptr;
    CHECK(dc->CreateBitmap(D2D1::SizeU(64, 64), checker, 64 * 4, &cp, &bmp), "CreateBitmap (checker)");

    static const WCHAR text1[] = L"Mali text 123";
    static const WCHAR text2[] = L"greyscale abc";

    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.0f, 0.25f, 0.5f, 1.0f));
    dc->FillRectangle(D2D1::RectF(10.0f, 10.0f, 60.0f, 60.0f), white);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(35.0f, 90.0f), 25.0f, 20.0f), white);
    dc->DrawText(text1, (UINT32)wcslen(text1), fmt, D2D1::RectF(80.0f, 5.0f, 500.0f, 60.0f), white);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->DrawText(text2, (UINT32)wcslen(text2), fmt, D2D1::RectF(80.0f, 60.0f, 500.0f, 115.0f), white);
    dc->DrawBitmap(bmp, D2D1::RectF(10.0f, 120.0f, 138.0f, 248.0f), 1.0f,
                   D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    dc->DrawBitmap(bmp, D2D1::RectF(200.0f, 150.0f, 264.0f, 214.0f), 0.5f,
                   D2D1_INTERPOLATION_MODE_LINEAR);
    CHECK(dc->EndDraw(), "EndDraw");
    dc->SetTarget(nullptr);
    ctx->Flush();

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage = nullptr;
    CHECK(dev->CreateTexture2D(&td, nullptr, &stage), "CreateTexture2D (staging)");
    ctx->CopyResource(stage, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m), "Map (staging)");

    const unsigned char *px = (const unsigned char *)m.pData;
    unsigned bg = *(const unsigned *)(px + (size_t)(H - 2) * m.RowPitch + (W - 2) * 4);
    printf("background (bottom right) 0x%08x, want 0xff004080\n", bg);
    printf("pixel in the rectangle (30,30) 0x%08x, want 0xffffffff\n",
           *(const unsigned *)(px + (size_t)30 * m.RowPitch + 30 * 4));
    printf("pixel in the ellipse (35,90) 0x%08x, want 0xffffffff\n",
           *(const unsigned *)(px + (size_t)90 * m.RowPitch + 35 * 4));
    printf("pixel in the checker (14,124) 0x%08x, want 0xff00ff00; (30,124) 0x%08x, want 0xffff0000\n",
           *(const unsigned *)(px + (size_t)124 * m.RowPitch + 14 * 4),
           *(const unsigned *)(px + (size_t)124 * m.RowPitch + 30 * 4));
    printf("pixels that differ from the background:\n");
    printf("  rectangle    %6u of 2500\n", count_region(px, m.RowPitch, 10, 10, 60, 60, bg));
    printf("  ellipse      %6u (about 1570)\n", count_region(px, m.RowPitch, 5, 65, 65, 115, bg));
    printf("  text line 1  %6u\n", count_region(px, m.RowPitch, 80, 5, 500, 60, bg));
    printf("  text line 2  %6u\n", count_region(px, m.RowPitch, 80, 60, 500, 115, bg));
    printf("  checker      %6u of 16384\n", count_region(px, m.RowPitch, 10, 120, 138, 248, bg));
    printf("  half checker %6u of 4096\n", count_region(px, m.RowPitch, 200, 150, 264, 214, bg));

    FILE *f = fopen(out, "wb");
    if (f) {
        BITMAPFILEHEADER bf = {};
        BITMAPINFOHEADER bi = {};
        bf.bfType = 0x4d42;
        bf.bfOffBits = sizeof(bf) + sizeof(bi);
        bf.bfSize = bf.bfOffBits + W * H * 4;
        bi.biSize = sizeof(bi);
        bi.biWidth = W;
        bi.biHeight = -H;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        fwrite(&bf, sizeof(bf), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        for (int y = 0; y < H; y++)
            fwrite(px + (size_t)y * m.RowPitch, 4, W, f);
        fclose(f);
        printf("wrote %s\n", out);
    } else {
        printf("could not write %s\n", out);
    }
    ctx->Unmap(stage, 0);
    return 0;
}
