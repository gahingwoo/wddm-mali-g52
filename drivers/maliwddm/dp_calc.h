/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DisplayPort over USB-C (UsbcDp, docs/USBC-DP.md), the arithmetic: no
 * register is touched here. The DW DP controller (DP0 at 0x27E40000), the
 * DP half of the USBDP combo PHY and VOP2 VP1, as dp.c programs them:
 * AUX commands, the sink's capabilities, the link rate, the training
 * adjustments, the PHY's drive levels, the video stream registers, and
 * VP1's pixel clock from VPLL.
 *
 * The firmware (edk2-rk3576 docs/usbc-dp.md) has already entered
 * DisplayPort Alt Mode with the partner, muxed two DP lanes next to USB3,
 * released the PHY's DP init reset, powered PD_VO1 with the controller's
 * clocks, and forced the controller's HPD input high. What is left is the
 * part Linux does in dw-dp.c and in phy-rockchip-usbdp.c's DP ops.
 *
 * Every number is from mainline Linux (linux-next 20260914):
 *
 *   drivers/gpu/drm/bridge/synopsys/dw-dp.c
 *       registers and fields, dw_dp_aux_transfer, dw_dp_link_configure,
 *       dw_dp_link_train_set_pattern, dw_dp_link_get_adjustments,
 *       dw_dp_link_clock_recovery / _channel_equalization, dw_dp_video_enable
 *       (TU, init threshold for dual pixel, HBLANK interval), the timing
 *       limits of dw_dp_bridge_atomic_check (hfp even, hsync >= 9,
 *       hbp >= 16)
 *   drivers/gpu/drm/rockchip/dw_dp-rockchip.c
 *       rk3576: max link rate 8.1 Gb/s, DW_DP_MP_DUAL_PIXEL
 *   drivers/phy/rockchip/phy-rockchip-usbdp.c
 *       CMN_DP_LINK (0x28C) TX_LINK_BW, CMN_SSC_EN (0x2D0), CMN_DP_RSTN
 *       (0x38C) CMN_RSTN, ROPLL lock (0x354), TRSV_ANA_TX_CLK (0x854 +
 *       0x800 n), the drive tables rk3576 uses (rbr_hbr_typec, hbr2, hbr3)
 *       at 0x810..0x81C + 0x800 n
 *   drivers/gpu/drm/rockchip/rockchip_vop2_reg.c, rockchip_drm_vop2.h
 *       RK3576_DP0_IF_CTRL 0x18C (EN, DCLK_OUT_EN, MUX, PIN_POL,
 *       PCLK_DIV), VP1 one pixel per clock
 *   drivers/clk/rockchip/clk-rk3576.c, clk-pll.c
 *       VPLL at PLL_CON(88) (CRU 0x160), mode MODE_CON0 [5:4], the
 *       pll_rk3588 layout (m [9:0], p [5:0], s [8:6], PWRDOWN, lock in
 *       CON6 bit 15), dclk_vp1_src CLKSEL_CON146 (parent [10:8], 2 = VPLL)
 *   include/drm/display/drm_dp.h
 *       DPCD addresses and bits
 *
 * The link is always two lanes: the PHY's two DP-muxed pads next to USB3
 * (pin assignment D), or the first two of C/E's four, on the same pins.
 * That carries everything VP1 can drive (2560x1600 at 60 Hz, reduced
 * blanking, is 268.5 MHz: 6.44 Gb/s of 8.64 at HBR2).
 *
 * Plain C with no WDK header, so that tests/dp-host can build it.
 */
#pragma once
#include "dual_calc.h"

/* ---- where things are ---- */

#define MW_DP_PA                0x27E40000u     /* DW DP, stream 0 at the base */
#define MW_DP_SIZE              0x1000u
#define MW_UDPHY_PMA_PA         0x2B018000u     /* USBDP PHY 0x2B010000 + 0x8000 */
#define MW_UDPHY_PMA_SIZE       0x3000u
#define MW_VO1_GRF_PA           0x26036000u

