// SPDX-License-Identifier: GPL-2.0
/*
 * The RK3576's HDMI output, driven from Windows: hot-plug detect, DDC (the
 * EDID and SCDC), the HDPTX PHY, the DW-HDMI-QP controller, VOP2 video
 * port 0, and the HDMI audio preset. Only ever reached with the HdmiHpd
 * setting on (hpd.c says what runs at which level).
 *
 * Ported from the firmware that lights this board, not invented: every
 * sequence below is edk2-rk3576's (main, ae2f55e), with its order, its
 * values and its delays:
 *
 *   HPD          DwHdmiQpLib.c DwHdmiReadHpd, DwHdmiQpConnectorPreInit
 *   DDC          DwHdmiQpLib.c DwHdmiI2cInit, DwHdmiI2cRead/Write,
 *                DwHdmiQpI2cXfer, DwHdmiReadEdidBlock, DwHdmiScdcRead/Write
 *   clocks       LCKFB/TaishanPi3M RockchipPlatformLib.c HdmiTxIomux
 *   controller   DwHdmiQpLib.c DwHdmiQpSetup, steps [0] to [11]
 *   PHY          PhyRockchipSamsungHdptxHdmi.c HdptxRopllCmnConfig,
 *                HdptxPrePowerUp, HdptxPostEnablePll,
 *                HdptxRopllTmdsModeConfig, HdptxPostEnableLane
 *   VOP2 VP0     Vop2Dxe.c Vop2Init, Vop2IfConfig, Vop2PostConfig,
 *                Vop2SetSmartWin, Vop2Enable, Vop2DsiHandoffOsHandler
 *   audio        DwHdmiQpAudio.c
 *
 * Where the firmware and mainline Linux disagree the firmware wins, since it
 * is what has put a picture on this board; the places are noted where they
 * are written. In short: no PRE_DITHER_DOWN_EN on RGB888 (mainline sets
 * it; memory project_rk3576_dspctrl_predither), DCLK_VP0 switched to the
 * PHY's pixel clock with VP0 in standby around the switch (mainline does it
 * from atomic_enable before standby is released), VIDEO_INTERFACE_CONFIG0
 * left 0, SCDC RR_Enable cleared (vendor), DDC SCL counts 0x085c085c
 * (mainline's, memory project_rk3576_ddc_timing), and no AMD packet with
 * audio.
 *
 * Board-specific steps of the firmware's HdmiTxIomux are not repeated: the
 * TaishanPi 3M's level-shifter enable (GPIO4 PC6) and the pin mux are left
 * as the firmware set them in DwHdmiQpConnectorPreInit, which runs on
 * every boot whichever screen it picks.
 *
 * Registers are mapped by physical address, as scanout.c maps the VOP2's:
 * GPU0's _CRS has the GPU and, for a panel, the backlight, and a resource
 * added there would move the backlight's index (backlight.c). See
 * docs/HDMI-HPD.md for the ACPI alternative.
 *
 * Writes to the VOP2 follow scanout.c's rule: the window and port registers
 * are written, never read back. Two VOP2 reads remain, both of status the
 * firmware itself polls: VP0's interrupt raw status for DSP_HOLD_VALID
 * (Vop2DsiHandoffOsHandler) and nothing else.
 */
#include "maliwddm.h"

/* ---- where things are (edk2-rk3576 DwHdmiQpLib.h, Vop2Regs.h, Soc.h) ---- */

#define PA_IOC          0x2604A000u     /* SYS GRF 0x26040000 + 0xA000: IOMUX, pulls, HPD */
#define PA_GPIO4        0x2AE40000u
#define PA_VO0_GRF      0x2601A000u
#define PA_HDPTX_GRF    0x26032000u
#define PA_HDPTX_PHY    0x2B000000u
#define PA_HDMI         0x27DA0000u
#define PA_CRU          0x27200000u
#define PA_PMU1CRU      0x27220000u
#define PA_PMU          0x27380000u
#define PA_VOP          0x27D00000u

#define SZ_PHY          0x2000u
#define SZ_HDMI         0x4000u
#define SZ_VOP          0x2000u

enum { M_IOC = 1, M_GPIO4 = 2, M_VO0 = 4, M_PHYGRF = 8, M_PHY = 16, M_CTL = 32, M_CRU = 64,
       M_PMU1CRU = 128, M_PMU = 256, M_VOP = 512, M_ALL = 1023 };

/* IOC (offsets within PA_IOC) */
#define IOC_MISC_CON0           0x400
#define IOC_HDMI_HPD_STATUS     0x440
#define HPD_LEVEL_INT           (1u << 3)
#define HPD_PORT_LEVEL          (1u << 6)
#define GPIO_EXT_PORT           0x70
#define GPIO4_PC1               (1u << 17)

/* PMU: power domain status (Vop2Rk3576.c, mainline pm-domains.c) */
#define PMU_REPAIR_STATUS       0x570
#define PD_VOP                  (1u << 11)
#define PD_VO0                  (1u << 15)

/* VO0 GRF */
#define VO0_GRF_SOC_CON1        0x04
#define VO0_GRF_SOC_CON8        0x20

/* HDPTX GRF */
#define GRF_HDPTX_CON0          0x00
#define LC_REF_CLK_SEL          (1u << 11)
#define HDPTX_I_PLL_EN          (1u << 7)
#define HDPTX_I_BIAS_EN         (1u << 6)
#define HDPTX_I_BGR_EN          (1u << 5)
#define HDPTX_MODE_SEL          (1u << 0)
#define GRF_HDPTX_STATUS        0x80
#define HDPTX_O_PLL_LOCK_DONE   (1u << 3)
#define HDPTX_O_PHY_CLK_RDY     (1u << 2)
#define HDPTX_O_PHY_RDY         (1u << 1)

/* PMU1CRU */
#define PMU1CRU_SOFTRST_CON00   0xA00
#define PMU1CRU_SOFTRST_CON01   0xA04
#define HDPTX_APB_RST           (1u << 1)
#define HDPTX_INIT_RST          (1u << 9)
#define HDPTX_CMN_RST           (1u << 10)
#define HDPTX_LANE_RST          (1u << 11)
#define HDPTX_ALL_RST           (HDPTX_INIT_RST | HDPTX_CMN_RST | HDPTX_LANE_RST)

/* CRU */
#define CRU_CLKSEL(n)           (0x300u + (n) * 4u)
#define CRU_GATE(n)             (0x800u + (n) * 4u)
#define CRU_SOFTRST(n)          (0xA00u + (n) * 4u)
#define CRU_GPLL_CON0           0x1C0
#define CRU_CPLL_CON0           0x1A0
#define CRU_MODE_CON00          0x280

/* HDPTX PHY: register n is at n * 4 */
#define PHYR(n)                 ((ULONG)(n) * 4u)

/* DW-HDMI-QP */
#define GLOBAL_SWRESET_REQUEST  0x40
#define AUDIO_SWINIT_P          (1u << 10)
#define GLOBAL_SWDISABLE        0x44
#define AUDIO_SWDISABLE         (1u << 10)
#define VIDEO_SWDISABLE         (1u << 6)
#define TIMER_BASE_CONFIG0      0x80
#define I2CM_SM_SCL_CONFIG0     0xe0
#define I2CM_FM_SCL_CONFIG0     0xe4
#define I2CM_CONTROL0           0xec
#define I2CM_INTERFACE_CONTROL0 0xf4
#define I2CM_ADDR               0xff000
#define I2CM_SLVADDR            0xfe0
#define I2CM_WR_MASK            0x1e
#define I2CM_EXT_READ           (1u << 4)
#define I2CM_FM_READ            (1u << 2)
#define I2CM_FM_WRITE           (1u << 1)
#define I2CM_FM_EN              (1u << 0)
#define I2CM_INTERFACE_CONTROL1 0xf8
#define I2CM_SEG_PTR            0x7f80
#define I2CM_SEG_ADDR           0x7f
#define I2CM_WRDATA_0_3         0xfc
#define I2CM_RDDATA_0_3         0x10c
#define FLT_CONFIG0             0x160
#define AUDIO_INTERFACE_CONFIG0 0x820
#define AUDIO_INTERFACE_CONTROL0 0x82c
#define FRAME_COMPOSER_CONFIG9  0x864
#define KEEPOUT_REKEY_CFG       (3u << 8)
#define KEEPOUT_REKEY_ALWAYS    (2u << 8)
#define HDCP2LOGIC_CONFIG0      0x8e0
#define HDCP2_BYPASS            (1u << 0)
#define SCRAMB_CONFIG0          0x960
#define LINK_CONFIG0            0x968
#define OPMODE_FRL_4LANES       (1u << 8)
#define OPMODE_DVI              (1u << 4)
#define OPMODE_FRL              (1u << 0)
#define PKTSCHED_PKT_CONFIG1    0xa9c
#define PKTSCHED_VSI_FIELDRATE  (1u << 14)
#define PKTSCHED_AVI_FIELDRATE  (1u << 12)
#define PKTSCHED_PKT_EN         0xaa8
#define PKTSCHED_AUDI_TX_EN     (1u << 15)
#define PKTSCHED_AVI_TX_EN      (1u << 13)
#define PKTSCHED_VSI_TX_EN      (1u << 12)
#define PKTSCHED_AMD_TX_EN      (1u << 8)
#define PKTSCHED_GCP_TX_EN      (1u << 3)
#define PKTSCHED_AUDS_TX_EN     (1u << 2)
#define PKTSCHED_ACR_TX_EN      (1u << 1)
#define PKTSCHED_PKT_CONTROL0   0xaac
#define PKT_VSI_CONTENTS0       0xbc0
#define PKT_VSI_CONTENTS7       0xbdc
#define PKT_AVI_CONTENTS0       0xbe0
#define PKT_AUDI_CONTENTS0      0xc20
#define AUDPKT_CONTROL0         0xe20
#define AUDPKT_ACR_CONTROL0     0xe40
#define AUDPKT_ACR_CONTROL1     0xe44
#define AUDPKT_CHSTATUS_OVR0    0xe60
#define AUDPKT_CHSTATUS_OVR1    0xe64
#define MAINUNIT_0_INT_MASK_N   0x3014
#define MAINUNIT_1_INT_STATUS   0x3020
#define MAINUNIT_1_INT_MASK_N   0x3024
#define MAINUNIT_1_INT_CLEAR    0x3028
#define I2CM_NACK_RCVD          (1u << 2)
#define I2CM_READ_REQUEST       (1u << 1)
#define I2CM_OP_DONE            (1u << 0)

