/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HDMI on the RK3576, the arithmetic: no register is touched here.
 *
 * Everything hdmi.c needs to compute before it writes anything: the timings
 * a monitor's EDID offers and what the sink is (HDMI, SCDC, basic audio),
 * the HDPTX PHY's ROPLL settings for a TMDS rate, the video port's timing
 * registers, the AVI and vendor InfoFrames as the controller's packet RAM
 * wants them, and the audio clock regeneration value.
 *
 * Every number comes from the firmware that lights this board, edk2-rk3576:
 *
 *   EDID         Drivers/LcdGraphicsOutputDxe/Edid.c (detailed timings,
 *                CEA data blocks)
 *   PHY          Library/DisplayLib/PhyRockchipSamsungHdptxHdmi.c
 *                (ROPLL_TMDS_CONFIG, HdptxPhyClkPllCalc)
 *   video port   Drivers/Vop2Dxe/Vop2Dxe.c (Vop2Init, Vop2IfConfig,
 *                Vop2PostConfig) and Vop2Rk3576.c
 *   InfoFrames   Library/DisplayLib/DwHdmiQpLib.c (HdmiConfigAviInfoframe,
 *                HdmiConfigVendorInfoframe)
 *   audio        Library/DisplayLib/DwHdmiQpAudio.c
 *
 * tests/hdmi-host checks it against values the firmware is known to produce
 * on this board (1920x1080@60: IF_CTRL 0x80000033, VP0_CLK_CTRL 1, the
 * 1485000 ROPLL entry, CTS 148500).
 *
 * Plain C with no WDK header, so that the host test can build it.
 */
#pragma once

#ifndef MW_HC_INLINE
#ifdef _MSC_VER
#define MW_HC_INLINE static __inline
#else
#define MW_HC_INLINE static inline
#endif
#endif

typedef unsigned char  mwh_u8;
typedef unsigned short mwh_u16;
typedef unsigned int   mwh_u32;

/* ---- a display timing ----
 *
 * The DRM layout DisplayModeToDrm() produces: sync start and end counted
 * from the start of the active line. Clock in kHz. Fw: the firmware's own
 * timing, of which only the active size, the totals and the clock are known
 * (display.c's TimingFromEdid or TimingMadeUp); it is never programmed, it
 * only says "what is on the wire now". */
typedef struct _MW_TIMING {
    mwh_u32 Clock;
    mwh_u16 HDisplay, HSyncStart, HSyncEnd, HTotal;
    mwh_u16 VDisplay, VSyncStart, VSyncEnd, VTotal;
    mwh_u8  PHSync, PVSync;     /* 1: positive sync */
    mwh_u8  Vic;                /* CEA-861 VIC, 0 none */
    mwh_u8  Fw;
} MW_TIMING;

/* The TMDS character rate the firmware will drive: 8 bits per component,
 * no scrambling (DwHdmiQpSetup writes SCRAMB_CONFIG0 0 and SCDC TMDS_CONFIG
 * 0), so 340 MHz is the ceiling (memory: project_rk3576_hdmi_340mhz_cap). */
#define MW_HDMI_MAX_CLOCK_KHZ   340000u
#define MW_HDMI_MIN_CLOCK_KHZ   25000u
#define MW_VP0_MAX_W            4096u
#define MW_VP0_MAX_H            2304u

MW_HC_INLINE int MwTimingEqual(const MW_TIMING *A, const MW_TIMING *B)
{
    return A->Clock == B->Clock && A->HDisplay == B->HDisplay && A->VDisplay == B->VDisplay &&
           A->HTotal == B->HTotal && A->VTotal == B->VTotal;
}

/* ---- CEA-861 VICs the firmware can be asked for ----
 *
 * The AVI InfoFrame carries the VIC of the mode (LcdGraphicsOutputDxe sets
 * ConnectorState->DisplayModeVic from the EDID's mode list). Only modes at
 * or below 340 MHz, progressive. 93..95 are 4K at 24/25/30 Hz, which the
 * firmware sends as HDMI VICs 3..1 in the vendor InfoFrame instead. */
