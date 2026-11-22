# HDMI hot-plug in maliwddm

Branch `hdmi-hpd`. Code: `drivers/maliwddm/hpd.c` (what Windows sees),
`drivers/maliwddm/hdmi.c` (the hardware), `drivers/maliwddm/hdmi_calc.h`
(the arithmetic, host-tested by `tests/hdmi-host`). Not run on hardware.

## The setting

A DWORD in the adapter's Device Parameters,
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters`.
Read once, when the adapter starts. Missing or 0 is today's driver.

| `HdmiHpd` | What runs |
|---|---|
| 0 | Nothing. The setting is read, nothing is mapped, allocated or written. |
| 1 | Sense only. HPD polled every 500 ms, the EDID read over the controller's DDC when a monitor appears and once at start. Everything goes to the registry trace. Windows is told nothing new. |
| 2 | Report. HDMI boot: the one child is hot-plug aware, Windows is told of plug and unplug, and the child's EDID is the one just read. No mode is ever set, so only a monitor that takes the firmware's timing gets a picture. A DSI boot behaves as 1. |
| 3 | Mode set. The driver programs VOP2 VP0, the DW-HDMI-QP controller and the HDPTX PHY itself. HDMI boot: a different monitor gets its own preferred timing. DSI boot (TaishanPi 3M panel): a second child, HDMI; Windows can show one or the other (Win+P), not both. HDMI audio is set up after each mode set. |

Other DWORDs, all optional:

| Name | Default | Meaning |
|---|---|---|
| `HdmiHpdSource` | 0 | Which bit is HPD: 0 `IOC_HDMI_HPD_STATUS` bit 3 (LEVEL_INT, what mainline and the firmware's HPD wait read), 1 bit 6 (vendor's HPD_PORT_LEVEL), 2 the GPIO4_PC1 pad. |
| `HdmiHpdPollMs` | 500 | Poll period, 100 to 5000. A change counts at its second equal reading. |
| `HdmiAudio` | 1 | 0: no audio setup after a mode set. |
| `HdmiMaxPixels` | 4096000 (2560x1600) | Size of the driver's own framebuffer, allocated at start at level 3. Modes larger than this are not offered. |

## What level 3 does, in order

On a commit of the HDMI target with a timing VP0 is not already driving
(hdmi.c `MwHdmiModeSet`, every step from edk2-rk3576):

1. `HdmiClocks`: the CRU and PMU1CRU gates and resets of `HdmiTxIomux`
   (not its GPIO: the firmware's PreInit leaves the TaishanPi's level
   shifters on).
2. VP0 to standby (`Vop2DsiHandoffOsHandler`), DCLK_VP0 back on the CRU's
   `dclk_vp0_src`, MIPI0 detached (PIN_POL 3 kept), HDMI0_IF_CTRL, VP0
   timing, post scaler, PRE_SCAN, line flag, VP_DSP_CTRL written once
   (AAAA, no pre-dither), Esmart0 onto the driver's framebuffer, standby
   released, 20 ms (`Vop2Init`, `Vop2SetSmartWin`, `Vop2Enable`).
3. `DwHdmiQpSetup` [0]..[11]: timer base, DDC SCL, packets cleared, video
   path enabled, ROPLL (table or calculated), DCLK_VP0 to the PHY pixel
   clock with VP0 in standby around it, TMDS/RGB/8 bpc, HDCP2 bypass, HDMI
   mode, no scrambling, SCDC cleared for a 2.0 sink, AVI and VSI InfoFrames,
   lanes, AVMUTE cleared.
4. Audio (`DwHdmiQpAudio.c`): mclk_sai6 12.288 MHz only if the firmware did
   not already leave it, then the controller: I2S, 48 kHz, 2 ch, N 6144,
   CTS = TMDS kHz, channel status 24-bit, Audio InfoFrame. Only for a sink
   with an HDMI VSDB and basic audio.

Committing the panel again (DSI boot): VP0 to standby, DCLK_VP0 = CPLL/15,
HDMI0 detached, MIPI0_IF_CTRL 0x80000033, the panel's timing, Esmart0 back
on the firmware's framebuffer, standby released, backlight on. The DSI host
and the panel are never touched.

Modes offered on HDMI: the EDID's detailed timings that are progressive, at
most 340 MHz, 4-pixel aligned, within `HdmiMaxPixels` and lockable by the
PHY; then 1920x1080@60 and 1280x720@60. On an HDMI boot the firmware's own
timing comes first for the monitor it lit.

## Board test plan

Prerequisites: TaishanPi 3M, Windows 11 23H2 booting from eMMC, the rescue
Windows in the boot menu, this package installed the usual way
(`pnputil /add-driver` + `malireinst`, reboot). With no `HdmiHpd` value
this build must behave exactly like 3.11.8.816: check that first.

Set a level (elevated):

    reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiHpd /t REG_DWORD /d 1 /f
    shutdown /r /t 0

Read the trace:

    reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" | findstr /i "Hpd_ Edid_ Mset_ Dsi_ Aud_ Scan_ Commit"

Clear it between runs: `reg delete "...\Device Parameters\maliwddm" /f`.

Step A, level 1, DSI boot (panel attached, HDMI unplugged at power-on):
1. Boot. Expect `Hpd_Level 1`, `Hpd_Mapped 0x3FF`, `Hpd_Powered 1`,
   `Hpd_Dsi 1`, `Hpd_State 0`, `Hpd_Children 1`. Desktop as before.
2. Plug the monitor. Within ~1 s: `Hpd_Changes 1`, `Hpd_State 1`,
   `Edid_Status 0`, `Edid_Bytes 256` (typical), `Edid_Id` = the monitor's
   manufacturer/product bytes, `Edid_Modes` > 0, `Edid_Mode0` = its native
   size as (W << 16 | H). `Hpd_Raw` shows the IOC status in bits 7:0, the
   pad in bit 8, IOC_MISC_CON0 in bits 23:16.
3. Unplug: `Hpd_Changes 2`, `Hpd_State 0`.
   - If `Hpd_State` never changes but `Hpd_Raw` does: set `HdmiHpdSource`
     to 1 or 2 (whichever bit follows the cable in `Hpd_Raw`), reboot,
     repeat.
   - `Edid_Status 0xC000009D` (not connected): HPD dropped during the read
     (same fix). `0xC00000B5` (timeout) with `Edid_Timeouts` high: DDC.
     `0xC00000A3` (not ready) with `Edid_Gates` non-zero: the controller's
     clocks are gated; level 3 ungates them, levels 1/2 do not.
4. Same on an HDMI boot (panel ribbon unplugged, monitor on): `Hpd_Dsi 0`,
   `Hpd_State 1` and an EDID at start.

Step B, level 2, HDMI boot: unplug and replug the same monitor. Windows
should drop the display on unplug and bring it back on replug
(`Hpd_Indicate 0` each time, `CommitNoPath` on unplug).

Step C, level 3, HDMI boot: boot with monitor A. Expect `Hpd_Switch 1`,
`Hpd_Reserve 16384000`, no `Mset_*` at all (the first commit matches the
firmware's timing). In Settings > Display pick another resolution, e.g.
1280x720: expect `Mset_Step 12`, `Mset_Status 0`, `Mset_PllStatus` and
`Mset_LaneStatus` with bits 3 and 2 or 1 set (0x0E/0x0F typical),
`Scan_Switch 2`, `Aud_Status 1` on a monitor with speakers, and a
picture at 720p. Then swap to monitor B with another native size.

Step D, level 3, DSI boot: boot on the panel, plug HDMI. Expect
`Hpd_Children 2`. Win+P "Second screen only": `Hpd_CommitTarget 1`,
`Mset_Step 12`, picture on HDMI, panel backlight off. Unplug HDMI:
Windows should go back to the panel: `Hpd_CommitTarget 0`, `Dsi_Step 3`,
`Scan_Switch 1`, picture on the panel. Repeat a few times. Try rotation on
each target.

`Mset_Step` is written at each stage: 1 clocks, 2 VP0 programmed, 3 PLL,
4 DCLK on the PHY, 7 SCDC, 10 lanes, 11 audio, 12 done. A hang or a
bugcheck leaves the last one reached. `Mset_Status` is an NTSTATUS:
`0xC00000B5` with `Mset_Step 3` or `10` is a PLL or lane lock timeout
(the GRF status words say which bits came up).

Back out: `reg delete ".../Device Parameters" /v HdmiHpd /f` from the
running system or from the rescue Windows (load the main system hive), and
reboot. If the desktop does not come up: boot the rescue Windows, or the
main one with the HDMI cable unplugged at level 1/2 (nothing is set there).

## Open risks

- Nothing here has run. The first board session decides which HPD bit is
  real (`Hpd_Raw`).
- DSI back from HDMI is the least certain: the DSI host keeps running with
  no pixels while VP0 is on HDMI; whether the panel resumes without its
  init sequence is unknown.
- One VidPN source: no extended desktop. Windows' default choice when the
  second display appears on a DSI boot is its own; Win+P decides.
- 4-pixel-unaligned widths (1366) and anything above 340 MHz are not
  offered; interlaced modes never.
- A swap of monitors faster than two polls is not seen as an unplug.
- After a hand-back (driver disabled while on our framebuffer) that
  framebuffer is deliberately leaked so Basic Display keeps a valid one.
- The audio driver is not told about hot-plug; it keeps streaming into a
  link that may be down for a moment during a mode set. See below.

## For the audio driver (woa-rk3576, not changed here)

The controller keeps the firmware's fixed format: I2S from SAI6, 2 ch,
48 kHz, 24-bit samples in 32-bit slots, N 6144 with CTS overridden from the
TMDS rate, so ACR stays valid while SAI6 is stopped. After a mode set the
driver rewrites all of it for the new rate (and sets mclk_sai6 only if
CLKSEL_CON155 does not already hold the firmware's value). The audio
driver needs no change to keep working; it has no jack state, so Windows
shows the HDMI endpoint whether a monitor is there or not. Reporting jack
state would need an interface between the two drivers that does not exist.

## ACPI

None needed: registers are mapped by physical address, as scanout.c maps
the VOP2. `docs/acpi/0001-gpu-asl-hdmi-blocks.patch` is an optional
private `HDMB` package for GPU0 naming the blocks and the HPD interrupt
(GSIV 399), not as `_CRS` entries (see the patch for why).