/* DW DP controller */
#define MW_DP_CCTL              0x200u
#define MW_DP_CCTL_SCRAMBLE_DIS (1u << 0)
#define MW_DP_CCTL_ENH_FRAME    (1u << 1)
#define MW_DP_CCTL_FAST_LT      (1u << 2)       /* DEFAULT_FAST_LINK_TRAIN_EN, reset 1 */
#define MW_DP_CCTL_FORCE_HPD    (1u << 4)
#define MW_DP_SOFT_RESET        0x204u
#define MW_DP_VSAMPLE_CTRL      0x300u
#define MW_DP_VIDEO_STREAM_EN   (1u << 5)
#define MW_DP_VINPUT_POL        0x30Cu
#define MW_DP_VIDEO_CONFIG1     0x310u
#define MW_DP_VIDEO_CONFIG2     0x314u
#define MW_DP_VIDEO_CONFIG3     0x318u
#define MW_DP_VIDEO_CONFIG4     0x31Cu
#define MW_DP_VIDEO_CONFIG5     0x320u
#define MW_DP_VIDEO_MSA1        0x324u
#define MW_DP_VIDEO_MSA2        0x328u
#define MW_DP_VIDEO_MSA3        0x32Cu
#define MW_DP_HBLANK_INTERVAL   0x330u
#define MW_DP_PHYIF_CTRL        0xA00u
#define MW_DP_AUX_CMD           0xB00u
#define MW_DP_AUX_STATUS        0xB04u
#define MW_DP_AUX_DATA0         0xB08u
#define MW_DP_GEN_INT           0xD00u
#define MW_DP_GEN_INT_AUX_REPLY (1u << 1)
#define MW_DP_GEN_INT_AUX_INVAL (1u << 3)
#define MW_DP_GEN_INT_VFIFO_OVF (1u << 6)
#define MW_DP_GEN_INT_EN        0xD04u
#define MW_DP_HPD_STATUS        0xD08u

/* PHYIF_CTRL fields */
#define MW_DP_PHYIF_TPS(x)      ((mwh_u32)(x) & 0xFu)
#define MW_DP_PHYIF_LANES(n)    ((mwh_u32)((n) / 2) << 6)   /* PHY_LANES = lanes / 2 */
#define MW_DP_PHYIF_XMIT(n)     ((mwh_u32)((1u << (n)) - 1) << 8)
#define MW_DP_PHYIF_PD(x)       ((mwh_u32)(x) << 17)        /* PHY_POWERDOWN: 0 P0, 3 P3 */
#define MW_DP_PHYIF_PD_MASK     (0xFu << 17)

/* USBDP PHY PMA (offsets from MW_UDPHY_PMA_PA) */
#define MW_UDPHY_LANE_MUX_EN    0x288u          /* [7:4] pad n is DP, [3:0] DP lane enable */
#define MW_UDPHY_DP_LINK        0x28Cu          /* [6:5] TX_LINK_BW */
#define MW_UDPHY_SSC_EN         0x2D0u          /* bit 1 ROPLL SSC */
#define MW_UDPHY_ROPLL_DONE     0x354u          /* bit 1 lock, bit 0 AFC */
#define MW_UDPHY_DP_RSTN        0x38Cu          /* bit 3 DP_INIT_RSTN, bit 2 DP_CMN_RSTN */
#define MW_UDPHY_DP_INIT_RSTN   (1u << 3)
#define MW_UDPHY_DP_CMN_RSTN    (1u << 2)
#define MW_UDPHY_TX_CLK(pad)    (0x854u + (mwh_u32)(pad) * 0x800u)
#define MW_UDPHY_TX_DRV(pad, i) (0x810u + (mwh_u32)(pad) * 0x800u + (mwh_u32)(i) * 4u)

/* VO1 GRF */
#define MW_VO1_GRF_SOC_CON0     0x000u
#define MW_VO1_HPD_SEL          (1u << 10)
#define MW_VO1_HPD_CFG          (1u << 11)

/* VOP2 */
#define MW_VOP_DP0_IF_CTRL      0x18Cu
#define MW_VP1_BASE             0xD00u
#define MW_VOP_LINE_FLAG1       0x074u
#define MW_VOP_VP1_INT_CLR      0x0B4u
#define MW_VOP_VP1_INT_RAW      0x0BCu
#define MW_DP_ESMART            1u              /* Esmart1: AXI bus 0, on VP1 as layer 2 */

/* VP1's limits (mainline rk3576 vp1 max_output, the vendor's 300 MHz dclk_max) */
#define MW_VP1_MAX_W            2560u
#define MW_VP1_MAX_H            1600u
#define MW_VP1_MAX_KHZ          300000u
#define MW_DP_MIN_KHZ           25000u
#define MW_DP_LANES             2u

