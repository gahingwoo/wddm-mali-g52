// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/scale.h: the source modes offered for a screen and the
 * Esmart0 values that scale one up to it.
 *
 * - The lists for the screens the driver meets: 2560x1440 HDMI with an
 *   EDID (a made-up one with the kinds of entries a monitor has, not the
 *   board's monitor's), 1920x1080 without one, the TaishanPi 3M's 800x1280
 *   portrait panel. Every entry fits (no larger than the screen, at most 8
 *   times smaller, even, not below 800x600), none twice, largest first.
 * - The scale factor against mainline's vop2_scale_factor
 *   (rockchip_drm_vop2.c), transcribed below, for every source and
 *   destination size up to 8x, and against what the factor is for: the
 *   last output pixel must sample inside the source.
 * - The window for each mode and scaling: inside the screen, centered,
 *   shape kept where asked, and register words checked by hand for the
 *   board's case (1920x1080 on 2560x1440).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/maliwddm/scale.h"

static int fails, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fails++;                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* mainline: DIV_ROUND_UP(src - 1 << shift, dst - 1) - 1, shift 16 up */
static unsigned MainlineFactor(unsigned src, unsigned dst)
{
    unsigned long long fac;

    if (src == dst)
        return 0;
    if (dst < 2)
        return 0xFFFF;
    if (src < 2)
        return 0;
    src--;
    dst--;
    fac = (((unsigned long long)src << 16) + dst - 1) / dst - 1;
    return fac > 0xFFFF ? 0xFFFF : (unsigned)fac;
}

/* An 18-byte detailed timing, progressive, active size only matters. */
static void Dtd(unsigned char *d, unsigned clk10k, unsigned ha, unsigned hb, unsigned va, unsigned vb)
{
    memset(d, 0, 18);
    d[0] = clk10k & 0xFF;
    d[1] = clk10k >> 8;
    d[2] = ha & 0xFF;
    d[3] = hb & 0xFF;
    d[4] = (ha >> 8) << 4 | (hb >> 8);
    d[5] = va & 0xFF;
    d[6] = vb & 0xFF;
    d[7] = (va >> 8) << 4 | (vb >> 8);
}

static unsigned MakeEdid(unsigned char *e)
{
    static const unsigned char hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    unsigned char s;

    memset(e, 0, 256);
    memcpy(e, hdr, 8);
    e[18] = 1;
    e[19] = 4;
    e[35] = 0x01;                       /* 800x600@60 */
    e[36] = 0x08 | 0x01;                /* 1024x768@60, 1280x1024@75 */
    for (int i = 38; i < 54; i++)
        e[i] = 1;                       /* unused standard timings */
    e[38] = 1920 / 8 - 31; e[39] = 3 << 6;      /* 1920x1080 16:9 */
    e[40] = 1680 / 8 - 31; e[41] = 0 << 6;      /* 1680x1050 16:10 */
    e[42] = 1280 / 8 - 31; e[43] = 2 << 6;      /* 1280x1024 5:4 */
    e[44] = 1440 / 8 - 31; e[45] = 0 << 6;      /* 1440x900 16:10 */
    Dtd(e + 54, 24170, 2560, 160, 1440, 41);    /* the native timing */
    Dtd(e + 72, 30213, 2560, 160, 1440, 41);    /* 75 Hz, same size */
    e[90 + 3] = 0xFC;                           /* a name descriptor: not a timing */
    e[108 + 3] = 0xFD;
    e[126] = 1;
    s = 0;
    for (int i = 0; i < 127; i++)
        s += e[i];
    e[127] = (unsigned char)(0x100 - s);
    /* CEA-861 block with one detailed timing, 1280x1440, and an interlaced
     * 1920x1080 that must not count. */
    e[128] = 0x02;
    e[129] = 3;
    e[130] = 4;
    Dtd(e + 132, 15600, 1280, 160, 1440, 41);
    Dtd(e + 150, 7425, 1920, 280, 540, 22);
    e[150 + 17] = 0x80;
    return 256;
}