typedef struct _MW_CEA_MODE {
    mwh_u8 Vic;
    mwh_u32 Clock;
    mwh_u16 H, HSs, HSe, Ht, V, VSs, VSe, Vt;
    mwh_u8 Pos;                 /* both syncs positive */
} MW_CEA_MODE;

static const MW_CEA_MODE MwCeaModes[] = {
    {  1,  25175,  640,  656,  752,  800,  480,  490,  492,  525, 0 },
    {  2,  27000,  720,  736,  798,  858,  480,  489,  495,  525, 0 },
    {  4,  74250, 1280, 1390, 1430, 1650,  720,  725,  730,  750, 1 },
    { 16, 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1 },
    { 17,  27000,  720,  732,  796,  864,  576,  581,  586,  625, 0 },
    { 19,  74250, 1280, 1720, 1760, 1980,  720,  725,  730,  750, 1 },
    { 31, 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, 1 },
    { 32,  74250, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, 1 },
    { 33,  74250, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, 1 },
    { 34,  74250, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1 },
    { 93, 297000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, 1 },
    { 94, 297000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, 1 },
    { 95, 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 1 },
};

MW_HC_INLINE void MwCeaTiming(const MW_CEA_MODE *C, MW_TIMING *T)
{
    T->Clock = C->Clock;
    T->HDisplay = C->H; T->HSyncStart = C->HSs; T->HSyncEnd = C->HSe; T->HTotal = C->Ht;
    T->VDisplay = C->V; T->VSyncStart = C->VSs; T->VSyncEnd = C->VSe; T->VTotal = C->Vt;
    T->PHSync = C->Pos; T->PVSync = C->Pos;
    T->Vic = C->Vic;
    T->Fw = 0;
}

/* The VIC of a timing, if it is one of the table's (a DTD of a CEA mode
 * carries no VIC; the firmware gets it from the SVD it matched). The clock
 * may differ by the 1000/1001 of a 59.94 Hz variant. */
MW_HC_INLINE mwh_u8 MwCeaVic(const MW_TIMING *T)
{
    for (unsigned i = 0; i < sizeof(MwCeaModes) / sizeof(MwCeaModes[0]); i++) {
        const MW_CEA_MODE *c = &MwCeaModes[i];
        mwh_u32 lo = c->Clock - c->Clock / 900;          /* 1000/1001 is 0.1 % */
        if (c->H == T->HDisplay && c->Ht == T->HTotal && c->HSs == T->HSyncStart &&
            c->HSe == T->HSyncEnd && c->V == T->VDisplay && c->Vt == T->VTotal &&
            c->VSs == T->VSyncStart && c->VSe == T->VSyncEnd &&
            T->Clock >= lo && T->Clock <= c->Clock)
            return c->Vic;
    }
    return 0;
}

/* ---- EDID ---- */

/* One 18-byte detailed timing descriptor, as EdidDetailedTimingToDisplayMode
 * reads it: refused when it is not a timing, interlaced, stereo, or its sync
 * does not fit in its blanking. */