/* ---- AUX (dw_dp_aux_transfer) ----
 *
 * AUX_CMD: type [31:28] (DP_AUX_* request: 8 native write, 9 native read,
 * 0 I2C write, 1 I2C read, 4 = MOT), address [27:8], I2C_ADDR_ONLY (bit 4)
 * for a zero-length transfer, else LEN_REQ [3:0] = bytes - 1. At most 16
 * bytes. AUX_STATUS: reply [7:4] (0 ACK, 1 NACK, 2 DEFER, 4 I2C NACK, 8 I2C
 * DEFER), timeout bit 17, bytes read [23:19] counting the reply byte. */
#define MW_AUX_NATIVE_WRITE     0x8u
#define MW_AUX_NATIVE_READ      0x9u
#define MW_AUX_I2C_WRITE        0x0u
#define MW_AUX_I2C_READ         0x1u
#define MW_AUX_I2C_MOT          0x4u

MW_HC_INLINE mwh_u32 MwAuxCmd(mwh_u32 Type, mwh_u32 Addr, mwh_u32 Len)
{
    mwh_u32 v = (Type & 0xF) << 28 | (Addr & 0xFFFFF) << 8;
    return Len == 0 ? (v | (1u << 4)) : (v | ((Len - 1) & 0xF));
}

#define MW_AUX_REPLY(st)        (((st) >> 4) & 0xFu)
#define MW_AUX_TIMEOUT(st)      (((st) >> 17) & 1u)
#define MW_AUX_BYTES(st)        (((st) >> 19) & 0x1Fu)

/* ---- the sink's receiver capabilities (DPCD 0x000..0x00F) ---- */

#define MW_DPCD_REV             0x000u
#define MW_DPCD_LINK_BW_SET     0x100u
#define MW_DPCD_LANE_COUNT_SET  0x101u
#define MW_DPCD_TRAINING_PAT    0x102u
#define MW_DPCD_LANE0_SET       0x103u
#define MW_DPCD_DOWNSPREAD      0x107u
#define MW_DPCD_CODING_SET      0x108u
#define MW_DPCD_SINK_COUNT      0x200u
#define MW_DPCD_LANE01_STATUS   0x202u          /* six bytes: 0x202..0x207 */
#define MW_DPCD_SET_POWER       0x600u
#define MW_DPCD_EXT_CAPS        0x2200u

typedef struct _MW_DP_CAPS {
    mwh_u32 Rev;
    mwh_u32 MaxRate;            /* kHz of the link symbol clock: 162000 .. 810000 */
    mwh_u32 MaxLanes;
    int Enhanced, Tps3, Tps4, Ssc, Coding8b10b;
    mwh_u32 AuxRdInterval;      /* DPCD 0x00E [6:0] */
    int Ext;                    /* 0x00E bit 7: the real caps are at 0x2200 */
} MW_DP_CAPS;

MW_HC_INLINE mwh_u32 MwDpRateOfBw(mwh_u32 Bw)
{
    switch (Bw) {
    case 0x06: return 162000;
    case 0x0A: return 270000;
    case 0x14: return 540000;
    case 0x1E: return 810000;
    default:   return 0;
    }
}

MW_HC_INLINE mwh_u32 MwDpBwOfRate(mwh_u32 Rate)
{
    return Rate == 162000 ? 0x06 : Rate == 270000 ? 0x0A : Rate == 540000 ? 0x14 : Rate == 810000 ? 0x1E : 0;
}

/* The PHY's TX_LINK_BW and drive table index: 0 RBR, 1 HBR, 2 HBR2, 3 HBR3. */
MW_HC_INLINE mwh_u32 MwUdphyBw(mwh_u32 Rate)
{
    return Rate == 162000 ? 0 : Rate == 270000 ? 1 : Rate == 540000 ? 2 : 3;
}

/* The rate this port uses at most: HBR2. Two lanes of HBR2 carry more than
 * VP1 can make; HBR3 needs TPS4 and a cable for it and buys nothing here. */
#define MW_DP_MAX_RATE          540000u

MW_HC_INLINE void MwDpCaps(const mwh_u8 *D, MW_DP_CAPS *C)
{
    mwh_u32 r = MwDpRateOfBw(D[1]);

    C->Rev = D[0];
    /* A sink that reports nothing we know: RBR, which every sink takes. */
    C->MaxRate = r == 0 ? 162000 : r;
    C->MaxLanes = D[2] & 0x1F;
    C->Enhanced = (D[2] >> 7) & 1;
    C->Tps3 = (D[2] >> 6) & 1;
    C->Tps4 = (D[3] >> 7) & 1;
    C->Ssc = D[3] & 1;
    C->Coding8b10b = D[6] & 1;
    C->AuxRdInterval = D[0x0E] & 0x7F;
    C->Ext = (D[0x0E] >> 7) & 1;
}

