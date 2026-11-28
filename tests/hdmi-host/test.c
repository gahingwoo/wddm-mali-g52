// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/hdmi_calc.h against numbers the firmware (edk2-rk3576)
 * is known to produce or contain:
 *
 *  - 1920x1080@60 on VP0: HDMI0_IF_CTRL 0x80000033 (Vop2IfConfig's comment,
 *    byte-identical to the vendor firmware, memory
 *    project_rk3576_vop2_mixers_stripes), VP0_CLK_CTRL 1 ("Linux reference:
 *    VP0_CLK_CTRL = 0x01 when HDMI works"), the VP timing words mainline's
 *    formula gives, and PRE_SCAN with BG_DLY 20.
 *  - The TaishanPi 3M panel (800x1280, 71 MHz, both syncs negative) on
 *    MIPI0: IF_CTRL with PIN_POL 3 and P888.
 *  - The ROPLL table entries for 1485000 and 2415000 kbps are found, not
 *    calculated (the firmware's note on 2415000 says the calculation is wrong
 *    for it), and a rate between entries is calculated.
 *  - The AVI InfoFrame for VIC 16 and its checksum; CTS 148500 at 148.5 MHz
 *    and 241500 at 241.5 MHz (DwHdmiQpAudio.c's own figures).
 *  - EDID parsing of a synthetic 1080p monitor with an HDMI VSDB, an HF-SCDB
 *    with SCDC, basic audio, and a 2560x1440@60 second DTD.
 */
#include <stdio.h>
#include <string.h>
#include "../../drivers/maliwddm/hdmi_calc.h"

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

/* An 18-byte DTD from a timing. */
static void put_dtd(unsigned char *d, unsigned clk_khz, unsigned ha, unsigned hfp, unsigned hsw, unsigned hb,
                    unsigned va, unsigned vfp, unsigned vsw, unsigned vb, int ph, int pv)
{
    unsigned c = clk_khz / 10;
    memset(d, 0, 18);
    d[0] = c & 0xFF;
    d[1] = c >> 8;
    d[2] = ha & 0xFF;
    d[3] = hb & 0xFF;
    d[4] = (unsigned char)(((ha >> 8) << 4) | (hb >> 8));
    d[5] = va & 0xFF;
    d[6] = vb & 0xFF;
    d[7] = (unsigned char)(((va >> 8) << 4) | (vb >> 8));
    d[8] = hfp & 0xFF;
    d[9] = hsw & 0xFF;
    d[10] = (unsigned char)(((vfp & 0xF) << 4) | (vsw & 0xF));
    d[11] = (unsigned char)(((hfp >> 8) << 6) | ((hsw >> 8) << 4) | ((vfp >> 4) << 2) | (vsw >> 4));
    d[17] = (unsigned char)(0x18 | (pv ? 0x04 : 0) | (ph ? 0x02 : 0));
}

static void checksum(unsigned char *b)
{
    unsigned char s = 0;
    for (int i = 0; i < 127; i++)
        s = (unsigned char)(s + b[i]);
    b[127] = (unsigned char)(256 - s);
}

static void test_vp(void)
{
    MW_TIMING t;
    MW_VP_REGS r;

    MwCeaTiming(&MwCeaModes[3], &t);            /* VIC 16 */
    CHECK(t.Vic == 16 && t.HDisplay == 1920, "table order");
    MwVpRegs(&t, 0, &r);
    CHECK(r.IfCtrl == 0x80000033, "HDMI0_IF_CTRL 1080p60 = 0x%08x", r.IfCtrl);
    CHECK(r.ClkCtrl == 1, "VP0_CLK_CTRL");
    CHECK(r.DspCtrl == 15, "DSP_CTRL AAAA = 0x%08x", r.DspCtrl);
    CHECK(r.HTotalHsEnd == (2200u << 16 | 44), "HTOTAL_HS_END 0x%08x", r.HTotalHsEnd);
    CHECK(r.HActStEnd == (192u << 16 | 2112), "HACT_ST_END 0x%08x", r.HActStEnd);
    CHECK(r.VTotalVsEnd == (1125u << 16 | 5), "VTOTAL_VS_END 0x%08x", r.VTotalVsEnd);
    CHECK(r.VActStEnd == (41u << 16 | 1121), "VACT_ST_END 0x%08x", r.VActStEnd);
    CHECK(r.PostHAct == r.HActStEnd && r.PostVAct == r.VActStEnd, "post = active");
    CHECK(r.PostSclFactor == 0x10001000 && r.PostSclCtrl == 0, "post scaler identity 0x%08x", r.PostSclFactor);
    CHECK(r.PreScan == ((20u + 960 - 1) << 16 | 44), "PRE_SCAN 0x%08x", r.PreScan);
    CHECK(r.LineFlag == (1121u << 16 | 1121), "LINE_FLAG");

    /* The panel: 800x1280, HFP 52 HSYNC 8 HBP 48, VFP 15 VSYNC 6 VBP 16, 71 MHz. */
    memset(&t, 0, sizeof(t));
    t.Clock = 71000;
    t.HDisplay = 800; t.HSyncStart = 852; t.HSyncEnd = 860; t.HTotal = 908;
    t.VDisplay = 1280; t.VSyncStart = 1295; t.VSyncEnd = 1301; t.VTotal = 1317;
    MwVpRegs(&t, 1, &r);
    CHECK(r.IfCtrl == 0x80000033, "MIPI0_IF_CTRL = 0x%08x (PIN_POL 3, MUX 0)", r.IfCtrl);
    CHECK(r.DspCtrl == 0, "DSI P888");
    CHECK(r.HTotalHsEnd == (908u << 16 | 8), "DSI HTOTAL");
    CHECK(r.PreScan == ((20u + 400 - 1) << 16 | 8), "DSI PRE_SCAN");

    /* The panel on VP2, as the vendor 6.1 kernel and Linux run it: the
     * vendor dump's MIPI0_IF_CTRL 0x8010003b (CFG_DONE_IMD, PCLK_DIV, PIN_POL
     * 3, MUX 2, CLK_OUT_EN, EN), VP2_CLK_CTRL 0, BG_DLY 16. */
    MwVpRegsPort(&t, 1, 2, &r);
    CHECK(r.IfCtrl == 0x8010003b, "MIPI0 on VP2 IF_CTRL 0x%08x", r.IfCtrl);
    CHECK(r.ClkCtrl == 0, "VP2_CLK_CTRL");
    CHECK(r.PreScan == ((16u + 400 - 1) << 16 | 8), "VP2 PRE_SCAN");

    /* Negative syncs on HDMI: PIN_POL 0. */
    MwCeaTiming(&MwCeaModes[0], &t);            /* VIC 1, -H -V */
    MwVpRegs(&t, 0, &r);
    CHECK(r.IfCtrl == 0x80000003, "640x480 IF_CTRL 0x%08x", r.IfCtrl);
}

static void test_pll(void)
{
    MW_ROPLL c;

    CHECK(MwRopllFor(1485000, &c) && c.Mdiv == 0x7b && c.Sdiv == 3 && c.SdmEn == 1 && c.SdmDeno == 4 &&
          c.SdmNum == 3 && c.SdcN == 5 && c.SdcNum == 5 && c.SdcDeno == 0x10, "1485000 table entry");
    CHECK(MwRopllFor(2415000, &c) && c.Mdiv == 101 && c.SdmDeno == 8 && c.SdmNum == 3, "2415000 table entry");
    CHECK(MwRopllFor(742500, &c) && c.Mdiv == 124 && c.Sdiv == 7, "742500 table entry");
    /* 1280x1024@60, 108 MHz: in the table as 1080000. */
    CHECK(MwRopllFor(1080000, &c) && c.SdmEn == 0, "1080000");
    /* 1680x1050@60 reduced blanking, 119 MHz: table. 1920x1200@60 RB,
     * 154 MHz: table. 1600x900@60 RB, 97.75 MHz: calculated. */
    CHECK(MwRopllFor(1190000, &c), "1190000");
    CHECK(MwRopllFor(1540000, &c), "1540000");
    CHECK(MwRopllCalc(977500, &c) && c.Mdiv >= 20 && c.Pdiv == 1, "977500 calculated, Mdiv %u", c.Mdiv);
    {
        /* The calculation's VCO: 24 MHz * (Mdiv - SDM correction) within 2..4 GHz,
         * and Sdiv even or 1. */
        unsigned sdiv = c.Sdiv + 1u;
        unsigned fvco = 977500u / 2 * sdiv;
        CHECK((sdiv == 1 || (sdiv % 2) == 0) && fvco >= 2000000 && fvco <= 4000000, "VCO %u sdiv %u", fvco, sdiv);
    }
    CHECK(MwHdptxCanDo(2700000), "270 MHz");
    /* 2560x1440@60 as the TaishanPi's monitor gives it (241.7 MHz, not the
     * table's 241.5): calculated, with mainline's rk_hdptx_phy_clk_pll_calc
     * result (Mdiv 101, sdc 312000, K/Lc 14/39, so 7000 kHz off 2424000;
     * SDC 5, 3/13). The firmware drives this monitor at this rate through
     * the same calculation on the CM5-IO. */
    CHECK(MwRopllFor(2417000, &c) && c.Mdiv == 101 && c.Sdiv == 1 && c.SdmEn == 1 && c.SdmNum == 14 &&
          c.SdmDeno == 39 && c.SdcN == 5 && c.SdcNum == 3 && c.SdcDeno == 13,
          "2417000: Mdiv %u Sdiv %u %u/%u SDC %u %u/%u", c.Mdiv, c.Sdiv, c.SdmNum, c.SdmDeno, c.SdcN, c.SdcNum, c.SdcDeno);
    /* 2560x1440@75, 302.13 MHz, also below 340 at 8 bpc. */
    CHECK(MwRopllFor(3021300, &c) && c.Mdiv == 126, "3021300 Mdiv %u", c.Mdiv);
}

static void test_infoframes(void)
{
    MW_TIMING t;
    unsigned w[5], v[3];
    unsigned char s = 0;

    MwCeaTiming(&MwCeaModes[3], &t);
    MwAviInfoframe(&t, 16, 0, w);
    CHECK(w[0] == 0x000D0200, "AVI CONTENTS0 0x%08x", w[0]);
    CHECK(w[1] == 0x00281027, "AVI CONTENTS1 0x%08x", w[1]);
    CHECK(w[2] == 0x00000010, "AVI CONTENTS2 0x%08x", w[2]);
    CHECK(w[3] == 0 && w[4] == 0, "AVI CONTENTS3/4");
    /* The checksum: header 0x82 2 13 and every payload byte sum to 0. */
    s = (unsigned char)(0x82 + 2 + 13);
    for (int i = 1; i < 5; i++)
        for (int j = 0; j < 4; j++)
            s = (unsigned char)(s + (w[i] >> (8 * j)));
    CHECK(s == 0, "AVI checksum sums to 0x%02x", s);

    MwVsiInfoframe(1, v);
    CHECK(v[0] == 0x00050181, "VSI CONTENTS0 0x%08x", v[0]);
    s = (unsigned char)(0x81 + 1 + 5);
    for (int i = 1; i < 3; i++)
        for (int j = 0; j < 4; j++)
            s = (unsigned char)(s + (v[i] >> (8 * j)));
    CHECK(s == 0, "VSI checksum sums to 0x%02x", s);
    CHECK(MwHdmiVicOf(95) == 1 && MwHdmiVicOf(16) == 0, "HDMI VIC");

    CHECK(MwAudioCts(148500) == 148500, "CTS 148.5 MHz = %u", MwAudioCts(148500));
    CHECK(MwAudioCts(241500) == 241500, "CTS 241.5 MHz");
}

static void test_edid(void)
{
    unsigned char e[256];
    MW_TIMING m[8];
    MW_SINK s;
    unsigned n;

    memset(e, 0, sizeof(e));
    e[0] = 0x00; memset(e + 1, 0xFF, 6); e[7] = 0x00;
    put_dtd(e + 54, 148500, 1920, 88, 44, 280, 1080, 4, 5, 45, 1, 1);
    put_dtd(e + 72, 241500, 2560, 48, 32, 160, 1440, 3, 5, 41, 1, 0);
    e[126] = 1;
    checksum(e);
    /* CEA extension: VSDB, HF-SCDB with SCDC, basic audio. */
    {
        unsigned char *x = e + 128;
        unsigned o = 4;
        x[0] = 0x02; x[1] = 3; x[3] = 0x40;
        x[o++] = 0x65; x[o++] = 0x03; x[o++] = 0x0C; x[o++] = 0x00; x[o++] = 0x10; x[o++] = 0x00;
        x[o++] = 0x66; x[o++] = 0xD8; x[o++] = 0x5D; x[o++] = 0xC4; x[o++] = 0x01; x[o++] = 0x78; x[o++] = 0x80;
        x[o++] = 0xE2; x[o++] = 0x00; x[o++] = 0x40;       /* VCDB, QS */
        x[2] = (unsigned char)o;
        put_dtd(x + o, 74250, 1280, 110, 40, 370, 720, 5, 5, 30, 1, 1);
        checksum(x);
    }
    MwEdidSink(e, sizeof(e), &s);
    CHECK(s.IsHdmi && s.Scdc && s.BasicAudio && s.RgbQs, "sink %u %u %u %u", s.IsHdmi, s.Scdc, s.BasicAudio, s.RgbQs);

    n = MwHdmiModes(e, sizeof(e), 2560 * 1600, m, 8);
    CHECK(n == 3, "modes %u (1080p, 1440p, 720p; the safe 1080p and 720p are the DTDs' own)", n);
    CHECK(n >= 1 && m[0].HDisplay == 1920 && m[0].Clock == 148500 && m[0].Vic == 16, "preferred first, VIC 16");
    CHECK(n >= 2 && m[1].HDisplay == 2560 && m[1].Vic == 0 && m[1].PHSync == 1 && m[1].PVSync == 0, "1440p");
    CHECK(n >= 3 && m[2].Vic == 4, "720p from the extension");

    /* A small reserve: no 1440p. */
    n = MwHdmiModes(e, sizeof(e), 1920 * 1080, m, 8);
    CHECK(n == 2, "modes within 1920x1080: %u", n);

    /* No EDID at all: the safe set. */
    n = MwHdmiModes(e, 0, 1920 * 1200, m, 8);
    CHECK(n == 2 && m[0].Vic == 16 && m[1].Vic == 4, "fallback modes %u", n);

    /* Above 340 MHz is not offered. */
    put_dtd(e + 54, 397980, 2560, 48, 32, 160, 1440, 3, 5, 41, 1, 0);
    n = MwHdmiModes(e, 128, 2560 * 1600, m, 8);
    CHECK(n >= 1 && m[0].Clock != 397980, "397.98 MHz refused");

    /* An interlaced DTD is refused. */
    put_dtd(e + 54, 74250, 1920, 88, 44, 280, 540, 2, 5, 22, 1, 1);
    e[54 + 17] |= 0x80;
    CHECK(!MwEdidDtd(e + 54, &m[0]), "interlaced refused");
}


/* ---- the full mode list (MwHdmiModes): every rate the EDID names ---- */

static int has_mode(const MW_TIMING *m, unsigned n, unsigned w, unsigned h, unsigned mhz_lo, unsigned mhz_hi)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned r = MwTimingMilliHz(&m[i]);
        if (m[i].HDisplay == w && m[i].VDisplay == h && r >= mhz_lo && r <= mhz_hi)
            return 1;
    }
    return 0;
}

