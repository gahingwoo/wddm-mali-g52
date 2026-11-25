# Two displays at once (HdmiExtend)

Branch `dual-display`. The TaishanPi 3M's 10.1 inch MIPI DSI panel and an
HDMI monitor as one extended desktop under Windows. Code:
`drivers/maliwddm/dual_calc.h` (decisions and register values, host-tested
by `tests/dual-host`), `hdmi.c` (`MwHdmiPanelToVp2`, `MwHdmiVp2Park`),
`hpd.c` (the end of the file: VidPN, commit, source 1),
`scanout.c` (`MwScan2*`), `command.c` (vsync and copies of source 1).
**Not run on hardware**: the board's DSI ribbon is broken. The board test
plan is [DUAL-DISPLAY-TEST.zh.md](DUAL-DISPLAY-TEST.zh.md).

No firmware change is needed. The firmware lights the panel on VP0 as
it always has, and everything below happens in the driver.

## The setting

A DWORD in `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters`,
read once when the adapter starts:

| `HdmiExtend` | What it does |
|---|---|
| 0 or missing | Off. The driver is byte for byte what it was: one VidPN source, `HdmiExclusive` decides whether the panel and HDMI take turns. |
| 1 | On a DSI boot with `HdmiHpd` 3 (which a DSI boot needs set by hand): two VidPN sources, the panel always connected, HDMI hot-plug aware. `HdmiExclusive` is ignored. On an HDMI boot it does nothing (`Dual_On 0`). |

To turn it off again: `reg delete "...\Device Parameters" /v HdmiExtend /f`
and reboot (or set 0). Nothing persists outside the setting.

## What runs where

What Linux does on this board (Armbian 7.3.5 mainline, and the vendor
6.1 kernel whose register dump of 2026-11-17 the host test checks
against): HDMI on VP0, the panel on VP2.

| Outputs in use | Panel | HDMI |
|---|---|---|
| Panel alone | VP0, Esmart0, as the firmware and the single display have it | off |
| HDMI alone | off (backlight off, MIPI0 on no port) | VP0, Esmart0 |
| Both (extend) | VP2, Esmart1 | VP0, Esmart0 |

The panel moves to VP2 only while both outputs are in use, and moves back
to VP0 through `MwHdmiDsiRestore`, the restore that already passed on the
board, when HDMI is unplugged or "PC screen only" is chosen. With HDMI
unplugged the hardware is in exactly the state it is in without
`HdmiExtend`.

The steps between arrangements (`MwDualSteps`, every pair checked by the
host test on a model of the hardware):

| From | To | Steps |
|---|---|---|
| panel on VP0 | both | panel to VP2 (VP0 to standby first), HDMI mode set on VP0 |
| both | panel alone | park VP2 (MIPI0 off it, standby, Esmart1 off), panel on VP0 |
| both | HDMI alone | backlight off, park VP2, HDMI as it is |
| HDMI alone | both | panel to VP2 (VP0 not touched), backlight on, HDMI as it is |
| panel on VP0 | HDMI alone | backlight off, HDMI mode set (the single display's switch) |

VP2 programming (`MwHdmiPanelToVp2`, values from `dual_calc.h`):
dclk_vp2_src = CPLL / 15 (66.67 MHz, as Linux; the divider rounds up from
the CPLL rate the CRU reports), DCLK_VP2 on it; `OVL_LAYER_SEL(2)` =
0xfff1 (Esmart1 is layer id 1 on VP2), the three VP2 mixers with the
firmware's four values, BG_DLY 16; Esmart1: port 2, delay 0, AXI bus 0
with read IDs 0x12/0x13, alpha map opaque, no scaling, ARGB8888 at the
firmware's framebuffer; VP2 timing in standby (P888, no pre-dither, as on
VP0); then VP0 to standby if it was the panel's, MIPI0_IF_CTRL = 0x8010003b
(MUX 2, PCLK_DIV, PIN_POL 3), VP2 released, 20 ms. The DSI host and the
panel are never touched, as on every panel restore.

Esmart1 rather than the vendor kernel's Esmart2: Esmart1 is on AXI bus 0
like Esmart0, so the VOP2 IOMMU setup scan-out already has covers it.
Mainline allows Esmart0 to 3 on VP2.

