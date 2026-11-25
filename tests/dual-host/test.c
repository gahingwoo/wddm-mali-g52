// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/dual_calc.h (HdmiExtend, docs/DUAL-DISPLAY.md) against
 * what is known to work on the TaishanPi 3M:
 *
 *  - The panel on VP2 as the vendor 6.1 kernel ran it, from its register
 *    dump of 2026-11-17 (tspi3m-backup/dsi-linux-dump-2026-11-17,
 *    vop2-active-regs.txt): MIPI0_IF_CTRL 0x8010003b, VP2 timing words
 *    (0xe30 0x019f0008, 0xe34 0x00380358, 0xe38 0x00160516, 0xe48
 *    0x038c0008, 0xe4c 0x00380358, 0xe50 0x05250006, 0xe54 0x00160516),
 *    VP2_CLK_CTRL 0, BG_DLY 16 in 0x870's top byte, the window's size words
 *    0x04ff031f and VIR 0x320, OVL_LAYER_SEL(2) 0xfff2 for its Esmart2.
 *  - dclk_vp2 = CPLL / 15 = 66.67 MHz for the 71 MHz panel (clk_summary.txt).
 *  - For VP0, the four CRU writes of hdmi.c's DclkToCru, and CFG_DONE
 *    0x00018001 as scanout.c writes it.
 *  - The topology rules, the panel's port for each combination of outputs,
 *    and the steps between every pair of arrangements: never a port taken
 *    from the panel while its backlight is on, never the panel on two ports.
 */
#include <stdio.h>
#include <string.h>
#include "../../drivers/maliwddm/dual_calc.h"

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

/* The TaishanPi 3M's panel, as hpd.c's TspiPanel has it. */
static MW_TIMING panel(void)
{
    MW_TIMING t;
    memset(&t, 0, sizeof(t));
    t.Clock = 71000;
    t.HDisplay = 800;
    t.HSyncStart = 852;
    t.HSyncEnd = 860;
    t.HTotal = 908;
    t.VDisplay = 1280;
    t.VSyncStart = 1295;
    t.VSyncEnd = 1301;
    t.VTotal = 1317;
    return t;
}

static mwh_u32 find(const MW_REGW *W, unsigned N, mwh_u32 Off, int *Found)
{
    for (unsigned i = 0; i < N; i++)
        if (W[i].Off == Off) {
            *Found = 1;
            return W[i].Val;
        }
    *Found = 0;
    return 0;
}