/* A DisplayID type I timing (20 bytes), values as stored (minus one). */
static void put_did(unsigned char *d, unsigned clk10k, unsigned ha, unsigned hb, unsigned hfp, unsigned hsw,
                    unsigned va, unsigned vb, unsigned vfp, unsigned vsw, int ph, int pv)
{
    unsigned v[8] = { ha - 1, hb - 1, hfp - 1, hsw - 1, va - 1, vb - 1, vfp - 1, vsw - 1 };
    memset(d, 0, 20);
    d[0] = (clk10k - 1) & 0xFF;
    d[1] = ((clk10k - 1) >> 8) & 0xFF;
    d[2] = (clk10k - 1) >> 16;
    for (int i = 0; i < 8; i++) {
        d[4 + 2 * i] = v[i] & 0xFF;
        d[5 + 2 * i] = (unsigned char)(v[i] >> 8);
    }
    if (ph) d[9] |= 0x80;
    if (pv) d[17] |= 0x80;
}

/* A 2560x1440 monitor of the kind on the board: preferred 2560x1440@60
 * (241.70 MHz, the AOC's), @75 and @100 as detailed timings, a range limits
 * descriptor, standard timings (some DMT, some only CVT), established
 * timings, a CTA block with an HDMI VSDB, SVDs from 640x480 to 4K60, and a
 * DisplayID block with 1920x1080@144. */