MW_HC_INLINE int MwEdidDtd(const mwh_u8 *D, MW_TIMING *T)
{
    mwh_u32 clk = (mwh_u32)D[0] | (mwh_u32)D[1] << 8;
    mwh_u32 ha = D[2] | (mwh_u32)(D[4] & 0xF0) << 4, hb = D[3] | (mwh_u32)(D[4] & 0x0F) << 8;
    mwh_u32 va = D[5] | (mwh_u32)(D[7] & 0xF0) << 4, vb = D[6] | (mwh_u32)(D[7] & 0x0F) << 8;
    mwh_u32 hfp = D[8] | (mwh_u32)(D[11] & 0xC0) << 2;
    mwh_u32 hsw = D[9] | (mwh_u32)(D[11] & 0x30) << 4;
    mwh_u32 vfp = (mwh_u32)(D[10] >> 4) | (mwh_u32)(D[11] & 0x0C) << 2;
    mwh_u32 vsw = (mwh_u32)(D[10] & 0x0F) | (mwh_u32)(D[11] & 0x03) << 4;
    mwh_u8 f = D[17];

    if (clk == 0 || (f & 0x80) || (f & 0x61) || ha == 0 || va == 0)
        return 0;
    if (hfp + hsw >= hb || vfp + vsw >= vb)
        return 0;
    T->Clock = clk * 10;
    T->HDisplay = (mwh_u16)ha;
    T->HSyncStart = (mwh_u16)(ha + hfp);
    T->HSyncEnd = (mwh_u16)(ha + hfp + hsw);
    T->HTotal = (mwh_u16)(ha + hb);
    T->VDisplay = (mwh_u16)va;
    T->VSyncStart = (mwh_u16)(va + vfp);
    T->VSyncEnd = (mwh_u16)(va + vfp + vsw);
    T->VTotal = (mwh_u16)(va + vb);
    T->PHSync = (mwh_u8)((f & 0x02) ? 1 : 0);
    T->PVSync = (mwh_u8)((f & 0x04) ? 1 : 0);
    T->Fw = 0;
    T->Vic = MwCeaVic(T);
    return 1;
}

/* What the sink is, from its CEA-861 extensions (EdidGetFeaturesCea). */
typedef struct _MW_SINK {
    mwh_u8 IsHdmi;          /* HDMI VSDB, OUI 00-0C-03 */
    mwh_u8 Scdc;            /* HF-SCDB, OUI C4-5D-D8, SCDC_Present */
    mwh_u8 BasicAudio;      /* CEA byte 3 bit 6 (DwHdmiQpAudioSinkHasBasicAudio) */
    mwh_u8 RgbQs;           /* video capability block, QS: selectable RGB range */
} MW_SINK;

MW_HC_INLINE void MwEdidSink(const mwh_u8 *E, mwh_u32 Len, MW_SINK *S)
{
    S->IsHdmi = S->Scdc = S->BasicAudio = S->RgbQs = 0;
    for (mwh_u32 b = 1; (b + 1) * 128 <= Len && b <= 4; b++) {
        const mwh_u8 *x = E + b * 128;
        mwh_u32 end = x[2], off;
        if (x[0] != 0x02)
            continue;
        if (x[1] >= 2 && (x[3] & 0x40))
            S->BasicAudio = 1;
        if (x[1] < 3 || end > 127)
            continue;
        for (off = 4; off < end;) {
            const mwh_u8 *d = x + off;
            mwh_u32 len = 1u + (d[0] & 0x1F), tag = d[0] >> 5;
            if (off + len > end)
                break;
            if (tag == 7 && len >= 3 && d[1] == 0x00 && (d[2] & 0x40))
                S->RgbQs = 1;                               /* VCDB: extended tag 0 */
            if (tag == 3 && len >= 4 && d[1] == 0x03 && d[2] == 0x0C && d[3] == 0x00)
                S->IsHdmi = 1;
            if (tag == 3 && len >= 7 && d[1] == 0xD8 && d[2] == 0x5D && d[3] == 0xC4 && (d[6] & 0x80))
                S->Scdc = 1;
            off += len;
        }
    }
}

/* Can the PHY make this TMDS rate? Defined below. */
MW_HC_INLINE int MwHdptxCanDo(mwh_u32 BitRateKbps);

/* Is this a timing hdmi.c may program: progressive, 8 bpc at or below
 * 340 MHz, within VP0's limits and the reserved framebuffer, a 4-pixel
 * aligned width (Vop2ModeFixup would otherwise widen the active line past
 * the primary), and a TMDS rate the PHY can lock to. */
MW_HC_INLINE int MwTimingUsable(const MW_TIMING *T, mwh_u32 MaxPixels)
{
    if (T->Clock < MW_HDMI_MIN_CLOCK_KHZ || T->Clock > MW_HDMI_MAX_CLOCK_KHZ)
        return 0;
    if (T->HDisplay == 0 || T->VDisplay == 0 || T->HDisplay > MW_VP0_MAX_W || T->VDisplay > MW_VP0_MAX_H)
        return 0;
    if ((T->HDisplay & 3) != 0 || (mwh_u32)T->HDisplay * T->VDisplay > MaxPixels)
        return 0;
    if (!(T->HSyncStart > T->HDisplay && T->HSyncEnd > T->HSyncStart && T->HTotal > T->HSyncEnd))
        return 0;
    if (!(T->VSyncStart > T->VDisplay && T->VSyncEnd > T->VSyncStart && T->VTotal > T->VSyncEnd))
        return 0;
    return MwHdptxCanDo(T->Clock * 10);
}