static void test_vp2_against_vendor(void)
{
    MW_TIMING t = panel();
    MW_VP_REGS r;
    MW_REGW w[MW_ESMART_WRITES], o[MW_OVL_WRITES];
    unsigned n;
    int f;

    MwVpRegsPort(&t, 1, 2, &r);
    CHECK(r.IfCtrl == 0x8010003bu, "MIPI0 on VP2 0x%08x", r.IfCtrl);
    CHECK(r.PreScan == 0x019f0008u, "VP2 PRE_SCAN 0x%08x", r.PreScan);
    CHECK(r.PostHAct == 0x00380358u, "VP2 POST_DSP_HACT 0x%08x", r.PostHAct);
    CHECK(r.PostVAct == 0x00160516u, "VP2 POST_DSP_VACT 0x%08x", r.PostVAct);
    CHECK(r.HTotalHsEnd == 0x038c0008u, "VP2 HTOTAL 0x%08x", r.HTotalHsEnd);
    CHECK(r.HActStEnd == 0x00380358u, "VP2 HACT 0x%08x", r.HActStEnd);
    CHECK(r.VTotalVsEnd == 0x05250006u, "VP2 VTOTAL 0x%08x", r.VTotalVsEnd);
    CHECK(r.VActStEnd == 0x00160516u, "VP2 VACT 0x%08x", r.VActStEnd);
    CHECK(r.ClkCtrl == 0, "VP2_CLK_CTRL %u", r.ClkCtrl);
    CHECK(r.DspCtrl == MW_VP_OUT_MODE_P888, "VP2 out mode P888, no pre-dither: 0x%08x", r.DspCtrl);
    CHECK(r.PostSclFactor == 0x10001000u, "VP2 post scaler identity 0x%08x", r.PostSclFactor);

    n = MwEsmartWrites(1, 2, 800, 1280, 3200, 0x12340000u, w);
    CHECK(n == MW_ESMART_WRITES, "Esmart1 writes %u", n);
    CHECK(find(w, n, 0x1AF4, &f) == 2 && f, "Esmart1 PORT_SEL 2");
    CHECK(find(w, n, 0x1AF8, &f) == 0 && f, "Esmart1 DLY_NUM 0 (reset value 0x17)");
    CHECK(find(w, n, 0x1A20, &f) == 0x04ff031fu && f, "Esmart1 ACT_INFO as the vendor's window");
    CHECK(find(w, n, 0x1A24, &f) == 0x04ff031fu && f, "Esmart1 DSP_INFO as the vendor's window");
    CHECK(find(w, n, 0x1A1C, &f) == 0x320u && f, "Esmart1 VIR 800 words");
    CHECK(find(w, n, 0x1A14, &f) == 0x12340000u && f, "Esmart1 address");
    CHECK(find(w, n, 0x1A04, &f) == (0x12u << 4 | 0x13u << 12) && f, "Esmart1 AXI read IDs 0x12/0x13 (firmware table)");
    CHECK(find(w, n, 0x1A08, &f) == 0x00010000u && f, "Esmart1 AXI bus 0, bit 16 as in the dump");
    CHECK(find(w, n, 0x1AD8, &f) == 0xFFFFFFFFu && f, "Esmart1 alpha map opaque");
    CHECK(find(w, n, 0x1A34, &f) == 0x10001000u && f, "Esmart1 scaler identity");
    CHECK(MwEsmartEnable(1, 1).Off == 0x1A10 && MwEsmartEnable(1, 1).Val == 1, "Esmart1 enable");
    CHECK(MwEsmartEnable(1, 0).Val == 0, "Esmart1 disable");
    /* Esmart2 as the vendor kernel ran it: bus 1 (AXI_CTRL 0x00010002 in the dump). */
    n = MwEsmartWrites(2, 2, 800, 1280, 3200, 0, w);
    CHECK(find(w, n, 0x1C08, &f) == 0x00010002u && f, "Esmart2 AXI_CTRL as the dump");
    CHECK(find(w, n, 0x1CF4, &f) == 2 && f, "Esmart2 PORT_SEL as the dump");

    CHECK(MwEsmartLayerId(2, 2) == 2, "Esmart2 on VP2 is layer id 2 (dump 0xfff2)");
    CHECK(MwEsmartLayerId(1, 2) == 1, "Esmart1 on VP2 is layer id 1");
    CHECK(MwEsmartLayerId(0, 0) == 2, "Esmart0 on VP0 is layer id 2");
    CHECK(MwEsmartLayerId(1, 0) == 0xF, "Esmart1 cannot go on VP0");
    n = MwOvlWrites(2, MwEsmartLayerId(2, 2), o);
    CHECK(n == MW_OVL_WRITES, "overlay writes %u", n);
    CHECK(find(o, n, 0x804, &f) == 0xfff2u && f, "OVL_LAYER_SEL(2) as the dump");
    CHECK((find(o, n, 0x870, &f) >> 24) == 0x10 && f, "BG_DLY 16 on VP2 as the dump");
    CHECK(find(o, n, 0x800, &f) == 0 && f, "OVL_CTRL(2) RGB");
    CHECK(find(o, n, 0x820, &f) == 0x00FF01A1u && find(o, n, 0x84C, &f) == 0x74u, "VP2 mixers, the firmware's values");
    n = MwOvlWrites(0, 2, o);
    CHECK(find(o, n, 0x604, &f) == 0xfff2u && (find(o, n, 0x670, &f) >> 24) == 20, "VP0 overlay: Esmart0, BG_DLY 20");
    n = MwOvlWrites(2, 0xF, o);
    CHECK(find(o, n, 0x804, &f) == 0xffffu, "a parked VP2 has every layer off");
}