static unsigned make_monitor(unsigned char *e, unsigned max_tmds_5mhz)
{
    unsigned o;
    memset(e, 0, 384);
    e[0] = 0x00; memset(e + 1, 0xFF, 6); e[7] = 0x00;
    e[18] = 1; e[19] = 4;
    e[35] = 0x21;                       /* 640x480@60, 800x600@60 */
    e[36] = 0x0A;                       /* 1024x768@60, @75 */
    for (int i = 38; i < 54; i++)
        e[i] = 1;
    e[38] = 1920 / 8 - 31; e[39] = (3 << 6) | 0;        /* 1920x1080@60: DMT */
    e[40] = 1920 / 8 - 31; e[41] = (3 << 6) | 60;       /* 1920x1080@120: CVT-RB */
    e[42] = 1600 / 8 - 31; e[43] = (3 << 6) | 0;        /* 1600x900@60: DMT (RB) */
    e[44] = 1440 / 8 - 31; e[45] = (0 << 6) | 15;       /* 1440x900@75: CVT-RB within the range */
    e[46] = 1680 / 8 - 31; e[47] = (0 << 6) | 0;        /* 1680x1050@60: DMT */
    e[48] = 1280 / 8 - 31; e[49] = (2 << 6) | 15;       /* 1280x1024@75: DMT */
    put_dtd(e + 54, 241700, 2560, 48, 32, 160, 1440, 3, 5, 41, 1, 1);
    put_dtd(e + 72, 302130, 2560, 48, 32, 160, 1440, 3, 5, 41, 1, 0);
    /* range limits: 48-146 Hz, 30-230 kHz, 600 MHz */
    e[90 + 3] = 0xFD; e[90 + 5] = 48; e[90 + 6] = 146; e[90 + 7] = 30; e[90 + 8] = 230; e[90 + 9] = 60;
    put_dtd(e + 108, 397980, 2560, 48, 32, 160, 1440, 3, 5, 41, 1, 0);   /* 100 Hz */
    e[126] = 2;
    checksum(e);
    {
        unsigned char *x = e + 128;
        o = 4;
        x[0] = 0x02; x[1] = 3; x[3] = 0x40;
        /* video data block: 16 (native), 4, 31, 63, 64, 97, 93, 1, 2 */
        x[o++] = (2 << 5) | 9;
        x[o++] = 0x80 | 16; x[o++] = 4; x[o++] = 31; x[o++] = 63; x[o++] = 64; x[o++] = 97; x[o++] = 93;
        x[o++] = 1; x[o++] = 2;
        /* HDMI VSDB with Max_TMDS_Clock */
        x[o++] = (3 << 5) | 7;
        x[o++] = 0x03; x[o++] = 0x0C; x[o++] = 0x00; x[o++] = 0x10; x[o++] = 0x00; x[o++] = 0x00;
        x[o++] = (unsigned char)max_tmds_5mhz;
        x[2] = (unsigned char)o;
        checksum(x);
    }
    {
        unsigned char *x = e + 256;
        x[0] = 0x70; x[1] = 0x12; x[3] = 0; x[4] = 0;
        o = 5;
        x[o++] = 0x03; x[o++] = 0; x[o++] = 20;                  /* type I, one timing */
        put_did(x + o, 33277, 1920, 160, 48, 32, 1080, 31, 3, 5, 1, 0);   /* 1920x1080@144 */
        o += 20;
        x[2] = (unsigned char)(o - 5);
        checksum(x);
    }
    return 384;
}

