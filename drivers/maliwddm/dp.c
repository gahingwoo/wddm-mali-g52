/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DisplayPort over USB-C (UsbcDp, docs/USBC-DP.md): the DW DP controller
 * (DP0), the DP half of the USBDP PHY and VOP2 VP1, for a second display
 * next to HDMI on VP0. Off unless the UsbcDp Device Parameter is 1, and
 * then only on an HDMI boot whose firmware entered DisplayPort Alt Mode
 * (edk2-rk3576 docs/usbc-dp.md, setup option "USB-C DisplayPort"); the
 * firmware leaves PD_VO1 up, the controller's clocks on, two DP lanes
 * muxed and enabled in the PHY and the controller's HPD forced high.
 *
 * What runs here is what mainline Linux runs in dw-dp.c and in
 * phy-rockchip-usbdp.c's DP phy_configure, polled: AUX (native and I2C over
 * AUX for the EDID), link training (clock recovery, channel equalisation,
 * rate fallback), the video stream; and VP1 with Esmart1, dclk_vp1 from
 * VPLL. Every register value comes from dp_calc.h, which tests/dp-host
 * checks. NOT RUN ON HARDWARE.
 *
 * Locking: every function here runs at PASSIVE_LEVEL with the caller
 * holding HwLock (hpd.c), as the HDMI mode sets do.
 */
#include "maliwddm.h"
#include "dp_calc.h"

#define PA_DP_PMU_REPAIR        0x570u
#define PD_VO1_ON               (1u << 14)
#define CRU_GATE67              0x90Cu
#define CRU_GATE68              0x910u

static __inline void Wr(PUCHAR B, ULONG Off, ULONG V)
{
    WRITE_REGISTER_ULONG((PULONG)(B + Off), V);
}

static __inline ULONG Rd(PUCHAR B, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(B + Off));
}

static __inline void Rmw(PUCHAR B, ULONG Off, ULONG Clear, ULONG Set)
{
    Wr(B, Off, (Rd(B, Off) & ~Clear) | Set);
}

static void Us(ULONG U)
{
    KeStallExecutionProcessor(U);
}

static void Ms(ULONG M)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)M * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static PUCHAR MapOne(ULONG Pa, SIZE_T Len)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = Pa;
    return (PUCHAR)MmMapIoSpaceEx(p, Len, PAGE_READWRITE | PAGE_NOCACHE);
}

/* PD_VO1 up and the controller's bus clock ungated: anything else and a
 * register access is a synchronous external abort. H->Pmu and H->Cru are
 * hdmi.c's mappings. */
static BOOLEAN Powered(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    if (H->Pmu == NULL || H->Cru == NULL)
        return FALSE;
    if (!(Rd(H->Pmu, PA_DP_PMU_REPAIR) & PD_VO1_ON))
        return FALSE;
    /* pclk_vo1_root (CON67 bit 3) and pclk_dp0 (CON68 bit 1): 1 is gated */
    return !(Rd(H->Cru, CRU_GATE67) & (1u << 3)) && !(Rd(H->Cru, CRU_GATE68) & (1u << 1));
}

/* ---- AUX ---- */

/* One AUX transaction (dw_dp_aux_transfer), polled: at most 16 bytes.
 * Reply: the sink's answer (0 ACK, 1 NACK, 2 DEFER, 4 I2C NACK, 8 I2C
 * DEFER). */
static NTSTATUS AuxOnce(MW_DP *D, ULONG Type, ULONG Addr, UCHAR *Buf, ULONG Len, ULONG *Reply)
{
    BOOLEAN read = (Type & 1) != 0;
    ULONG st, w;

    *Reply = 0xF;
    if (Len > 16)
        return STATUS_INVALID_PARAMETER;
    if (!read)
        for (ULONG i = 0; i < Len; i += 4) {
            w = 0;
            for (ULONG k = 0; k < 4 && i + k < Len; k++)
                w |= (ULONG)Buf[i + k] << (8 * k);
            Wr(D->Ctl, MW_DP_AUX_DATA0 + i, w);
        }
    Wr(D->Ctl, MW_DP_GEN_INT, MW_DP_GEN_INT_AUX_REPLY | MW_DP_GEN_INT_AUX_INVAL);
    Wr(D->Ctl, MW_DP_AUX_CMD, MwAuxCmd(Type, Addr, Len));
    for (w = 0; w < 1000; w++) {
        if (Rd(D->Ctl, MW_DP_GEN_INT) & (MW_DP_GEN_INT_AUX_REPLY | MW_DP_GEN_INT_AUX_INVAL))
            break;
        Us(10);
    }
    Wr(D->Ctl, MW_DP_GEN_INT, MW_DP_GEN_INT_AUX_REPLY | MW_DP_GEN_INT_AUX_INVAL);
    if (w == 1000)
        return STATUS_IO_TIMEOUT;
    st = Rd(D->Ctl, MW_DP_AUX_STATUS);
    if (MW_AUX_TIMEOUT(st))
        return STATUS_IO_TIMEOUT;
    *Reply = MW_AUX_REPLY(st);
    if (read && Len != 0 && *Reply == 0) {
        if (MW_AUX_BYTES(st) == 0 || MW_AUX_BYTES(st) - 1 != Len)
            return STATUS_DEVICE_BUSY;
        for (ULONG i = 0; i < Len; i += 4) {
            w = Rd(D->Ctl, MW_DP_AUX_DATA0 + i);
            for (ULONG k = 0; k < 4 && i + k < Len; k++)
                Buf[i + k] = (UCHAR)(w >> (8 * k));
        }
    }
    return STATUS_SUCCESS;
}

