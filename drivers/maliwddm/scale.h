/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Source modes smaller than the screen, and the VOP2 window that scales
 * one up to it (docs/DISPLAY-MODES.md). The arithmetic side only:
 *
 * - MwSclModes: the list of source modes Windows is offered for a screen of
 *   W x H, the screen's own size first, then the sizes the monitor's EDID
 *   names and the usual desktop sizes, each no larger than the screen in
 *   either direction and no more than 8 times smaller (the Esmart window's
 *   upscale limit on the RK3576, max_upscale_factor 8 in both mainline and
 *   the vendor kernel's rk3576_vop_win_data).
 * - MwSclWindow: for a source mode, the screen and a scaling (centered,
 *   stretched, aspect ratio kept), Esmart0's register values: ACT_INFO (the
 *   size read), DSP_INFO and DSP_ST (where it is shown), SCL_FACTOR_YRGB and
 *   SCL_CTRL. The factor is the vendor kernel's vop3_scale_factor for
 *   scale-up, the filters its rk3576 Esmart0 ones (bicubic across, bilinear
 *   down the screen). Only scale-up and no scaling occur: no source mode is
 *   larger than the screen, so the gt2/gt4 bits in REGION0_CTRL stay 0 and
 *   that register (which also holds the window's enable and format) is never
 *   written.
 *
 * tests/modes-host checks this file. Plain C with no WDK header.
 */
#pragma once

#ifndef MW_SCL_INLINE
#ifdef _MSC_VER
#define MW_SCL_INLINE static __inline
#else
#define MW_SCL_INLINE static inline
#endif
#endif

#define MW_SCL_MAX_MODES 32

/* How a smaller source fills the screen; the values are this file's, the
 * D3DKMDT_VIDPN_PRESENT_PATH_SCALING ones are mapped in display.c. */
#define MW_SCL_IDENTITY  0      /* the screen's own size, no scaling */
#define MW_SCL_CENTERED  1      /* one to one, in the middle, black around */
#define MW_SCL_STRETCHED 2      /* to the whole screen */
#define MW_SCL_ASPECT    3      /* as large as it goes with its shape kept, in the middle */

/* Esmart0 (VOP2 base 0x1800), REGION0, mainline rockchip_drm_vop2.h. */
#define MW_ESMART0_YRGB_MST     0x1814u
#define MW_ESMART0_VIR          0x181Cu
#define MW_ESMART0_ACT_INFO     0x1820u
#define MW_ESMART0_DSP_INFO     0x1824u
#define MW_ESMART0_DSP_ST       0x1828u
#define MW_ESMART0_SCL_CTRL     0x1830u
#define MW_ESMART0_SCL_FACTOR   0x1834u

/* SCL_CTRL fields: YRGB hor mode [1:0], hor filter [3:2], ver mode [5:4],
 * ver filter [7:6]. Modes: 0 none, 1 up, 2 down. Filters: up 1 bilinear,
 * 2 bicubic; down 1 bilinear. */
#define MW_SCL_MODE_NONE 0u
#define MW_SCL_MODE_UP   1u
#define MW_SCL_FILTER_BIL 1u
#define MW_SCL_FILTER_BIC 2u

typedef struct _MW_SCL_SIZE {
    unsigned short W, H;
} MW_SCL_SIZE;

typedef struct _MW_SCL_WIN {
    unsigned X, Y, W, H;                /* where the picture is on the screen */
    unsigned ActInfo, DspInfo, DspSt;
    unsigned SclFactor, SclCtrl;
} MW_SCL_WIN;

/* Of the same shape as the screen, to within 1% (1366x768 and 1360x768 on
 * a 16:9 screen are): stretched then keeps the aspect ratio near enough,
 * and is what Windows is offered for it. */
MW_SCL_INLINE int MwSclSameShape(unsigned W, unsigned H, unsigned Sw, unsigned Sh)
{
    unsigned long long a = (unsigned long long)W * Sh, b = (unsigned long long)H * Sw;
    unsigned long long d = a > b ? a - b : b - a;

    return d * 100 <= b;
}

/* The scaling a source mode is offered with: identity for the screen's
 * size, stretched for its shape, aspect-ratio-kept for another shape. */
MW_SCL_INLINE unsigned MwSclDefault(unsigned W, unsigned H, unsigned Sw, unsigned Sh)
{
    if (W == Sw && H == Sh)
        return MW_SCL_IDENTITY;
    return MwSclSameShape(W, H, Sw, Sh) ? MW_SCL_STRETCHED : MW_SCL_ASPECT;
}

/* Whether a source mode of W x H can be shown on the screen this way. */
MW_SCL_INLINE int MwSclFits(unsigned W, unsigned H, unsigned Sw, unsigned Sh)
{
    return W >= 2 && H >= 2 && W <= Sw && H <= Sh && W * 8 >= Sw && H * 8 >= Sh &&
           W <= 0xFFFF && H <= 0xFFFF;
}

/* The vendor kernel's vop3_scale_factor for SCALE_UP (and 0 for none):
 * floor((src - 1) << 16 / (dst - 1)), lowered until fac * (dst - 1) >> 16
 * is below src - 1, so that the last output pixel never reads past the last
 * input pixel. */
MW_SCL_INLINE unsigned MwSclFactor(unsigned Src, unsigned Dst)
{
    unsigned long long fac;

    if (Src >= Dst)
        return 0;
    if (Src == 1) {             /* its zero-divide workaround */
        Src++;
        Dst++;
    }
    fac = ((unsigned long long)(Src - 1) << 16) / (Dst - 1);
    for (int i = 0; i < 100 && fac > 0; i++) {
        if ((fac * (Dst - 1) >> 16) < Src - 1)
            break;
        fac--;
    }
    return fac > 0xFFFF ? 0xFFFF : (unsigned)fac;
}

/* Esmart0's values for a W x H source on an Sw x Sh screen. 0 if it cannot
 * be shown (too large, more than 8 times smaller). Identity, or a source of
 * the screen's size, is the window as the firmware set it: everything at
 * the screen's size and SCL_CTRL, SCL_FACTOR 0. */
MW_SCL_INLINE int MwSclWindow(unsigned W, unsigned H, unsigned Sw, unsigned Sh, unsigned How, MW_SCL_WIN *O)
{
    unsigned dw, dh, hm, vm;

    if (!MwSclFits(W, H, Sw, Sh))
        return 0;
    if (W == Sw && H == Sh)
        How = MW_SCL_IDENTITY;
    switch (How) {
    case MW_SCL_IDENTITY:
        if (W != Sw || H != Sh)
            return 0;
        dw = Sw;
        dh = Sh;
        break;
    case MW_SCL_CENTERED:
        dw = W;
        dh = H;
        break;
    case MW_SCL_STRETCHED:
        dw = Sw;
        dh = Sh;
        break;
    case MW_SCL_ASPECT:
        if (MwSclSameShape(W, H, Sw, Sh)) {
            dw = Sw;
            dh = Sh;
        } else if ((unsigned long long)W * Sh > (unsigned long long)H * Sw) {
            /* wider than the screen: full width, bars above and below */
            dw = Sw;
            dh = (unsigned)(((unsigned long long)H * Sw + W / 2) / W);
        } else {
            /* narrower: full height, bars left and right */
            dh = Sh;
            dw = (unsigned)(((unsigned long long)W * Sh + H / 2) / H);
        }
        /* even sizes, never smaller than the source (scale-up only) */
        if (dw < Sw) dw &= ~1u;
        if (dh < Sh) dh &= ~1u;
        if (dw < W) dw = W;
        if (dh < H) dh = H;
        break;
    default:
        return 0;
    }
    O->W = dw;
    O->H = dh;
    O->X = (Sw - dw) / 2;
    O->Y = (Sh - dh) / 2;
    O->ActInfo = (H - 1) << 16 | ((W - 1) & 0xFFFF);
    O->DspInfo = (dh - 1) << 16 | ((dw - 1) & 0xFFFF);
    O->DspSt = O->Y << 16 | (O->X & 0xFFFF);
    O->SclFactor = MwSclFactor(H, dh) << 16 | MwSclFactor(W, dw);
    hm = W < dw ? MW_SCL_MODE_UP : MW_SCL_MODE_NONE;
    vm = H < dh ? MW_SCL_MODE_UP : MW_SCL_MODE_NONE;
    /* As the vendor kernel: the up filter when scaling up, the down one
     * (bilinear) otherwise; a window not scaled at all is left as the
     * firmware has it, all zero. */
    if (hm == MW_SCL_MODE_NONE && vm == MW_SCL_MODE_NONE)
        O->SclCtrl = 0;
    else
        O->SclCtrl = hm | (hm == MW_SCL_MODE_UP ? MW_SCL_FILTER_BIC : MW_SCL_FILTER_BIL) << 2 |
                     vm << 4 | MW_SCL_FILTER_BIL << 6;
    return 1;
}

/* ---- the list ---- */


MW_SCL_INLINE void MwSclAdd(MW_SCL_SIZE *L, unsigned *N, unsigned Max, unsigned W, unsigned H,
                            unsigned Sw, unsigned Sh)
{
    unsigned lo = W < H ? W : H, hi = W < H ? H : W;

    /* Not below 800x600 (600x800 on a portrait screen), even widths, and
     * no more than 8 times smaller. */
    if (!MwSclFits(W, H, Sw, Sh) || lo < 600 || hi < 800 || (W & 1) || (H & 1))
        return;
    for (unsigned i = 0; i < *N; i++)
        if (L[i].W == W && L[i].H == H)
            return;
    if (*N >= Max)
        return;
    L[*N].W = (unsigned short)W;
    L[*N].H = (unsigned short)H;
    (*N)++;
}

/* The detailed timings of an 18-byte descriptor list: their active size,
 * progressive only. */
MW_SCL_INLINE void MwSclAddDtds(const unsigned char *E, unsigned From, unsigned To, MW_SCL_SIZE *L,
                                unsigned *N, unsigned Max, unsigned Sw, unsigned Sh)
{
    for (unsigned off = From; off + 18 <= To; off += 18) {
        const unsigned char *d = E + off;
        unsigned clk = d[0] | (unsigned)d[1] << 8;
        unsigned ha = d[2] | (unsigned)(d[4] & 0xF0) << 4;
        unsigned va = d[5] | (unsigned)(d[7] & 0xF0) << 4;

        if (clk == 0)
            continue;           /* a display descriptor, not a timing */
        if (d[17] & 0x80)
            continue;           /* interlaced */
        MwSclAdd(L, N, Max, ha, va, Sw, Sh);
    }
}

/* Every size the EDID names: established timings (byte 35 to 37), standard
 * timings (38 to 53), detailed timings in the base block and in CEA-861
 * extension blocks. Refresh rates do not matter: the screen keeps its
 * timing whatever the source mode. A portrait screen (a panel turned on its
 * side) gets each size turned too. */
MW_SCL_INLINE void MwSclAddEdid(const unsigned char *E, unsigned Len, MW_SCL_SIZE *L, unsigned *N,
                                unsigned Max, unsigned Sw, unsigned Sh)
{
    static const struct { unsigned char Byte, Bit; unsigned short W, H; } est[] = {
        { 35, 0x01, 800, 600 }, { 35, 0x02, 800, 600 }, { 36, 0x80, 800, 600 }, { 36, 0x40, 800, 600 },
        { 36, 0x08, 1024, 768 }, { 36, 0x04, 1024, 768 }, { 36, 0x02, 1024, 768 },
        { 36, 0x01, 1280, 1024 }, { 37, 0x80, 1152, 870 },
    };
    int portrait = Sh > Sw;

    if (E == 0 || Len < 128)
        return;
    for (unsigned i = 0; i < sizeof(est) / sizeof(est[0]); i++)
        if (E[est[i].Byte] & est[i].Bit) {
            if (portrait)
                MwSclAdd(L, N, Max, est[i].H, est[i].W, Sw, Sh);
            else
                MwSclAdd(L, N, Max, est[i].W, est[i].H, Sw, Sh);
        }
    for (unsigned off = 38; off < 54; off += 2) {
        unsigned b0 = E[off], b1 = E[off + 1], w, h;

        if ((b0 == 1 && b1 == 1) || b0 == 0)
            continue;           /* unused */
        w = (b0 + 31) * 8;
        switch (b1 >> 6) {
        case 0:  h = E[18] == 1 && E[19] < 3 ? w : w * 10 / 16; break;     /* 1:1 before EDID 1.3 */
        case 1:  h = w * 3 / 4; break;
        case 2:  h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        if (portrait)
            MwSclAdd(L, N, Max, h, w, Sw, Sh);
        else
            MwSclAdd(L, N, Max, w, h, Sw, Sh);
    }
    MwSclAddDtds(E, 54, 126, L, N, Max, Sw, Sh);
    for (unsigned b = 1; (b + 1) * 128 <= Len && b <= E[126]; b++) {
        const unsigned char *x = E + b * 128;
        unsigned d = x[2];

        if (x[0] != 0x02 || d < 4 || d > 127)
            continue;           /* not CEA-861, or no detailed timings */
        MwSclAddDtds(x, d, 127, L, N, Max, Sw, Sh);
    }
}

/* The source modes for an Sw x Sh screen: the screen's size first, then the
 * rest from the largest down. Scaled 0: the screen's size alone. Returns
 * the count (at least 1). */
MW_SCL_INLINE unsigned MwSclModes(unsigned Sw, unsigned Sh, const unsigned char *Edid, unsigned EdidLen,
                                  int Scaled, MW_SCL_SIZE *L, unsigned Max)
{
    /* the usual desktop sizes, landscape; for a portrait screen they are turned */
    static const MW_SCL_SIZE common[] = {
        { 3840, 2160 }, { 3440, 1440 }, { 3200, 1800 }, { 2560, 1600 }, { 2560, 1440 },
        { 2560, 1080 }, { 2048, 1536 }, { 1920, 1440 }, { 1920, 1200 }, { 1920, 1080 },
        { 1680, 1050 }, { 1600, 1200 }, { 1600, 1024 }, { 1600, 900 },  { 1440, 900 },
        { 1366, 768 },  { 1360, 768 },  { 1280, 1024 }, { 1280, 960 },  { 1280, 800 },
        { 1280, 768 },  { 1280, 720 },  { 1152, 864 },  { 1024, 768 },  { 800, 600 },
    };
    unsigned n = 0;

    if (Max == 0)
        return 0;
    L[0].W = (unsigned short)Sw;
    L[0].H = (unsigned short)Sh;
    n = 1;
    if (!Scaled)
        return n;
    MwSclAddEdid(Edid, EdidLen, L, &n, Max, Sw, Sh);
    for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
        if (Sh > Sw)
            MwSclAdd(L, &n, Max, common[i].H, common[i].W, Sw, Sh);
        else
            MwSclAdd(L, &n, Max, common[i].W, common[i].H, Sw, Sh);
    }
    /* largest first (by width, then height), the screen's size staying first */
    for (unsigned i = 2; i < n; i++)
        for (unsigned j = i; j > 1; j--) {
            MW_SCL_SIZE a = L[j - 1], b = L[j];
            if (a.W > b.W || (a.W == b.W && a.H >= b.H))
                break;
            L[j - 1] = b;
            L[j] = a;
        }
    return n;
}
