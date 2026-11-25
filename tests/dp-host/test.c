// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/dp_calc.h (UsbcDp, docs/USBC-DP.md) against mainline
 * Linux's numbers, worked out by hand from the sources dp_calc.h names:
 *
 *  - AUX_CMD for a 16-byte DPCD read at 0 (dw_dp_aux_transfer), an I2C
 *    address-only stop, an EDID offset write with MOT.
 *  - VPLL settings for the common DP modes, each exact, VCO in range, the
 *    divider even (the vendor's rule), and the CRU writes for one of them.
 *  - TU and init threshold for 1080p60 and 1440p60 on two lanes
 *    (dw_dp_video_enable's dual-pixel branch), the HBLANK interval.
 *  - The drive tables rk3576 uses, picked by rate (RBR/HBR share one).
 *  - The lanes the firmware sets for each plug orientation (UsbHcd.c
 *    Rk3576UsbDpDpEnable): normal pads 2 and 3, flipped 0 and 1.
 *  - Training: CR/EQ status, the request clamp of
 *    dw_dp_link_get_adjustments, the lane-set byte.
 *  - DP0_IF_CTRL for 1080p60 on VP1 (rk3576_set_intf_mux), staged without EN.
 *  - The mode filter: VP1's limits, the controller's porch limits, what
 *    two lanes carry.
 */
#include <stdio.h>
#include <string.h>
#include "../../drivers/maliwddm/dp_calc.h"

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

static MW_TIMING mode(mwh_u32 clk, unsigned hd, unsigned hss, unsigned hse, unsigned ht,
                      unsigned vd, unsigned vss, unsigned vse, unsigned vt, int ph, int pv)
{
    MW_TIMING t;
    memset(&t, 0, sizeof(t));
    t.Clock = clk;
    t.HDisplay = (mwh_u16)hd; t.HSyncStart = (mwh_u16)hss; t.HSyncEnd = (mwh_u16)hse; t.HTotal = (mwh_u16)ht;
    t.VDisplay = (mwh_u16)vd; t.VSyncStart = (mwh_u16)vss; t.VSyncEnd = (mwh_u16)vse; t.VTotal = (mwh_u16)vt;
    t.PHSync = (mwh_u8)ph; t.PVSync = (mwh_u8)pv;
    return t;
}

static void test_aux(void)
{
    CHECK(MwAuxCmd(MW_AUX_NATIVE_READ, 0x000, 16) == 0x9000000Fu, "native read 16 at 0: 0x%08x",
          MwAuxCmd(MW_AUX_NATIVE_READ, 0, 16));
    CHECK(MwAuxCmd(MW_AUX_NATIVE_WRITE, 0x100, 2) == 0x80010001u, "native write 2 at 0x100");
    CHECK(MwAuxCmd(MW_AUX_I2C_READ, 0x50, 0) == 0x10005010u, "I2C address-only read: 0x%08x",
          MwAuxCmd(MW_AUX_I2C_READ, 0x50, 0));
    CHECK(MwAuxCmd(MW_AUX_I2C_WRITE | MW_AUX_I2C_MOT, 0x50, 1) == 0x40005000u, "EDID offset write with MOT");
    CHECK(MW_AUX_BYTES(17u << 19) == 17 && MW_AUX_REPLY(0x20) == 2 && MW_AUX_TIMEOUT(1u << 17), "status fields");
}

static void test_vpll(void)
{
    static const mwh_u32 clk[] = { 148500, 241500, 154000, 268500, 108000, 74250, 138500, 297000 };
    for (unsigned i = 0; i < sizeof(clk) / sizeof(clk[0]); i++) {
        MW_VPLL v;
        unsigned long long vco;
        int ok = MwVpllFor(clk[i], &v);
        CHECK(ok, "VPLL for %u kHz", clk[i]);
        if (!ok)
            continue;
        vco = 24000ull * v.M / v.P;
        CHECK(24000ull * v.M % v.P == 0, "%u: integer VCO", clk[i]);
        CHECK(vco >= 2250000 && vco <= 4500000, "%u: VCO %llu in range", clk[i], vco);
        CHECK((vco >> v.S) == v.Khz, "%u: Fout", clk[i]);
        CHECK(v.Khz == clk[i] * v.Div, "%u: exact, div %u", clk[i], v.Div);
        CHECK(v.Div % 2 == 0 && v.Div <= 16, "%u: even divider", clk[i]);
        CHECK(v.M >= 64 && v.M <= 1023 && v.P >= 1 && v.P <= 63 && v.S <= 6, "%u: field ranges", clk[i]);
    }
    {
        MW_VPLL v;
        MW_REGW w[MW_VPLL_WRITES];
        unsigned n;
        MwVpllFor(148500, &v);
        CHECK(v.Khz == 594000 && v.Div == 4 && v.M == 99 && v.P == 1 && v.S == 2,
              "1080p60: VPLL 594 MHz / 4 (the vendor's rate), m %u p %u s %u", v.M, v.P, v.S);
        n = MwVpllWrites(&v, w);
        CHECK(n == 6, "six writes");
        CHECK(w[0].Off == 0x280 && w[0].Val == 0x00300000u, "VPLL to slow mode first");
        CHECK(w[2].Off == 0x160 && w[2].Val == (0x03FF0000u | 99), "m");
        CHECK(w[3].Off == 0x164 && w[3].Val == (0x01FF0000u | 2u << 6 | 1), "p and s");
        CHECK(w[5].Off == 0x164 && w[5].Val == 0x20000000u, "power up");
        CHECK(MwVpllNormal().Val == 0x00300010u, "normal mode");
    }
    {
        MW_REGW w[MW_DCLK_WRITES];
        unsigned n = MwDclkWrites(1, MW_DCLK_VPLL, 4, w);
        CHECK(n == 5, "VP1's dclk: five writes (gates in two registers)");
        CHECK(w[2].Off == 0x548 && w[2].Val == (0x07FF0000u | 2u << 8 | 3), "CLKSEL_CON146: VPLL / 4");
    }
}

static void test_video(void)
{
    MW_TIMING t = mode(148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1);
    MW_DP_VIDEO v;

    t.Vic = 16;
    /* HBR, 2 lanes: ts = 445500 * 64 / 540 = 52800 -> 52.8 bytes per TU. */
    MwDpVideo(&t, 270000, 2, &v);
    CHECK(v.AvgTu == 52 && v.AvgTuFrac == 8, "1080p60 HBR x2: TU %u.%u", v.AvgTu, v.AvgTuFrac);
    /* t1 = 333 * 2, t2 = 67500 * 1000 / 148500 = 454, t3 = 53: 666 * 454 * 53 / 1e6 = 16 -> 40 */
    CHECK(v.InitThr == 40, "1080p60 HBR x2: init threshold %u", v.InitThr);
    CHECK(v.Vsample == 0x00210000u, "dual pixel, RGB 8 bit: 0x%08x", v.Vsample);
    CHECK(v.Pol == 3, "both syncs positive");
    CHECK(v.Cfg1 == (1920u << 16 | 280u << 2), "CONFIG1 0x%08x", v.Cfg1);
    CHECK(v.Cfg2 == (45u << 16 | 1080), "CONFIG2");
    CHECK(v.Cfg3 == (44u << 16 | 88), "CONFIG3: hsync 44, hfp 88");
    CHECK(v.Cfg4 == (5u << 16 | 4), "CONFIG4: vsync 5, vfp 4");
    CHECK(v.Msa1 == (41u << 16 | 192), "MSA1: vstart 41, hstart 192");
    CHECK(v.Msa2 == 0x20000000u, "MISC0 RGB 8 bpc");
    CHECK(v.Hblank == (0x10000u | (280u * 67500 / 148500)), "HBLANK interval 0x%08x", v.Hblank);

    /* 2560x1440@60 CVT-RB, HBR2 x2: 241500*3 = 724500; *64 / 1080 = 42933 */
    t = mode(241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, 1, 0);
    MwDpVideo(&t, 540000, 2, &v);
    CHECK(v.AvgTu == 42 && v.AvgTuFrac == 9, "1440p60 HBR2 x2: TU %u.%u", v.AvgTu, v.AvgTuFrac);
    CHECK(v.Pol == 2, "hsync positive, vsync negative");
}

static void test_drive(void)
{
    mwh_u8 d[4];
    MwUdphyDrive(0, 0, 0, d);
    CHECK(d[0] == 0x20 && d[1] == 0x10 && d[2] == 0x42 && d[3] == 0xe5, "RBR vs0 pe0");
    MwUdphyDrive(1, 2, 0, d);
    CHECK(d[0] == 0x27 && d[3] == 0x67, "HBR vs2 pe0: the typec entry");
    MwUdphyDrive(2, 0, 3, d);
    CHECK(d[0] == 0x2a && d[1] == 0x19, "HBR2 vs0 pe3");
    MwUdphyDrive(3, 3, 3, d);
    CHECK(d[0] == 0x28 && d[1] == 0x10, "HBR3 vs3: pre-emphasis clamped to 0");
    CHECK(MwUdphyTxClkInv(1) && !MwUdphyTxClkInv(2), "TXCLK_INV at RBR/HBR only");
    CHECK(MwUdphyBw(162000) == 0 && MwUdphyBw(540000) == 2, "PHY bw codes");
}

static void test_lanes(void)
{
    mwh_u8 p[2];
    /* normal: mux 0xC0 | enables 0x0C, GRF lane 0 on pad 2, lane 1 on pad 3 */
    CHECK(MwDpLanePads(0xCC, 0x0C40, p) == 2 && p[0] == 2 && p[1] == 3, "normal: pads %u %u", p[0], p[1]);
    /* flipped: mux 0x30 | 0x03, lane 0 on pad 0, lane 1 on pad 1, AUX inverted */
    CHECK(MwDpLanePads(0x33, 0x0F04, p) == 2 && p[0] == 0 && p[1] == 1, "flipped: pads %u %u", p[0], p[1]);
    CHECK(MwDpLanePads(0xC0, 0x0040, p) == 0, "USB only (no enables): no DP lanes");
}

static void test_train(void)
{
    mwh_u8 s[6] = { 0x11, 0, 0, 0, 0, 0 };
    MW_DP_TRAIN t;
    memset(&t, 0, sizeof(t));
    CHECK(MwDpCrOk(s, 2) && !MwDpEqOk(s, 2), "CR on both lanes, no EQ");
    s[0] = 0x77; s[2] = 1;
    CHECK(MwDpEqOk(s, 2), "EQ, symbol lock, interlane align");
    s[2] = 0;
    CHECK(!MwDpEqOk(s, 2), "no interlane align: not done");
    /* lane 0 asks vs 3 pe 2 (impossible: clamp vs to 1), lane 1 vs 1 pe 1 */
    s[4] = (mwh_u8)(0x3 | 2 << 2 | (1 | 1 << 2) << 4);
    CHECK(MwDpAdjust(s, 2, &t) == 1, "changed");
    CHECK(t.Vs[0] == 1 && t.Pe[0] == 2 && t.Vs[1] == 1 && t.Pe[1] == 1, "clamped %u/%u %u/%u",
          t.Vs[0], t.Pe[0], t.Vs[1], t.Pe[1]);
    CHECK(MwDpAdjust(s, 2, &t) == 0, "same request: not changed");
    CHECK(MwDpLaneSet(&t, 0) == (1 | 2 << 3), "lane set byte 0x%02x", MwDpLaneSet(&t, 0));
    s[4] = 0x03 | (3u << 2);
    MwDpAdjust(s, 1, &t);
    CHECK(t.Pe[0] == 3 && t.PeMax[0] && t.Vs[0] == 0, "pe 3: max reached, vs 0");
    CHECK(MwDpPatternDpcd(1) == 0x21 && MwDpPatternDpcd(3) == 0x23 && MwDpPatternDpcd(4) == 0x07 &&
          MwDpPatternDpcd(0) == 0, "TRAINING_PATTERN_SET values");
}

static void test_caps_and_modes(void)
{
    mwh_u8 d[16] = { 0x12, 0x14, 0xC4, 0x81, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    MW_DP_CAPS c;
    MW_TIMING t;

    MwDpCaps(d, &c);
    CHECK(c.MaxRate == 540000 && c.MaxLanes == 4 && c.Enhanced && c.Tps3 && c.Tps4 && c.Ssc && c.Coding8b10b,
          "DPCD 1.2 HBR2 x4 caps");
    CHECK(MwDpStartRate(&c) == 540000 && MwDpEqPattern(&c) == 4, "start HBR2, TPS4");
    d[1] = 0x1E;
    MwDpCaps(d, &c);
    CHECK(MwDpStartRate(&c) == 540000, "HBR3 sink: still HBR2 here");
    CHECK(MwDpLowerRate(540000) == 270000 && MwDpLowerRate(162000) == 0, "downgrade");
    CHECK(MwDpCrDelayUs(&c) == 100 && MwDpEqDelayUs(&c) == 400, "no AUX read interval: 100/400 us");

    t = mode(148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1);
    CHECK(MwDpTimingOk(&t, 270000, 2560 * 1600), "1080p60 on HBR x2");
    CHECK(!MwDpTimingOk(&t, 162000, 2560 * 1600), "1080p60 does not fit RBR x2");
    t = mode(268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646, 1, 0);
    CHECK(MwDpTimingOk(&t, 540000, 2560 * 1600), "2560x1600@60 RB on HBR2 x2");
    t = mode(297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 1, 1);
    CHECK(!MwDpTimingOk(&t, 540000, 4096 * 2304), "4K30: wider than VP1");
    t = mode(148500, 1920, 2009, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1);
    CHECK(!MwDpTimingOk(&t, 270000, 2560 * 1600), "odd front porch refused");
    {
        MW_TIMING m[16];
        mwh_u32 n = MwDpModes(NULL, 0, 270000, 2560 * 1600, m, 16);
        CHECK(n == 2 && m[0].HDisplay == 1920 && m[1].HDisplay == 1280, "no EDID: the safe pair, %u", n);
    }
}

static void test_vop(void)
{
    MW_TIMING t = mode(148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1);
    MW_VP_REGS r;
    MW_DUAL_WIN w;

    MwVpRegsPort(&t, 0, 1, &r);
    CHECK(r.IfCtrl == 0x80100037u, "DP0_IF_CTRL on VP1: 0x%08x", r.IfCtrl);
    CHECK(MwDp0IfStaged(r.IfCtrl) == 0x80100036u, "staged without EN");
    CHECK(r.ClkCtrl == 0 && r.DspCtrl == MW_VP_OUT_MODE_AAAA, "VP1: one pixel a clock, AAAA");
    CHECK(MwCfgDone(1) == 0x00028002u, "CFG_DONE VP1");
    CHECK(MwEsmartLayerId(MW_DP_ESMART, 1) == 2, "Esmart1 is layer 2 on VP1");
    CHECK(MwDpDualWin(MW_DPX_DP, &w) && w.Vp == 1 && w.Esmart == 1 && w.Mst == 0x1A14, "DP's window");
    CHECK(MwDpDualWin(MW_DPX_HDMI, &w) && w.Vp == 0 && w.Esmart == 0 && w.Mst == 0x1814, "HDMI's window");
    {
        MW_REGW o[MW_OVL_WRITES];
        unsigned n = MwOvlWrites(1, 2, o);
        CHECK(o[0].Off == 0x700 && o[1].Off == 0x704 && o[1].Val == 0xFFF2 && o[n - 1].Off == 0x770 &&
              o[n - 1].Val == 0x10000000u, "VP1 overlay");
    }
}

int main(void)
{
    test_aux();
    test_vpll();
    test_video();
    test_drive();
    test_lanes();
    test_train();
    test_caps_and_modes();
    test_vop();
    printf("dp-host: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