/* The first rate to train at: the sink's best, at most HBR2. */
MW_HC_INLINE mwh_u32 MwDpStartRate(const MW_DP_CAPS *C)
{
    return C->MaxRate < MW_DP_MAX_RATE ? C->MaxRate : MW_DP_MAX_RATE;
}

/* The next rate down after a failed training (dw_dp_link_downgrade), 0
 * after RBR. */
MW_HC_INLINE mwh_u32 MwDpLowerRate(mwh_u32 Rate)
{
    return Rate > 540000 ? 540000 : Rate > 270000 ? 270000 : Rate > 162000 ? 162000 : 0;
}

/* How long to wait between writing a training set and reading the status
 * (drm_dp_link_train_clock_recovery_delay / _channel_eq_delay), in us:
 * 100 / 400 when the sink gives no interval or is DP 1.4, else 4 ms each. */
MW_HC_INLINE mwh_u32 MwDpCrDelayUs(const MW_DP_CAPS *C)
{
    return (C->AuxRdInterval == 0 || C->Rev >= 0x14) ? 100u : C->AuxRdInterval * 4000u;
}

MW_HC_INLINE mwh_u32 MwDpEqDelayUs(const MW_DP_CAPS *C)
{
    return C->AuxRdInterval == 0 ? 400u : C->AuxRdInterval * 4000u;
}

/* ---- training (dw-dp.c, drm_dp_helper.c) ---- */

/* DPCD 0x202..0x207: lane n's status nibble in byte n / 2 (CR_DONE bit 0,
 * CHANNEL_EQ bit 1, SYMBOL_LOCKED bit 2), INTERLANE_ALIGN_DONE in 0x204
 * bit 0, lane n's adjust request in 0x206 + n / 2 (voltage [1:0], pre
 * [3:2], the high nibble for the odd lane). */
MW_HC_INLINE mwh_u32 MwDpLaneStatus(const mwh_u8 *S, unsigned Lane)
{
    return (S[Lane / 2] >> (4 * (Lane & 1))) & 0xF;
}

MW_HC_INLINE int MwDpCrOk(const mwh_u8 *S, unsigned Lanes)
{
    for (unsigned i = 0; i < Lanes; i++)
        if (!(MwDpLaneStatus(S, i) & 1))
            return 0;
    return 1;
}

MW_HC_INLINE int MwDpEqOk(const mwh_u8 *S, unsigned Lanes)
{
    if (!(S[2] & 1))
        return 0;
    for (unsigned i = 0; i < Lanes; i++)
        if ((MwDpLaneStatus(S, i) & 7) != 7)
            return 0;
    return 1;
}

typedef struct _MW_DP_TRAIN {
    mwh_u8 Vs[4], Pe[4];
    mwh_u8 VsMax[4], PeMax[4];
} MW_DP_TRAIN;

/* dw_dp_link_get_adjustments: take the sink's request, with pre-emphasis
 * at most 3 and swing at most 3 - pre-emphasis. 1 if it differs from what
 * was there. Unlike dw-dp.c the comparison is after the clamp, so a sink
 * that keeps asking for a combination the PHY cannot drive counts as the
 * same request (and the five-tries rule ends clock recovery) instead of a
 * new one every time. */
MW_HC_INLINE int MwDpAdjust(const mwh_u8 *S, unsigned Lanes, MW_DP_TRAIN *T)
{
    int changed = 0;

    for (unsigned i = 0; i < Lanes; i++) {
        mwh_u32 adj = (S[4 + i / 2] >> (4 * (i & 1))) & 0xF;
        mwh_u32 v = adj & 3, p = (adj >> 2) & 3;
        mwh_u32 vmax = 3 - p;

        if (v > vmax)
            v = vmax;
        if (v != T->Vs[i] || p != T->Pe[i])
            changed = 1;
        T->Pe[i] = (mwh_u8)p;
        T->PeMax[i] = p >= 3;
        T->Vs[i] = (mwh_u8)v;
        T->VsMax[i] = v >= 3;
    }
    return changed;
}