static void test_clock(void)
{
    MW_REGW w[MW_DCLK_WRITES];
    unsigned n, div;

    div = MwDclkDiv(1000000, 71000);
    CHECK(div == 15, "CPLL / 15 for 71 MHz, got %u", div);
    CHECK(1000000 / div == 66666, "66.67 MHz as Linux's dclk_vp2");
    CHECK(MwDclkDiv(1000000, 1000000) == 1 && MwDclkDiv(1000000, 1000) == 256, "divider clamped to 1..256");
    CHECK(MwDclkDiv(0, 71000) == 0 && MwDclkDiv(1000000, 0) == 0, "no clock, no divider");
    CHECK(MwDclkDiv(1188000, 71000) == 17, "GPLL: rounded up, never above the mode");

    /* VP0: exactly hdmi.c's DclkToCru(H, 15). */
    n = MwDclkWrites(0, MW_DCLK_CPLL, 15, w);
    CHECK(n == 4, "VP0 four writes, got %u", n);
    CHECK(w[0].Off == 0x8F4 && w[0].Val == 0x20002000u, "gate DCLK_VP0 0x%x 0x%08x", w[0].Off, w[0].Val);
    CHECK(w[1].Off == 0x54C && w[1].Val == 0x08000000u, "DCLK_VP0 on its source 0x%08x", w[1].Val);
    CHECK(w[2].Off == 0x544 && w[2].Val == (0x7FFu << 16 | 1u << 8 | 14), "dclk_vp0_src CPLL/15 0x%08x", w[2].Val);
    CHECK(w[3].Off == 0x8F4 && w[3].Val == ((1u << 10 | 1u << 13) << 16), "ungate 0x%08x", w[3].Val);

    /* VP2: CLKSEL_CON147 [10:0] and bit 13, GATE_CON61 bit 12, GATE_CON62 bit 1. */
    n = MwDclkWrites(2, MW_DCLK_CPLL, 15, w);
    CHECK(n == 5, "VP2 five writes, got %u", n);
    CHECK(w[0].Off == 0x8F8 && w[0].Val == 0x00020002u, "gate DCLK_VP2 0x%x 0x%08x", w[0].Off, w[0].Val);
    CHECK(w[1].Off == 0x54C && w[1].Val == 0x20000000u, "DCLK_VP2 on dclk_vp2_src 0x%08x", w[1].Val);
    CHECK(w[2].Off == 0x54C && w[2].Val == (0x7FFu << 16 | 1u << 8 | 14), "dclk_vp2_src CPLL/15 0x%08x", w[2].Val);
    CHECK((w[2].Val >> 16 & (1u << 11 | 1u << 12 | 1u << 13)) == 0, "the source write leaves the three DCLK muxes alone");
    CHECK(w[3].Off == 0x8F4 && w[3].Val == (1u << 12) << 16, "ungate dclk_vp2_src");
    CHECK(w[4].Off == 0x8F8 && w[4].Val == (1u << 1) << 16, "ungate DCLK_VP2");
    CHECK(MwDclkWrites(3, MW_DCLK_CPLL, 15, w) == 0 && MwDclkWrites(2, MW_DCLK_CPLL, 0, w) == 0, "refused");
}

static void test_topology(void)
{
    MW_DUAL_PATH p[3];
    mwh_u8 t[2];

    CHECK(MwDualCheck(p, 0, 2) == MW_DUAL_OK, "no path: everything off");
    p[0] = (MW_DUAL_PATH){ 0, 0, 1 };
    CHECK(MwDualCheck(p, 1, 2) == MW_DUAL_OK, "panel alone on source 0");
    p[0] = (MW_DUAL_PATH){ 0, 1, 1 };
    CHECK(MwDualCheck(p, 1, 2) == MW_DUAL_OK, "HDMI alone on source 0");
    p[0] = (MW_DUAL_PATH){ 1, 1, 1 };
    CHECK(MwDualCheck(p, 1, 2) == MW_DUAL_OK, "HDMI alone on source 1");
    p[0] = (MW_DUAL_PATH){ 0, 0, 1 };
    p[1] = (MW_DUAL_PATH){ 1, 1, 1 };
    CHECK(MwDualCheck(p, 2, 2) == MW_DUAL_OK, "extend");
    MwDualMap(p, 2, t);
    CHECK(t[0] == 0 && t[1] == 1, "extend map %u %u", t[0], t[1]);
    p[0] = (MW_DUAL_PATH){ 0, 1, 0 };
    p[1] = (MW_DUAL_PATH){ 1, 0, 1 };
    CHECK(MwDualCheck(p, 2, 2) == MW_DUAL_OK, "extend the other way round, source 0 rotated");
    MwDualMap(p, 2, t);
    CHECK(t[0] == 1 && t[1] == 0, "swapped map");
    p[1] = (MW_DUAL_PATH){ 1, 0, 0 };
    CHECK(MwDualCheck(p, 2, 2) == MW_DUAL_ROTATED, "source 1 rotated");
    p[0] = (MW_DUAL_PATH){ 0, 0, 1 };
    p[1] = (MW_DUAL_PATH){ 0, 1, 1 };
    CHECK(MwDualCheck(p, 2, 2) == MW_DUAL_CLONE, "clone refused");
    p[1] = (MW_DUAL_PATH){ 1, 0, 1 };
    CHECK(MwDualCheck(p, 2, 2) == MW_DUAL_TGT_TWICE, "two sources on the panel");
    p[1] = (MW_DUAL_PATH){ 2, 1, 1 };
    CHECK(MwDualCheck(p, 2, 2) == MW_DUAL_BAD_ID, "source 2");
    p[1] = (MW_DUAL_PATH){ 1, 2, 1 };
    CHECK(MwDualCheck(p, 2, 2) == MW_DUAL_BAD_ID, "target 2");
    p[2] = (MW_DUAL_PATH){ 1, 1, 1 };
    CHECK(MwDualCheck(p, 3, 2) == MW_DUAL_TOO_MANY, "three paths");
    MwDualMap(p, 0, t);
    CHECK(t[0] == MW_DUAL_NONE && t[1] == MW_DUAL_NONE, "empty map");

    CHECK(MwDualPanelVpFor(1, 0, MW_PANEL_VP2) == MW_PANEL_VP0, "panel alone: VP0, as the single display");
    CHECK(MwDualPanelVpFor(1, 1, MW_PANEL_VP0) == MW_PANEL_VP2, "both: panel on VP2");
    CHECK(MwDualPanelVpFor(0, 1, MW_PANEL_VP2) == MW_PANEL_OFF, "HDMI alone: panel off");
    for (unsigned c = 0; c < 3; c++) {
        unsigned cur = c == 0 ? MW_PANEL_VP0 : c == 1 ? MW_PANEL_VP2 : MW_PANEL_OFF;
        CHECK(MwDualPanelVpFor(0, 0, cur) == cur, "nothing on: stays");
    }
}