/* VOP2 (RK3576) */
#define VOP_CFG_DONE            0x000
#define VOP_CFG_DONE_VP0        0x00018001u
#define VOP_LINE_FLAG0          0x070
#define VOP_VP0_INT_CLR         0x0A4
#define VOP_VP0_INT_RAW         0x0AC
#define VP_INT_DSP_HOLD_VALID   (1u << 6)
#define VOP_MIPI0_IF_CTRL       0x180
#define VOP_HDMI0_IF_CTRL       0x184
#define VP0_DSP_CTRL            0xC00
#define VP0_CLK_CTRL            0xC0C
#define VP0_PRE_SCAN_HTIMING    0xC30
#define VP0_POST_DSP_HACT_INFO  0xC34
#define VP0_POST_DSP_VACT_INFO  0xC38
#define VP0_POST_SCL_FACTOR     0xC3C
#define VP0_POST_SCL_CTRL       0xC40
#define VP0_DSP_HTOTAL_HS_END   0xC48
#define VP0_DSP_HACT_ST_END     0xC4C
#define VP0_DSP_VTOTAL_VS_END   0xC50
#define VP0_DSP_VACT_ST_END     0xC54
#define ESMART0_YRGB_MST        0x1814
#define ESMART0_VIR             0x181C
#define ESMART0_ACT_INFO        0x1820
#define ESMART0_DSP_INFO        0x1824
#define ESMART0_DSP_ST          0x1828

/* ---- access ---- */

static __inline void Wr(PUCHAR B, ULONG Off, ULONG V)
{
    WRITE_REGISTER_ULONG((PULONG)(B + Off), V);
}

static __inline ULONG Rd(PUCHAR B, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(B + Off));
}

/* A hiword-masked write: the upper half says which bits of the lower may change. */
static __inline void Hw(PUCHAR B, ULONG Off, ULONG Mask, ULONG Val)
{
    Wr(B, Off, (Mask << 16) | (Val & Mask));
}

static __inline void Mod(PUCHAR B, ULONG Off, ULONG Val, ULONG Mask)
{
    Wr(B, Off, (Rd(B, Off) & ~Mask) | Val);
}

static void Us(ULONG Us_)
{
    KeStallExecutionProcessor(Us_);
}

/* PASSIVE_LEVEL: a wait of a few ms or more, which may run long. */
static void Ms(ULONG Ms_)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)Ms_ * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static PUCHAR MapOne(ULONG Pa, SIZE_T Len)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = Pa;
    return (PUCHAR)MmMapIoSpaceEx(p, Len, PAGE_READWRITE | PAGE_NOCACHE);
}

typedef struct _REGION {
    SIZE_T Field;               /* offset of the PUCHAR in MW_HDMI */
    ULONG Pa;
    SIZE_T Len;
    ULONG Bit;
} REGION;

static const REGION Regions[] = {
    { FIELD_OFFSET(MW_HDMI, Ioc), PA_IOC, PAGE_SIZE, M_IOC },
    { FIELD_OFFSET(MW_HDMI, Gpio4), PA_GPIO4, PAGE_SIZE, M_GPIO4 },
    { FIELD_OFFSET(MW_HDMI, Vo0Grf), PA_VO0_GRF, PAGE_SIZE, M_VO0 },
    { FIELD_OFFSET(MW_HDMI, PhyGrf), PA_HDPTX_GRF, PAGE_SIZE, M_PHYGRF },
    { FIELD_OFFSET(MW_HDMI, Phy), PA_HDPTX_PHY, SZ_PHY, M_PHY },
    { FIELD_OFFSET(MW_HDMI, Ctl), PA_HDMI, SZ_HDMI, M_CTL },
    { FIELD_OFFSET(MW_HDMI, Cru), PA_CRU, PAGE_SIZE, M_CRU },
    { FIELD_OFFSET(MW_HDMI, Pmu1Cru), PA_PMU1CRU, PAGE_SIZE, M_PMU1CRU },
    { FIELD_OFFSET(MW_HDMI, Pmu), PA_PMU, PAGE_SIZE, M_PMU },
    { FIELD_OFFSET(MW_HDMI, Vop), PA_VOP, SZ_VOP, M_VOP },
};

static PUCHAR *RegionVa(MW_HDMI *H, ULONG I)
{
    return (PUCHAR *)((PUCHAR)H + Regions[I].Field);
}

