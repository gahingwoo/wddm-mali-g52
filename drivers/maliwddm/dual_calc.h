/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Two displays at once on the TaishanPi 3M (HdmiExtend, docs/DUAL-DISPLAY.md),
 * the arithmetic and the decisions: no register is touched here.
 *
 * The hardware arrangement is what Linux runs on this board (Armbian 7.3.5
 * mainline, and the vendor 6.1 kernel's register dump of 2026-11-17 in
 * tspi3m-backup/dsi-linux-dump-2026-11-17): HDMI on VP0, the MIPI DSI panel
 * on VP2. The firmware and the single-display driver put the panel on VP0,
 * and that stays so whenever the panel is the only output in use; the
 * panel moves to VP2 only while both outputs are.
 *
 *   window      VP0 shows Esmart0 (as today); VP2 shows Esmart1. Mainline
 *               rk3576_vop_win_data: Esmart0 possible_vp_mask VP0|VP2,
 *               Esmart1 VP1|VP2, layer_sel_id on VP2: Esmart0 0, Esmart1 1,
 *               Esmart2 2 (the vendor dump's OVL_LAYER_SEL(2) 0xfff2 with
 *               Esmart2), Esmart3 3; AXI bus and read IDs as the firmware's
 *               Vop2SetSmartWin table (Esmart1: bus 0, 0x12, 0x13).
 *   overlay     RK3576_OVL_CTRL(vp) 0x600 + vp * 0x100, OVL_LAYER_SEL
 *               0x604 + vp * 0x100 (0xf: layer off), the three mixers with
 *               the firmware's four values (memory:
 *               project_rk3576_vop2_mixers_stripes), BG_MIX_CTRL 0x670 +
 *               vp * 0x100 with BG_DLY 20 on VP0 and 16 on VP1/VP2
 *               (rk3576_vop2_setup_bg_dly).
 *   clock       dclk_vp2_src from CPLL (CLKSEL_CON147 [10:8] = 1, [7:0] =
 *               div - 1), DCLK_VP2 on it (CON147 bit 13 = 0), gates
 *               GATE_CON61 bit 12 and GATE_CON62 bit 1 (mainline
 *               clk-rk3576.c). The divider rounds up as the firmware's
 *               Vop2IfConfig does: CPLL 1 GHz / 15 = 66.67 MHz for the
 *               panel's 71 MHz mode, as Linux has it (clk_summary.txt).
 *   port        MIPI0_IF_CTRL MUX 2 with PCLK_DIV: 0x8010003b
 *               (hdmi_calc.h MwVpRegsPort, the vendor dump's value).
 *
 * tests/dual-host checks all of it against the vendor dump and against
 * the single-display driver's own sequences.
 *
 * Plain C with no WDK header, so that the host test can build it.
 */
#pragma once
#include "hdmi_calc.h"

/* ---- targets, sources, and where the panel's pixels come from ---- */

#define MW_DUAL_PANEL           0u      /* target (child) 0 on a DSI boot: the panel */
#define MW_DUAL_HDMI            1u      /* target (child) 1: HDMI */
#define MW_DUAL_SOURCES         2u
#define MW_DUAL_NONE            0xFFu

#define MW_PANEL_VP0            0u      /* the firmware's arrangement, and the single display's */
#define MW_PANEL_VP2            2u      /* while both outputs are in use */
#define MW_PANEL_OFF            0xFu    /* no port drives MIPI0 (HDMI alone) */

typedef struct _MW_DUAL_PATH {
    mwh_u8 Src, Tgt;
    mwh_u8 RotIdentity;         /* 1: the path's rotation is identity or not pinned */
} MW_DUAL_PATH;

/* IsSupportedVidPn's answer for a topology (hpd.c reports it as Dual_IsSup):
 * 0 fine; 1 a source with two paths (clone, not offered); 2 a target with
 * two paths; 3 a source or target that does not exist; 4 more paths than
 * there are targets; 5 source 1 rotated (only source 0 rotates, display.c's
 * rotation is the adapter's, not per source). No path at all is fine: it is
 * everything being switched off. */
#define MW_DUAL_OK              0u
#define MW_DUAL_CLONE           1u
#define MW_DUAL_TGT_TWICE       2u
#define MW_DUAL_BAD_ID          3u
#define MW_DUAL_TOO_MANY        4u
#define MW_DUAL_ROTATED         5u

MW_HC_INLINE unsigned MwDualCheck(const MW_DUAL_PATH *P, unsigned N, unsigned Targets)
{
    if (N > Targets || N > MW_DUAL_SOURCES)
        return MW_DUAL_TOO_MANY;
    for (unsigned i = 0; i < N; i++) {
        if (P[i].Src >= MW_DUAL_SOURCES || P[i].Tgt >= Targets)
            return MW_DUAL_BAD_ID;
        for (unsigned j = 0; j < i; j++) {
            if (P[j].Src == P[i].Src)
                return MW_DUAL_CLONE;
            if (P[j].Tgt == P[i].Tgt)
                return MW_DUAL_TGT_TWICE;
        }
        if (P[i].Src != 0 && !P[i].RotIdentity)
            return MW_DUAL_ROTATED;
    }
    return MW_DUAL_OK;
}

/* Each source's target in a checked topology, MW_DUAL_NONE for a source
 * without a path. */
MW_HC_INLINE void MwDualMap(const MW_DUAL_PATH *P, unsigned N, mwh_u8 Tgt[MW_DUAL_SOURCES])
{
    for (unsigned s = 0; s < MW_DUAL_SOURCES; s++)
        Tgt[s] = MW_DUAL_NONE;
    for (unsigned i = 0; i < N; i++)
        if (P[i].Src < MW_DUAL_SOURCES)
            Tgt[P[i].Src] = P[i].Tgt;
}

/* Where the panel goes for the outputs in use. Nothing in use (every path
 * switched off): leave it where it is. */
MW_HC_INLINE unsigned MwDualPanelVpFor(int PanelOn, int HdmiOn, unsigned Cur)
{
    if (PanelOn && HdmiOn)
        return MW_PANEL_VP2;
    if (PanelOn)
        return MW_PANEL_VP0;
    if (HdmiOn)
        return MW_PANEL_OFF;
    return Cur;
}

/* ---- the steps from one arrangement to the next ----
 *
 * hpd.c carries them out in this order, each under HwLock:
 *   PARK       VP2 to standby, Esmart1 off, VP2's layer off, MIPI0 off VP2
 *   PANEL_VP0  the panel on VP0 (hdmi.c MwHdmiDsiRestore, board-proven)
 *   PANEL_VP2  the panel onto VP2; VP0 to standby first if it was the
 *              panel's, else VP0 (HDMI) is not touched
 *   HDMI       HDMI on VP0 at the target's timing, if VP0 does not already
 *              drive it; MIPI0 is left alone while the panel is on VP2
 *   BL_ON/OFF  the panel's backlight
 * The backlight goes off before the panel loses its port and on after it
 * has one again, as the single-display switch does it (hpd.c Apply). */
#define MW_DSTEP_PARK           1u
#define MW_DSTEP_PANEL_VP0      2u
#define MW_DSTEP_PANEL_VP2      3u
#define MW_DSTEP_HDMI           4u
#define MW_DSTEP_BL_ON          5u
#define MW_DSTEP_BL_OFF         6u
#define MW_DSTEP_MAX            6u

MW_HC_INLINE unsigned MwDualSteps(unsigned Cur, unsigned Next, int HdmiOn, mwh_u8 *Out)
{
    unsigned n = 0;

    switch (Next) {
    case MW_PANEL_VP0:
        if (Cur == MW_PANEL_VP2)
            Out[n++] = MW_DSTEP_PARK;
        if (Cur != MW_PANEL_VP0)
            Out[n++] = MW_DSTEP_PANEL_VP0;
        if (Cur == MW_PANEL_OFF)
            Out[n++] = MW_DSTEP_BL_ON;
        break;
    case MW_PANEL_VP2:
        if (Cur != MW_PANEL_VP2)
            Out[n++] = MW_DSTEP_PANEL_VP2;
        if (Cur == MW_PANEL_OFF)
            Out[n++] = MW_DSTEP_BL_ON;
        if (HdmiOn)
            Out[n++] = MW_DSTEP_HDMI;
        break;
    case MW_PANEL_OFF:
        if (Cur != MW_PANEL_OFF)
            Out[n++] = MW_DSTEP_BL_OFF;
        if (Cur == MW_PANEL_VP2)
            Out[n++] = MW_DSTEP_PARK;
        if (HdmiOn)
            Out[n++] = MW_DSTEP_HDMI;
        break;
    default:
        break;
    }
    return n;
}

/* The steps packed for the trace, a nibble each, first step lowest. */
MW_HC_INLINE mwh_u32 MwDualStepsPacked(const mwh_u8 *S, unsigned N)
{
    mwh_u32 v = 0;
    for (unsigned i = 0; i < N && i < 8; i++)
        v |= (mwh_u32)(S[i] & 0xF) << (4 * i);
    return v;
}

/* ---- the window a target is shown through ---- */

#define MW_VOP_CFG_DONE         0x000u
#define MW_ESMART_BASE(i)       (0x1800u + (mwh_u32)(i) * 0x200u)
#define MW_ESMART_CTRL1         0x04u
#define MW_ESMART_AXI_CTRL      0x08u
#define MW_ESMART_REGION0_CTRL  0x10u
#define MW_ESMART_YRGB_MST      0x14u
#define MW_ESMART_VIR           0x1Cu
#define MW_ESMART_ACT_INFO      0x20u
#define MW_ESMART_DSP_INFO      0x24u
#define MW_ESMART_DSP_ST        0x28u
#define MW_ESMART_SCL_CTRL      0x30u
#define MW_ESMART_SCL_FACTOR    0x34u
#define MW_ESMART_ALPHA_MAP     0xD8u
#define MW_ESMART_PORT_SEL      0xF4u
#define MW_ESMART_DLY_NUM       0xF8u

/* CFG_DONE for one port: CFG_DONE_EN, the port's bit and its write mask
 * (Vop2SetSmartWin: CFG_DONE_EN | BIT(vp) | BIT(vp) << 16). VP0 is the
 * 0x00018001 that scanout.c and hdmi.c write. */
MW_HC_INLINE mwh_u32 MwCfgDone(unsigned Vp)
{
    return 0x8000u | (1u << Vp) | (1u << (Vp + 16));
}

typedef struct _MW_DUAL_WIN {
    mwh_u32 Esmart;             /* 0..3 */
    mwh_u32 Vp;
    mwh_u32 Mst;                /* the window's YRGB_MST register */
    mwh_u32 Vir;
    mwh_u32 CfgDone;
} MW_DUAL_WIN;

/* FALSE when nothing shows the target in this arrangement (the panel with
 * MW_PANEL_OFF). */
MW_HC_INLINE int MwDualWin(unsigned Target, unsigned PanelVp, MW_DUAL_WIN *W)
{
    unsigned esmart, vp;

    if (Target == MW_DUAL_HDMI) {
        esmart = 0;
        vp = 0;
    } else if (Target == MW_DUAL_PANEL && PanelVp == MW_PANEL_VP0) {
        esmart = 0;
        vp = 0;
    } else if (Target == MW_DUAL_PANEL && PanelVp == MW_PANEL_VP2) {
        esmart = 1;
        vp = 2;
    } else {
        return 0;
    }
    W->Esmart = esmart;
    W->Vp = vp;
    W->Mst = MW_ESMART_BASE(esmart) + MW_ESMART_YRGB_MST;
    W->Vir = MW_ESMART_BASE(esmart) + MW_ESMART_VIR;
    W->CfgDone = MwCfgDone(vp);
    return 1;
}

/* ---- register writes, as lists hdmi.c writes in order ---- */

typedef struct _MW_REGW {
    mwh_u32 Off, Val;
} MW_REGW;

/* An Esmart window on a port, showing W x H at Addr with Pitch bytes per
 * line, ARGB8888, no scaling, at the origin. Only registers the firmware or
 * mainline write on a window enable, each written whole: the VOP2 is not
 * read back (hdmi.c). CTRL0 is left alone (its reset value has the
 * window's scale engine number, the vendor kernel's scale_engine_num, and
 * zero CSC/R2Y, which is RGB in and out). AXI_CTRL keeps bit 16, set in
 * every Esmart of the vendor dump, enabled or not. */
#define MW_ESMART_WRITES        12u
MW_HC_INLINE unsigned MwEsmartWrites(unsigned Esmart, unsigned Vp, unsigned W, unsigned H, unsigned Pitch,
                                     mwh_u32 Addr, MW_REGW *Out)
{
    static const mwh_u8 bus[4] = { 0, 0, 1, 1 };
    static const mwh_u8 yrid[4] = { 0x10, 0x12, 0x0A, 0x0C };
    static const mwh_u8 uvrid[4] = { 0x11, 0x13, 0x0B, 0x0D };
    mwh_u32 b = MW_ESMART_BASE(Esmart & 3);
    mwh_u32 size = ((mwh_u32)H - 1) << 16 | (((mwh_u32)W - 1) & 0xFFFF);
    unsigned n = 0;

    Out[n].Off = b + MW_ESMART_PORT_SEL;   Out[n++].Val = Vp & 3;
    Out[n].Off = b + MW_ESMART_DLY_NUM;    Out[n++].Val = 0;
    Out[n].Off = b + MW_ESMART_AXI_CTRL;   Out[n++].Val = 0x00010000u | (mwh_u32)bus[Esmart & 3] << 1;
    Out[n].Off = b + MW_ESMART_CTRL1;      Out[n++].Val = (mwh_u32)yrid[Esmart & 3] << 4 | (mwh_u32)uvrid[Esmart & 3] << 12;
    Out[n].Off = b + MW_ESMART_ALPHA_MAP;  Out[n++].Val = 0xFFFFFFFFu;
    Out[n].Off = b + MW_ESMART_SCL_CTRL;   Out[n++].Val = 0;
    Out[n].Off = b + MW_ESMART_SCL_FACTOR; Out[n++].Val = 0x10001000u;
    Out[n].Off = b + MW_ESMART_VIR;        Out[n++].Val = Pitch / 4;
    Out[n].Off = b + MW_ESMART_YRGB_MST;   Out[n++].Val = Addr;
    Out[n].Off = b + MW_ESMART_ACT_INFO;   Out[n++].Val = size;
    Out[n].Off = b + MW_ESMART_DSP_INFO;   Out[n++].Val = size;
    Out[n].Off = b + MW_ESMART_DSP_ST;     Out[n++].Val = 0;
    return n;
}

/* The window's enable, written after everything else (Vop2SetSmartWin
 * enables last): REGION0_CTRL with format 0 (ARGB8888) and EN, or 0. */
MW_HC_INLINE MW_REGW MwEsmartEnable(unsigned Esmart, int On)
{
    MW_REGW w;
    w.Off = MW_ESMART_BASE(Esmart & 3) + MW_ESMART_REGION0_CTRL;
    w.Val = On ? 1u : 0u;
    return w;
}

/* Layer selection ids by port (mainline rk3576_vop_win_data layer_sel_id),
 * 0xF where the window cannot go. */
MW_HC_INLINE unsigned MwEsmartLayerId(unsigned Esmart, unsigned Vp)
{
    static const mwh_u8 id[4][3] = {
        { 2, 0xF, 0 },          /* Esmart0 */
        { 0xF, 2, 1 },          /* Esmart1 */
        { 3, 0xF, 2 },          /* Esmart2 */
        { 0xF, 3, 3 },          /* Esmart3 */
    };
    return (Esmart < 4 && Vp < 3) ? id[Esmart][Vp] : 0xFu;
}

#define MW_OVL_CTRL(vp)         (0x600u + (mwh_u32)(vp) * 0x100u)
#define MW_OVL_LAYER_SEL(vp)    (0x604u + (mwh_u32)(vp) * 0x100u)
#define MW_OVL_MIX0(vp)         (0x620u + (mwh_u32)(vp) * 0x100u)
#define MW_OVL_BG_MIX_CTRL(vp)  (0x670u + (mwh_u32)(vp) * 0x100u)

/* A port's overlay with one layer, Esmart at zpos 0: RGB overlay, the
 * layer selection, the three mixers with the firmware's values, BG_DLY.
 * Layer 0xF: the port's layers all off (a port parked). */
#define MW_OVL_WRITES           15u
MW_HC_INLINE unsigned MwOvlWrites(unsigned Vp, unsigned LayerId, MW_REGW *Out)
{
    static const mwh_u32 mix[4] = { 0x00FF01A1u, 0x00FF0060u, 0x00000020u, 0x00000074u };
    unsigned n = 0;

    Out[n].Off = MW_OVL_CTRL(Vp);      Out[n++].Val = 0;
    Out[n].Off = MW_OVL_LAYER_SEL(Vp); Out[n++].Val = 0xFFF0u | (LayerId & 0xF);
    for (unsigned m = 0; m < 3; m++)
        for (unsigned k = 0; k < 4; k++) {
            Out[n].Off = MW_OVL_MIX0(Vp) + m * 0x10u + k * 4u;
            Out[n++].Val = mix[k];
        }
    Out[n].Off = MW_OVL_BG_MIX_CTRL(Vp);
    Out[n++].Val = (Vp == 0 ? 20u : 16u) << 24;
    return n;
}

/* ---- the port's pixel clock ---- */

#define MW_CRU_CLKSEL(n)        (0x300u + (mwh_u32)(n) * 4u)
#define MW_CRU_GATE(n)          (0x800u + (mwh_u32)(n) * 4u)
#define MW_DCLK_GPLL            0u
#define MW_DCLK_CPLL            1u
#define MW_DCLK_VPLL            2u

/* The divider for Pixel kHz from Parent kHz, rounded up so the clock is
 * never above the mode's (Vop2IfConfig: (1000000 + clock - 1) / clock with
 * CPLL taken as 1 GHz), 1 to 256. 0 for no clock. */
MW_HC_INLINE unsigned MwDclkDiv(mwh_u32 ParentKhz, mwh_u32 PixelKhz)
{
    unsigned long long d;

    if (ParentKhz == 0 || PixelKhz == 0)
        return 0;
    d = ((unsigned long long)ParentKhz + PixelKhz - 1) / PixelKhz;
    if (d < 1)
        d = 1;
    if (d > 256)
        d = 256;
    return (unsigned)d;
}

/* dclk_vp<n>_src = Parent / Div and DCLK_VP<n> on it, with both gated
 * across the change: the same four steps as hdmi.c's DclkToCru for VP0
 * (gate, DCLK_VPn back on its source, the source's mux and divider,
 * ungate). Hiword-masked writes. */
#define MW_DCLK_WRITES          5u      /* 4 on VP0, where both gates are in one register */
MW_HC_INLINE unsigned MwDclkWrites(unsigned Vp, unsigned Parent, unsigned Div, MW_REGW *Out)
{
    /* source select register, source gate (con, bit), DCLK mux bit in
     * CLKSEL_CON147, DCLK gate (con, bit) */
    static const mwh_u8 sel[3] = { 145, 146, 147 };
    static const mwh_u8 srcGate[3][2] = { { 61, 10 }, { 61, 11 }, { 61, 12 } };
    static const mwh_u8 mux[3] = { 11, 12, 13 };
    static const mwh_u8 dclkGate[3][2] = { { 61, 13 }, { 62, 0 }, { 62, 1 } };
    unsigned n = 0;

    if (Vp > 2 || Div < 1 || Div > 256)
        return 0;
    Out[n].Off = MW_CRU_GATE(dclkGate[Vp][0]);
    Out[n++].Val = (1u << dclkGate[Vp][1]) << 16 | 1u << dclkGate[Vp][1];
    Out[n].Off = MW_CRU_CLKSEL(147);
    Out[n++].Val = (1u << mux[Vp]) << 16;
    Out[n].Off = MW_CRU_CLKSEL(sel[Vp]);
    Out[n++].Val = 0x7FFu << 16 | (Parent & 7) << 8 | (Div - 1);
    if (srcGate[Vp][0] == dclkGate[Vp][0]) {
        Out[n].Off = MW_CRU_GATE(srcGate[Vp][0]);
        Out[n++].Val = ((1u << srcGate[Vp][1]) | (1u << dclkGate[Vp][1])) << 16;
    } else {
        Out[n].Off = MW_CRU_GATE(srcGate[Vp][0]);
        Out[n++].Val = (1u << srcGate[Vp][1]) << 16;
        Out[n].Off = MW_CRU_GATE(dclkGate[Vp][0]);
        Out[n++].Val = (1u << dclkGate[Vp][1]) << 16;
    }
    return n;
}

/* ---- which source a primary belongs to ---- */

typedef struct _MW_DUAL_SRCSIZE {
    int Active;
    unsigned W, H;
} MW_DUAL_SRCSIZE;

/* By its size: the committed source mode it has. The runtime does not say
 * which source a primary is for when the UMD creates it, and DescribeAllocation
 * has to report that source's refresh rate. Both sources of the same size,
 * or neither: source 0, which is what the single-source driver answers. */
MW_HC_INLINE unsigned MwDualSourceOf(unsigned W, unsigned H, const MW_DUAL_SRCSIZE S[MW_DUAL_SOURCES])
{
    int m0 = S[0].Active && S[0].W == W && S[0].H == H;
    int m1 = S[1].Active && S[1].W == W && S[1].H == H;

    return (m1 && !m0) ? 1u : 0u;
}