/* Every step list between every pair of arrangements, run on a model of
 * the hardware: where the panel is, whether VP0 is HDMI's, the backlight. */
static void test_steps(void)
{
    static const unsigned vps[3] = { MW_PANEL_VP0, MW_PANEL_VP2, MW_PANEL_OFF };
    mwh_u8 s[8];
    unsigned n;

    for (unsigned a = 0; a < 3; a++)
        for (unsigned b = 0; b < 3; b++)
            for (int hdmi = 0; hdmi <= 1; hdmi++) {
                unsigned cur = vps[a], next = vps[b];
                /* The model. The panel's port; VP0 is the panel's when it is
                 * on VP0, else HDMI's or nobody's; the backlight is on when
                 * the panel has a port. */
                unsigned panel = cur;
                int bl = cur != MW_PANEL_OFF, vp0hdmi = cur != MW_PANEL_VP0, ok = 1, sawHdmi = 0;
                int hdmiNeeded = hdmi && next != MW_PANEL_VP0;

                if (next == MW_PANEL_OFF && !hdmi)
                    continue;   /* the panel off means HDMI alone (MwDualPanelVpFor) */

                n = MwDualSteps(cur, next, hdmiNeeded, s);
                CHECK(n <= 8, "%u steps", n);
                for (unsigned i = 0; i < n; i++) {
                    switch (s[i]) {
                    case MW_DSTEP_PARK:
                        ok &= panel == MW_PANEL_VP2;
                        /* Going dark for good: the backlight first. */
                        if (next == MW_PANEL_OFF)
                            ok &= !bl;
                        panel = MW_PANEL_OFF;
                        break;
                    case MW_DSTEP_PANEL_VP0:
                        ok &= panel == MW_PANEL_OFF;    /* never on two ports */
                        panel = MW_PANEL_VP0;
                        vp0hdmi = 0;
                        break;
                    case MW_DSTEP_PANEL_VP2:
                        ok &= panel != MW_PANEL_VP2;
                        if (panel == MW_PANEL_VP0)
                            vp0hdmi = 0;        /* VP0 in standby, nobody's */
                        panel = MW_PANEL_VP2;
                        break;
                    case MW_DSTEP_HDMI:
                        /* The mode set takes VP0, and MIPI0 off it if the
                         * panel was there (hdmi.c VpProgram): only dark. */
                        if (panel == MW_PANEL_VP0) {
                            ok &= !bl;
                            panel = MW_PANEL_OFF;
                        }
                        vp0hdmi = 1;
                        sawHdmi = 1;
                        break;
                    case MW_DSTEP_BL_ON:
                        ok &= panel != MW_PANEL_OFF;
                        bl = 1;
                        break;
                    case MW_DSTEP_BL_OFF:
                        bl = 0;
                        break;
                    default:
                        ok = 0;
                    }
                }
                CHECK(ok, "steps from %u to %u valid", cur, next);
                CHECK(panel == next, "from %u to %u the panel ends on %u", cur, next, panel);
                CHECK(bl == (next != MW_PANEL_OFF), "from %u to %u backlight %d", cur, next, bl);
                CHECK(!hdmiNeeded || (sawHdmi && vp0hdmi), "from %u to %u HDMI set", cur, next);
                CHECK(n > 0 || (cur == next && !hdmiNeeded), "from %u to %u: steps when anything changes", cur, next);
            }

    /* The common ones, spelled out. */
    n = MwDualSteps(MW_PANEL_VP0, MW_PANEL_VP2, 1, s);
    CHECK(n == 2 && s[0] == MW_DSTEP_PANEL_VP2 && s[1] == MW_DSTEP_HDMI, "plug HDMI, extend: panel to VP2 then HDMI");
    CHECK(MwDualStepsPacked(s, n) == 0x43u, "packed 0x%x", MwDualStepsPacked(s, n));
    n = MwDualSteps(MW_PANEL_VP2, MW_PANEL_VP0, 0, s);
    CHECK(n == 2 && s[0] == MW_DSTEP_PARK && s[1] == MW_DSTEP_PANEL_VP0, "unplug: park VP2, panel back on VP0");
    n = MwDualSteps(MW_PANEL_VP2, MW_PANEL_OFF, 1, s);
    CHECK(n == 3 && s[0] == MW_DSTEP_BL_OFF && s[1] == MW_DSTEP_PARK && s[2] == MW_DSTEP_HDMI, "second screen only");
    n = MwDualSteps(MW_PANEL_OFF, MW_PANEL_VP2, 1, s);
    CHECK(n == 3 && s[0] == MW_DSTEP_PANEL_VP2 && s[1] == MW_DSTEP_BL_ON && s[2] == MW_DSTEP_HDMI, "HDMI only to extend");
    n = MwDualSteps(MW_PANEL_VP0, MW_PANEL_VP0, 0, s);
    CHECK(n == 0, "panel alone stays: nothing written");
}

