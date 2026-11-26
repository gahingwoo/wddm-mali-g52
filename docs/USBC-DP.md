# USB-C DisplayPort output (UsbcDp)

[English](USBC-DP.md) | [简体中文](USBC-DP.zh-CN.md)

On `main` since 2026-11-26, in this repo and in edk2-rk3576. The RK3576's DisplayPort
controller drives a monitor on the USB-C port (DisplayPort Alt Mode) as a
second display next to HDMI, under Windows 11 23H2. Both halves are off by
default. **Nothing here has run on a board.**

Code: `drivers/maliwddm/dp_calc.h` (the arithmetic, checked by
`tests/dp-host`), `dp.c` (the hardware), `hpd.c` (`UsbcDp` in settings,
children, VidPN, `DpDualCommit`, `DpPoll`), `scanout.c` (DP's framebuffer
in the VOP2 IOMMU). Firmware: edk2-rk3576 `docs/usbc-dp.md`.

## What a user gets

- A USB-C monitor, a USB-C dock with a DP or HDMI output, or a USB-C to
  DP/HDMI adapter, **plugged in before power-on**, shows an extended desktop
  next to HDMI. Windows can set its resolution and refresh rate from its
  EDID, up to 2560x1600 at 60 Hz.
- A monitor that is switched off and on again, or the cable behind an
  adapter unplugged and replugged, is seen again within about two seconds
  (the driver asks the sink over AUX, see "HPD" below).
- Not yet: plugging the USB-C cable in after boot, a monitor that charges
  the board through the port, a DSI boot (TaishanPi 3M panel) with DP, DP
  audio, DP in the firmware's boot screen.

## Boards

| Board | USB-C wired for DP | Evidence |
|---|---|---|
| TaishanPi 3M | yes | LCEDA schematic v1.4: nets `DP_TX_AUXP/N` from the SoC to SBU1/SBU2 of the receptacle; `TYPEC_DPTX_AUX_PUPDCTL1` on ball AL3 = GPIO4_C4, `..._PUPDCTL2` on AK1 = GPIO4_C5; all four SuperSpeed pairs (`USB3_OTG0_SSTX1/2`, `SSRX1/2`) to the receptacle; FUSB302 U40 VCONN pins 12/13 on VCCA_3V3_S0 |
| ArmSoM CM5-IO | yes | carrier schematic V1.1: `TYPEC_AUXP/N` on SBU1/SBU2; mainline DTS: FUSB302 with `altmodes { displayport { svid = 0xff01 } }`, `sbu1-dc-gpios = GPIO4_C4`, `sbu2-dc-gpios = GPIO4_C5`, the USBDP PHY as orientation switch and DP alt mode mux |
| ROCK 4D | no | its USB-C port is power only |

Both boards use the same pins, the same PHY and the same Type-C controller,
so the firmware code is shared; only the FUSB302's I2C bus (I2C2 on the
TaishanPi, I2C0 on the CM5-IO) and the VBUS switch (GPIO0_B6, GPIO2_B6)
differ.

## The hardware

```
FUSB302 (I2C) --- CC ---+
                        USB-C receptacle
USBDP PHY pads 0..3 ----+  SS pairs   (two DP lanes + USB3, or four DP lanes)
USBDP PHY AUX ----------+  SBU1/SBU2  (biased by GPIO4_C4/C5 by orientation)
     ^ lanes, AUX polarity, HPD: VO1_GRF_SOC_CON0
DW DP controller DP0 0x27E40000 (PD_VO1) <-- VOP2 VP1 (DP0_IF_CTRL 0x18C)
```

- **DP controller**: Synopsys DW DP, `rockchip,rk3576-dp` in mainline
  (`drivers/gpu/drm/bridge/synopsys/dw-dp.c`, `rockchip/dw_dp-rockchip.c`),
  dual pixel mode on the RK3576, up to 8.1 Gb/s a lane. In PD_VO1; clocks
  pclk_dp0, clk_aux16mhz_0 (GPLL / 74), aclk_dp0.