/* The DRM core's retries: native up to 32 times 500 us apart (a timeout
 * retried at once), I2C NACK final, DEFER of either kind retried. Tries
 * 1 for a quick presence check. */
static NTSTATUS Aux(MW_DP *D, ULONG Type, ULONG Addr, UCHAR *Buf, ULONG Len, ULONG Tries)
{
    NTSTATUS st = STATUS_IO_TIMEOUT;
    ULONG reply;

    for (ULONG t = 0; t < Tries; t++) {
        st = AuxOnce(D, Type, Addr, Buf, Len, &reply);
        if (st == STATUS_IO_TIMEOUT)
            continue;
        if (NT_SUCCESS(st) && reply == 0)
            return STATUS_SUCCESS;
        if (NT_SUCCESS(st) && (reply == 1 || reply == 4))
            return STATUS_DEVICE_PROTOCOL_ERROR;
        Us(500);
    }
    D->AuxFails++;
    return NT_SUCCESS(st) ? STATUS_IO_DEVICE_ERROR : st;
}

static NTSTATUS DpcdRead(MW_DP *D, ULONG Addr, UCHAR *Buf, ULONG Len)
{
    for (ULONG o = 0; o < Len; o += 16) {
        NTSTATUS st = Aux(D, MW_AUX_NATIVE_READ, Addr + o, Buf + o, min(16, Len - o), 32);
        if (!NT_SUCCESS(st))
            return st;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DpcdWrite(MW_DP *D, ULONG Addr, const UCHAR *Buf, ULONG Len)
{
    UCHAR b[16];

    for (ULONG o = 0; o < Len; o += 16) {
        ULONG n = min(16, Len - o);
        NTSTATUS st;
        RtlCopyMemory(b, Buf + o, n);
        st = Aux(D, MW_AUX_NATIVE_WRITE, Addr + o, b, n, 32);
        if (!NT_SUCCESS(st))
            return st;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DpcdW1(MW_DP *D, ULONG Addr, UCHAR V)
{
    return DpcdWrite(D, Addr, &V, 1);
}

/* EDID block Block (128 bytes) over I2C-over-AUX: the segment pointer
 * (0x30) for blocks 2 and 3, the offset, 16-byte reads with MOT, an
 * address-only read to stop (drm_do_get_edid's transfers as
 * drm_dp_i2c_xfer splits them). */
static NTSTATUS EdidBlock(MW_DP *D, ULONG Block, UCHAR *Out)
{
    UCHAR seg = (UCHAR)(Block / 2), off = (UCHAR)((Block & 1) * 128);
    NTSTATUS st = STATUS_SUCCESS;

    if (seg != 0)
        st = Aux(D, MW_AUX_I2C_WRITE | MW_AUX_I2C_MOT, 0x30, &seg, 1, 8);
    if (NT_SUCCESS(st))
        st = Aux(D, MW_AUX_I2C_WRITE | MW_AUX_I2C_MOT, 0x50, &off, 1, 8);
    for (ULONG o = 0; NT_SUCCESS(st) && o < 128; o += 16)
        st = Aux(D, MW_AUX_I2C_READ | MW_AUX_I2C_MOT, 0x50, Out + o, 16, 8);
    (void)Aux(D, MW_AUX_I2C_READ, 0x50, NULL, 0, 2);
    return st;
}

static BOOLEAN EdidOk(const UCHAR *E)
{
    static const UCHAR hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    UCHAR sum = 0;

    for (ULONG i = 0; i < 128; i++)
        sum = (UCHAR)(sum + E[i]);
    return sum == 0 && RtlCompareMemory(E, hdr, 8) == 8;
}

/* ---- the PHY's DP half (phy-rockchip-usbdp.c rk_udphy_dp_phy_configure) ---- */

/* The link rate: DP_CMN_RSTN low, TX_LINK_BW, no SSC, DP_CMN_RSTN high,
 * the ROPLL's lock; with the controller's PHY interface in P3 around it
 * and the lanes and transmitters set after (dw_dp_phy_configure). */
static NTSTATUS PhyRate(MW_DP *D, ULONG Rate)
{
    ULONG bw = MwUdphyBw(Rate), v = 0;

    Rmw(D->Ctl, MW_DP_PHYIF_CTRL, MW_DP_PHYIF_PD_MASK, MW_DP_PHYIF_PD(3));
    Rmw(D->Pma, MW_UDPHY_DP_RSTN, MW_UDPHY_DP_CMN_RSTN, 0);
    Rmw(D->Pma, MW_UDPHY_DP_LINK, 3u << 5, bw << 5);
    Rmw(D->Pma, MW_UDPHY_SSC_EN, 1u << 1, 0);
    Rmw(D->Pma, MW_UDPHY_DP_RSTN, 0, MW_UDPHY_DP_CMN_RSTN);
    for (ULONG i = 0; i < 100; i++) {
        v = Rd(D->Pma, MW_UDPHY_ROPLL_DONE);
        if ((v & 3) == 3)
            break;
        Us(10);
    }
    D->RopllDone = v;
    if ((v & 3) != 3)
        return STATUS_DEVICE_NOT_READY;
    Rmw(D->Ctl, MW_DP_PHYIF_CTRL, 3u << 6, MW_DP_PHYIF_LANES(MW_DP_LANES));
    Rmw(D->Ctl, MW_DP_PHYIF_CTRL, MW_DP_PHYIF_PD_MASK, MW_DP_PHYIF_PD(0));
    Rmw(D->Ctl, MW_DP_PHYIF_CTRL, 0xFu << 8, MW_DP_PHYIF_XMIT(MW_DP_LANES));
    return STATUS_SUCCESS;
}

/* Swing and pre-emphasis on both lanes' pads, then the same to the sink
 * (dw_dp_link_train_update_vs_emph). */
static NTSTATUS UpdateDrive(MW_DP *D, ULONG Rate)
{
    ULONG bw = MwUdphyBw(Rate);
    UCHAR set[MW_DP_LANES];

    for (ULONG l = 0; l < MW_DP_LANES; l++) {
        ULONG pad = D->Pad[l];
        UCHAR drv[4];
        Rmw(D->Pma, MW_UDPHY_TX_CLK(pad), 1u << 1, MwUdphyTxClkInv(bw) ? 1u << 1 : 0);
        MwUdphyDrive(bw, D->Train.Vs[l], D->Train.Pe[l], drv);
        for (ULONG i = 0; i < 4; i++)
            Wr(D->Pma, MW_UDPHY_TX_DRV(pad, i), drv[i]);
        set[l] = MwDpLaneSet(&D->Train, l);
    }
    return DpcdWrite(D, MW_DPCD_LANE0_SET, set, MW_DP_LANES);
}

static NTSTATUS SetPattern(MW_DP *D, ULONG Tps)
{
    Rmw(D->Ctl, MW_DP_CCTL, MW_DP_CCTL_SCRAMBLE_DIS, (Tps != 0 && Tps != 4) ? MW_DP_CCTL_SCRAMBLE_DIS : 0);
    Rmw(D->Ctl, MW_DP_PHYIF_CTRL, 0xF, MW_DP_PHYIF_TPS(Tps));
    return DpcdW1(D, MW_DPCD_TRAINING_PAT, MwDpPatternDpcd(Tps));
}

/* One full training at Rate (dw_dp_link_configure, _clock_recovery,
 * _channel_equalization). Trace: Dp_TrainStep (1 configure, 2 clock
 * recovery, 3 equalisation, 4 done) and Dp_TrainStatus (the last 0x202..
 * 0x205). */
static NTSTATUS TrainAt(MW_ADAPTER *A, MW_DP *D, ULONG Rate)
{
    UCHAR s[6] = { 0 }, b[2];
    NTSTATUS st;
    ULONG same = 0, it, eq;
    BOOLEAN cr = FALSE, ok = FALSE;

    MwLog(A, L"Dp_TrainStep", 1);
    st = PhyRate(D, Rate);
    if (!NT_SUCCESS(st)) {
        MwLog(A, L"Dp_Ropll", D->RopllDone);
        return st;
    }
    Rmw(D->Ctl, MW_DP_CCTL, MW_DP_CCTL_ENH_FRAME, D->Caps.Enhanced ? MW_DP_CCTL_ENH_FRAME : 0);
    b[0] = (UCHAR)MwDpBwOfRate(Rate);
    b[1] = (UCHAR)(MW_DP_LANES | (D->Caps.Enhanced ? 0x80 : 0));
    st = DpcdWrite(D, MW_DPCD_LINK_BW_SET, b, 2);
    b[0] = 0;                                   /* no spread: the ROPLL's SSC is off */
    b[1] = D->Caps.Coding8b10b ? 1 : 0;
    if (NT_SUCCESS(st))
        st = DpcdWrite(D, MW_DPCD_DOWNSPREAD, b, 2);
    if (!NT_SUCCESS(st))
        return st;
    RtlZeroMemory(&D->Train, sizeof(D->Train));

    MwLog(A, L"Dp_TrainStep", 2);
    st = SetPattern(D, 1);
    for (it = 0; NT_SUCCESS(st) && it < 40; it++) {
        st = UpdateDrive(D, Rate);
        if (!NT_SUCCESS(st))
            break;
        Us(MwDpCrDelayUs(&D->Caps));
        st = DpcdRead(D, MW_DPCD_LANE01_STATUS, s, 6);
        if (!NT_SUCCESS(st))
            break;
        if (MwDpCrOk(s, MW_DP_LANES)) {
            cr = TRUE;
            break;
        }
        if (!MwDpAdjust(s, MW_DP_LANES, &D->Train)) {
            if (++same == 5)
                break;
        } else {
            same = 0;
        }
    }

    if (NT_SUCCESS(st) && cr) {
        MwLog(A, L"Dp_TrainStep", 3);
        eq = MwDpEqPattern(&D->Caps);
        st = SetPattern(D, eq);
        for (it = 1; NT_SUCCESS(st) && it < 5; it++) {
            st = UpdateDrive(D, Rate);
            if (!NT_SUCCESS(st))
                break;
            Us(MwDpEqDelayUs(&D->Caps));
            st = DpcdRead(D, MW_DPCD_LANE01_STATUS, s, 6);
            if (!NT_SUCCESS(st))
                break;
            if (!MwDpCrOk(s, MW_DP_LANES))
                break;
            if (MwDpEqOk(s, MW_DP_LANES)) {
                ok = TRUE;
                break;
            }
            (void)MwDpAdjust(s, MW_DP_LANES, &D->Train);
        }
    }
    (void)SetPattern(D, 0);
    MwLog(A, L"Dp_TrainStatus", (ULONG)s[0] | (ULONG)s[1] << 8 | (ULONG)s[2] << 16 | (ULONG)s[3] << 24);
    MwLog(A, L"Dp_TrainDrive", (ULONG)D->Train.Vs[0] | (ULONG)D->Train.Pe[0] << 4 |
                               (ULONG)D->Train.Vs[1] << 8 | (ULONG)D->Train.Pe[1] << 12);
    if (!NT_SUCCESS(st))
        return st;
    if (!ok)
        return STATUS_LINK_FAILED;
    MwLog(A, L"Dp_TrainStep", 4);
    return STATUS_SUCCESS;
}

/* The fastest rate the sink takes (at most HBR2) that trains and carries
 * T, stepping down as dw_dp_link_downgrade does. */
static NTSTATUS Train(MW_ADAPTER *A, MW_DP *D, const MW_TIMING *T)
{
    NTSTATUS st = STATUS_LINK_FAILED;

    for (ULONG r = MwDpStartRate(&D->Caps); r != 0; r = MwDpLowerRate(r)) {
        if (!MwDpLinkCarries(T->Clock, r, MW_DP_LANES))
            break;
        st = TrainAt(A, D, r);
        MwLog(A, L"Dp_TrainRate", r / 1000 | ((ULONG)st & 0xFFFF) << 16);
        if (NT_SUCCESS(st)) {
            D->Rate = r;
            return st;
        }
        D->TrainFails++;
    }
    D->Rate = 0;
    return st;
}

/* ---- start, presence ---- */

static const struct {
    SIZE_T Field;
    ULONG Pa;
    SIZE_T Len;
} DpRegions[] = {
    { FIELD_OFFSET(MW_DP, Ctl), MW_DP_PA, MW_DP_SIZE },
    { FIELD_OFFSET(MW_DP, Pma), MW_UDPHY_PMA_PA, MW_UDPHY_PMA_SIZE },
    { FIELD_OFFSET(MW_DP, Grf), MW_VO1_GRF_PA, PAGE_SIZE },
};

void MwDpUnmap(MW_ADAPTER *A)
{
    MW_DP *D = &A->Hdmi.Dp;

    for (ULONG i = 0; i < ARRAYSIZE(DpRegions); i++) {
        PUCHAR *va = (PUCHAR *)((PUCHAR)D + DpRegions[i].Field);
        if (*va != NULL) {
            MmUnmapIoSpace(*va, DpRegions[i].Len);
            *va = NULL;
        }
    }
}

/* What the firmware left (Dp_Fw: LANE_MUX_EN << 16 | VO1_GRF_SOC_CON0's
 * low 16 bits), the lanes found (Dp_Pads), the controller's HPD state
 * (Dp_HpdStatus), the sink's DPCD (Dp_Caps*), the EDID and the modes.
 * TRUE: a DP sink to report. From MwHpdStart, before the children are
 * counted. */
BOOLEAN MwDpStart(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    MW_DP *D = &H->Dp;
    UCHAR dpcd[16];
    ULONG mux, grf, n;
    NTSTATUS st;

    D->Present = FALSE;
    if (!Powered(A)) {
        MwLog(A, L"Dp_Off", 1);                 /* the firmware did not power PD_VO1 */
        return FALSE;
    }
    for (ULONG i = 0; i < ARRAYSIZE(DpRegions); i++) {
        PUCHAR *va = (PUCHAR *)((PUCHAR)D + DpRegions[i].Field);
        if (*va == NULL)
            *va = MapOne(DpRegions[i].Pa, DpRegions[i].Len);
        if (*va == NULL) {
            MwLog(A, L"Dp_Off", 2);
            MwDpUnmap(A);
            return FALSE;
        }
    }
    mux = Rd(D->Pma, MW_UDPHY_LANE_MUX_EN) & 0xFF;
    grf = Rd(D->Grf, MW_VO1_GRF_SOC_CON0) & 0xFFFF;
    MwLog(A, L"Dp_Fw", mux << 16 | grf);
    n = MwDpLanePads(mux, grf, D->Pad);
    MwLog(A, L"Dp_Pads", n | (ULONG)D->Pad[0] << 8 | (ULONG)D->Pad[1] << 16);
    if (n != MW_DP_LANES || !(grf & MW_VO1_HPD_SEL) || !(grf & MW_VO1_HPD_CFG) ||
        !(Rd(D->Pma, MW_UDPHY_DP_RSTN) & MW_UDPHY_DP_INIT_RSTN)) {
        MwLog(A, L"Dp_Off", 3);                 /* no alt mode from the firmware */
        return FALSE;
    }

    /* dw_dp_init_hw: no automatic fast link training, AUX reply events. */
    Rmw(D->Ctl, MW_DP_CCTL, MW_DP_CCTL_FAST_LT, 0);
    Rmw(D->Ctl, MW_DP_GEN_INT_EN, 0, MW_DP_GEN_INT_AUX_REPLY);
    MwLog(A, L"Dp_HpdStatus", Rd(D->Ctl, MW_DP_HPD_STATUS));

    st = DpcdRead(D, MW_DPCD_REV, dpcd, 16);
    if (NT_SUCCESS(st) && (dpcd[0x0E] & 0x80)) {
        UCHAR ext[16];
        if (NT_SUCCESS(DpcdRead(D, MW_DPCD_EXT_CAPS, ext, 16)))
            RtlCopyMemory(dpcd, ext, 14);       /* the extended caps, keeping 0x00E/0x00F */
    }
    MwLog(A, L"Dp_CapsStatus", (ULONG)st);
    if (!NT_SUCCESS(st))
        return FALSE;
    RtlCopyMemory(D->Dpcd, dpcd, sizeof(D->Dpcd));
    MwDpCaps(dpcd, &D->Caps);
    MwLog(A, L"Dp_Caps0", (ULONG)dpcd[0] | (ULONG)dpcd[1] << 8 | (ULONG)dpcd[2] << 16 | (ULONG)dpcd[3] << 24);
    MwLog(A, L"Dp_Caps5", (ULONG)dpcd[5] | (ULONG)dpcd[6] << 8 | (ULONG)dpcd[0x0E] << 16);
    if (D->Caps.MaxLanes < MW_DP_LANES) {
        MwLog(A, L"Dp_Off", 4);                 /* a one-lane sink */
        return FALSE;
    }
    D->Present = TRUE;
    (void)MwDpReadEdid(A);
    return TRUE;
}

/* The EDID and the modes from it, at the fastest rate the sink takes. */
BOOLEAN MwDpReadEdid(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    MW_DP *D = &H->Dp;
    UCHAR e[MW_EDID_MAX];
    ULONG len = 0, ext;
    NTSTATUS st;
    MW_TIMING m[MW_HDMI_MODES];
    ULONG n;

    st = EdidBlock(D, 0, e);
    if (NT_SUCCESS(st) && EdidOk(e)) {
        len = 128;
        ext = min((ULONG)e[126], MW_EDID_MAX / 128 - 1);
        for (ULONG b = 1; b <= ext; b++) {
            if (!NT_SUCCESS(EdidBlock(D, b, e + 128 * b)))
                break;
            len += 128;
        }
    }
    MwLog(A, L"Dp_EdidSize", len);
    MwLog(A, L"Dp_EdidStatus", (ULONG)st);
    n = MwDpModes(len ? e : NULL, len, MwDpStartRate(&D->Caps), (mwh_u32)(D->ResBytes / 4), m, MW_HDMI_MODES);
    if (H->Default60)
        MwHdmiDefaultFirst(m, n);
    KeWaitForSingleObject(&H->StateLock, Executive, KernelMode, FALSE, NULL);
    RtlCopyMemory(D->Edid, e, len);
    D->EdidSize = len;
    RtlCopyMemory(D->Modes, m, n * sizeof(MW_TIMING));
    D->ModeCount = n;
    KeReleaseMutex(&H->StateLock, FALSE);
    MwLog(A, L"Dp_Modes", n);
    if (n != 0)
        MwLog(A, L"Dp_Mode0", (ULONG)m[0].HDisplay << 16 | m[0].VDisplay);
    return len != 0;
}

/* Is a sink there: DPCD SINK_COUNT answers, and a branch device (an
 * adapter, DPCD 0x005 bit 0) has a sink behind it. One AUX attempt. */
BOOLEAN MwDpSinkThere(MW_ADAPTER *A)
{
    MW_DP *D = &A->Hdmi.Dp;
    UCHAR c = 0;

    if (!D->Present || !Powered(A))
        return FALSE;
    if (!NT_SUCCESS(Aux(D, MW_AUX_NATIVE_READ, MW_DPCD_SINK_COUNT, &c, 1, 2)))
        return FALSE;
    return !(D->Dpcd[5] & 1) || (c & 0xBF) != 0;
}

/* ---- VP1 ---- */

static BOOLEAN Vp1Standby(MW_HDMI *H)
{
    Wr(H->Vop, MW_VOP_VP1_INT_CLR, (1u << 6) << 16 | (1u << 6));        /* DSP_HOLD_VALID */
    Wr(H->Vop, MW_VP1_BASE + 0x00, MW_VP_DSP_STANDBY);
    Wr(H->Vop, MW_VOP_CFG_DONE, MwCfgDone(1));
    for (ULONG w = 0; w < 500; w++) {
        if (Rd(H->Vop, MW_VOP_VP1_INT_RAW) & (1u << 6)) {
            Wr(H->Vop, MW_VOP_VP1_INT_CLR, (1u << 6) << 16 | (1u << 6));
            return TRUE;
        }
        Us(100);
    }
    Wr(H->Vop, MW_VOP_VP1_INT_CLR, (1u << 6) << 16 | (1u << 6));
    return FALSE;
}

/* VPLL to V->Khz with dclk_vp1 gated, then dclk_vp1_src = VPLL / Div and
 * DCLK_VP1 on it (dual_calc.h MwDclkWrites). */
static NTSTATUS Vp1Clock(MW_ADAPTER *A, const MW_VPLL *V)
{
    MW_HDMI *H = &A->Hdmi;
    MW_REGW w[MW_VPLL_WRITES > MW_DCLK_WRITES ? MW_VPLL_WRITES : MW_DCLK_WRITES], nw;
    ULONG n, lock = 0;

    Wr(H->Cru, MW_CRU_GATE(62), (1u << 0) << 16 | 1u << 0);           /* DCLK_VP1 gated */
    n = MwVpllWrites(V, w);
    for (ULONG i = 0; i < n; i++)
        Wr(H->Cru, w[i].Off, w[i].Val);
    for (ULONG i = 0; i < 1000; i++) {
        lock = Rd(H->Cru, MW_CRU_VPLL_CON(6));
        if (lock & MW_VPLL_LOCK)
            break;
        Us(10);
    }
    MwLog(A, L"Dp_VpllLock", lock);
    if (!(lock & MW_VPLL_LOCK))
        return STATUS_DEVICE_NOT_READY;
    nw = MwVpllNormal();
    Wr(H->Cru, nw.Off, nw.Val);
    n = MwDclkWrites(1, MW_DCLK_VPLL, V->Div, w);
    for (ULONG i = 0; i < n; i++)
        Wr(H->Cru, w[i].Off, w[i].Val);
    Us(2000);
    return STATUS_SUCCESS;
}

/* DP off and VP1 let go of: the stream, the sink to D3 (best effort), the
 * transmitters, DP0 off its port, VP1 to standby, Esmart1 and VP1's layer
 * off. VPLL and the PHY stay as they are. */
void MwDpPark(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    MW_DP *D = &H->Dp;
    MW_REGW w[MW_OVL_WRITES], en;
    ULONG n;

    if (!D->Present || !Powered(A) || !MwHdmiPowered(A))
        return;
    Rmw(D->Ctl, MW_DP_VSAMPLE_CTRL, MW_DP_VIDEO_STREAM_EN, 0);
    Wr(H->Vop, MW_VOP_DP0_IF_CTRL, MW_IF_CFG_DONE_IMD);
    (void)DpcdW1(D, MW_DPCD_SET_POWER, 2);
    Rmw(D->Ctl, MW_DP_PHYIF_CTRL, 0xFu << 8, 0);
    MwLog(A, L"Dp_ParkHold", Vp1Standby(H));
    en = MwEsmartEnable(MW_DP_ESMART, FALSE);
    Wr(H->Vop, en.Off, en.Val);
    n = MwOvlWrites(1, 0xF, w);
    for (ULONG i = 0; i < n; i++)
        Wr(H->Vop, w[i].Off, w[i].Val);
    Wr(H->Vop, MW_VOP_CFG_DONE, MwCfgDone(1));
    D->Live = FALSE;
    D->Rate = 0;
    RtlZeroMemory(&D->Wire, sizeof(D->Wire));
}

/* DP at T, VP1 scanning out FbAddr (Pitch bytes a line) through Esmart1.
 * Trace: Dp_Step (1 VP1 standby, 2 clock, 3 window, 4 timing, 5 link, 6
 * video, 7 released), Dp_Div, Dp_Vpll, Dp_Video5, Dp_Status. */
NTSTATUS MwDpModeSet(MW_ADAPTER *A, const MW_TIMING *T, ULONG FbAddr, ULONG Pitch)
{
    MW_HDMI *H = &A->Hdmi;
    MW_DP *D = &H->Dp;
    MW_VP_REGS r;
    MW_VPLL v;
    MW_DP_VIDEO vid;
    MW_REGW w[MW_ESMART_WRITES > MW_OVL_WRITES ? MW_ESMART_WRITES : MW_OVL_WRITES], en;
    ULONG n;
    NTSTATUS st;

    if (!D->Present || !Powered(A) || !MwHdmiPowered(A)) {
        MwLog(A, L"Dp_Status", (ULONG)STATUS_DEVICE_NOT_READY);
        return STATUS_DEVICE_NOT_READY;
    }
    if (!MwVpllFor(T->Clock, &v)) {
        MwLog(A, L"Dp_Status", (ULONG)STATUS_GRAPHICS_INVALID_VIDPN);
        return STATUS_GRAPHICS_INVALID_VIDPN;
    }
    D->ModeSets++;
    MwVpRegsPort(T, FALSE, 1, &r);
    MwLog(A, L"Dp_Vpll", v.M | v.P << 10 | v.S << 16 | v.Div << 20);

    /* The old stream off, the port off DP0, VP1 to standby. */
    Rmw(D->Ctl, MW_DP_VSAMPLE_CTRL, MW_DP_VIDEO_STREAM_EN, 0);
    Wr(H->Vop, MW_VOP_DP0_IF_CTRL, MW_IF_CFG_DONE_IMD);
    MwLog(A, L"Dp_Step", 1);
    MwLog(A, L"Dp_Hold", Vp1Standby(H));

    MwLog(A, L"Dp_Step", 2);
    st = Vp1Clock(A, &v);
    if (!NT_SUCCESS(st))
        goto fail;

    MwLog(A, L"Dp_Step", 3);
    n = MwOvlWrites(1, MwEsmartLayerId(MW_DP_ESMART, 1), w);
    for (ULONG i = 0; i < n; i++)
        Wr(H->Vop, w[i].Off, w[i].Val);
    n = MwEsmartWrites(MW_DP_ESMART, 1, T->HDisplay, T->VDisplay, Pitch, FbAddr, w);
    for (ULONG i = 0; i < n; i++)
        Wr(H->Vop, w[i].Off, w[i].Val);
    en = MwEsmartEnable(MW_DP_ESMART, TRUE);
    Wr(H->Vop, en.Off, en.Val);

    /* VP1's timing with STANDBY set, VP_DSP_CTRL written whole once
     * (memory: project_rk3576_vop2_single_write); DP0 on VP1 without EN. */
    MwLog(A, L"Dp_Step", 4);
    Wr(H->Vop, MW_VP1_BASE + 0x0C, r.ClkCtrl);
    Wr(H->Vop, MW_VP1_BASE + 0x00, r.DspCtrl | MW_VP_DSP_STANDBY);
    Wr(H->Vop, MW_VP1_BASE + 0x48, r.HTotalHsEnd);
    Wr(H->Vop, MW_VP1_BASE + 0x4C, r.HActStEnd);
    Wr(H->Vop, MW_VP1_BASE + 0x54, r.VActStEnd);
    Wr(H->Vop, MW_VP1_BASE + 0x50, r.VTotalVsEnd);
    Wr(H->Vop, MW_VP1_BASE + 0x34, r.PostHAct);
    Wr(H->Vop, MW_VP1_BASE + 0x38, r.PostVAct);
    Wr(H->Vop, MW_VP1_BASE + 0x3C, r.PostSclFactor);
    Wr(H->Vop, MW_VP1_BASE + 0x40, r.PostSclCtrl);
    Wr(H->Vop, MW_VP1_BASE + 0x30, r.PreScan);
    Wr(H->Vop, MW_VOP_LINE_FLAG1, r.LineFlag);
    Wr(H->Vop, MW_VOP_DP0_IF_CTRL, MwDp0IfStaged(r.IfCtrl));
    Wr(H->Vop, MW_VOP_CFG_DONE, MwCfgDone(1));

    /* The link: the sink to D0 (drm_dp_link_power_up waits 1 ms), trained. */
    MwLog(A, L"Dp_Step", 5);
    if (D->Caps.Rev >= 0x11) {
        (void)DpcdW1(D, MW_DPCD_SET_POWER, 1);
        Ms(1);
    }
    st = Train(A, D, T);
    if (!NT_SUCCESS(st))
        goto fail;

    /* The stream: timing, MSA, TU, then VP1 out of standby, DP0 enabled on
     * it and the stream on (vendor order: the encoder up, then the
     * interface's EN). */
    MwLog(A, L"Dp_Step", 6);
    MwDpVideo(T, D->Rate, MW_DP_LANES, &vid);
    Rmw(D->Ctl, MW_DP_VSAMPLE_CTRL, (3u << 21) | (0x1Fu << 16), vid.Vsample);
    Wr(D->Ctl, MW_DP_VINPUT_POL, vid.Pol);
    Wr(D->Ctl, MW_DP_VIDEO_CONFIG1, vid.Cfg1);
    Wr(D->Ctl, MW_DP_VIDEO_CONFIG2, vid.Cfg2);
    Wr(D->Ctl, MW_DP_VIDEO_CONFIG3, vid.Cfg3);
    Wr(D->Ctl, MW_DP_VIDEO_CONFIG4, vid.Cfg4);
    Wr(D->Ctl, MW_DP_VIDEO_CONFIG5, vid.Cfg5);
    Wr(D->Ctl, MW_DP_VIDEO_MSA1, vid.Msa1);
    Wr(D->Ctl, MW_DP_VIDEO_MSA2, vid.Msa2);
    Wr(D->Ctl, MW_DP_VIDEO_MSA3, vid.Msa3);
    Wr(D->Ctl, MW_DP_HBLANK_INTERVAL, vid.Hblank);
    MwLog(A, L"Dp_Video5", vid.Cfg5);

    MwLog(A, L"Dp_Step", 7);
    Wr(H->Vop, MW_VP1_BASE + 0x00, r.DspCtrl);
    Wr(H->Vop, MW_VOP_CFG_DONE, MwCfgDone(1));
    Wr(H->Vop, MW_VOP_DP0_IF_CTRL, r.IfCtrl);
    MwLog(A, L"Dp_IfCtrl", r.IfCtrl);
    Wr(D->Ctl, MW_DP_GEN_INT, MW_DP_GEN_INT_VFIFO_OVF);
    Rmw(D->Ctl, MW_DP_VSAMPLE_CTRL, 0, MW_DP_VIDEO_STREAM_EN);
    Ms(40);
    /* A video FIFO overflow says the TU or the pixel clock is wrong. */
    MwLog(A, L"Dp_GenInt", Rd(D->Ctl, MW_DP_GEN_INT));
    D->Live = TRUE;
    D->Wire = *T;
    MwLog(A, L"Dp_Status", 0);
    return STATUS_SUCCESS;

fail:
    D->ModeSetFails++;
    MwLog(A, L"Dp_Status", (ULONG)st);
    MwDpPark(A);
    return st;
}