static void test_modes(void)
{
    unsigned char e[384];
    MW_TIMING m[48], t, k;
    MW_EDID_LIMITS lim;
    unsigned n, len = make_monitor(e, 68);              /* 340 MHz */

    MwEdidLimits(e, len, &lim);
    CHECK(lim.MaxClock == 340000 && lim.HaveRange && lim.MinVHz == 48 && lim.MaxVHz == 146 &&
          lim.MaxHKhz == 230 && lim.RangeMaxClock == 600000, "limits %u %u %u-%u", lim.MaxClock, lim.HaveRange,
          lim.MinVHz, lim.MaxVHz);

    n = MwHdmiModes(e, len, 2560 * 1600, m, 48);
    printf("modes:");
    for (unsigned i = 0; i < n; i++)
        printf(" %ux%u@%u.%03u", m[i].HDisplay, m[i].VDisplay, MwTimingMilliHz(&m[i]) / 1000,
               MwTimingMilliHz(&m[i]) % 1000);
    printf("\n");
    CHECK(n >= 1 && m[0].HDisplay == 2560 && m[0].Clock == 241700, "the preferred mode first");
    CHECK(has_mode(m, n, 2560, 1440, 74900, 75100), "2560x1440@75");
    CHECK(!has_mode(m, n, 2560, 1440, 99000, 101000), "2560x1440@100 is 398 MHz: not offered");
    CHECK(!has_mode(m, n, 2560, 1440, 143000, 145000), "2560x1440@144: not offered");
    CHECK(has_mode(m, n, 1920, 1080, 59990, 60010) && has_mode(m, n, 1920, 1080, 59930, 59950), "1080p 60 and 59.94");
    CHECK(has_mode(m, n, 1920, 1080, 49990, 50010), "1080p50 (VIC 31)");
    CHECK(has_mode(m, n, 1920, 1080, 99990, 100010), "1080p100 (VIC 64)");
    CHECK(has_mode(m, n, 1920, 1080, 119990, 120010) && has_mode(m, n, 1920, 1080, 119870, 119890),
          "1080p 120 and 119.88 (VIC 63)");
    CHECK(has_mode(m, n, 1920, 1080, 143900, 144100), "1080p144 from DisplayID");
    CHECK(!has_mode(m, n, 3840, 2160, 0, 999999), "4K: not in the reserved framebuffer, 4K60 above 340 MHz");
    CHECK(has_mode(m, n, 1280, 720, 59990, 60010) && has_mode(m, n, 640, 480, 59900, 60010) &&
          has_mode(m, n, 720, 480, 59900, 60010), "SVDs 4, 1, 2");
    CHECK(has_mode(m, n, 640, 480, 59990, 60010) && !has_mode(m, n, 640, 480, 59850, 59900),
          "VIC 1 at 59.94 and 60, not 59.88");
    CHECK(has_mode(m, n, 1680, 1050, 59800, 60100) && has_mode(m, n, 1280, 1024, 74900, 75100) &&
          has_mode(m, n, 1024, 768, 59900, 60100) && has_mode(m, n, 1024, 768, 74900, 75100) &&
          has_mode(m, n, 800, 600, 60000, 60400) && has_mode(m, n, 1600, 900, 59900, 60100) &&
          has_mode(m, n, 1440, 900, 74800, 75200), "standard and established timings");
    /* Every one usable, under the cap, none twice. */
    for (unsigned i = 0; i < n; i++) {
        CHECK(MwTimingUsable(&m[i], 2560 * 1600) && m[i].Clock <= 340000, "mode %u usable", i);
        for (unsigned j = 0; j < i; j++)
            CHECK(!MwTimingSameMode(&m[i], &m[j]), "modes %u and %u the same", i, j);
    }
    /* The CTA modes carry their VIC, the 59.94 variants too. */
    for (unsigned i = 0; i < n; i++)
        if (m[i].HDisplay == 1920 && m[i].VDisplay == 1080 && m[i].HTotal == 2200 && m[i].Clock == 297000)
            CHECK(m[i].Vic == 63, "1080p120 VIC %u", m[i].Vic);

    /* A sink that says 300 MHz: 1080p144 (333 MHz) and 1440p75 (302 MHz) go. */
    len = make_monitor(e, 60);
    n = MwHdmiModes(e, len, 2560 * 1600, m, 48);
    CHECK(!has_mode(m, n, 1920, 1080, 143900, 144100) && !has_mode(m, n, 2560, 1440, 74900, 75100) &&
          has_mode(m, n, 1920, 1080, 119990, 120010), "VSDB Max_TMDS_Clock 300 MHz");

    /* No range limits: a standard timing that is not DMT (1440x900@75) is
     * not offered. A small array: no overflow. */
    len = make_monitor(e, 68);
    memset(e + 90, 0, 18);
    e[90 + 3] = 0x10;
    checksum(e);
    MwEdidLimits(e, len, &lim);
    CHECK(!lim.HaveRange, "no range");
    n = MwHdmiModes(e, len, 2560 * 1600, m, 48);
    CHECK(!has_mode(m, n, 1440, 900, 74800, 75200) && has_mode(m, n, 1600, 900, 59900, 60100), "CVT needs the range");
    n = MwHdmiModes(e, len, 2560 * 1600, m, 5);
    CHECK(n == 5, "capped at 5: %u", n);

    /* CVT reduced blanking against Linux's drm_cvt_mode numbers. */
    CHECK(MwCvtRb(2560, 1440, 60, &t) && t.Clock == 241500 && t.HTotal == 2720 && t.VTotal == 1481 &&
          t.HSyncStart == 2608 && t.HSyncEnd == 2640 && t.VSyncStart == 1443 && t.VSyncEnd == 1448 &&
          t.PHSync == 1 && t.PVSync == 0, "CVT-RB 2560x1440@60: %u %u %u", t.Clock, t.HTotal, t.VTotal);
    CHECK(MwCvtRb(1920, 1080, 60, &t) && t.Clock == 138500 && t.HTotal == 2080 && t.VTotal == 1111,
          "CVT-RB 1920x1080@60: %u %u %u", t.Clock, t.HTotal, t.VTotal);
    CHECK(MwCvtRb(1920, 1200, 60, &t) && t.Clock == 154000 && t.VTotal == 1235, "CVT-RB 1920x1200@60 = DMT: %u %u",
          t.Clock, t.VTotal);

    /* Every DMT entry is the rate it says, and has a PLL setting. */
    for (unsigned i = 0; i < sizeof(MwDmtModes) / sizeof(MwDmtModes[0]); i++) {
        unsigned r;
        MwDmtTiming(&MwDmtModes[i], &t);
        r = MwTimingMilliHz(&t);
        CHECK(r > MwDmtModes[i].Hz * 985u && r < MwDmtModes[i].Hz * 1015u, "DMT %u: %u mHz", i, r);
        CHECK(MwTimingUsable(&t, 4096 * 2304), "DMT %u usable", i);
    }
    for (unsigned i = 0; i < sizeof(MwCeaModes) / sizeof(MwCeaModes[0]); i++) {
        MwCeaTiming(&MwCeaModes[i], &t);
        CHECK(MwTimingUsable(&t, 4096 * 2304), "VIC %u usable", MwCeaModes[i].Vic);
        CHECK(MwCeaVic(&t) == MwCeaModes[i].Vic || MwCeaModes[i].Vic == 33 || MwCeaModes[i].Vic == 62,
              "VIC %u found back as %u", MwCeaModes[i].Vic, MwCeaVic(&t));
    }

    /* The firmware's timing made whole again (display.c's made-up 1080p). */
    memset(&t, 0, sizeof(t));
    t.Clock = 148500; t.HDisplay = 1920; t.VDisplay = 1080; t.HTotal = 2200; t.VTotal = 1125; t.Fw = 1;
    CHECK(MwKnownTiming(&t, &k) && k.Vic == 16 && k.HSyncStart == 2008 && !k.Fw, "the firmware's 1080p is VIC 16");
    t.Clock = 241700; t.HDisplay = 2560; t.VDisplay = 1440; t.HTotal = 2720; t.VTotal = 1481;
    CHECK(!MwKnownTiming(&t, &k), "the AOC's 241.70 MHz is no table's");
}