- **PHY**: the USB/DP combo PHY behind DRD0
  (`drivers/phy/rockchip/phy-rockchip-usbdp.c`). The firmware already brings
  its USB3 half up at ExitBootServices with two pads muxed to USB and two to
  DP (mainline's default Type-C mapping), so DP is added next to USB3
  without resetting it.
- **VOP2**: any port can drive DP0. VP0 is HDMI's; VP2 is the TaishanPi
  panel's in HdmiExtend; DP uses **VP1** (2560x1600 max, 300 MHz dclk in the
  vendor kernel), Esmart1 (AXI bus 0, under the IOMMU set-up scan-out
  already has), dclk_vp1 from **VPLL**, which nothing else uses (VP0 runs
  from the HDMI PHY PLL, VP2 from CPLL).
- **HPD**: there is no HPD wire on USB-C. The partner reports HPD in PD
  messages (DP Status, Attention). On the RK3576 the controller's HPD input
  comes from VO1_GRF_SOC_CON0 bits 10/11 when the PHY driver says so.

## Who negotiates the Alt Mode under Windows: the decision

Options looked at (sources in the commit that adds this file and in the
edk2 doc):

| Option | What it takes | Verdict |
|---|---|---|
| UCSI (`UcmUcsiCx` + `UcmUcsiAcpiClient`) | A PPM: firmware on an EC that runs PD and alt modes. We have no EC; a PPM in another Windows driver would hold the whole PD stack anyway, and UCSI still has no path for HPD to a display driver | no |
| `UcmTcpciCx` client | Microsoft's software PD policy engine with DisplayPort alt mode events (`IOCTL_UCMTCPCI_PORT_CONTROLLER_DISPLAYPORT_CONFIGURED`, `..._HPD_STATUS_CHANGED`) over a TCPCI-shaped interface. The FUSB302 is not TCPCI (its own register map, FIFO tokens; Linux keeps `fusb302.c` apart from `tcpci.c`), so the client maps TCPCI semantics onto it. Needs the I2C (`rk3xi2c`) and GPIO (`rk3xgpio`) drivers of woa-rk3576 and an ACPI device for the FUSB302; on the TaishanPi the firmware's RTC runtime service also uses I2C2 | the right way to do **hot-plug**; phase 2 |
| Own KMDF driver (UcmCx client or plain) | Our own PD and alt mode stack in Windows | same cost as the firmware's plus Windows plumbing; only if UcmTcpciCx turns out to be missing or unworkable on 23H2 arm64 |
| **Firmware enters the Alt Mode at boot** | PD contract and VDMs in UEFI, once, at ExitBootServices; Windows sees a DP controller with a sink already on the wire | **chosen for phase 1**: the cheapest path to "plugged in before boot gives a picture" |

Why the firmware path holds up after boot:

- Nothing in PD needs periodic messages once the contract and the mode are
  set; the FUSB302 keeps acknowledging (automatic GoodCRC) what the partner
  sends.
- HPD is forced high by the firmware whenever the partner took the
  configuration, and this driver checks for a sink over AUX instead
  (`MwDpSinkThere`: DPCD answers, and an adapter, a branch device, reports a
  sink behind it). That covers a monitor switched off and on and an
  adapter's far side being replugged.

What it cannot do, and why phase 2 exists:

- A cable plugged in after boot gets no VBUS, no PD, no alt mode.
- A partner request that wants an answer (a new Request, Soft_Reset) goes
  unanswered; the partner then sends Hard Reset, which takes the alt mode
  down. Most monitors and adapters are silent after Configure; docks may not
  be.

## How the two halves meet

Nothing is passed between firmware and driver but the hardware state:

| The firmware leaves | The driver reads it as |
|---|---|
| PD_VO1 on, pclk_vo1_root and pclk_dp0 ungated | `Powered()`; otherwise it never touches DP0 or the VO1 GRF (an access would be a synchronous external abort) |
| PMA `0x288` low nibble: the two DP lane enables | `MwDpLanePads()` with the GRF's lane select: which PHY pad carries DP lane 0 and 1 (normal: 2 and 3; flipped: 0 and 1) |
| PMA `0x38C` bit 3 DP_INIT_RSTN | the PHY's DP half is out of init reset |
| VO1_GRF_SOC_CON0 bits 10/11 set | the alt mode was entered: `Dp_Off` 3 otherwise |

## What the driver does

Start (`MwDpStart`, from `MwHpdStart` on an HDMI boot at `HdmiHpd` 3): the
checks above, `DEFAULT_FAST_LINK_TRAIN_EN` off, DPCD 0x000..0x00F (and the
extended caps at 0x2200), the EDID over I2C-over-AUX (four blocks at most),
the modes. If a sink answers, two VidPN sources and two children are
reported: child 0 HDMI, child 1 DisplayPort (`D3DKMDT_VOT_DISPLAYPORT_EXTERNAL`).