## What Windows sees

- Two sources, two children (panel = internal, always connected; HDMI =
  hot-plug aware). Windows' default for a second display is Extend.
- Any source may drive any target; one path per source and per target.
  Clone (Duplicate in Win+P) is refused: it would need one primary on two
  windows of different sizes, which nothing here sets up.
- Rotation: source 0 only (the driver's rotation is the adapter's). The
  path from source 1 is offered identity only. When the panel is on
  source 0 (the usual case: it is the display Windows starts with), it
  rotates as before.
- Modes: the panel's one mode; on HDMI everything `HdmiHpd` 3 offers.
  Independent per output.
- Each source has its own framebuffer: the firmware's for the panel, the
  driver's reserved one for HDMI. Flips are scanned out through each
  source's window (two register writes), copied by the copy thread when
  they cannot be, and completed by the emulated vsync, which now notifies
  each active target.
- A primary's refresh rate (`DescribeAllocation`) is its source's mode's.
  The runtime does not say which source a UMD-created primary is for, so
  the size decides (`MwDualSourceOf`): the panel's 800x1280 and any HDMI
  mode never share a size.

## Trace values

Under `Device Parameters\maliwddm`, as the rest of HDMI-HPD.md:

| Name | Meaning |
|---|---|
| `Hpd_ExtendSetting`, `Dual_On` | The setting as read; whether two sources were reported. |
| `Dual_IsSup` | Last changed IsSupportedVidPn answer: paths \| topology reason << 4 \| path reason << 8 \| supported << 12 \| source 0's target << 16 \| source 1's << 24 (0xFF none). Topology reasons: 1 clone, 2 a target twice, 3 no such source/target, 4 too many paths, 5 source 1 rotated. Path reasons as `Hpd_IsSup`. |
| `Dual_Commits`, `Dual_Commit` | Commits; source 0's target << 24 \| source 1's << 16 \| panel port before << 8 \| after (0 VP0, 2 VP2, 15 off). |
| `Dual_Steps` | The steps run, a nibble each, first lowest: 1 park, 2 panel on VP0, 3 panel to VP2, 4 HDMI, 5 backlight on, 6 off. |
| `Dual_Step` | Each step as it ends: step << 28 \| status. |
| `Dual_P2Step`, `Dual_P2Div`, `Dual_P2Hold`, `Dual_P2Vp0Hold`, `Dual_P2IfCtrl`, `Dual_P2Status` | The panel's move to VP2: last stage reached (1 VP2 standby, 2 clock, 3 overlay and window, 4 timing, 5 VP0 standby, 6 MIPI0 on VP2, 7 done), the dclk divider, whether each port reported DSP_HOLD, the IF_CTRL written. |
| `Dual_ParkHold` | VP2 reported DSP_HOLD when parked. |
| `Dual_Bind0`, `Dual_Bind1`, `Dual_Same0`, `Dual_Same1` | A source given a target: target << 28 \| Esmart << 24 \| port << 20 \| width & 0x3FF << 10 \| height & 0x3FF; or nothing changed. |
| `Dual_PanelVp`, `Dual_CommitStatus`, `Dual_Fails` | After the commit. |
| `Per2_*` | With `Per_*`: source 1's Flips, Scanned, Copied, CopySkips, NotMapped, Invisible, Vsyncs, ToFbs, Blts, Target, State (active \| visible << 1 \| on a primary << 2), Fb (W << 16 \| H), PanelVp, Src0Target. |

## Open risks

- Nothing here has run. The least certain step is the panel's move to VP2
  while VP0 lets go of it; it is the same unknown (the DSI host keeps
  running and gets its pixels from another port) that the VP0 panel
  restore passed, but VP2 itself has never been driven by this driver or
  the firmware.
- VP2's `max_output` in mainline is 1920x1080, and the panel is 1280
  lines. Both Linux kernels run it on VP2 at 800x1280, so the limit is not
  a hardware one for this width.
- Windows may persist a source-to-target assignment the commit refuses
  (clone). It then keeps the old arrangement; Win+P Extend sets it again.
- The emulated vsync runs at about 60 Hz for both targets; the panel's
  real rate is 55.7 Hz, as before.
- `HdmiDiag` and the `Chk*` snapshots describe VP0 only.