static int Has(const MW_SCL_SIZE *L, unsigned n, unsigned w, unsigned h)
{
    for (unsigned i = 0; i < n; i++)
        if (L[i].W == w && L[i].H == h)
            return 1;
    return 0;
}

static void CheckList(const char *what, const MW_SCL_SIZE *L, unsigned n, unsigned sw, unsigned sh)
{
    CHECK(n >= 1 && L[0].W == sw && L[0].H == sh, "%s: the screen's size first", what);
    for (unsigned i = 1; i < n; i++) {
        unsigned w = L[i].W, h = L[i].H, lo = w < h ? w : h, hi = w < h ? h : w;
        CHECK(w <= sw && h <= sh && !(w == sw && h == sh), "%s: %ux%u not smaller", what, w, h);
        CHECK(w * 8 >= sw && h * 8 >= sh, "%s: %ux%u more than 8x", what, w, h);
        CHECK(!(w & 1) && !(h & 1), "%s: %ux%u odd", what, w, h);
        CHECK(lo >= 600 && hi >= 800, "%s: %ux%u below 800x600", what, w, h);
        for (unsigned j = 1; j < i; j++)
            CHECK(L[j].W != w || L[j].H != h, "%s: %ux%u twice", what, w, h);
        if (i >= 2)
            CHECK(L[i - 1].W > w || (L[i - 1].W == w && L[i - 1].H > h), "%s: order at %u", what, i);
    }
}

static void CheckWindow(unsigned w, unsigned h, unsigned sw, unsigned sh, unsigned how)
{
    MW_SCL_WIN o;
    int ok = MwSclWindow(w, h, sw, sh, how, &o);

    CHECK(ok, "%ux%u on %ux%u how %u refused", w, h, sw, sh, how);
    if (!ok)
        return;
    CHECK(o.X + o.W <= sw && o.Y + o.H <= sh, "%ux%u how %u: outside the screen", w, h, how);
    CHECK(o.W >= w && o.H >= h, "%ux%u how %u: scales down", w, h, how);
    CHECK(o.X == (sw - o.W) / 2 && o.Y == (sh - o.H) / 2, "%ux%u how %u: not centered", w, h, how);
    CHECK(o.ActInfo == ((h - 1) << 16 | (w - 1)), "ACT_INFO");
    CHECK(o.DspInfo == ((o.H - 1) << 16 | (o.W - 1)), "DSP_INFO");
    CHECK(o.DspSt == (o.Y << 16 | o.X), "DSP_ST");
    if (how == MW_SCL_STRETCHED || (w == sw && h == sh))
        CHECK(o.W == sw && o.H == sh, "%ux%u stretched: not the whole screen", w, h);
    if (how == MW_SCL_CENTERED)
        CHECK(o.W == w && o.H == h, "%ux%u centered: scaled", w, h);
    if (how == MW_SCL_ASPECT) {
        /* one side is the screen's, and the shape is kept to a pixel or two */
        CHECK(o.W == sw || o.H == sh, "%ux%u aspect: neither side full", w, h);
        if (!MwSclSameShape(w, h, sw, sh)) {
            double a = (double)w / h, b = (double)o.W / o.H;
            CHECK(a / b < 1.004 && b / a < 1.004, "%ux%u aspect: shape %f vs %f", w, h, a, b);
        }
    }
    /* the scaler: every output pixel samples inside the source */
    {
        unsigned fx = o.SclFactor & 0xFFFF, fy = o.SclFactor >> 16;
        if (o.W > w)
            CHECK(((unsigned long long)(o.W - 1) * fx >> 16) < w - 1 + 1, "%ux%u: x past the source", w, h);
        if (o.H > h)
            CHECK(((unsigned long long)(o.H - 1) * fy >> 16) < h - 1 + 1, "%ux%u: y past the source", w, h);
        CHECK((o.W == w) == (fx == 0) && (o.H == h) == (fy == 0), "%ux%u: factor for no scaling", w, h);
        CHECK(((o.SclCtrl & 3) == (o.W > w ? 1u : 0u)) && (((o.SclCtrl >> 4) & 3) == (o.H > h ? 1u : 0u)),
              "%ux%u: SCL_CTRL modes 0x%x", w, h, o.SclCtrl);
    }
}

