# Two displays at once (HdmiExtend)

The TaishanPi 3M's DSI panel and an HDMI monitor as one extended desktop.
Off by default; CI only (not run on the board). Board test:
[DUAL-DISPLAY-TEST.zh.md](DUAL-DISPLAY-TEST.zh.md). Code: `dual_calc.h`
(decisions and register values, `tests/dual-host`), `hdmi.c`
(`MwHdmiPanelToVp2`, `MwHdmiVp2Park`), the end of `hpd.c`, `scanout.c`
(`MwScan2*`), `command.c`. No firmware change.

## Turning it on

`HdmiExtend` 1 takes effect on a DSI boot with `HdmiHpd` 3, which a DSI
boot needs set by hand. It reports two VidPN sources and ignores
`HdmiExclusive`. On an HDMI boot it does nothing (`Dual_On 0`). Set 0 or
delete the value and reboot to turn it off. Settings: see
[SETTINGS.md](SETTINGS.md).

## How it works

| Outputs in use | Panel | HDMI |
|---|---|---|
| Panel alone | VP0, Esmart0 (as the firmware) | off |
| HDMI alone | off (backlight off, MIPI0 on no port) | VP0, Esmart0 |
| Both | VP2, Esmart1 | VP0, Esmart0 |

The panel moves to VP2 only while both are in use and goes back to VP0
through the panel restore when HDMI is unplugged or "PC screen only" is
chosen. With HDMI unplugged the hardware is as it is without `HdmiExtend`.

| From | To | Steps (`MwDualSteps`) |
|---|---|---|
| panel on VP0 | both | panel to VP2 (VP0 to standby first), HDMI mode set on VP0 |
| both | panel alone | park VP2 (MIPI0 off it, standby, Esmart1 off), panel on VP0 |
| both | HDMI alone | backlight off, park VP2 |
| HDMI alone | both | panel to VP2, backlight on |
| panel on VP0 | HDMI alone | backlight off, HDMI mode set |

VP2 for the panel: dclk_vp2_src = CPLL / 15 (66.67 MHz), DCLK_VP2 on it,
`OVL_LAYER_SEL(2)` 0xfff1, the three VP2 mixers with the firmware's
values, BG_DLY 16; Esmart1 on port 2, AXI bus 0, read IDs 0x12/0x13,
opaque, no scaling, ARGB8888 at the firmware's framebuffer; VP2 timing in
standby; then VP0 to standby, MIPI0_IF_CTRL 0x8010003b (MUX 2, PCLK_DIV,
PIN_POL 3), VP2 released. The DSI host and the panel are never touched.
Esmart1 is used because it is on AXI bus 0 like Esmart0, so the existing
VOP2 IOMMU setup covers it.

What Windows sees: two sources and two children (panel internal and
always connected, HDMI hot-plug aware); Extend by default. Any source may
drive any target, one path per source and per target. Clone (Duplicate)
is refused. Rotation on source 0 only; source 1 is identity. Modes are
independent: the panel's one mode, and on HDMI everything `HdmiHpd` 3
offers. Each source has its own framebuffer and window; the emulated
vsync notifies each active target. A UMD-created primary's source is
decided by its size (`MwDualSourceOf`).

## Trace values

Under `Device Parameters\maliwddm`, with those in [HDMI-HPD.md](HDMI-HPD.md):

| Name | Meaning |
|---|---|
| `Hpd_ExtendSetting`, `Dual_On` | The setting as read; two sources reported. |
| `Dual_IsSup` | Last changed IsSupportedVidPn: paths \| topology reason << 4 \| path reason << 8 \| supported << 12 \| source 0's target << 16 \| source 1's << 24 (0xFF none). Topology reasons: 1 clone, 2 a target twice, 3 no such source/target, 4 too many paths, 5 source 1 rotated. |
| `Dual_Commits`, `Dual_Commit` | Commits; source 0's target << 24 \| source 1's << 16 \| panel port before << 8 \| after (0 VP0, 2 VP2, 15 off). |
| `Dual_Steps` | Steps run, a nibble each, first lowest: 1 park, 2 panel on VP0, 3 panel to VP2, 4 HDMI, 5 backlight on, 6 off. |
| `Dual_Step` | Each step as it ends: step << 28 \| status. |
| `Dual_P2Step`, `Dual_P2Div`, `Dual_P2Hold`, `Dual_P2Vp0Hold`, `Dual_P2IfCtrl`, `Dual_P2Status` | The move to VP2: last stage (1 VP2 standby, 2 clock, 3 overlay and window, 4 timing, 5 VP0 standby, 6 MIPI0 on VP2, 7 done), divider, DSP_HOLD seen per port, IF_CTRL written. |
| `Dual_ParkHold` | VP2 reported DSP_HOLD when parked. |
| `Dual_Bind0`, `Dual_Bind1`, `Dual_Same0`, `Dual_Same1` | A source given a target: target << 28 \| Esmart << 24 \| port << 20 \| width & 0x3FF << 10 \| height & 0x3FF; or nothing changed. |
| `Dual_PanelVp`, `Dual_CommitStatus`, `Dual_Fails` | After the commit. |
| `Per2_*` | Source 1's snapshot: Flips, Scanned, Copied, CopySkips, NotMapped, Invisible, Vsyncs, ToFbs, Blts, Target, State (active \| visible << 1 \| on a primary << 2), Fb, PanelVp, Src0Target. |

## Known issues

- Not run. The least certain step is the panel's move to VP2; VP2 has
  never been driven by this driver or the firmware.
- Mainline gives VP2 `max_output` 1920x1080 and the panel has 1280 lines;
  both Linux kernels run it on VP2 at 800x1280.
- Windows may keep a refused source-to-target assignment (clone); Win+P
  Extend sets it again.
- The emulated vsync is about 60 Hz for both targets; the panel runs at
  55.7 Hz.
- `HdmiDiag` and the `Chk*` snapshots describe VP0 only.