/* The modes to offer: the EDID's detailed timings in order (the first is the
 * monitor's preferred mode, which is what the firmware picks), then the
 * safe set, 1920x1080@60 and 1280x720@60, which every HDMI sink in practice
 * takes and which the firmware falls back to when DDC fails. No two with the
 * same active size and totals. Returns the count. */
MW_HC_INLINE mwh_u32 MwHdmiModes(const mwh_u8 *E, mwh_u32 Len, mwh_u32 MaxPixels,
                                 MW_TIMING *Out, mwh_u32 Max)
{
    mwh_u32 n = 0;
    MW_TIMING t;

    if (Len >= 128) {
        for (mwh_u32 b = 0; (b + 1) * 128 <= Len && b <= 4 && n < Max; b++) {
            const mwh_u8 *x = E + b * 128;
            mwh_u32 first, last;
            if (b == 0) {
                first = 54;
                last = 126;
            } else if (x[0] == 0x02 && x[2] >= 4 && x[2] < 127) {
                first = x[2];
                last = 127;
            } else {
                continue;
            }
            for (mwh_u32 o = first; o + 18 <= last && n < Max; o += 18) {
                int dup = 0;
                if (!MwEdidDtd(x + o, &t) || !MwTimingUsable(&t, MaxPixels))
                    continue;
                for (mwh_u32 i = 0; i < n; i++)
                    dup |= MwTimingEqual(&Out[i], &t);
                if (!dup)
                    Out[n++] = t;
            }
        }
    }
    {
        static const mwh_u8 safe[] = { 16, 4 };
        for (mwh_u32 s = 0; s < sizeof(safe) && n < Max; s++) {
            for (unsigned i = 0; i < sizeof(MwCeaModes) / sizeof(MwCeaModes[0]); i++) {
                int dup = 0;
                if (MwCeaModes[i].Vic != safe[s])
                    continue;
                MwCeaTiming(&MwCeaModes[i], &t);
                if (!MwTimingUsable(&t, MaxPixels))
                    break;
                for (mwh_u32 k = 0; k < n; k++)
                    dup |= MwTimingEqual(&Out[k], &t);
                if (!dup)
                    Out[n++] = t;
                break;
            }
        }
    }
    return n;
}

/* ---- the HDPTX PHY's ROPLL (PhyRockchipSamsungHdptxHdmi.c) ---- */

typedef struct _MW_ROPLL {
    mwh_u32 BitRate;            /* kbps: the TMDS character rate times 10 */
    mwh_u8 Mdiv, MdivAfc, Pdiv, Refdiv, Sdiv;
    mwh_u8 SdmEn, SdmDeno, SdmNumSign, SdmNum;
    mwh_u8 SdcN, SdcNum, SdcDeno;
} MW_ROPLL;

/* ROPLL_TMDS_CONFIG, the fields RopllCmnConfig uses, in the firmware's order
 * (the struct there carries more fields, which it never reads). */