int main(void)
{
    unsigned char edid[256];
    unsigned elen = MakeEdid(edid);
    MW_SCL_SIZE L[MW_SCL_MAX_MODES];
    unsigned n;
    MW_SCL_WIN o;

    /* 2560x1440 with an EDID, as the board's HDMI boot. */
    n = MwSclModes(2560, 1440, edid, elen, 1, L, MW_SCL_MAX_MODES);
    printf("2560x1440, EDID:");
    for (unsigned i = 0; i < n; i++)
        printf(" %ux%u", L[i].W, L[i].H);
    printf("\n");
    CheckList("2560x1440", L, n, 2560, 1440);
    CHECK(Has(L, n, 1920, 1080) && Has(L, n, 1600, 900) && Has(L, n, 1280, 720) && Has(L, n, 1366, 768),
          "16:9 sizes missing");
    CHECK(Has(L, n, 1680, 1050) && Has(L, n, 1440, 900) && Has(L, n, 1280, 1024) && Has(L, n, 1024, 768) &&
          Has(L, n, 800, 600), "EDID and other shapes missing");
    CHECK(Has(L, n, 1280, 1440), "the CEA block's detailed timing missing");
    CHECK(!Has(L, n, 1920, 540), "an interlaced timing counted");
    CHECK(!Has(L, n, 3840, 2160) && !Has(L, n, 2560, 1600), "larger than the screen");
    CHECK(n <= MW_SCL_MAX_MODES, "too many");

    n = MwSclModes(2560, 1440, edid, elen, 0, L, MW_SCL_MAX_MODES);
    CHECK(n == 1 && L[0].W == 2560 && L[0].H == 1440, "ScaledModes 0: not the screen alone");

    /* 1920x1080, no EDID. */
    n = MwSclModes(1920, 1080, NULL, 0, 1, L, MW_SCL_MAX_MODES);
    CheckList("1920x1080", L, n, 1920, 1080);
    CHECK(Has(L, n, 1600, 900) && Has(L, n, 1280, 720) && Has(L, n, 1024, 768) && !Has(L, n, 1920, 1200),
          "1920x1080 list");

    /* The TaishanPi 3M's panel, portrait. */
    n = MwSclModes(800, 1280, NULL, 0, 1, L, MW_SCL_MAX_MODES);
    printf("800x1280:");
    for (unsigned i = 0; i < n; i++)
        printf(" %ux%u", L[i].W, L[i].H);
    printf("\n");
    CheckList("800x1280", L, n, 800, 1280);
    CHECK(Has(L, n, 720, 1280) && Has(L, n, 768, 1024) && Has(L, n, 600, 800), "portrait sizes missing");
    CHECK(!Has(L, n, 1280, 720), "a landscape size on a portrait screen");

    /* The factor, against mainline's, for every pair up to 8x, and in range. */
    for (unsigned src = 2; src <= 640; src++)
        for (unsigned dst = src; dst <= src * 8 && dst <= 2600; dst += (dst < 700 ? 1 : 7)) {
            unsigned f = MwSclFactor(src, dst);
            CHECK(f == MainlineFactor(src, dst), "factor %u->%u: %u vs mainline %u", src, dst, f,
                  MainlineFactor(src, dst));
            if (dst > src)
                CHECK(((unsigned long long)f * (dst - 1) >> 16) < src - 1, "factor %u->%u past the end", src, dst);
        }
    CHECK(MwSclFactor(1920, 2560) == MainlineFactor(1920, 2560) && MwSclFactor(1080, 1440) == MainlineFactor(1080, 1440),
          "board factors");

    /* By hand: 1920x1080 stretched on 2560x1440. */
    CHECK(MwSclWindow(1920, 1080, 2560, 1440, MW_SCL_STRETCHED, &o), "1920x1080");
    CHECK(o.ActInfo == 0x0437077F && o.DspInfo == 0x059F09FF && o.DspSt == 0, "1920x1080 sizes 0x%x 0x%x",
          o.ActInfo, o.DspInfo);
    CHECK(o.SclCtrl == 0x59, "1920x1080 SCL_CTRL 0x%x", o.SclCtrl);
    CHECK((o.SclFactor & 0xFFFF) == 0xBFF9 && (o.SclFactor >> 16) == 0xBFF4, "1920x1080 SCL_FACTOR 0x%08x",
          o.SclFactor);
    /* 1280x1024 kept 5:4: 1800x1440 at x 380. */
    CHECK(MwSclWindow(1280, 1024, 2560, 1440, MW_SCL_ASPECT, &o) && o.W == 1800 && o.H == 1440 && o.X == 380 &&
          o.Y == 0, "1280x1024 aspect %ux%u at %u,%u", o.W, o.H, o.X, o.Y);
    /* 2560x1080: no scaling at all, bars above and below. */
    CHECK(MwSclWindow(2560, 1080, 2560, 1440, MW_SCL_ASPECT, &o) && o.W == 2560 && o.H == 1080 && o.Y == 180 &&
          o.SclCtrl == 0 && o.SclFactor == 0, "2560x1080 aspect");
    /* the screen's own size: the firmware's window */
    CHECK(MwSclWindow(2560, 1440, 2560, 1440, MW_SCL_IDENTITY, &o) && o.ActInfo == o.DspInfo && o.DspSt == 0 &&
          o.SclCtrl == 0 && o.SclFactor == 0, "identity");
    CHECK(!MwSclWindow(1920, 1080, 2560, 1440, MW_SCL_IDENTITY, &o), "identity for a smaller mode");
    CHECK(!MwSclWindow(2600, 1440, 2560, 1440, MW_SCL_STRETCHED, &o), "larger than the screen");
    CHECK(!MwSclWindow(300, 1440, 2560, 1440, MW_SCL_STRETCHED, &o), "more than 8x");
    CHECK(MwSclDefault(2560, 1440, 2560, 1440) == MW_SCL_IDENTITY && MwSclDefault(1920, 1080, 2560, 1440) == MW_SCL_STRETCHED &&
          MwSclDefault(1366, 768, 2560, 1440) == MW_SCL_STRETCHED && MwSclDefault(1280, 1024, 2560, 1440) == MW_SCL_ASPECT,
          "default scaling");

    /* Every listed mode, every scaling, on every screen. */
    {
        static const unsigned screens[][2] = { { 2560, 1440 }, { 1920, 1080 }, { 3840, 2160 }, { 1920, 1200 },
                                               { 800, 1280 }, { 1280, 1024 } };
        for (unsigned s = 0; s < sizeof(screens) / sizeof(screens[0]); s++) {
            unsigned sw = screens[s][0], sh = screens[s][1];
            n = MwSclModes(sw, sh, edid, elen, 1, L, MW_SCL_MAX_MODES);
            CheckList("screen", L, n, sw, sh);
            for (unsigned i = 0; i < n; i++) {
                if (i == 0) {
                    CheckWindow(L[i].W, L[i].H, sw, sh, MW_SCL_IDENTITY);
                    continue;
                }
                CheckWindow(L[i].W, L[i].H, sw, sh, MW_SCL_CENTERED);
                CheckWindow(L[i].W, L[i].H, sw, sh, MW_SCL_STRETCHED);
                CheckWindow(L[i].W, L[i].H, sw, sh, MW_SCL_ASPECT);
                CheckWindow(L[i].W, L[i].H, sw, sh, MwSclDefault(L[i].W, L[i].H, sw, sh));
            }
        }
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
