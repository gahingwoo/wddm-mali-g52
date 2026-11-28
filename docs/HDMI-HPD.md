# HDMI hot-plug in maliwddm

HDMI hot-plug, the driver's own HDMI mode set and HDMI audio. Works on the
TaishanPi 3M at `HdmiHpd` 3. Code: `drivers/maliwddm/hpd.c` (what Windows
sees), `hdmi.c` (the hardware), `hdmi_calc.h` (the arithmetic, host-tested
by `tests/hdmi-host`).

Settings: see [SETTINGS.md](SETTINGS.md). Modes and refresh rates:
[DISPLAY-MODES.md](DISPLAY-MODES.md). Both displays at once:
[DUAL-DISPLAY.md](DUAL-DISPLAY.md). History: [history.md](history.md).

## Levels

`HdmiHpd` is read once, when the adapter starts. With no value it is 3 on
an HDMI boot and 0 when the firmware handed over the DSI panel. A value
always wins. The trace value `Hpd_LevelFrom` says where the level came
from: 0 the value, 1 the HDMI boot default, 2 the panel boot default.

| `HdmiHpd` | What runs |
|---|---|
| 0 | Nothing. The setting is read; nothing is mapped, allocated or written. |
| 1 | Sense only. HPD is polled, the EDID is read over the controller's DDC at start and when a monitor appears, and everything goes to the trace. Windows is told nothing new. |
| 2 | Report. HDMI boot: the child is hot-plug aware, Windows hears of plug and unplug, and gets the EDID just read. No mode is set, so only a monitor that takes the firmware's timing shows a picture. A DSI boot behaves as 1. |
| 3 | Mode set. The driver programs VOP2 VP0, the DW-HDMI-QP controller and the HDPTX PHY. HDMI boot: each monitor gets its own modes. DSI boot: a second child, HDMI. HDMI audio is set up after each mode set. |

On a DSI boot at level 3, `HdmiExclusive` (on by default) reports the
panel as gone while a monitor is on HDMI, so the two take turns: plugging
HDMI moves the desktop there, unplugging lights the panel again after
about a second. With one VidPN source and both displays reported, the HDMI
desktop stays black, so leave it on unless `HdmiExtend` is set.

## How it works

HPD. The connected bit is `IOC_HDMI_HPD_STATUS` bit 3 (LEVEL_INT), the bit
mainline's `dw_hdmi_qp_rk3576_read_hpd` and the firmware read
(`HdmiHpdSource` 0). The driver polls it every `HdmiHpdPollMs` and uses a
level only after it has held for `HdmiHpdDebounceMs` with no edge. The
debounce is 1 s because a replug bounces for about 0.66 s. Windows hears
of a monitor about 1.2 s after the cable goes in. Unplugging is believed
only after HPD has been seen high once since the driver started, so a sink
without HPD that the firmware lit anyway is never reported gone. The driver
never writes `IOC_MISC_CON0`.

Mode set (`hdmi.c` `MwHdmiModeSet`, every step taken from edk2-rk3576),
on a commit of the HDMI target with a timing VP0 is not already driving:

1. CRU and PMU1CRU gates and resets of the HDMI block.
2. VP0 to standby, DCLK_VP0 back on `dclk_vp0_src`, MIPI0 detached,
   HDMI0_IF_CTRL, VP0 timing, post scaler, PRE_SCAN, line flag,
   VP_DSP_CTRL written once (no pre-dither), Esmart0 onto the driver's
   framebuffer, standby released.