static const MW_ROPLL MwRopllTable[] = {
    { 5940000, 124,  124,  1, 1, 0,    1, 62,  1, 16,  5, 0,    1 },
    { 3712500, 155,  155,  1, 1, 1,    1, 62,  1, 16,  5, 0,    1 },
    { 2970000, 124,  124,  1, 1, 1,    1, 62,  1, 16,  5, 0,    1 },
    { 2415000, 101,  101,  1, 1, 1,    1, 8,   1, 3,   5, 0,    1 },
    { 1620000, 135,  135,  1, 1, 3,    1, 4,   0, 3,   5, 5,    0x10 },
    { 1856250, 155,  155,  1, 1, 3,    1, 62,  1, 16,  5, 0,    1 },
    { 1540000, 193,  193,  1, 1, 5,    1, 193, 1, 32,  2, 1,    1 },
    { 1485000, 0x7b, 0x7b, 1, 1, 3,    1, 4,   0, 3,   5, 5,    0x10 },
    { 1462500, 122,  122,  1, 1, 3,    1, 244, 1, 16,  2, 1,    1 },
    { 1190000, 149,  149,  1, 1, 5,    1, 149, 1, 16,  2, 1,    1 },
    { 1065000, 89,   89,   1, 1, 3,    1, 89,  1, 16,  1, 0,    1 },
    { 1080000, 135,  135,  1, 1, 5,    0, 0x9, 0, 0x05, 0, 0x14, 0x18 },
    { 855000,  214,  214,  1, 1, 11,   1, 214, 1, 16,  2, 1,    1 },
    { 835000,  105,  105,  1, 1, 5,    1, 42,  1, 16,  1, 0,    1 },
    { 928125,  155,  155,  1, 1, 7,    1, 62,  1, 16,  5, 0,    1 },
    { 742500,  124,  124,  1, 1, 7,    1, 62,  1, 16,  5, 0,    1 },
    { 650000,  162,  162,  1, 1, 11,   1, 54,  0, 16,  4, 1,    1 },
    { 337500,  0x70, 0x70, 1, 1, 0xf,  1, 0x2, 0, 0x01, 5, 1,   1 },
    { 400000,  100,  100,  1, 1, 11,   0, 0x9, 0, 0x05, 0, 0x14, 0x18 },
    { 270000,  0x5a, 0x5a, 1, 1, 0xf,  0, 0x9, 0, 0x05, 0, 0x14, 0x18 },
    { 251750,  84,   84,   1, 1, 0xf,  1, 168, 1, 16,  4, 1,    1 },
};

/* linux/lib/math/rational.c, as the firmware carries it. */
MW_HC_INLINE void MwRationalBest(mwh_u32 Gn, mwh_u32 Gd, mwh_u32 MaxN, mwh_u32 MaxD, mwh_u32 *Bn, mwh_u32 *Bd)
{
    mwh_u32 n = Gn, d = Gd, n0 = 0, d0 = 1, n1 = 1, d1 = 0, n2, d2;

    for (;;) {
        mwh_u32 dp, a;
        if (d == 0)
            break;
        dp = d;
        a = n / d;
        d = n % d;
        n = dp;
        n2 = n0 + a * n1;
        d2 = d0 + a * d1;
        if (n2 > MaxN || d2 > MaxD) {
            mwh_u32 t1 = (MaxN - n0) / n1, t2 = d1 ? (MaxD - d0) / d1 : t1;
            mwh_u32 t = t1 < t2 ? t1 : t2;
            if (2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
                n1 = n0 + t * n1;
                d1 = d0 + t * d1;
            }
            break;
        }
        n0 = n1;
        n1 = n2;
        d0 = d1;
        d1 = d2;
    }
    *Bn = n1;
    *Bd = d1;
}