/* TRAINING_LANEn_SET: swing [1:0], MAX_SWING_REACHED bit 2, pre-emphasis
 * [4:3], MAX_PRE_EMPHASIS_REACHED bit 5. */
MW_HC_INLINE mwh_u8 MwDpLaneSet(const MW_DP_TRAIN *T, unsigned Lane)
{
    return (mwh_u8)(T->Vs[Lane] | (T->VsMax[Lane] ? 4u : 0) | (mwh_u32)T->Pe[Lane] << 3 | (T->PeMax[Lane] ? 0x20u : 0));
}

/* The channel equalisation pattern: TPS4 if the sink has it, else TPS3,
 * else TPS2; as DPCD 0x102's value and the controller's TPS_SEL. TPS1 to
 * TPS3 go with scrambling off (bit 5 in 0x102, SCRAMBLE_DIS in CCTL). */
MW_HC_INLINE mwh_u32 MwDpEqPattern(const MW_DP_CAPS *C)
{
    return C->Tps4 ? 4u : C->Tps3 ? 3u : 2u;
}

MW_HC_INLINE mwh_u8 MwDpPatternDpcd(mwh_u32 Tps)
{
    if (Tps == 0)
        return 0;
    if (Tps == 4)
        return 0x07;
    return (mwh_u8)(Tps | 0x20);
}

/* ---- the PHY's drive levels (rk3576: dp_tx_ctrl_cfg{,_typec} the same) ---- */

MW_HC_INLINE void MwUdphyDrive(mwh_u32 Bw, mwh_u32 Vs, mwh_u32 Pe, mwh_u8 Out[4])
{
    static const mwh_u8 rbr_hbr[4][4][4] = {
        { { 0x20, 0x10, 0x42, 0xe5 }, { 0x26, 0x14, 0x42, 0xe5 }, { 0x29, 0x18, 0x42, 0xe5 }, { 0x2b, 0x1c, 0x43, 0xe7 } },
        { { 0x23, 0x10, 0x42, 0xe7 }, { 0x2a, 0x17, 0x43, 0xe7 }, { 0x2b, 0x1a, 0x43, 0xe7 } },
        { { 0x27, 0x10, 0x43, 0x67 }, { 0x2b, 0x17, 0x43, 0xe7 } },
        { { 0x29, 0x10, 0x43, 0xe7 } },
    };
    static const mwh_u8 hbr2[4][4][4] = {
        { { 0x21, 0x10, 0x42, 0xe5 }, { 0x26, 0x14, 0x42, 0xe5 }, { 0x26, 0x16, 0x43, 0xe5 }, { 0x2a, 0x19, 0x43, 0xe7 } },
        { { 0x24, 0x10, 0x42, 0xe7 }, { 0x2a, 0x17, 0x43, 0xe7 }, { 0x2b, 0x1a, 0x43, 0xe7 } },
        { { 0x28, 0x10, 0x42, 0xe7 }, { 0x2b, 0x17, 0x43, 0xe7 } },
        { { 0x28, 0x10, 0x43, 0xe7 } },
    };
    static const mwh_u8 hbr3[4][4][4] = {
        { { 0x21, 0x10, 0x42, 0xe5 }, { 0x26, 0x14, 0x42, 0xe5 }, { 0x26, 0x16, 0x43, 0xe5 }, { 0x29, 0x18, 0x43, 0xe7 } },
        { { 0x24, 0x10, 0x42, 0xe7 }, { 0x2a, 0x18, 0x43, 0xe7 }, { 0x2b, 0x1b, 0x43, 0xe7 } },
        { { 0x27, 0x10, 0x42, 0xe7 }, { 0x2b, 0x18, 0x43, 0xe7 } },
        { { 0x28, 0x10, 0x43, 0xe7 } },
    };
    const mwh_u8 (*t)[4][4] = Bw <= 1 ? rbr_hbr : Bw == 2 ? hbr2 : hbr3;

    if (Vs > 3)
        Vs = 3;
    if (Pe > 3 - Vs)
        Pe = 3 - Vs;
    for (unsigned i = 0; i < 4; i++)
        Out[i] = t[Vs][Pe][i];
}

/* TRSV_ANA_TX_CLK bit 1 (LN_ANA_TX_SER_TXCLK_INV): the pad's lane_mux_sel
 * (1 on a DP pad) at RBR and HBR, 0 at HBR2 and HBR3. */