3. Controller: timer base, DDC SCL, packets cleared, video path, ROPLL
   (table or calculated, matching mainline's `rk_hdptx_phy_clk_pll_calc`),
   DCLK_VP0 to the PHY pixel clock, TMDS RGB 8 bpc, HDCP2 bypass, HDMI
   mode, no scrambling, SCDC cleared for a 2.0 sink, AVI and VSI
   InfoFrames, lanes, AVMUTE cleared.
4. Audio, only for a sink with an HDMI VSDB and basic audio: mclk_sai6
   12.288 MHz unless the firmware set it, I2S, 48 kHz, 2 ch, N 6144,
   CTS from the TMDS rate, Audio InfoFrame.

A failed mode set puts the previous timing back. Committing the panel
again (DSI boot): VP0 to standby, DCLK_VP0 = CPLL/15, HDMI0 detached,
MIPI0_IF_CTRL 0x80000033, the panel's timing, Esmart0 back on the
firmware's framebuffer, standby released, backlight on. The DSI host and
the panel are never touched.

The driver runs 8 bpc (RGB888 from an ARGB8888 Esmart0 window, TMDS clock =
pixel clock), as the firmware does. Linux runs 10 bpc; if 10 bpc is added,
the 340 MHz cap applies to clock x 1.25, which drops 2560x1440@75.

At level 3 a primary describes the committed target's exact refresh rate
(`Hpd_Refresh`, hundredths of a Hz). A primary described at 60/1 on a
59.998 Hz mode left the HDMI desktop black. Level 0 does the same for a
firmware EDID mode that is not exactly 60 Hz.

The panel child is reported `D3DKMDT_VOT_INTERNAL` with
`HpdAwarenessAlwaysConnected` (`MwHpdQueryChildRelations`), which makes it
the internal display for `DisplaySwitch /internal`. CCD can decide a
topology in user mode without asking the driver; `ccdtest.exe` (`tools/windows/ccdtest.c`)
(in the `m3test-arm64` artifact) lists the CCD paths and calls
`SetDisplayConfig` with a topology, printing its Win32 error
(`ccdtest`, `ccdtest validate internal`, `ccdtest internal`).

## Trace values

Under `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm`,
flushed as each is written:

    reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" | findstr /i "Hpd_ Edid_ Mset_ Dsi_ Aud_ Scan_ Commit"
    reg delete "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" /f

Clear the trace between runs with the second line.

| Value | Meaning |
|---|---|
| `Hpd_Level`, `Hpd_LevelFrom` | The level in use and where it came from (above). |
| `Hpd_Mapped`, `Hpd_Powered`, `Hpd_Dsi`, `Hpd_Children`, `Hpd_Switch`, `Hpd_Reserve`, `Hpd_ReservePa` | Start state: blocks mapped (0x3FF all), power, DSI boot, children reported, level 3 switching on, the 16 MB framebuffer reserved and its address. |
| `Hpd_State`, `Hpd_Changes`, `Hpd_Edges` | Connected now; debounced changes; raw edges. `Hpd_Edges` above `Hpd_Changes` is bounce that was filtered out. |
| `Hpd_Raw` | IOC status in bits 7:0, the GPIO4_PC1 pad in bit 8, IOC_MISC_CON0 in 23:16. |
| `Hpd_Indicate`, `Hpd_IndicatePanel` | Status of the last hot-plug indication to Windows, HDMI and panel. |
| `Hpd_CommitTarget`, `Hpd_CommitMode`, `Hpd_Commits`, `Hpd_CommitAsk`, `Hpd_CommitRot`, `CommitNoPath` | Commits: target, mode (W << 16 \| H), every commit before any decision (target << 28 \| W << 14 \| H), the path's rotation; a commit with no path. |
| `Hpd_IsSup`, `Hpd_IsSupSize` | Last changed IsSupportedVidPn answer: target \| reason << 8 \| supported << 16 \| paths << 24. Reasons: 1 more than one path or not source 0, 2 no modes, 3 pinned source size not one of the target's (`Hpd_IsSupSize`), 4 pinned target mode not ours, 5 sizes differ. |
| `Hpd_Enum` | Last changed cofunctional enumeration: target \| source pinned << 8 \| target pinned << 9 \| ours << 10 \| modes << 16 \| pinned source width / 8 << 20. |
| `Hpd_Refresh`, `Hpd_Default60`, `Hpd_RestoreOk` | Committed refresh in 1/100 Hz; the 60 Hz default; a restore after a failed mode set. |
| `Edid_Status`, `Edid_Bytes`, `Edid_Id`, `Edid_Modes`, `Edid_Mode0`, `Edid_M<i>`, `Edid_R<i>`, `Edid_C<i>` | DDC read status (NTSTATUS), length, manufacturer and product, mode count, each mode's size, refresh (1/100 Hz) and clock (kHz), up to 24. |
| `Edid_Timeouts`, `Edid_Gates` | DDC timeouts; controller clock gates found closed. |
| `Mset_Step`, `Mset_Status` | Last mode set stage: 1 clocks, 2 VP0 programmed, 3 PLL, 4 DCLK on the PHY, 7 SCDC, 10 lanes, 11 audio, 12 done. A hang or bugcheck leaves the last stage reached. Status is an NTSTATUS. |
| `Mset_PllStatus`, `Mset_LaneStatus`, `Mset_Restore`, `Mset_RestoreMode` | GRF lock status words (0x0E or 0x0F typical); the restore after a failure. |
| `Mset_W_*` | Every VP0 and Esmart0 value written. At 2560x1440@60: DspCtrl 0xF, HTotalHsEnd 0x0AA00020, HActStEnd 0x00700A70, VTotalVsEnd 0x05C90005, VActStEnd 0x002605C6, PreScan 0x05130020, ClkCtrl 1, IfCtrl 0x80000033, Vir 0xA00, YrgbMst = `Hpd_ReservePa`, ActInfo 0x059F09FF. |
| `Dsi_Step` | Panel restore stage (3 done). |
| `Aud_Status` | 1 when audio was set up. |
| `Scan_Switch` | Where Esmart0 reads: 1 the firmware's framebuffer, 2 the driver's. |
| `Chk1_*` (+3 s), `Chk2_*` (+10 s) | After each mode set or panel restore: Flips, Scanned, Copied, NotMapped, Invisible, Mismatch, Refused (W << 16 \| H), Maps, Showing, ScanAddr, Fb, Mmu0/Mmu1 status and fault address. |
| `Chk1_VpRaw` | VP0 interrupt raw status 3 s after the mode set. Bit 4: VP0 asked for pixels and got none (window fetch failing: address, IOMMU, bandwidth). |
| `Per_*`, `Per_N` | Snapshot every 5 s for 2 min after the last mode set, then every 30 s (fields below). |
| `Blk_Why`, `Blk_EventMs`, `Blk_AfterMs`, `Blk_*` | First event after a mode set that can blank the screen: 1 visibility off, 3 a commit with no path, 0x20 + `ToFbFrom` the window moved onto the driver's framebuffer; when, and a snapshot. |
| `Fail_*` | A DDI that failed. |

Snapshot fields: `_Flips`, `_Scanned`, `_Copied`, `_NotMapped`,
`_Invisible`, `_Refused`, `_Maps`; `_Showing` (bit 0 window on a primary,
1 scan-out on, 2 paging, 3 visible, 4 path active, 5 driver framebuffer,
6 Hold, 7 vsync interrupt on, 15:8 rotation); `_LastFlip` and `_LastWhy`
(0 scanned out, 1 off, 2 not entered, 3 rotated, 4 another size,
9 invisible); `_VisOn`, `_VisOff`, `_VisOff1Ms`..`4Ms`, `_VisOffN`;
`_IsSup`, `_Enum`, `_RecFunc`, `_Status`, `_Desc` (DDI call counts);
`_Sup0`..`_Sup5` (IsSupportedVidPn answers by reason); `_Sup2Path` (last
two-path VidPN refused, (source << 8 \| target) of each path);
`_Vsyncs`, `_VsyncTgt`, `_IntOn`, `_IntOff`; `_Presents`,
`_PresentFlips`, `_Blts`, `_Fills`, `_LastPresent`, `_LastPresentSrc`,
`_LastPresentDst`; `_Creates`, `_Destroys`, `_PrimCreates`, `_LastPrim`,
`_LastPrimPitch`, `_Describes`, `_LastDesc`, `_LastDescRef`, `_Ref`,
`_SharedRef`; `_ToFbFrom` (1 visibility off, 2 a blt present to the
primary, 3 the copy thread, 4 a primary unmapped, 5 an IOMMU fault,
6 scan-out off, 7 the bugcheck screen); `_FbSkips`; `_Mmu0`, `_Mmu1`,
`_Fault`, `_Ms`.

### HdmiDiag

For a locked monitor with a black picture:

| `HdmiDiag` | Shows | Bars seen | Still black |
|---|---|---|---|
| 0 | Normal operation. | | |
| 1 | VP0's own colour bars (`VP0_COLOR_BAR_CTRL` 0x00640101) after the mode set. | VP0, the port mux and the HDMI side work; look at the window or its pixels. | The HDMI side (data path, AVMUTE) or VP0. |
| 2 | Eight colour bars and a white frame in the driver's framebuffer, with flips kept off it for the whole HDMI session. | Window address, stride, size and IOMMU entries are right; look at the flips. | The window does not show the driver's framebuffer. |

## Testing

Prerequisites: the package installed the usual way (`pnputil /add-driver`,
reboot), a rescue Windows in the boot menu. Set a level from an elevated
prompt and reboot:

    reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiHpd /t REG_DWORD /d 1 /f
    shutdown /r /t 0

Level 1, DSI boot, HDMI unplugged at power-on: expect `Hpd_Level 1`,
`Hpd_Mapped 0x3FF`, `Hpd_Powered 1`, `Hpd_Dsi 1`, `Hpd_State 0`,
`Hpd_Children 1`. Plug a monitor: within about 1.5 s `Hpd_Changes 1`,
`Hpd_State 1`, `Edid_Status 0`, `Edid_Bytes 256`, `Edid_Mode0` its native
size. Unplug: `Hpd_Changes 2`, `Hpd_State 0`. On an HDMI boot: `Hpd_Dsi 0`,
`Hpd_State 1` and an EDID at start.

- `Hpd_State` never changes but `Hpd_Raw` does: set `HdmiHpdSource` to the
  bit that follows the cable (1 or 2), reboot.
- `Edid_Status 0xC000009D`: HPD dropped during the read (same fix).
  `0xC00000B5` with `Edid_Timeouts` high: DDC. `0xC00000A3` with
  `Edid_Gates` non-zero: controller clocks gated; level 3 ungates them,
  levels 1 and 2 do not.

Level 2, HDMI boot: unplug and replug; Windows drops the display and
brings it back (`Hpd_Indicate 0`, `CommitNoPath` on unplug).

Level 3, HDMI boot: expect `Hpd_Switch 1`, `Hpd_Reserve 16384000`, no
`Mset_*` at first (the first commit matches the firmware's timing). Pick
another resolution: `Mset_Step 12`, `Mset_Status 0`, `Scan_Switch 2`,
`Aud_Status 1` on a monitor with speakers. `Mset_Status 0xC00000B5` with
`Mset_Step` 3 or 10 is a PLL or lane lock timeout.

Level 3, DSI boot: plug HDMI: `Hpd_Children 2`, the desktop on HDMI
(`Hpd_CommitTarget 1`, `Mset_Step 12`), panel backlight off. Unplug:
`Hpd_CommitTarget 0`, `Dsi_Step 3`, `Scan_Switch 1`, picture on the panel.

A black HDMI desktop: run `HdmiDiag` 1, then 2, then 0, each from a
cleared trace and a cold boot, and send the screen, `Mset_W_*`, `Chk1_*`,
`Chk2_*`, `Per_*`, `Blk_*`, `Hpd_IsSup`, `Hpd_Enum` and `Hpd_Commit*`.

### Going back

    reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiHpd /t REG_DWORD /d 0 /f

then reboot. Deleting the value returns to the boot-dependent default.
Without a picture: boot with the HDMI cable unplugged (levels 1 and 2 set
nothing), or set the value from the rescue Windows with the main SYSTEM
hive loaded (`reg load`).

## Known issues

- One VidPN source unless `HdmiExtend` is on: no extended desktop on a DSI
  boot. Clone and extend VidPNs are refused.
- Above 340 MHz TMDS only with scrambling, for a sink with SCDC
  ([DISPLAY-MODES.md](DISPLAY-MODES.md)). 4-pixel-unaligned
  widths (1366) and interlaced modes are not offered.
- A monitor swap faster than the 1 s debounce is not seen as an unplug.
- After a hand-back (driver disabled while HDMI is on the driver's
  framebuffer), that framebuffer is leaked on purpose so Basic Display
  keeps a valid one.
- Windows' boot animation looks squashed on a 2560x1440 primary after a
  mode set on a DSI boot: it is drawn at the panel's aspect. It ends when
  DWM takes over; the driver scales nothing there.
- The audio driver (woa-rk3576) is not told about hot-plug and has no
  jack state, so Windows shows the HDMI endpoint with or without a
  monitor. The controller keeps the firmware's format (I2S from SAI6, 2 ch,
  48 kHz, 24-bit in 32-bit slots, N 6144, CTS from the TMDS rate), so ACR
  stays valid while SAI6 is stopped, and the driver rewrites it after each
  mode set. The link may drop for a moment during a mode set.
- A MEMORY_MANAGEMENT (0x1A) bugcheck once followed a panel restore. The
  audit found one race (the framebuffer pointer and size changing while a
  blt or a visibility-off clear wrote), now fixed: writers are drained
  before the switch and late ones skipped (`_FbSkips`). If 0x1A comes
  again, its first parameter says the kind: 0x41790, 0x41284/0x41287 and
  0x61941 point away from a write past a mapping, which would more likely
  be 0x50 or 0x1A 0x41201.

## ACPI

None needed: registers are mapped by physical address, as scanout.c maps
the VOP2, because a resource added to GPU0's `_CRS` would move the
backlight's index. `docs/acpi/0001-gpu-asl-hdmi-blocks.patch` is the ACPI
alternative: an optional private `HDMB` package for GPU0 naming the blocks
and the HPD interrupt (GSIV 399), not as `_CRS` entries (the patch says
why).