/* HdptxPhyClkPllCalc, for a rate the table does not have. */
MW_HC_INLINE int MwRopllCalc(mwh_u32 DataRate, MW_ROPLL *Cfg)
{
    const mwh_u32 fref = 24000, nn = 8;
    mwh_u32 fout = DataRate / 2, fvco = 0, mdiv = 0, sdiv, sdc = 0;
    mwh_u32 k = 0, lc = 0, ksub = 0, lcsub = 0;

    for (sdiv = 1; sdiv <= 16; sdiv++) {
        if ((sdiv % 2) && sdiv != 1)
            continue;
        fvco = fout * sdiv;
        if (fvco < 2000000 || fvco > 4000000)
            continue;
        mdiv = (fvco + fref - 1) / fref;
        if (mdiv < 20 || mdiv > 255)
            continue;
        if (fref * mdiv - fvco) {
            for (sdc = 264000; sdc <= 750000; sdc += fref)
                if (sdc * nn > fref * mdiv)
                    break;
            if (sdc > 750000)
                continue;
            MwRationalBest(fref * mdiv - fvco, sdc / 16, 0x7F, 0xFF, &k, &lc);
            MwRationalBest(sdc * nn - fref * mdiv, sdc, 0x7F, 0xFF, &ksub, &lcsub);
        }
        break;
    }
    if (sdiv > 16)
        return 0;
    if (Cfg != 0) {
        Cfg->BitRate = DataRate;
        Cfg->Mdiv = (mwh_u8)mdiv;
        Cfg->MdivAfc = (mwh_u8)mdiv;
        Cfg->Pdiv = 1;
        Cfg->Refdiv = 1;
        Cfg->Sdiv = (mwh_u8)(sdiv - 1);
        Cfg->SdmEn = (mwh_u8)(k > 0 ? 1 : 0);
        /* The firmware leaves the rest of its zeroed config as it is when
         * SDM is off. */
        Cfg->SdmDeno = 0; Cfg->SdmNumSign = 0; Cfg->SdmNum = 0;
        Cfg->SdcN = 0; Cfg->SdcNum = 0; Cfg->SdcDeno = 0;
        if (Cfg->SdmEn) {
            Cfg->SdmDeno = (mwh_u8)lc;
            Cfg->SdmNumSign = 1;
            Cfg->SdmNum = (mwh_u8)k;
            Cfg->SdcN = (mwh_u8)(nn - 3);
            Cfg->SdcNum = (mwh_u8)ksub;
            Cfg->SdcDeno = (mwh_u8)lcsub;
        }
    }
    return 1;
}

/* HdptxRopllCmnConfig's choice: the table entry of exactly this rate, else
 * the calculation. 0: the firmware would refuse the rate. */
MW_HC_INLINE int MwRopllFor(mwh_u32 BitRate, MW_ROPLL *Cfg)
{
    for (unsigned i = 0; i < sizeof(MwRopllTable) / sizeof(MwRopllTable[0]); i++)
        if (MwRopllTable[i].BitRate == BitRate) {
            if (Cfg != 0)
                *Cfg = MwRopllTable[i];
            return 1;
        }
    return MwRopllCalc(BitRate, Cfg);
}

MW_HC_INLINE int MwHdptxCanDo(mwh_u32 BitRateKbps)
{
    return MwRopllFor(BitRateKbps, 0);
}

/* ---- the video port (Vop2Init, Vop2IfConfig, Vop2PostConfig) ---- */

#define MW_VP_OUT_MODE_P888     0u      /* DSI: DwMipiDsi2Lib sets P888 */
#define MW_VP_OUT_MODE_AAAA     15u     /* HDMI: DwHdmiQpConnectorInit sets AAAA; VP0 is 10-bit capable */
#define MW_VP_DSP_STANDBY       0x80000000u
#define MW_IF_EN                (1u << 0)
#define MW_IF_CLK_OUT_EN        (1u << 1)
#define MW_IF_MUX(vp)           ((unsigned)(vp) << 2)
#define MW_IF_PIN_POL(v)        ((unsigned)(v) << 4)
#define MW_IF_CFG_DONE_IMD      (1u << 31)

typedef struct _MW_VP_REGS {
    mwh_u32 DspCtrl;            /* VP_DSP_CTRL, STANDBY clear */
    mwh_u32 HTotalHsEnd, HActStEnd, VTotalVsEnd, VActStEnd;
    mwh_u32 PostHAct, PostVAct, PostSclFactor, PostSclCtrl;
    mwh_u32 PreScan;
    mwh_u32 LineFlag;
    mwh_u32 ClkCtrl;            /* VP0_CLK_CTRL: dclk_core / 2, dclk_out / 1 */
    mwh_u32 IfCtrl;             /* HDMI0_IF_CTRL or MIPI0_IF_CTRL */
} MW_VP_REGS;

/* VP0 only, progressive, RGB888 bus, no overscan: the values Vop2Init and
 * Vop2Rk3576VpComputeRegs agree on for this board. Dsi selects MIPI0's
 * output mode and pin polarity; anything else is HDMI0. */