/* The default mode (MwHdmiDefaultFirst): the largest size at 60 Hz first,
 * nothing dropped. */
static unsigned find_mode(const MW_TIMING *m, unsigned n, unsigned w, unsigned h, unsigned lo, unsigned hi)
{
    for (unsigned i = 0; i < n; i++)
        if (m[i].HDisplay == w && m[i].VDisplay == h && MwTimingMilliHz(&m[i]) >= lo && MwTimingMilliHz(&m[i]) <= hi)
            return i;
    return n;
}

static void test_default60(void)
{
    unsigned char e[384];
    MW_TIMING m[48], c[48], t;
    unsigned n, i75, i60, len = make_monitor(e, 68);

    n = MwHdmiModes(e, len, 2560 * 1600, m, 48);
    i75 = find_mode(m, n, 2560, 1440, 74900, 75100);
    i60 = find_mode(m, n, 2560, 1440, 59900, 60100);
    CHECK(i75 < n && i60 < n, "the monitor has 1440p60 and 1440p75");

    /* A monitor (or firmware) that puts 1440p75 first: 1440p60 becomes the
     * default, and 1440p75 is still offered. */
    memcpy(c, m, sizeof(m));
    t = c[0];
    c[0] = c[i75];
    c[i75] = t;
    MwHdmiDefaultFirst(c, n);
    CHECK(c[0].HDisplay == 2560 && c[0].VDisplay == 1440 && MwTimingFrom60(&c[0]) <= 100,
          "default %ux%u@%u mHz", c[0].HDisplay, c[0].VDisplay, MwTimingMilliHz(&c[0]));
    CHECK(find_mode(c, n, 2560, 1440, 74900, 75100) < n, "1440p75 still selectable");
    for (unsigned i = 0; i < n; i++)
        CHECK(find_mode(c, n, m[i].HDisplay, m[i].VDisplay, MwTimingMilliHz(&m[i]), MwTimingMilliHz(&m[i])) < n,
              "mode %u kept", i);

    /* Already right: unchanged. */
    memcpy(c, m, sizeof(m));
    MwHdmiDefaultFirst(c, n);
    CHECK(memcmp(c, m, n * sizeof(m[0])) == 0 || MwTimingFrom60(&c[0]) <= MwTimingFrom60(&m[0]),
          "a 60 Hz first mode stays");
    CHECK(c[0].HDisplay == 2560 && MwTimingFrom60(&c[0]) <= 100, "1440p60 first");

    /* 1080p: 60.000 before 59.94, 120, 144. */
    {
        MW_TIMING k[16];
        unsigned kn = 0;
        for (unsigned i = 0; i < n && kn < 16; i++)
            if (m[i].HDisplay == 1920 && m[i].VDisplay == 1080 && MwTimingFrom60(&m[i]) > 10)
                k[kn++] = m[i];
        for (unsigned i = 0; i < n && kn < 16; i++)
            if (m[i].HDisplay == 1920 && m[i].VDisplay == 1080 && MwTimingFrom60(&m[i]) <= 10)
                k[kn++] = m[i];
        MwHdmiDefaultFirst(k, kn);
        CHECK(kn >= 3 && MwTimingFrom60(&k[0]) <= 10, "1080p: 60.000 first, got %u mHz", MwTimingMilliHz(&k[0]));
    }

    /* 1440 only at 75 (60 removed): 1440p75 stays the default, not 1080p60. */
    {
        unsigned cn = 0;
        for (unsigned i = 0; i < n; i++)
            if (i != i60)
                c[cn++] = m[i];
        t = c[0];
        for (unsigned i = 0; i < cn; i++)
            if (c[i].HDisplay == 1920 && c[i].VDisplay == 1080 && MwTimingFrom60(&c[i]) <= 10) {
                c[0] = c[i];
                c[i] = t;
                break;
            }
        MwHdmiDefaultFirst(c, cn);
        CHECK(c[0].HDisplay == 2560 && c[0].VDisplay == 1440, "largest size kept: %ux%u", c[0].HDisplay, c[0].VDisplay);
    }

    /* The safe list (no EDID): 1080p60 first. One mode or none: nothing moves. */
    n = MwHdmiModes(NULL, 0, 2560 * 1600, m, 48);
    MwHdmiDefaultFirst(m, n);
    CHECK(n >= 1 && m[0].HDisplay == 1920 && MwTimingFrom60(&m[0]) <= 10, "no EDID: 1080p60");
    MwHdmiDefaultFirst(m, 1);
    MwHdmiDefaultFirst(m, 0);
}