MW_HC_INLINE int MwUdphyTxClkInv(mwh_u32 Bw)
{
    return Bw <= 1;
}

/* Which PHY pad carries DP lane 0 and 1: the pads whose enable bit the
 * firmware set in LANE_MUX_EN [3:0], and VO1_GRF_SOC_CON0 [7:0], two bits a
 * pad, saying which DP lane drives it. Returns the number of lanes found
 * (2 when the firmware did its part), the pads in Pad[]. */
MW_HC_INLINE unsigned MwDpLanePads(mwh_u32 MuxEn, mwh_u32 Grf, mwh_u8 Pad[2])
{
    unsigned n = 0;

    Pad[0] = Pad[1] = 0xFF;
    for (unsigned pad = 0; pad < 4; pad++) {
        mwh_u32 lane;
        if (!(MuxEn & (1u << pad)) || !(MuxEn & (1u << (pad + 4))))
            continue;
        lane = (Grf >> (2 * pad)) & 3;
        if (lane < 2 && Pad[lane] == 0xFF) {
            Pad[lane] = (mwh_u8)pad;
            n++;
        }
    }
    return n;
}

/* ---- a mode the link and VP1 can carry ---- */

/* Bytes of pixel data per link symbol clock: 3 a pixel (RGB 8 bpc), Lanes
 * bytes a symbol clock; at most 62 of every 64-byte transfer unit, the
 * margin dw_dp_video_enable's TU arithmetic needs. */
MW_HC_INLINE int MwDpLinkCarries(mwh_u32 ClockKhz, mwh_u32 Rate, mwh_u32 Lanes)
{
    return (unsigned long long)ClockKhz * 3u * 64u <= (unsigned long long)Rate * Lanes * 62u;
}

/* ---- VP1's pixel clock: VPLL / Div ----
 *
 * pll_rk3588: Fout = 24 MHz * m / p >> s, VCO = 24 MHz * m / p in 2.25 to
 * 4.5 GHz (clk-pll.c), m 64..1023, p 1..63, s 0..6. The vendor kernel
 * (rockchip_drm_clk.c) programs VPLL to an even multiple of the pixel
 * clock and divides it down with dclk_vp1_src; this does the same, the
 * divider even from 2 to 16 with VPLL between 500 MHz and 1.6 GHz, the
 * first exact setting found. dclk_vp0 (HDMI, the HDPTX PLL) and dclk_vp2
 * (CPLL / 15) do not use VPLL. */
typedef struct _MW_VPLL {
    mwh_u32 M, P, S, Div;
    mwh_u32 Khz;                /* VPLL's rate */
} MW_VPLL;

MW_HC_INLINE int MwVpllFor(mwh_u32 PixelKhz, MW_VPLL *V)
{
    if (PixelKhz == 0)
        return 0;
    for (mwh_u32 div = 2; div <= 16; div += 2) {
        mwh_u32 f = PixelKhz * div;
        if (f < 500000 || f > 1600000)
            continue;
        for (mwh_u32 s = 0; s <= 6; s++) {
            unsigned long long vco = (unsigned long long)f << s;
            if (vco < 2250000 || vco > 4500000)
                continue;
            for (mwh_u32 p = 1; p <= 8; p++) {
                unsigned long long mp = vco * p;
                if (mp % 24000 != 0)
                    continue;
                if (mp / 24000 < 64 || mp / 24000 > 1023)
                    continue;
                V->M = (mwh_u32)(mp / 24000);
                V->P = p;
                V->S = s;
                V->Div = div;
                V->Khz = f;
                return 1;
            }
        }
    }
    return 0;
}

/* VPLL's register writes, hiword masked, in clk-pll.c's order with the
 * mode switched to slow around them (rockchip_rk3588_pll_set_params):
 * slow, power down, m, p and s, k 0, power up; then the caller waits for
 * CON6 bit 15 and writes MwVpllNormal. */
#define MW_CRU_VPLL_CON(n)      (0x160u + (mwh_u32)(n) * 4u)
#define MW_CRU_MODE_CON0        0x280u
#define MW_VPLL_WRITES          6u
#define MW_VPLL_LOCK            (1u << 15)

