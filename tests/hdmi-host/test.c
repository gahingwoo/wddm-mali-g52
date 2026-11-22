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

int main(void)
{
    test_vp();
    test_pll();
    test_infoframes();
    test_edid();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