Mode set (`MwDpModeSet`, from `DpDualCommit`):

1. Stream off, DP0 off its port, VP1 to standby (DSP_HOLD_VALID).
2. VPLL to an even multiple of the pixel clock (`MwVpllFor`, exact or not
   offered), dclk_vp1_src = VPLL / Div.
3. Overlay (Esmart1 as VP1's layer 2, the three mixers, BG_DLY 16) and
   Esmart1 on DP's own framebuffer.
4. VP1 timing with STANDBY set, VP_DSP_CTRL written whole once (AAAA);
   DP0_IF_CTRL with MUX 1, PCLK_DIV, polarity, without EN.
5. The link: sink to D0, the PHY's link rate (DP_CMN_RSTN, TX_LINK_BW,
   ROPLL lock), two lanes, clock recovery (TPS1) and channel equalisation
   (TPS4, TPS3 or TPS2) with the sink's swing and pre-emphasis requests
   written to the PHY's drive registers; rate steps down HBR2, HBR, RBR.
6. Video: timing, MSA, TU and init threshold (dual pixel), HBLANK interval.
7. VP1 out of standby, DP0_IF_CTRL EN, stream on; `Dp_GenInt` shows a video
   FIFO overflow if the TU or the pixel clock is wrong.

Rates: at most HBR2 (5.4 Gb/s). Two lanes of HBR2 carry 8.64 Gb/s, more
than VP1's largest mode needs (2560x1600 at 60 Hz reduced blanking: 268.5
MHz, 6.44 Gb/s). Lanes: always two, whatever the pin assignment; see the
edk2 doc for why D is preferred and C works the same way.

## The setting

`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters`:

| `UsbcDp` | What it does |
|---|---|
| 0 or missing | Off. Nothing is mapped, nothing is reserved, the driver is what it was. |
| 1 | At start, on an HDMI boot with `HdmiHpd` 3 (the HDMI default): DP's framebuffer is reserved (16 MiB, VP1's largest mode), and if the firmware entered the alt mode and a sink answers, DP is the second display. Ignored on a DSI boot and with `HdmiExtend` 1. |

The firmware's half is the setup option "USB-C DisplayPort (experimental)"
(Device Manager, RK3576 settings). Both are needed.

## Trace values

Under `Device Parameters\maliwddm`:

| Name | Meaning |
|---|---|
| `Dp_Setting`, `Dp_On`, `Dp_Connected` | the setting; DP reported; a sink there |
| `Dp_Off` | why not: 1 PD_VO1 off or clocks gated, 2 a mapping failed, 3 no alt mode from the firmware (lanes, HPD bits, DP_INIT_RSTN), 4 a one-lane sink |
| `Dp_Fw` | PMA 0x288 << 16 \| VO1_GRF_SOC_CON0 low half |
| `Dp_Pads` | lanes found \| pad of lane 0 << 8 \| pad of lane 1 << 16 |
| `Dp_HpdStatus` | the controller's HPD_STATUS (state 7 in bits 11:9 = plugged) |
| `Dp_CapsStatus`, `Dp_Caps0`, `Dp_Caps5` | DPCD read status; bytes 0..3; bytes 5, 6, 0x0E |
| `Dp_EdidSize`, `Dp_EdidStatus`, `Dp_Modes`, `Dp_Mode0` | the EDID and the modes from it |
| `Dp_Commits`, `Dp_Commit`, `Dp_CommitStatus`, `Dp_ModeSet` | commits (source 0's target << 24 \| source 1's << 16) |
| `Dp_Step` | mode set stage: 1 standby, 2 clock, 3 window, 4 timing, 5 link, 6 video, 7 released |
| `Dp_Vpll`, `Dp_VpllLock`, `Dp_Hold` | m \| p << 10 \| s << 16 \| div << 20; CON6; VP1 reached standby |
| `Dp_TrainStep`, `Dp_TrainRate`, `Dp_TrainStatus`, `Dp_TrainDrive`, `Dp_Ropll` | training: 1 configure 2 CR 3 EQ 4 done; rate in MHz \| status << 16; DPCD 0x202..0x205; vs/pe per lane; the ROPLL's lock bits on failure |
| `Dp_Video5`, `Dp_IfCtrl`, `Dp_GenInt`, `Dp_Status` | VIDEO_CONFIG5; DP0_IF_CTRL; GENERAL_INTERRUPT 40 ms after (bit 6 = FIFO overflow); result |
| `Dp_Changes`, `Dp_Indicate`, `Dp_ParkHold` | sink came or went; DxgkCbIndicateChildStatus; VP1 standby on park |