MW_HC_INLINE unsigned MwVpllWrites(const MW_VPLL *V, MW_REGW *Out)
{
    unsigned n = 0;

    Out[n].Off = MW_CRU_MODE_CON0;   Out[n++].Val = 0x3u << 20;                          /* [5:4] = 0: slow */
    Out[n].Off = MW_CRU_VPLL_CON(1); Out[n++].Val = 1u << 29 | 1u << 13;                 /* PWRDOWN */
    Out[n].Off = MW_CRU_VPLL_CON(0); Out[n++].Val = 0x3FFu << 16 | (V->M & 0x3FF);
    Out[n].Off = MW_CRU_VPLL_CON(1); Out[n++].Val = 0x1FFu << 16 | (V->S & 7) << 6 | (V->P & 0x3F);
    Out[n].Off = MW_CRU_VPLL_CON(2); Out[n++].Val = 0xFFFFu << 16;                      /* k 0 */
    Out[n].Off = MW_CRU_VPLL_CON(1); Out[n++].Val = 1u << 29;                            /* power up */
    return n;
}

MW_HC_INLINE MW_REGW MwVpllNormal(void)
{
    MW_REGW w;
    w.Off = MW_CRU_MODE_CON0;
    w.Val = 0x3u << 20 | 1u << 4;
    return w;
}

/* A timing dp.c may program: progressive, within VP1, a clock VPLL can make
 * exactly, the controller's limits (dw_dp_bridge_atomic_check: front porch
 * even, sync at least 9, back porch at least 16), a width of whole 4-pixel
 * groups as on VP0, and what two lanes at Rate carry. */
MW_HC_INLINE int MwDpTimingOk(const MW_TIMING *T, mwh_u32 Rate, mwh_u32 MaxPixels)
{
    MW_VPLL v;
    mwh_u32 hfp, hs, hbp;

    if (T->Clock < MW_DP_MIN_KHZ || T->Clock > MW_VP1_MAX_KHZ)
        return 0;
    if (T->HDisplay == 0 || T->VDisplay == 0 || T->HDisplay > MW_VP1_MAX_W || T->VDisplay > MW_VP1_MAX_H)
        return 0;
    if ((T->HDisplay & 3) != 0 || (mwh_u32)T->HDisplay * T->VDisplay > MaxPixels)
        return 0;
    if (!(T->HSyncStart > T->HDisplay && T->HSyncEnd > T->HSyncStart && T->HTotal > T->HSyncEnd))
        return 0;
    if (!(T->VSyncStart > T->VDisplay && T->VSyncEnd > T->VSyncStart && T->VTotal > T->VSyncEnd))
        return 0;
    hfp = (mwh_u32)T->HSyncStart - T->HDisplay;
    hs = (mwh_u32)T->HSyncEnd - T->HSyncStart;
    hbp = (mwh_u32)T->HTotal - T->HSyncEnd;
    if ((hfp & 1) != 0 || hs < 9 || hbp < 16)
        return 0;
    if (!MwDpLinkCarries(T->Clock, Rate, MW_DP_LANES))
        return 0;
    return MwVpllFor(T->Clock, &v);
}

/* The EDID's modes (hdmi_calc.h MwHdmiModes: the same sources, the same
 * order) that MwDpTimingOk takes at Rate. */
MW_HC_INLINE mwh_u32 MwDpModes(const mwh_u8 *E, mwh_u32 Len, mwh_u32 Rate, mwh_u32 MaxPixels,
                               MW_TIMING *Out, mwh_u32 Max)
{
    MW_TIMING all[48];
    mwh_u32 n = MwHdmiModes(E, Len, MaxPixels, all, 48), k = 0;

    for (mwh_u32 i = 0; i < n && k < Max; i++)
        if (MwDpTimingOk(&all[i], Rate, MaxPixels))
            Out[k++] = all[i];
    return k;
}

/* ---- the video stream (dw_dp_video_enable, RGB 8 bpc, dual pixel) ---- */

typedef struct _MW_DP_VIDEO {
    mwh_u32 Vsample;            /* PIXEL_MODE 1 (dual) | VIDEO_MAPPING 1 (RGB 8 bit), stream off */
    mwh_u32 Pol, Cfg1, Cfg2, Cfg3, Cfg4, Cfg5, Msa1, Msa2, Msa3, Hblank;
    mwh_u32 AvgTu, AvgTuFrac, InitThr;
} MW_DP_VIDEO;