/* The AOC Q27B3S2 on the TaishanPi 3M (2026-11-28, from Windows' copy):
 * HDMI VSDB 340 MHz, HF-VSDB SCDC_Present and 600 MHz, and an extension
 * DTD 2560x1440@100 at 397.98 MHz. */
static const unsigned char aoc_q27b3s2[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x05, 0xe3, 0x03, 0x27, 0x04, 0x2c, 0x00, 0x00,
    0x02, 0x22, 0x01, 0x03, 0x80, 0x3c, 0x22, 0x78, 0x2e, 0x99, 0x65, 0xac, 0x4f, 0x49, 0xa6, 0x27,
    0x0f, 0x50, 0x54, 0xbf, 0xef, 0x00, 0xd1, 0xc0, 0xb3, 0x00, 0x95, 0x00, 0x81, 0x80, 0x81, 0xc0,
    0x31, 0x68, 0x45, 0x68, 0x61, 0x68, 0x6a, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20,
    0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xff, 0x00, 0x31, 0x41, 0x51,
    0x51, 0x31, 0x48, 0x41, 0x30, 0x31, 0x31, 0x32, 0x36, 0x38, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x51,
    0x32, 0x37, 0x42, 0x33, 0x53, 0x32, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd,
    0x00, 0x30, 0x64, 0x1e, 0x96, 0x3c, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x4f,
    0x02, 0x03, 0x3c, 0xf1, 0x4b, 0x10, 0x1f, 0x05, 0x14, 0x04, 0x13, 0x03, 0x12, 0x02, 0x11, 0x01,
    0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x38, 0x44,
    0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0x80, 0x00, 0xe3, 0x05, 0xe3, 0x01, 0xe6, 0x06, 0x07, 0x01,
    0x62, 0x62, 0x00, 0x68, 0x1a, 0x00, 0x00, 0x01, 0x01, 0x30, 0x64, 0xe6, 0x76, 0x9b, 0x00, 0x78,
    0xa0, 0xa0, 0x2d, 0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1e, 0x05, 0x76,
    0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20, 0x98, 0x04, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1a,
    0xf0, 0x3c, 0x00, 0xd0, 0x51, 0xa0, 0x35, 0x50, 0x60, 0x88, 0x3a, 0x00, 0x55, 0x50, 0x21, 0x00,
    0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9b,
};