/* Any video port: VP0 has two pixels per clock (dclk_core / 2, no
 * PCLK_DIV), VP1 and VP2 one (CLK_CTRL 0, PCLK_DIV set), as Vop2IfConfig
 * and mainline rk3576_set_intf_mux have them; BG_DLY is 20 on VP0 and 16 on
 * VP1 and VP2 (rk3576_vop2_setup_bg_dly). For the extended desktop with the
 * panel on VP2 (docs/HDMI-HPD.md); hdmi.c uses VP0 only so far. */
#define MW_IF_PCLK_DIV          (1u << 20)
MW_HC_INLINE void MwVpRegsPort(const MW_TIMING *T, int Dsi, unsigned Vp, MW_VP_REGS *R);

MW_HC_INLINE void MwVpRegs(const MW_TIMING *T, int Dsi, MW_VP_REGS *R)
{
    MwVpRegsPort(T, Dsi, 0, R);
}

MW_HC_INLINE void MwVpRegsPort(const MW_TIMING *T, int Dsi, unsigned Vp, MW_VP_REGS *R)
{
    mwh_u32 hsync = (mwh_u32)T->HSyncEnd - T->HSyncStart;
    mwh_u32 hact_st = (mwh_u32)T->HTotal - T->HSyncStart, hact_end = hact_st + T->HDisplay;
    mwh_u32 vsync = (mwh_u32)T->VSyncEnd - T->VSyncStart;
    mwh_u32 vact_st = (mwh_u32)T->VTotal - T->VSyncStart, vact_end = vact_st + T->VDisplay;
    mwh_u32 bg_dly = Vp == 0 ? 20u : 16u;   /* 10 + 8 + 2 on VP0, 10 + 6 on VP1/VP2 (Rk3576BgDly) */
    mwh_u32 pre_hs = hsync < 8 ? 8 : hsync;
    mwh_u32 pol;

    R->DspCtrl = Dsi ? MW_VP_OUT_MODE_P888 : MW_VP_OUT_MODE_AAAA;
    R->HTotalHsEnd = (mwh_u32)T->HTotal << 16 | hsync;
    R->HActStEnd = hact_st << 16 | hact_end;
    R->VTotalVsEnd = (mwh_u32)T->VTotal << 16 | vsync;
    R->VActStEnd = vact_st << 16 | vact_end;
    /* Margins 100 (Vop2PostConfig's default): the post scaler passes the
     * line through, apart from rounding an odd size down to even. */
    {
        mwh_u32 hs = T->HDisplay & ~1u, vs = T->VDisplay & ~1u;
        R->PostHAct = hact_st << 16 | (hact_st + hs);
        R->PostVAct = vact_st << 16 | (vact_st + vs);
        R->PostSclFactor = ((((mwh_u32)T->VDisplay - 1) << 12) / (vs - 1)) << 16 |
                           ((((mwh_u32)T->HDisplay - 1) << 12) / (hs - 1) & 0xFFFF);
        R->PostSclCtrl = (T->HDisplay != hs ? 1u : 0u) | (T->VDisplay != vs ? 2u : 0u);
    }
    R->PreScan = (bg_dly + (T->HDisplay >> 1) - 1) << 16 | pre_hs;
    R->LineFlag = vact_end << 16 | vact_end;
    R->ClkCtrl = Vp == 0 ? 1u : 0u;
    /* PIN_POL: HDMI from the mode's sync (Vop2IfConfig, "1 means positive");
     * MIPI0 always 3, as the firmware and Linux leave it (the TaishanPi's
     * panel stays dark otherwise, memory: project_tspi3m_uefi_linux_display_touch). */
    pol = Dsi ? 3u : ((T->PHSync ? 1u : 0u) | (T->PVSync ? 2u : 0u));
    R->IfCtrl = MW_IF_EN | MW_IF_CLK_OUT_EN | MW_IF_CFG_DONE_IMD | MW_IF_MUX(Vp & 3) | MW_IF_PIN_POL(pol) |
                (Vp != 0 ? MW_IF_PCLK_DIV : 0u);
}