## Size

| Piece | Lines |
|---|---|
| `dp_calc.h` (AUX, caps, training rules, drive tables, VPLL, video, mode filter) | ~600 |
| `dp.c` (AUX, EDID, PHY rate and drive, training, VP1, park) | ~700 |
| `hpd.c` and `scanout.c` integration (settings, reserve, children, `DpDualCommit`, `DpPoll`, framebuffer kinds) | ~350 |
| `tests/dp-host` | ~250 |

Firmware side (edk2): PD and alt mode ~760, PHY/clock/power ~150, setup
option and boards ~200.

Phase 2 (hot-plug, a UcmTcpciCx client over `rk3xi2c` and `rk3xgpio`):
estimated 1500 to 2500 lines plus ACPI, and the TaishanPi's I2C2 shared with
the RTC's runtime service has to be settled first.

## Board test plan

Needs: a TaishanPi 3M or CM5-IO with HDMI on a monitor; a second monitor on
USB-C through, in order, (1) a USB-C to HDMI or DP adapter cable, (2) a
monitor with a USB-C input that does not charge, (3) a dock with a DP or
HDMI output; a serial console.

Set-up, in this order:

1. Firmware: edk2-rk3576 from `main` (the `tspi3m-firmware` or
   `cm5io-firmware` CI artifact). Flash only the firmware range so
   Windows and the UEFI variables survive.
2. In setup, RK3576 settings: "USB-C DisplayPort (experimental)" Enabled.
   The USB-C monitor must be plugged in before power-on (no hot-plug).
3. Driver: the `wddm-mali-g52-driver-arm64` artifact from `main`,
   installed as usual. Then:

   ```
   reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v UsbcDp /t REG_DWORD /d 1 /f
   shutdown /r /t 0
   ```

Back out: `UsbcDp` 0 (or delete it), and the setup option Disabled.

1. **Firmware alone** (option on, driver setting 0), each partner, both
   plug orientations, n >= 3 each: serial shows `DP alt mode: configured`,
   `pin D` (dock) or `pin C` (adapter), `UsbDpPhy DP: PD_VO1 on, lanes 0x..`.
   USB3 on the dock still at SuperSpeed (Device Manager). Without a partner,
   and with a USB disk, boot time unchanged.
2. **Driver**: `Dp_On` 1, `Dp_Caps0` sane, `Dp_Modes` > 2, Windows shows two
   displays; extend works; set 1920x1080, 2560x1440, 1280x720 on the DP one;
   `Dp_TrainStep` 4 and `Dp_GenInt` bit 6 clear each time.
3. **Both orientations** of the USB-C plug.
4. **Monitor off and on**, adapter's HDMI cable out and in: `Dp_Changes`,
   the display comes back.
5. **HDMI unaffected**: with `UsbcDp` 0, the registry trace and the HDMI
   behaviour are what they were (mode changes, hot-plug).
6. **Shutdown and reboot** clean (no WHEA: the earlier XHC0 SError came
   from the USB3 side of this PHY).

## Risks

- None of the register sequences has run. The least certain parts:
  setting DP_INIT_RSTN in the USB3 bring-up (mainline does it in the same
  place for DP+USB mode), PD_VO1's power-up from the firmware, VPLL being
  free under Windows, the dual pixel TU values.
- Pin assignment C with only two lanes used: the partner's lanes 2 and 3
  get the USB3 transmitter. They are not trained (lane count 2), but a
  sink that insists on four lanes would not show a picture.
- A dock that sends PD messages after Configure that need an answer will
  reset the alt mode under Windows (phase 1 has no port driver).
- `MwHdmiModes` builds the DP mode list, so a mode the HDMI PHY could not
  make is left out of DP's list too (few in practice).
- Esmart1 is the panel's window in HdmiExtend; the two features exclude
  each other.
- A primary's refresh rate is looked up by its size (`MwDualSourceOf`, as
  HdmiExtend does): with HDMI and DP at the same resolution, source 1's
  primaries report source 0's rate.