static int has_clk(const MW_TIMING *m, mwh_u32 n, unsigned w, unsigned h, unsigned clk_lo, unsigned clk_hi)
{
    for (mwh_u32 i = 0; i < n; i++)
        if (m[i].HDisplay == w && m[i].VDisplay == h && m[i].Clock >= clk_lo && m[i].Clock <= clk_hi)
            return 1;
    return 0;
}

static void test_scrambling(void)
{
    MW_SINK s;
    MW_EDID_LIMITS l;
    MW_TIMING m[64];
    MW_ROPLL c;
    mwh_u32 n;
    unsigned char e[256];

    MwEdidSink(aoc_q27b3s2, 256, &s);
    CHECK(s.IsHdmi && s.Scdc && s.HfMaxKhz == 600000, "AOC: HDMI, SCDC, 600 MHz (got %u %u %u)", s.IsHdmi, s.Scdc, s.HfMaxKhz);
    MwEdidLimits(aoc_q27b3s2, 256, &l);
    CHECK(l.MaxClock == 340000, "without HdmiScrambling the cap stays 340 MHz, got %u", l.MaxClock);
    MwEdidLimitsHf(aoc_q27b3s2, 256, &l, 1);
    CHECK(l.MaxClock == 600000, "with it the HF-VSDB's 600 MHz, got %u", l.MaxClock);

    n = MwHdmiModesHf(aoc_q27b3s2, 256, 2560 * 1600, m, 64, 0);
    CHECK(!has_clk(m, n, 2560, 1440, 397000, 399000), "no 1440p100 without scrambling");
    CHECK(has_clk(m, n, 2560, 1440, 241000, 242000), "1440p60 always");
    n = MwHdmiModesHf(aoc_q27b3s2, 256, 2560 * 1600, m, 64, 1);
    CHECK(has_clk(m, n, 2560, 1440, 397000, 399000), "1440p100 with scrambling (%u modes)", n);
    CHECK(n > 0 && m[0].Clock == 241700, "the preferred mode stays first, got %u", n ? m[0].Clock : 0);

    /* The same sink without SCDC_Present: never above 340 MHz. */
    memcpy(e, aoc_q27b3s2, 256);
    for (unsigned i = 128 + 4; i < 128 + 0x3c; i++)
        if (e[i] == 0x67 && e[i + 1] == 0xd8)
            e[i + 6] &= 0x7f;
    MwEdidSink(e, 256, &s);
    CHECK(!s.Scdc, "SCDC_Present cleared");
    MwEdidLimitsHf(e, 256, &l, 1);
    CHECK(l.MaxClock == 340000, "no SCDC: 340 MHz, got %u", l.MaxClock);

    /* An HF-SCDB (extended tag 0x79) counts as an HF-VSDB does. */
    {
        const unsigned char scdb[8] = { 0xE7, 0x79, 0x00, 0x00, 0x01, 0x78, 0x80, 0x00 };
        CHECK(MwHfBlock(scdb, 8), "HF-SCDB recognised");
    }

    /* The PHY makes both rates (TMDS rate x 10, kbps). */
    CHECK(MwRopllFor(3979800, &c) && c.Sdiv == 1, "ROPLL for 1440p100 (sdiv %u)", c.Sdiv);
    CHECK(MwRopllFor(5940000, &c) && c.Mdiv == 124, "ROPLL for 4K60 from the table");
    {
        MW_TIMING t = { 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 1, 1, 97, 0 };
        CHECK(MwTimingUsable(&t, 3840 * 2160) && MwHdmiScrambled(&t), "4K60 usable and scrambled");
        CHECK(!MwTimingUsable(&t, 2560 * 1600), "4K60 needs HdmiMaxPixels for 3840x2160");
        t.Clock = 600001;
        CHECK(!MwTimingUsable(&t, 3840 * 2160), "above 600 MHz never");
    }
}

int main(void)
{
    test_scrambling();
    test_vp();
    test_pll();
    test_infoframes();
    test_edid();
    test_modes();
    test_default60();
    /* The HdmiHpd level: an explicit value wins, none is 3 on HDMI, 0 on the panel. */
    CHECK(MwHpdLevelFor(MW_HPD_UNSET, 0) == 3 && MwHpdLevelFor(MW_HPD_UNSET, 1) == 0, "defaults");
    for (unsigned v = 0; v <= 3; v++)
        CHECK(MwHpdLevelFor(v, 0) == v && MwHpdLevelFor(v, 1) == v, "explicit %u", v);
    CHECK(MwHpdLevelFor(7, 0) == 3 && MwHpdLevelFor(7, 1) == 3, "above 3 is 3");
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