MW_HC_INLINE void MwDpVideo(const MW_TIMING *T, mwh_u32 Rate, mwh_u32 Lanes, MW_DP_VIDEO *V)
{
    mwh_u32 hblank = (mwh_u32)T->HTotal - T->HDisplay;
    mwh_u32 vblank = (mwh_u32)T->VTotal - T->VDisplay;
    mwh_u32 peak = T->Clock * 24 / 8;
    mwh_u32 link = (Rate / 1000) * Lanes;
    mwh_u32 ts = peak * 64 / link;
    mwh_u32 avg = ts / 1000, frac = ts / 100 - avg * 10;
    mwh_u32 t1 = (1000 / 3) * Lanes;                    /* 8 bpc RGB, dual pixel */
    mwh_u32 t2 = (Rate / 4) * 1000 / T->Clock;
    mwh_u32 t3 = frac ? avg + 1 : avg;
    mwh_u32 thr = (mwh_u32)((unsigned long long)t1 * t2 * t3 / (1000 * 1000));
    mwh_u8 vic = T->Vic;

    if (thr <= 16 || avg < 10)
        thr = 40;
    V->Vsample = 1u << 21 | 1u << 16;
    V->Pol = (T->PHSync ? 2u : 0) | (T->PVSync ? 1u : 0);
    V->Cfg1 = (mwh_u32)T->HDisplay << 16 | (hblank & 0x3FFF) << 2;
    /* R_V_BLANK_IN_OSC for the CEA modes dw-dp.c lists (all interlaced or
     * odd-vblank ones, which MwDpTimingOk never passes; kept for the
     * record). */
    if (vic == 5 || vic == 6 || vic == 7 || vic == 10 || vic == 11 || vic == 20 || vic == 21 || vic == 22 ||
        vic == 39 || vic == 25 || vic == 26 || vic == 40 || vic == 44 || vic == 45 || vic == 46 ||
        vic == 50 || vic == 51 || vic == 54 || vic == 55 || vic == 58 || vic == 59)
        V->Cfg1 |= 1;
    V->Cfg2 = vblank << 16 | T->VDisplay;
    V->Cfg3 = ((mwh_u32)T->HSyncEnd - T->HSyncStart) << 16 | ((mwh_u32)T->HSyncStart - T->HDisplay);
    V->Cfg4 = ((mwh_u32)T->VSyncEnd - T->VSyncStart) << 16 | ((mwh_u32)T->VSyncStart - T->VDisplay);
    V->Cfg5 = ((thr >> 6) & 3) << 21 | (frac & 0xF) << 16 | (thr & 0x7F) << 7 | (avg & 0x7F);
    V->Msa1 = ((mwh_u32)T->VTotal - T->VSyncStart) << 16 | ((mwh_u32)T->HTotal - T->HSyncStart);
    V->Msa2 = 0x20u << 24;                              /* MISC0: RGB, 8 bpc (DP_MSA_MISC_8_BPC) */
    V->Msa3 = 0;
    V->Hblank = 1u << 16 | ((hblank * (Rate / 4) / T->Clock) & 0xFFFF);
    V->AvgTu = avg;
    V->AvgTuFrac = frac;
    V->InitThr = thr;
}

/* ---- VP1 and the DP0 interface ----
 *
 * hdmi_calc.h MwVpRegsPort(T, 0, 1, &r) has everything for VP1: AAAA
 * output (VP1 is 10-bit capable), CLK_CTRL 0 (one pixel per clock),
 * IF_CTRL EN | DCLK_OUT_EN | MUX 1 | PIN_POL | PCLK_DIV | CFG_DONE_IMD,
 * which is what rk3576_set_intf_mux writes for DP0 on VP1. The vendor
 * kernel enables the interface in two steps: clock out and mux with the
 * port, EN only once the DP encoder is up (rockchip_drm_vop2.c
 * vop2_crtc_atomic_enable, output_post_enable); dp.c does the same. */
MW_HC_INLINE mwh_u32 MwDp0IfStaged(mwh_u32 IfCtrl)
{
    return IfCtrl & ~MW_IF_EN;
}

/* What hpd.c shows each DP-extend target through: HDMI (target 0) Esmart0
 * on VP0 as always, DP (target 1) Esmart1 on VP1. */
#define MW_DPX_HDMI             0u
#define MW_DPX_DP               1u

MW_HC_INLINE int MwDpDualWin(unsigned Target, MW_DUAL_WIN *W)
{
    unsigned esmart, vp;

    if (Target == MW_DPX_HDMI) {
        esmart = 0;
        vp = 0;
    } else if (Target == MW_DPX_DP) {
        esmart = MW_DP_ESMART;
        vp = 1;
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