static void test_windows(void)
{
    MW_DUAL_WIN w;

    CHECK(MwDualWin(MW_DUAL_HDMI, MW_PANEL_VP2, &w) && w.Esmart == 0 && w.Vp == 0 && w.Mst == 0x1814 &&
              w.CfgDone == 0x00018001u, "HDMI: Esmart0 on VP0 as scanout.c");
    CHECK(MwDualWin(MW_DUAL_PANEL, MW_PANEL_VP0, &w) && w.Mst == 0x1814 && w.Vir == 0x181C && w.CfgDone == 0x00018001u,
          "panel alone: Esmart0 on VP0, the single display's");
    CHECK(MwDualWin(MW_DUAL_PANEL, MW_PANEL_VP2, &w) && w.Esmart == 1 && w.Vp == 2 && w.Mst == 0x1A14 &&
              w.Vir == 0x1A1C && w.CfgDone == 0x00048004u, "panel with HDMI: Esmart1 on VP2, CFG_DONE 0x%08x", w.CfgDone);
    CHECK(!MwDualWin(MW_DUAL_PANEL, MW_PANEL_OFF, &w), "panel off: no window");
    CHECK(!MwDualWin(5, MW_PANEL_VP0, &w), "no such target");
    CHECK(MwCfgDone(1) == 0x00028002u, "VP1 CFG_DONE");
    /* Each window may go on its port (mainline possible_vp_mask). */
    CHECK(MwEsmartLayerId(0, 0) != 0xF && MwEsmartLayerId(1, 2) != 0xF, "windows allowed on their ports");
}

static void test_source_of(void)
{
    MW_DUAL_SRCSIZE s[2] = { { 1, 800, 1280 }, { 1, 2560, 1440 } };

    CHECK(MwDualSourceOf(800, 1280, s) == 0, "panel primary is source 0's");
    CHECK(MwDualSourceOf(2560, 1440, s) == 1, "HDMI primary is source 1's");
    CHECK(MwDualSourceOf(1920, 1080, s) == 0, "unknown size: source 0");
    s[1].Active = 0;
    CHECK(MwDualSourceOf(2560, 1440, s) == 0, "source 1 off: source 0");
    s[0] = (MW_DUAL_SRCSIZE){ 1, 2560, 1440 };
    s[1] = (MW_DUAL_SRCSIZE){ 1, 800, 1280 };
    CHECK(MwDualSourceOf(800, 1280, s) == 1 && MwDualSourceOf(2560, 1440, s) == 0, "swapped sources");
    s[1] = (MW_DUAL_SRCSIZE){ 1, 2560, 1440 };
    CHECK(MwDualSourceOf(2560, 1440, s) == 0, "same size on both: source 0");
}

int main(void)
{
    test_vp2_against_vendor();
    test_clock();
    test_topology();
    test_steps();
    test_windows();
    test_source_of();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