NTSTATUS MwHdmiMap(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    for (ULONG i = 0; i < ARRAYSIZE(Regions); i++) {
        PUCHAR *va = RegionVa(H, i);
        if (*va == NULL)
            *va = MapOne(Regions[i].Pa, Regions[i].Len);
        if (*va != NULL)
            H->Mapped |= Regions[i].Bit;
    }
    MwLog(A, L"Hpd_Mapped", H->Mapped);
    return H->Mapped == M_ALL ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

void MwHdmiUnmap(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    for (ULONG i = 0; i < ARRAYSIZE(Regions); i++) {
        PUCHAR *va = RegionVa(H, i);
        if (*va != NULL) {
            MmUnmapIoSpace(*va, Regions[i].Len);
            *va = NULL;
        }
    }
    H->Mapped = 0;
}

/* PD_VOP and PD_VO0 up (the firmware's Rk3576DisplayPowerDomainsOn): the
 * controller, the PHY GRF and the VOP2 answer only then, and an access to
 * a domain that is off is a synchronous external abort. */
BOOLEAN MwHdmiPowered(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    ULONG v;

    if (H->Mapped != M_ALL)
        return FALSE;
    v = Rd(H->Pmu, PMU_REPAIR_STATUS);
    return (v & (PD_VOP | PD_VO0)) == (PD_VOP | PD_VO0);
}

/* The controller's and the PHY's bus clocks ungated (what HdmiTxIomux
 * ungates: CRU GATE_CON63 bits 0, 1, 3, 12-14 and GATE_CON64 bits 7-9,
 * PMU1CRU GATE_CON0 bit 1). DDC is only tried then: a register access to a
 * block whose clock is gated would be a bus error. A mode set ungates them
 * itself (HdmiClocks). Raw: the three gate words packed for the trace. */
static BOOLEAN Clocked(MW_HDMI *H, ULONG *Raw)
{
    ULONG g63 = Rd(H->Cru, 0x800 + 63 * 4), g64 = Rd(H->Cru, 0x800 + 64 * 4);
    ULONG p0 = Rd(H->Pmu1Cru, 0x800);

    *Raw = (g63 & 0x700B) | (g64 & 0x0380) << 16 | (p0 & 0x2) << 30;
    return (g63 & 0x700B) == 0 && (g64 & 0x0380) == 0 && (p0 & 0x2) == 0;
}

/* ---- hot-plug detect ----
 *
 * IOC_HDMI_HPD_STATUS, as DwHdmiReadHpd reads it. Bit 3 (LEVEL_INT) is what
 * mainline's dw_hdmi_qp_rk3576_read_hpd tests and what the firmware's HPD
 * wait in ConnectorPreInit polls; it follows the pad only because the
 * firmware leaves HPD_INT_CLR set in IOC_MISC_CON0 (SetIomux). Bit 6 is the
 * vendor's HDMITX_HPD_PORT_LEVEL. HdmiHpdSource picks which; the raw word
 * goes to the trace with the GPIO4_PC1 pad level in bit 8. */
BOOLEAN MwHdmiReadHpd(MW_ADAPTER *A, ULONG *Raw)
{
    MW_HDMI *H = &A->Hdmi;
    ULONG st, pad;

    if ((H->Mapped & (M_IOC | M_GPIO4)) != (M_IOC | M_GPIO4)) {
        *Raw = 0xFFFFFFFFu;
        return FALSE;
    }
    st = Rd(H->Ioc, IOC_HDMI_HPD_STATUS);
    pad = (Rd(H->Gpio4, GPIO_EXT_PORT) & GPIO4_PC1) ? 1u : 0u;
    *Raw = (st & 0xFF) | pad << 8 | (Rd(H->Ioc, IOC_MISC_CON0) & 0xFF) << 16;
    switch (H->Source) {
    case 1:  return (st & HPD_PORT_LEVEL) != 0;
    case 2:  return pad != 0;
    default: return (st & HPD_LEVEL_INT) != 0;
    }
}

/* ---- DDC: the controller's I2C master ---- */

typedef struct _DDC {
    MW_ADAPTER *A;
    PUCHAR C;
    ULONG SlaveReg;
    BOOLEAN IsRegAddr, IsSegment;
    ULONG64 Deadline;           /* interrupt time, 100 ns */
    ULONG Timeouts, Nacks;
} DDC;

static ULONG64 Now100ns(void)
{
    return KeQueryInterruptTime();
}

/* DwHdmiI2cInit. */
static void DdcInit(PUCHAR C)
{
    Wr(C, MAINUNIT_0_INT_MASK_N, 0);
    Wr(C, MAINUNIT_1_INT_MASK_N, 0);
    Wr(C, TIMER_BASE_CONFIG0, 0x179A7B00);     /* 396 MHz */
    Wr(C, I2CM_CONTROL0, 0x01);                 /* software reset */
    Wr(C, I2CM_SM_SCL_CONFIG0, 0x085c085c);
    Wr(C, I2CM_FM_SCL_CONFIG0, 0x085c085c);
    Mod(C, I2CM_INTERFACE_CONTROL0, 0, I2CM_FM_EN);
    Wr(C, MAINUNIT_1_INT_CLEAR, I2CM_OP_DONE | I2CM_NACK_RCVD);
}

/* One operation: started, then up to 20 ms for DONE or NACK, polled every
 * 20 us where the firmware waits 1 ms (a Windows wait of 1 ms can be 15).
 * 0: done; 1: timed out; 2: NACK. */
static int DdcWait(DDC *D)
{
    for (ULONG t = 0; t < 1000; t++) {
        ULONG intr;
        Us(20);
        intr = Rd(D->C, MAINUNIT_1_INT_STATUS) & (I2CM_OP_DONE | I2CM_READ_REQUEST | I2CM_NACK_RCVD);
        if (intr) {
            Wr(D->C, MAINUNIT_1_INT_CLEAR, intr);
            return (intr & I2CM_NACK_RCVD) ? 2 : 0;
        }
    }
    return 1;
}

/* DwHdmiI2cRead: one byte at a time ("I2CM_NBYTES > 0 seems broken"). The
 * retry budget is the firmware's (50, a timeout costs 10), and the whole read
 * stops at the deadline so that a missing sink cannot hold the thread. */
static NTSTATUS DdcRead(DDC *D, UCHAR *Buf, ULONG Len)
{
    MW_ADAPTER *a = D->A;
    ULONG raw;

    if (!D->IsRegAddr) {
        D->SlaveReg = 0;
        D->IsRegAddr = TRUE;
    }
    while (Len--) {
        LONG retry;
        int r = 1;
        Mod(D->C, I2CM_INTERFACE_CONTROL0, D->SlaveReg << 12, I2CM_ADDR);
        for (retry = 50; retry > 0;) {
            if (!MwHdmiReadHpd(a, &raw) || Now100ns() > D->Deadline)
                return STATUS_DEVICE_NOT_CONNECTED;
            Mod(D->C, I2CM_INTERFACE_CONTROL0, D->IsSegment ? I2CM_EXT_READ : I2CM_FM_READ, I2CM_WR_MASK);
            r = DdcWait(D);
            if (r == 1) {
                D->Timeouts++;
                Wr(D->C, I2CM_CONTROL0, 0x01);
                Mod(D->C, I2CM_INTERFACE_CONTROL0, 0, I2CM_WR_MASK);
                retry -= 10;
                continue;
            }
            if (r == 2) {
                D->Nacks++;
                Wr(D->C, I2CM_CONTROL0, 0x01);
                Mod(D->C, I2CM_INTERFACE_CONTROL0, 0, I2CM_WR_MASK);
                retry--;
                Ms(10);
                continue;
            }
            break;
        }
        if (retry <= 0)
            return r == 1 ? STATUS_IO_TIMEOUT : STATUS_DEVICE_PROTOCOL_ERROR;
        *Buf++ = (UCHAR)(Rd(D->C, I2CM_RDDATA_0_3) & 0xFF);
        D->SlaveReg++;
        Mod(D->C, I2CM_INTERFACE_CONTROL0, 0, I2CM_WR_MASK);
    }
    D->IsSegment = FALSE;
    return STATUS_SUCCESS;
}

/* DwHdmiI2cWrite. */
static NTSTATUS DdcWrite(DDC *D, const UCHAR *Buf, ULONG Len)
{
    MW_ADAPTER *a = D->A;
    ULONG raw;

    if (!D->IsRegAddr) {
        D->SlaveReg = Buf[0];
        Len--;
        Buf++;
        D->IsRegAddr = TRUE;
    }
    while (Len--) {
        LONG retry;
        int r = 1;
        for (retry = 50; retry > 0;) {
            if (!MwHdmiReadHpd(a, &raw) || Now100ns() > D->Deadline)
                return STATUS_DEVICE_NOT_CONNECTED;
            Wr(D->C, I2CM_WRDATA_0_3, *Buf);
            Mod(D->C, I2CM_INTERFACE_CONTROL0, D->SlaveReg << 12, I2CM_ADDR);
            Mod(D->C, I2CM_INTERFACE_CONTROL0, I2CM_FM_WRITE, I2CM_WR_MASK);
            r = DdcWait(D);
            if (r == 1) {
                D->Timeouts++;
                Wr(D->C, I2CM_CONTROL0, 0x01);
                Mod(D->C, I2CM_INTERFACE_CONTROL0, 0, I2CM_WR_MASK);
                retry -= 10;
                continue;
            }
            if (r == 2) {
                D->Nacks++;
                Wr(D->C, I2CM_CONTROL0, 0x01);
                Mod(D->C, I2CM_INTERFACE_CONTROL0, 0, I2CM_WR_MASK);
                retry--;
                Ms(10);
                continue;
            }
            break;
        }
        Mod(D->C, I2CM_INTERFACE_CONTROL0, 0, I2CM_WR_MASK);
        if (retry <= 0)
            return r == 1 ? STATUS_IO_TIMEOUT : STATUS_DEVICE_PROTOCOL_ERROR;
        Buf++;
        D->SlaveReg++;
    }
    return STATUS_SUCCESS;
}

typedef struct _DDC_MSG {
    UCHAR Addr;
    BOOLEAN Read;
    ULONG Len;
    UCHAR *Buf;
} DDC_MSG;

static void Msg(DDC_MSG *M, UCHAR Addr, BOOLEAN Read, ULONG Len, UCHAR *Buf)
{
    M->Addr = Addr;
    M->Read = Read;
    M->Len = Len;
    M->Buf = Buf;
}

#define DDC_SEGMENT_ADDR    0x30
#define DDC_ADDR            0x50
#define SCDC_ADDR           0x54

/* DwHdmiQpI2cXfer. */
static NTSTATUS DdcXfer(DDC *D, DDC_MSG *M, ULONG N)
{
    UCHAR addr = M[0].Addr;
    NTSTATUS st = STATUS_SUCCESS;

    for (ULONG i = 0; i < N; i++)
        if (M[i].Len == 0)
            return STATUS_NOT_SUPPORTED;
    Mod(D->C, MAINUNIT_1_INT_MASK_N, I2CM_NACK_RCVD | I2CM_OP_DONE, I2CM_NACK_RCVD | I2CM_OP_DONE);
    if (addr == DDC_SEGMENT_ADDR && M[0].Len == 1)
        addr = DDC_ADDR;
    Mod(D->C, I2CM_INTERFACE_CONTROL0, (ULONG)addr << 5, I2CM_SLVADDR);
    D->IsRegAddr = FALSE;
    D->IsSegment = FALSE;
    for (ULONG i = 0; i < N && NT_SUCCESS(st); i++) {
        if (M[i].Addr == DDC_SEGMENT_ADDR && M[i].Len == 1) {
            D->IsSegment = TRUE;
            Mod(D->C, I2CM_INTERFACE_CONTROL1, DDC_SEGMENT_ADDR, I2CM_SEG_ADDR);
            Mod(D->C, I2CM_INTERFACE_CONTROL1, (ULONG)M[i].Buf[0] << 7, I2CM_SEG_PTR);
        } else if (M[i].Read) {
            st = DdcRead(D, M[i].Buf, M[i].Len);
        } else {
            st = DdcWrite(D, M[i].Buf, M[i].Len);
        }
    }
    Mod(D->C, MAINUNIT_1_INT_MASK_N, 0, I2CM_OP_DONE | I2CM_NACK_RCVD);
    return st;
}

static NTSTATUS EdidBlock(DDC *D, ULONG Block, UCHAR *Buf)
{
    UCHAR seg = (UCHAR)(Block >> 1), base = (UCHAR)(Block * 128);
    DDC_MSG m[3];
    ULONG skip = seg > 0 ? 0 : 1;

    Msg(&m[0], DDC_SEGMENT_ADDR, FALSE, 1, &seg);
    Msg(&m[1], DDC_ADDR, FALSE, 1, &base);
    Msg(&m[2], DDC_ADDR, TRUE, 128, Buf);
    return DdcXfer(D, m + skip, 3 - skip);
}

static BOOLEAN BlockOk(const UCHAR *B, ULONG Index)
{
    static const UCHAR header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    UCHAR sum = 0;

    for (ULONG i = 0; i < 128; i++)
        sum = (UCHAR)(sum + B[i]);
    if (Index == 0)
        return sum == 0 && RtlCompareMemory(B, header, sizeof(header)) == sizeof(header);
    /* An extension with a bad checksum is let through, as display.c's
     * EdidLength and Linux do (KVM switches that edit CEA blocks). */
    return TRUE;
}

static NTSTATUS ScdcWrite(DDC *D, UCHAR Reg, UCHAR Val)
{
    UCHAR b[2];
    DDC_MSG m;

    b[0] = Reg;
    b[1] = Val;
    Msg(&m, SCDC_ADDR, FALSE, 2, b);
    return DdcXfer(D, &m, 1);
}

static NTSTATUS ScdcRead(DDC *D, UCHAR Reg, UCHAR *Val)
{
    DDC_MSG m[2];

    Msg(&m[0], SCDC_ADDR, FALSE, 1, &Reg);
    Msg(&m[1], SCDC_ADDR, TRUE, 1, Val);
    return DdcXfer(D, m, 2);
}

/* DwHdmiQpConnectorGetEdid: the base block and up to four extensions, each
 * tried four times if its checksum is bad. PASSIVE_LEVEL, under HwLock.
 * Trace: Edid_Status, Edid_Timeouts, Edid_Nacks, Edid_Ms. */
NTSTATUS MwHdmiReadEdid(MW_ADAPTER *A, UCHAR *Buf, ULONG Max, ULONG *Len)
{
    DDC d;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG ext = 0, b;
    ULONG64 t0 = Now100ns();

    *Len = 0;
    if (!MwHdmiPowered(A)) {
        MwLog(A, L"Edid_Status", (ULONG)STATUS_DEVICE_POWERED_OFF);
        return STATUS_DEVICE_POWERED_OFF;
    }
    {
        ULONG gates;
        BOOLEAN on = Clocked(&A->Hdmi, &gates);
        MwLog(A, L"Edid_Gates", gates);
        if (!on) {
            MwLog(A, L"Edid_Status", (ULONG)STATUS_DEVICE_NOT_READY);
            return STATUS_DEVICE_NOT_READY;
        }
    }
    RtlZeroMemory(&d, sizeof(d));
    d.A = A;
    d.C = A->Hdmi.Ctl;
    d.Deadline = t0 + 2000 * 10000ull;          /* 2 s for all of it */
    DdcInit(d.C);
    for (b = 0; b <= ext && (b + 1) * 128 <= Max; b++) {
        ULONG retry;
        for (retry = 4; retry > 0; retry--) {
            st = EdidBlock(&d, b, Buf + b * 128);
            if (!NT_SUCCESS(st))
                break;
            if (BlockOk(Buf + b * 128, b))
                break;
        }
        if (!NT_SUCCESS(st))
            break;
        if (retry == 0) {
            st = STATUS_DATA_ERROR;
            break;
        }
        if (b == 0)
            ext = min((ULONG)Buf[126], 3u);
        *Len = (b + 1) * 128;
    }
    if (*Len != 0 && !NT_SUCCESS(st) && b > 0)
        st = STATUS_SUCCESS;                    /* the base block and what followed it */
    MwLog(A, L"Edid_Status", (ULONG)st);
    MwLog(A, L"Edid_Timeouts", d.Timeouts);
    MwLog(A, L"Edid_Nacks", d.Nacks);
    MwLog(A, L"Edid_Ms", (ULONG)((Now100ns() - t0) / 10000));
    return st;
}

/* ---- clocks and resets (HdmiTxIomux, without the board's GPIO) ---- */

static void HdmiClocks(MW_HDMI *H)
{
    Wr(H->Cru, CRU_GATE(61), 0x3F0D0000);       /* VOP root and VP clocks */
    Wr(H->Cru, CRU_GATE(62), 0x00030000);       /* DCLK_VP1, DCLK_VP2 */
    Wr(H->Cru, CRU_GATE(63), 0x700B0000);       /* VO0 root, HDCP0 bus clocks */
    Wr(H->Cru, CRU_GATE(64), 0x03800000);       /* HDMI TX */
    Wr(H->Pmu1Cru, 0x800, (1u << 1) << 16);     /* PCLK_HDPTX_APB */
    Wr(H->Pmu1Cru, 0x804, (1u << 13) << 16);    /* CLK_HDMITXHDP */
    Wr(H->Pmu1Cru, 0x814, (1u << 0) << 16);     /* PCLK_PMUPHY_ROOT */
    Hw(H->PhyGrf, GRF_HDPTX_CON0, HDPTX_I_BIAS_EN | HDPTX_I_BGR_EN, HDPTX_I_BIAS_EN | HDPTX_I_BGR_EN);
    Us(100);
    Wr(H->Cru, CRU_SOFTRST(64), (1u << 9) << 16);       /* SRST_HDMITX0_REF */
    Wr(H->Cru, CRU_SOFTRST(75), (1u << 1) << 16);       /* SRST_LINKSYM_HDMITXPHY0 */
    Wr(H->Pmu1Cru, PMU1CRU_SOFTRST_CON01, (1u << 13) << 16);   /* SRST_HDMITXHDP */
    Ms(5);
}

/* ---- the HDPTX PHY ---- */

static __inline void PhyW(MW_HDMI *H, ULONG Reg, ULONG V)
{
    Wr(H->Phy, PHYR(Reg), V);
}

static __inline void PhyBits(MW_HDMI *H, ULONG Reg, ULONG Mask, ULONG V)
{
    ULONG o = Rd(H->Phy, PHYR(Reg));
    Wr(H->Phy, PHYR(Reg), (o & ~Mask) | (V & Mask));
}

/* HdptxPrePowerUp (RK_HDPTX_QUIESCE_BEFORE_POWERDOWN off, as built). */
static void PhyPrePowerUp(MW_HDMI *H)
{
    Hw(H->Pmu1Cru, PMU1CRU_SOFTRST_CON00, HDPTX_APB_RST, HDPTX_APB_RST);
    Us(20);
    Hw(H->Pmu1Cru, PMU1CRU_SOFTRST_CON00, HDPTX_APB_RST, 0);
    Hw(H->Pmu1Cru, PMU1CRU_SOFTRST_CON01, HDPTX_ALL_RST, HDPTX_ALL_RST);
    Hw(H->PhyGrf, GRF_HDPTX_CON0, HDPTX_I_PLL_EN | HDPTX_I_BIAS_EN | HDPTX_I_BGR_EN, 0);
}

/* HdptxPostEnablePll (HDPTX_LOCK_RETRIES 1). */
static NTSTATUS PhyPostEnablePll(MW_HDMI *H, ULONG *Status)
{
    ULONG v = 0;

    Hw(H->PhyGrf, GRF_HDPTX_CON0, HDPTX_I_BIAS_EN | HDPTX_I_BGR_EN, HDPTX_I_BIAS_EN | HDPTX_I_BGR_EN);
    Us(10);
    Hw(H->Pmu1Cru, PMU1CRU_SOFTRST_CON01, HDPTX_INIT_RST, 0);
    Us(10);
    Hw(H->PhyGrf, GRF_HDPTX_CON0, HDPTX_I_PLL_EN, HDPTX_I_PLL_EN);
    Us(10);
    Hw(H->Pmu1Cru, PMU1CRU_SOFTRST_CON01, HDPTX_CMN_RST, 0);
    Us(50);
    for (ULONG i = 0; i < 50; i++) {
        v = Rd(H->PhyGrf, GRF_HDPTX_STATUS);
        if ((v & HDPTX_O_PHY_CLK_RDY) && (v & HDPTX_O_PLL_LOCK_DONE)) {
            *Status = v;
            return STATUS_SUCCESS;
        }
        Us(20);
    }
    *Status = v;
    return STATUS_IO_TIMEOUT;
}

/* HdptxRopllCmnConfig. The fixed part in the firmware's order. */
static const USHORT CmnFixed[][2] = {
    { 0x08, 0x00 }, { 0x09, 0x0c }, { 0x0A, 0x83 }, { 0x0B, 0x06 }, { 0x0C, 0x20 }, { 0x0D, 0xb8 },
    { 0x0E, 0x0f }, { 0x0F, 0x0f }, { 0x10, 0x04 }, { 0x11, 0x01 }, { 0x12, 0x26 }, { 0x13, 0x22 },
    { 0x14, 0x24 }, { 0x15, 0x77 }, { 0x16, 0x08 }, { 0x17, 0x20 }, { 0x18, 0x04 }, { 0x19, 0x48 },
    { 0x1A, 0x01 }, { 0x1B, 0x00 }, { 0x1C, 0x01 }, { 0x1D, 0x64 }, { 0x1E, 0x14 }, { 0x1F, 0x00 },
    { 0x20, 0x00 }, { 0x21, 0x00 }, { 0x22, 0x11 }, { 0x23, 0x00 }, { 0x24, 0x00 }, { 0x25, 0x53 },
    { 0x26, 0x00 }, { 0x27, 0x00 }, { 0x28, 0x01 }, { 0x29, 0x01 }, { 0x2A, 0x00 }, { 0x2B, 0x00 },
    { 0x2C, 0x00 }, { 0x2D, 0x00 }, { 0x2E, 0x04 }, { 0x2F, 0x00 }, { 0x30, 0x20 }, { 0x31, 0x30 },
    { 0x32, 0x0b }, { 0x33, 0x23 }, { 0x34, 0x00 }, { 0x35, 0x00 }, { 0x38, 0x00 }, { 0x39, 0x00 },
    { 0x3A, 0x00 }, { 0x3B, 0x00 }, { 0x3C, 0x80 }, { 0x3D, 0x40 }, { 0x3E, 0x0c }, { 0x3F, 0x83 },
    { 0x40, 0x06 }, { 0x41, 0x20 }, { 0x42, 0x78 }, { 0x43, 0x00 }, { 0x44, 0x46 }, { 0x45, 0x24 },
    { 0x46, 0xdd }, { 0x47, 0x00 }, { 0x48, 0x11 }, { 0x49, 0xfa }, { 0x4A, 0x08 }, { 0x4B, 0x00 },
    { 0x4C, 0x01 }, { 0x4D, 0x64 }, { 0x4E, 0x34 }, { 0x4F, 0x00 }, { 0x50, 0x00 },
};

static NTSTATUS PhyRopllCmnConfig(MW_HDMI *H, const MW_ROPLL *Cfg, ULONG *Status)
{
    Hw(H->PhyGrf, GRF_HDPTX_CON0, HDPTX_MODE_SEL, 0);        /* HDMI, not DP */
    PhyPrePowerUp(H);
    Hw(H->PhyGrf, GRF_HDPTX_CON0, LC_REF_CLK_SEL, 0);
    for (ULONG i = 0; i < ARRAYSIZE(CmnFixed); i++)
        PhyW(H, CmnFixed[i][0], CmnFixed[i][1]);
    PhyW(H, 0x51, Cfg->Mdiv);
    PhyW(H, 0x55, Cfg->MdivAfc);
    PhyW(H, 0x59, (ULONG)Cfg->Pdiv << 4 | Cfg->Refdiv);
    PhyW(H, 0x5A, (ULONG)Cfg->Sdiv << 4);
    PhyW(H, 0x5C, 0x25);
    PhyW(H, 0x5D, 0x0c);
    PhyW(H, 0x5E, 0x4f);
    PhyBits(H, 0x5E, 1u << 6, (ULONG)Cfg->SdmEn << 6);         /* ROPLL_SDM_EN */
    if (!Cfg->SdmEn)
        PhyBits(H, 0x5E, 0xf, 0);
    PhyW(H, 0x5F, 0x01);
    PhyBits(H, 0x64, 1u << 3, (ULONG)Cfg->SdmNumSign << 3);    /* ROPLL_SDM_NUM_SIGN_RBR */
    PhyW(H, 0x65, Cfg->SdmNum);
    PhyW(H, 0x60, Cfg->SdmDeno);
    PhyBits(H, 0x69, 0x7, Cfg->SdcN);                          /* ROPLL_SDC_N_RBR */
    PhyW(H, 0x6C, Cfg->SdcNum);
    PhyW(H, 0x70, Cfg->SdcDeno);
    PhyW(H, 0x6B, 0x04);
    PhyW(H, 0x73, 0x30);
    PhyW(H, 0x74, 0x04);
    PhyW(H, 0x75, 0x20);
    PhyW(H, 0x76, 0x30);
    PhyW(H, 0x77, 0x08);
    PhyW(H, 0x78, 0x0c);
    PhyW(H, 0x79, 0x00);
    PhyW(H, 0x7B, 0x00);
    PhyW(H, 0x7C, 0x00);
    PhyW(H, 0x7D, 0x00);
    PhyW(H, 0x7E, 0x00);
    PhyW(H, 0x7F, 0x00);
    PhyW(H, 0x80, 0x00);
    PhyW(H, 0x81, 0x01);
    PhyW(H, 0x82, 0x04);
    PhyW(H, 0x83, 0x24);
    PhyW(H, 0x84, 0x20);
    PhyW(H, 0x85, 0x03);
    PhyBits(H, 0x86, 0xF0, (ULONG)Cfg->Sdiv << 4);              /* PLL_PCG_POSTDIV_SEL */
    PhyBits(H, 0x86, 0x0E, 0);                                  /* PLL_PCG_CLK_SEL: 8 bpc */
    PhyBits(H, 0x86, 0x01, 0x01);                               /* PLL_PCG_CLK_EN */
    PhyW(H, 0x87, 0x04);
    PhyW(H, 0x89, 0x00);
    PhyW(H, 0x8A, 0x55);
    PhyW(H, 0x8B, 0x25);
    PhyW(H, 0x8C, 0x2c);
    PhyW(H, 0x8D, 0x22);
    PhyW(H, 0x8E, 0x14);
    PhyW(H, 0x8F, 0x20);
    PhyW(H, 0x90, 0x00);
    PhyW(H, 0x91, 0x00);
    PhyW(H, 0x92, 0x00);
    PhyW(H, 0x93, 0x00);
    PhyW(H, 0x95, 0x00);
    PhyW(H, 0x97, 0x02);
    PhyW(H, 0x99, 0x04);
    PhyW(H, 0x9A, 0x11);
    PhyW(H, 0x9B, 0x00);
    return PhyPostEnablePll(H, Status);
}

/* HdptxPostEnableLane (HDPTX_LOCK_RETRIES 1). */
static NTSTATUS PhyPostEnableLane(MW_HDMI *H, ULONG *Status)
{
    ULONG v = 0;

    Hw(H->Pmu1Cru, PMU1CRU_SOFTRST_CON01, HDPTX_LANE_RST, 0);
    Hw(H->PhyGrf, GRF_HDPTX_CON0, HDPTX_I_BIAS_EN | HDPTX_I_BGR_EN, HDPTX_I_BIAS_EN | HDPTX_I_BGR_EN);
    PhyW(H, 0x207, 0x0f);                       /* four lanes */
    PhyW(H, 0x301, 0x80);                       /* drivers out of electrical idle */
    PhyW(H, 0x401, 0x80);
    PhyW(H, 0x501, 0x80);
    PhyW(H, 0x601, 0x80);
    Us(200);
    for (ULONG i = 0; i < 50; i++) {
        v = Rd(H->PhyGrf, GRF_HDPTX_STATUS);
        if ((v & HDPTX_O_PHY_RDY) && (v & HDPTX_O_PLL_LOCK_DONE)) {
            *Status = v;
            return STATUS_SUCCESS;
        }
        Us(100);
    }
    *Status = v;
    return STATUS_IO_TIMEOUT;
}

/* HdptxRopllTmdsModeConfig. */
static NTSTATUS PhyTmdsModeConfig(MW_HDMI *H, ULONG BitRate, ULONG *Status)
{
    static const USHORT lane[][2] = {
        { 0x03, 0x0c }, { 0x07, 0x20 }, { 0x0A, 0x17 }, { 0x0B, 0x77 }, { 0x0C, 0x77 }, { 0x0D, 0x77 },
        { 0x0E, 0x38 }, { 0x10, 0x03 }, { 0x11, 0x0f }, { 0x12, 0x00 }, { 0x16, 0x02 }, { 0x1B, 0x01 },
        { 0x1E, 0x02 }, { 0x1F, 0x15 }, { 0x20, 0xa0 },
    };

    PhyW(H, 0x114, 0x00);
    PhyW(H, 0x115, 0x00);
    PhyW(H, 0x116, 0x00);
    PhyW(H, 0x117, 0x00);
    PhyW(H, 0x200, 0x06);
    if (BitRate >= 3400000) {
        PhyW(H, 0x201, 0x00);                   /* 1/40 bit rate clock */
        PhyW(H, 0x202, 0x00);
        PhyW(H, 0x203, 0x0f);
        PhyW(H, 0x204, 0xff);
        PhyW(H, 0x205, 0xff);
    } else {
        PhyW(H, 0x201, 0x07);                   /* 1/10 */
        PhyW(H, 0x202, 0xc1);
        PhyW(H, 0x203, 0xf0);
        PhyW(H, 0x204, 0x7c);
        PhyW(H, 0x205, 0x1f);
    }
    PhyW(H, 0x206, 0x07);
    /* Lanes 0..3 at 0x300, 0x400, 0x500, 0x600; the clock lane (3) has
     * 0x0a in LN_LANE_MODE where the data lanes have 0x02. */
    for (ULONG l = 0; l < 4; l++)
        for (ULONG i = 0; i < ARRAYSIZE(lane); i++)
            PhyW(H, 0x300 + l * 0x100 + lane[i][0], (l == 3 && lane[i][0] == 0x1E) ? 0x0a : lane[i][1]);
    for (ULONG l = 0; l < 4; l++)
        PhyW(H, 0x303 + l * 0x100, 0x2f);
    for (ULONG l = 0; l < 4; l++)
        PhyW(H, 0x305 + l * 0x100, 0x03);
    for (ULONG l = 0; l < 4; l++)
        PhyW(H, 0x306 + l * 0x100, 0x1c);
    return PhyPostEnableLane(H, Status);
}

/* ---- VOP2 video port 0 ---- */

static __inline void VopW(MW_HDMI *H, ULONG Off, ULONG V)
{
    Wr(H->Vop, Off, V);
}

/* VP0 into standby at the end of the frame, as Vop2DsiHandoffOsHandler
 * does it: the whole register, STANDBY alone, and DSP_HOLD_VALID in the
 * raw status says when. TRUE if it said so within 50 ms. */
static BOOLEAN VpStandby(MW_HDMI *H)
{
    VopW(H, VOP_VP0_INT_CLR, (VP_INT_DSP_HOLD_VALID << 16) | VP_INT_DSP_HOLD_VALID);
    VopW(H, VP0_DSP_CTRL, MW_VP_DSP_STANDBY);
    VopW(H, VOP_CFG_DONE, VOP_CFG_DONE_VP0);
    for (ULONG w = 0; w < 500; w++) {
        if (Rd(H->Vop, VOP_VP0_INT_RAW) & VP_INT_DSP_HOLD_VALID) {
            VopW(H, VOP_VP0_INT_CLR, (VP_INT_DSP_HOLD_VALID << 16) | VP_INT_DSP_HOLD_VALID);
            return TRUE;
        }
        Us(100);
    }
    VopW(H, VOP_VP0_INT_CLR, (VP_INT_DSP_HOLD_VALID << 16) | VP_INT_DSP_HOLD_VALID);
    return FALSE;
}

/* The port, the timing and the window, with VP0 in standby; then VP0 out of
 * it (Vop2Init, Vop2SetSmartWin, Vop2Enable). Everything else the firmware
 * programs on VP0 at boot is the same for HDMI and DSI and is left as it
 * is: the mixers and SYS_PD_CTRL, ACM out of bypass, auto gating off, the
 * AXI urgency, BG_DLY, the window's format, AXI IDs, port select and scaler
 * (identity at both sizes). */
static void VpProgram(MW_HDMI *H, const MW_TIMING *T, BOOLEAN Dsi, ULONG FbAddr, ULONG Pitch,
                      const MW_VP_REGS *R)
{
    if (Dsi) {
        /* HDMI0 off the port (EN, CLK_OUT_EN, MUX 0), then MIPI0 on. */
        VopW(H, VOP_HDMI0_IF_CTRL, MW_IF_CFG_DONE_IMD | (R->IfCtrl & 0x30));
        VopW(H, VOP_MIPI0_IF_CTRL, R->IfCtrl);
    } else {
        /* MIPI0 off the port as the firmware's DSI handoff leaves it:
         * CFG_DONE_IMD and PIN_POL 3 (memory: project_tspi3m_uefi_linux_display_touch). */
        VopW(H, VOP_MIPI0_IF_CTRL, MW_IF_CFG_DONE_IMD | MW_IF_PIN_POL(3));
        VopW(H, VOP_HDMI0_IF_CTRL, R->IfCtrl);
    }
    VopW(H, VP0_CLK_CTRL, R->ClkCtrl);
    /* VP_DSP_CTRL is written once, computed whole (memory:
     * project_rk3576_vop2_single_write), with STANDBY still set. */
    VopW(H, VP0_DSP_CTRL, R->DspCtrl | MW_VP_DSP_STANDBY);
    VopW(H, VP0_DSP_HTOTAL_HS_END, R->HTotalHsEnd);
    VopW(H, VP0_DSP_HACT_ST_END, R->HActStEnd);
    VopW(H, VP0_DSP_VACT_ST_END, R->VActStEnd);
    VopW(H, VP0_DSP_VTOTAL_VS_END, R->VTotalVsEnd);
    VopW(H, VP0_POST_DSP_HACT_INFO, R->PostHAct);
    VopW(H, VP0_POST_DSP_VACT_INFO, R->PostVAct);
    VopW(H, VP0_POST_SCL_FACTOR, R->PostSclFactor);
    VopW(H, VP0_POST_SCL_CTRL, R->PostSclCtrl);
    VopW(H, VP0_PRE_SCAN_HTIMING, R->PreScan);
    VopW(H, VOP_LINE_FLAG0, R->LineFlag);
    /* Esmart0: the framebuffer, its stride in words, the active and display
     * sizes, at the origin. */
    VopW(H, ESMART0_VIR, Pitch / 4);
    VopW(H, ESMART0_YRGB_MST, FbAddr);
    VopW(H, ESMART0_ACT_INFO, ((ULONG)T->VDisplay - 1) << 16 | (((ULONG)T->HDisplay - 1) & 0xFFFF));
    VopW(H, ESMART0_DSP_INFO, ((ULONG)T->VDisplay - 1) << 16 | (((ULONG)T->HDisplay - 1) & 0xFFFF));
    VopW(H, ESMART0_DSP_ST, 0);
    VopW(H, VOP_CFG_DONE, VOP_CFG_DONE_VP0);
    /* Vop2Enable: STANDBY clear, commit, 20 ms. Before the HDMI setup, which
     * needs the pixel clock running (memory: project_rk3576_standby_serror). */
    VopW(H, VP0_DSP_CTRL, R->DspCtrl);
    VopW(H, VOP_CFG_DONE, VOP_CFG_DONE_VP0);
    Ms(20);
}

/* DCLK_VP0 back on the CRU's dclk_vp0_src (CLKSEL_CON147 bit 11 = 0), gated
 * across the switch as DwHdmiQpSetup step [4] gates it. VP0 must be in
 * standby. With Div, dclk_vp0_src is set to CPLL / Div first (Vop2IfConfig's
 * MIPI0 branch). */
static void DclkToCru(MW_HDMI *H, ULONG Div)
{
    Wr(H->Cru, CRU_GATE(61), 0x20002000);       /* gate DCLK_VP0 */
    Wr(H->Cru, CRU_CLKSEL(147), 0x08000000);    /* DCLK_VP0 = dclk_vp0_src */
    if (Div != 0)
        Wr(H->Cru, CRU_CLKSEL(145), (0x7FFu << 16) | (1u << 8) | (Div - 1));
    Wr(H->Cru, CRU_GATE(61), (1u << 10 | 1u << 13) << 16);      /* ungate src and DCLK_VP0 */
    Us(2000);
}

/* ---- the controller: DwHdmiQpSetup ---- */

typedef struct _MSET {
    MW_ADAPTER *A;
    MW_HDMI *H;
    DDC D;
    ULONG Step;
} MSET;

static void Step(MSET *M, ULONG S)
{
    M->Step = S;
    MwLog(M->A, L"Mset_Step", S);
}

static void Infoframes(MW_HDMI *H, const MW_TIMING *T, const MW_SINK *S, BOOLEAN Hdmi20)
{
    PUCHAR c = H->Ctl;
    mwh_u32 avi[5], vsi[3];
    mwh_u8 cea = T->Vic, hvic = MwHdmiVicOf(cea);

    /* HdmiConfigInfoframes: an HDMI VIC goes in the VSI only, and a VIC
     * above 64 only to an HDMI 2.0 sink. */
    if (hvic > 0 || (!Hdmi20 && cea > 64))
        cea = 0;
    MwAviInfoframe(T, cea, S->RgbQs, avi);
    Wr(c, PKT_AVI_CONTENTS0, avi[0]);
    for (ULONG i = 0; i < 4; i++)
        Wr(c, PKT_AVI_CONTENTS0 + 4 + i * 4, avi[1 + i]);
    Mod(c, PKTSCHED_PKT_CONFIG1, 0, PKTSCHED_AVI_FIELDRATE);
    Mod(c, PKTSCHED_PKT_EN, PKTSCHED_AVI_TX_EN, PKTSCHED_AVI_TX_EN);

    Mod(c, PKTSCHED_PKT_EN, 0, PKTSCHED_VSI_TX_EN);
    if (hvic == 0)
        return;
    MwVsiInfoframe(hvic, vsi);
    Wr(c, PKT_VSI_CONTENTS0, vsi[0]);
    Wr(c, PKT_VSI_CONTENTS0 + 4, vsi[1]);
    Wr(c, PKT_VSI_CONTENTS0 + 8, vsi[2]);
    Wr(c, PKT_VSI_CONTENTS7, 0);
    Mod(c, PKTSCHED_PKT_CONFIG1, 0, PKTSCHED_VSI_FIELDRATE);
    Mod(c, PKTSCHED_PKT_EN, PKTSCHED_VSI_TX_EN, PKTSCHED_VSI_TX_EN);
}

static NTSTATUS HdmiSetup(MSET *M, const MW_TIMING *T, const MW_VP_REGS *R)
{
    MW_HDMI *H = M->H;
    PUCHAR c = H->Ctl;
    ULONG bitrate = T->Clock * 10, st = 0;
    MW_ROPLL cfg;
    NTSTATUS s;

    if (!MwRopllFor(bitrate, &cfg))
        return STATUS_NOT_SUPPORTED;

    /* [0] interrupts masked, the timer base at 396 MHz; [0b] the I2C master
     * again for it. */
    Wr(c, MAINUNIT_0_INT_MASK_N, 0);
    Wr(c, MAINUNIT_1_INT_MASK_N, 0);
    Wr(c, TIMER_BASE_CONFIG0, 0x179A7B00);
    Wr(c, I2CM_CONTROL0, 0x01);
    Wr(c, I2CM_SM_SCL_CONFIG0, 0x085c085c);
    Wr(c, I2CM_FM_SCL_CONFIG0, 0x085c085c);
    Mod(c, I2CM_INTERFACE_CONTROL0, 0, I2CM_FM_EN);
    Wr(c, MAINUNIT_1_INT_CLEAR, I2CM_OP_DONE | I2CM_NACK_RCVD);
    /* [1b] nothing left scheduled from before. */
    Wr(c, PKTSCHED_PKT_EN, 0);
    Wr(c, PKTSCHED_PKT_CONTROL0, 0);
    /* [2] the video data path out of software disable. */
    Mod(c, GLOBAL_SWDISABLE, 0, VIDEO_SWDISABLE);

    /* [3] the PHY PLL. */
    Step(M, 3);
    s = PhyRopllCmnConfig(H, &cfg, &st);
    MwLog(M->A, L"Mset_PllStatus", st);
    if (!NT_SUCCESS(s))
        return s;

    /* [4] DCLK_VP0 onto clk_hdmiphy_pixel0, with VP0 in standby across the
     * switch. The firmware reads VP0_DSP_CTRL back here; this writes the
     * value it computed instead (scanout.c's rule). */
    Step(M, 4);
    VopW(H, VP0_DSP_CTRL, R->DspCtrl | MW_VP_DSP_STANDBY);
    VopW(H, VOP_CFG_DONE, VOP_CFG_DONE_VP0);
    Ms(20);
    Wr(H->Cru, CRU_GATE(61), 0x20002000);
    Wr(H->Cru, CRU_CLKSEL(147), 0x08000800);
    Wr(H->Cru, CRU_GATE(61), 0x20000000);
    Us(2000);
    VopW(H, VP0_DSP_CTRL, R->DspCtrl);
    VopW(H, VOP_CFG_DONE, VOP_CFG_DONE_VP0);
    Ms(20);

    /* [4d] TMDS, not FRL; [5] RGB, 8 bpc; [6] HDCP2 bypass; [7] HDMI, not
     * DVI (forced, as the firmware does); [7b] no scrambling. */
    Hw(H->Vo0Grf, VO0_GRF_SOC_CON1, 1u << 0, 0);
    Hw(H->Vo0Grf, VO0_GRF_SOC_CON8, 0xFFu << 4, 0);
    Mod(c, HDCP2LOGIC_CONFIG0, HDCP2_BYPASS, HDCP2_BYPASS);
    Mod(c, LINK_CONFIG0, 0, OPMODE_FRL | OPMODE_FRL_4LANES | OPMODE_DVI);
    Wr(c, SCRAMB_CONFIG0, 0);

    /* [7c] an HDMI 2.0 sink: read requests off, and TMDS_CONFIG 0 (1/10
     * clock ratio, no scrambling), three tries. Not fatal. */
    if (H->Sink.Scdc) {
        UCHAR rb = 0xFF;
        NTSTATUS ss = STATUS_UNSUCCESSFUL;
        Step(M, 7);
        M->D.Deadline = KeQueryInterruptTime() + 500 * 10000ull;
        (void)ScdcWrite(&M->D, 0x31, 0x00);
        for (ULONG i = 0; i < 3; i++) {
            ss = ScdcWrite(&M->D, 0x20, 0x00);
            if (NT_SUCCESS(ss))
                break;
            Us(2000);
        }
        if (NT_SUCCESS(ss))
            (void)ScdcRead(&M->D, 0x20, &rb);
        MwLog(M->A, L"Mset_Scdc", NT_SUCCESS(ss) ? rb : (ULONG)ss);
    }

    /* [9] InfoFrames. */
    Mod(c, FRAME_COMPOSER_CONFIG9, KEEPOUT_REKEY_ALWAYS, KEEPOUT_REKEY_CFG);
    Wr(c, FLT_CONFIG0, 0);
    Infoframes(H, T, &H->Sink, H->Sink.Scdc);

    /* [10] the lanes. */
    Step(M, 10);
    s = PhyTmdsModeConfig(H, bitrate, &st);
    MwLog(M->A, L"Mset_LaneStatus", st);
    if (!NT_SUCCESS(s))
        return s;
    Ms(5);

    /* [11] AVMUTE cleared: GCP with CLEAR_AVMUTE, after 50 ms (vendor U-Boot). */
    Ms(50);
    Wr(c, PKTSCHED_PKT_CONTROL0, 2);
    Mod(c, PKTSCHED_PKT_EN, PKTSCHED_GCP_TX_EN, PKTSCHED_GCP_TX_EN);
    return STATUS_SUCCESS;
}

/* ---- audio: DwHdmiQpAudio.c ---- */

static ULONG PllRate(MW_HDMI *H, ULONG Con0, ULONG ModeShift)
{
    ULONG c0, c1, m, p, s;
    SHORT k;
    ULONG64 rate;

    if (((Rd(H->Cru, CRU_MODE_CON00) >> ModeShift) & 0x3) != 1)
        return 0;
    c0 = Rd(H->Cru, Con0);
    c1 = Rd(H->Cru, Con0 + 4);
    k = (SHORT)(Rd(H->Cru, Con0 + 8) & 0xFFFF);
    if (c0 & (1u << 15))
        return 0;
    m = c0 & 0x3FF;
    p = c1 & 0x3F;
    s = (c1 >> 6) & 0x7;
    if (m == 0 || p == 0)
        return 0;
    rate = 24000000ull * m;
    if (k != 0)
        rate = (ULONG64)((LONG64)rate + (LONG64)24000000 * k / 65536);
    return (ULONG)(rate / ((ULONG64)p << s));
}

static ULONG Gcd(ULONG a, ULONG b)
{
    while (b != 0) {
        ULONG t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* mclk_sai6 = 12.288 MHz from clk_audio_frac_0, only if the firmware did not
 * already leave it so (CLKSEL_CON155 [10:8] = 1, divider 1). */
static BOOLEAN AudioMclk(MW_HDMI *H)
{
    const ULONG target = 256u * 48000u;
    ULONG rates[2], sel, num = 0, den = 0;

    if ((Rd(H->Cru, CRU_CLKSEL(155)) & 0xFFF) == (1u << 8))
        return TRUE;
    rates[0] = PllRate(H, CRU_GPLL_CON0, 2);
    rates[1] = PllRate(H, CRU_CPLL_CON0, 8);
    for (sel = 0; sel < 2; sel++) {
        ULONG g;
        if (rates[sel] == 0 || rates[sel] < 20u * target)
            continue;
        g = Gcd(rates[sel], target);
        num = target / g;
        den = rates[sel] / g;
        if (num <= 0xFFFF && den <= 0xFFFF)
            break;
    }
    if (sel == 2)
        return FALSE;
    Hw(H->Cru, CRU_CLKSEL(13), 0x3, sel);
    Wr(H->Cru, CRU_CLKSEL(12), (num << 16) | den);
    Hw(H->Cru, CRU_GATE(1), 1u << 10, 0);
    Hw(H->Cru, CRU_CLKSEL(155), 0xFFF, 1u << 8);
    Hw(H->Cru, CRU_GATE(65), (1u << 7) | (1u << 8) | (1u << 9), 0);
    return TRUE;
}

/* DwHdmiQpAudioProgramController: 48 kHz, 2 channels, 24-bit samples in
 * 32-bit I2S slots from SAI6, CTS from the TMDS rate. */
static ULONG AudioSetup(MW_HDMI *H, const MW_TIMING *T)
{
    PUCHAR c = H->Ctl;
    ULONG cts = MwAudioCts(T->Clock), v;
    const ULONG all = PKTSCHED_ACR_TX_EN | PKTSCHED_AUDS_TX_EN | PKTSCHED_AMD_TX_EN | PKTSCHED_AUDI_TX_EN;

    v = Rd(c, GLOBAL_SWDISABLE);
    Wr(c, GLOBAL_SWDISABLE, v & ~AUDIO_SWDISABLE);
    Wr(c, GLOBAL_SWRESET_REQUEST, AUDIO_SWINIT_P);
    v = Rd(c, PKTSCHED_PKT_EN);
    Wr(c, PKTSCHED_PKT_EN, v & ~all);
    Wr(c, AUDIO_INTERFACE_CONTROL0, 1);         /* AUDIO_FIFO_CLR_P */
    Wr(c, AUDIO_INTERFACE_CONFIG0, 0x15);       /* I2S, FIFO init on overflow, line 0, ASP */
    Wr(c, AUDPKT_ACR_CONTROL0, 6144);
    Wr(c, AUDPKT_ACR_CONTROL1, (cts << 4) | (1u << 1));
    Wr(c, AUDPKT_CHSTATUS_OVR0, 0x00000004);
    Wr(c, AUDPKT_CHSTATUS_OVR1, 0x00000B02);
    Wr(c, AUDPKT_CONTROL0, (1u << 12) | 1u);    /* PBIT_FORCE_EN, CHSTATUS_OVR_EN */
    Wr(c, PKT_AUDI_CONTENTS0, 0x000A0100);
    Wr(c, PKT_AUDI_CONTENTS0 + 4, 0x00000170);
    Wr(c, PKT_AUDI_CONTENTS0 + 8, 0);
    Wr(c, PKT_AUDI_CONTENTS0 + 12, 0);
    v = Rd(c, PKTSCHED_PKT_EN);
    Wr(c, PKTSCHED_PKT_EN, v | PKTSCHED_ACR_TX_EN | PKTSCHED_AUDI_TX_EN);
    v = Rd(c, PKTSCHED_PKT_EN);
    Wr(c, PKTSCHED_PKT_EN, v | PKTSCHED_AUDS_TX_EN);
    return cts;
}

/* ---- the two mode sets ---- */

/* HDMI on VP0 at T, scanning out FbAddr. PASSIVE_LEVEL, HwLock held, flips
 * held off (Hdmi.Busy). Trace: Mset_Step (1 clocks, 2 port, 3 PLL, 4 dclk,
 * 7 SCDC, 10 lanes, 11 audio, 12 done), Mset_Status, Mset_PllStatus,
 * Mset_LaneStatus, Mset_Clock, Mset_Size, Aud_Status, Aud_Cts. */
NTSTATUS MwHdmiModeSet(MW_ADAPTER *A, const MW_TIMING *T, ULONG FbAddr, ULONG Pitch)
{
    MW_HDMI *H = &A->Hdmi;
    MSET m;
    MW_VP_REGS r;
    NTSTATUS st;
    BOOLEAN held;

    RtlZeroMemory(&m, sizeof(m));
    m.A = A;
    m.H = H;
    m.D.A = A;
    m.D.C = H->Ctl;
    MwLog(A, L"Mset_Clock", T->Clock);
    MwLog(A, L"Mset_Size", (ULONG)T->HDisplay << 16 | T->VDisplay);
    if (!MwHdmiPowered(A) || !MwTimingUsable(T, 0xFFFFFFFFu)) {
        MwLog(A, L"Mset_Status", (ULONG)STATUS_DEVICE_NOT_READY);
        return STATUS_DEVICE_NOT_READY;
    }
    MwVpRegs(T, FALSE, &r);
    Step(&m, 1);
    HdmiClocks(H);
    Step(&m, 2);
    held = VpStandby(H);
    MwLog(A, L"Mset_Hold", held);
    /* The PHY is about to be reset: VP0 off its pixel clock and onto the
     * CRU's first, as on every boot before step [4]. */
    DclkToCru(H, 0);
    VpProgram(H, T, FALSE, FbAddr, Pitch, &r);
    st = HdmiSetup(&m, T, &r);
    if (NT_SUCCESS(st) && H->Audio && H->Sink.IsHdmi && H->Sink.BasicAudio) {
        Step(&m, 11);
        if (AudioMclk(H)) {
            MwLog(A, L"Aud_Cts", AudioSetup(H, T));
            MwLog(A, L"Aud_Status", 1);
        } else {
            MwLog(A, L"Aud_Status", 2);
        }
    } else if (NT_SUCCESS(st)) {
        MwLog(A, L"Aud_Status", H->Audio ? 3 : 0);
    }
    if (NT_SUCCESS(st))
        Step(&m, 12);
    MwLog(A, L"Mset_Status", (ULONG)st);
    return st;
}

/* Back to the TaishanPi 3M's panel on MIPI0: the DSI host and the panel were
 * never stopped, only VP0 was taken off them. The panel's timing and clock
 * are the firmware's (Tspi3mDsiPanel.c, Vop2IfConfig: CPLL / 15). Trace:
 * Dsi_Step, Dsi_Status. */
NTSTATUS MwHdmiDsiRestore(MW_ADAPTER *A, ULONG FbAddr, ULONG Pitch)
{
    MW_HDMI *H = &A->Hdmi;
    MW_VP_REGS r;
    ULONG div;

    if (!H->HavePanel || !MwHdmiPowered(A)) {
        MwLog(A, L"Dsi_Status", (ULONG)STATUS_DEVICE_NOT_READY);
        return STATUS_DEVICE_NOT_READY;
    }
    div = (1000000 + H->Panel.Clock - 1) / H->Panel.Clock;
    MwVpRegs(&H->Panel, TRUE, &r);
    MwLog(A, L"Dsi_Step", 1);
    MwLog(A, L"Dsi_Hold", VpStandby(H));
    DclkToCru(H, div);
    MwLog(A, L"Dsi_Step", 2);
    VpProgram(H, &H->Panel, TRUE, FbAddr, Pitch, &r);
    MwLog(A, L"Dsi_Step", 3);
    MwLog(A, L"Dsi_Status", 0);
    return STATUS_SUCCESS;
}