/* ---- InfoFrames (DwHdmiQpLib.c) ---- */

/* AVI, laid out as HdmiConfigAviInfoframe writes it: W[0] = PKT_AVI_CONTENTS0
 * (version and length in bits 15:8 and 23:16), W[1..4] = CONTENTS1..4
 * (checksum, PB1..PB13). The VIC is 0 for modes above 64 unless the sink
 * is HDMI 2.0, and for a 4K mode that has an HDMI VIC (HdmiConfigInfoframes);
 * Q is full range only for a QS sink. */
MW_HC_INLINE void MwAviInfoframe(const MW_TIMING *T, mwh_u8 Vic, int RgbQs, mwh_u32 W[5])
{
    mwh_u8 b[17] = { 0 };
    mwh_u8 sum = 0;

    b[0] = 0x82;
    b[1] = 2;
    b[2] = 13;
    b[4] = 0x10;
    b[5] = 0x08;
    if ((mwh_u32)T->HDisplay * 9 == (mwh_u32)T->VDisplay * 16)
        b[5] = (mwh_u8)(b[5] | 0x20);
    else if ((mwh_u32)T->HDisplay * 3 == (mwh_u32)T->VDisplay * 4)
        b[5] = (mwh_u8)(b[5] | 0x10);
    b[7] = Vic;
    if (RgbQs)
        b[6] = 0x2 << 2;
    for (int i = 0; i < 17; i++)
        sum = (mwh_u8)(sum + b[i]);
    b[3] = (mwh_u8)(256 - sum);
    W[0] = (mwh_u32)b[1] << 8 | (mwh_u32)b[2] << 16;
    for (int i = 0; i < 4; i++) {
        mwh_u32 v = 0;
        for (int j = 0; j < 4 && i * 4 + j < 14; j++)
            v |= (mwh_u32)b[i * 4 + j + 3] << (8 * j);
        W[1 + i] = v;
    }
}

MW_HC_INLINE mwh_u8 MwHdmiVicOf(mwh_u8 CeaVic)
{
    switch (CeaVic) {
    case 95: return 1;
    case 94: return 2;
    case 93: return 3;
    case 98: return 4;
    default: return 0;
    }
}

/* The vendor-specific (HDMI 1.4b) InfoFrame for an HDMI VIC, as
 * HdmiConfigVendorInfoframe writes it: W[0] = PKT_VSI_CONTENTS0, W[1..2] =
 * CONTENTS1..2 (CONTENTS7 is written 0 separately). */
MW_HC_INLINE void MwVsiInfoframe(mwh_u8 HdmiVic, mwh_u32 W[3])
{
    mwh_u8 b[9] = { 0x81, 1, 5, 0, 0x03, 0x0c, 0x00, 0x1 << 5, 0 };
    mwh_u8 sum = 0;
    mwh_u32 v = 0;
    int r = 1;

    b[8] = HdmiVic;
    for (int i = 0; i < 9; i++)
        sum = (mwh_u8)(sum + b[i]);
    b[3] = (mwh_u8)(256 - sum);
    W[0] = (mwh_u32)b[2] << 16 | (mwh_u32)b[1] << 8 | b[0];
    W[1] = W[2] = 0;
    for (int i = 3; i < 9; i++) {
        if (i % 4 == 3) v = b[i];
        if (i % 4 == 0) v |= (mwh_u32)b[i] << 8;
        if (i % 4 == 1) v |= (mwh_u32)b[i] << 16;
        if (i % 4 == 2) v |= (mwh_u32)b[i] << 24;
        if (i % 4 == 2 || i == 8) {
            if (r <= 2)
                W[r] = v;
            r++;
        }
    }
}

/* ---- audio (DwHdmiQpAudio.c): 48 kHz, N 6144, CTS = TMDS / 1000 ---- */

MW_HC_INLINE mwh_u32 MwAudioCts(mwh_u32 ClockKhz)
{
    unsigned long long t = (unsigned long long)ClockKhz * 1000u * 6144u + (128u * 48000u) / 2;
    return (mwh_u32)(t / (128u * 48000u));
}
