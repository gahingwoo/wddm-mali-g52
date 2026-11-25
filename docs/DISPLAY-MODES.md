# Display resolutions in maliwddm

Branch `display-modes`, off `wddm2x`. Code: `drivers/maliwddm/scale.h` (the
mode list and the window arithmetic, host-tested by `tests/modes-host`),
`display.c` (what Windows is offered and what it commits), `scanout.c`
(the VOP2 window), `command.c` (the CPU copy). Built by CI; not run on the
board yet.

## The report

TaishanPi 3M, HDMI only (the DSI ribbon is broken), an AOC monitor at
2560x1440 set by the firmware, `HdmiHpd` 1. Settings > Display:

- "Display resolution" greyed out, with one entry, "2560 x 1440 (Recommended)".
- "Scale" works (the user changed it; corrected after a first report that it did not).

## Why there was one resolution

At `HdmiHpd` 0 to 2 the HDMI link is never retimed from Windows: the
target has one mode, the firmware's timing. The source modes are what
Windows lists as resolutions, and the driver offered one, the firmware's
size. A switch for smaller source modes existed since `e081abf`
(`ScaledModes`, default 0), but `scanout.c` never scaled: a smaller primary
was copied into the top left corner of the framebuffer. So it stayed off,
and `SourceModes` was 1 in every board trace.

The EDID is not the problem. The firmware leaves it in the DSDT
(`GPU0.EDID`, edk2-rk3576 main since `32de639`), the driver hands it to
Windows in `QueryDeviceDescriptor`, and the board's trace of build 333
(2026-11-24, `HdmiHpd` 1) has `EdidEval` 0, `EdidBytes` 0x100 and
`EdidTiming` 1: both blocks reached Windows and the target mode is the
EDID's own detailed timing. At `HdmiHpd` 2 and 3 the EDID comes from
`hpd.c`'s DDC read instead and is re-read on every plug. At 0 and 1 a
monitor swapped while Windows runs keeps the boot monitor's EDID: the child
is "always connected" there by design.

## What the driver does now

`ScaledModes` is on by default. When scan-out is set up (it is unless the
VOP2 or the IOMMU tables could not be had) and `HdmiHpd` is below 3:

1. **The list** (`MwSclModes`). The firmware's size first, then every size
   the EDID names (established timings, standard timings, detailed timings
   in the base block and in CEA-861 blocks, progressive only) and the usual
   desktop sizes (3840x2160 down to 800x600). Kept: no larger than the
   screen in either direction, at most 8 times smaller (the Esmart window's
   upscale limit on the RK3576, `max_upscale_factor = 8` in mainline and in
   the vendor kernel), even sizes, not below 800x600. Largest first, at most
   32. A portrait screen (the panel, 800x1280) gets the same sizes turned.
   For a 2560x1440 monitor with a typical EDID the host test lists:

   2560x1440, 2560x1080, 1920x1440, 1920x1200, 1920x1080, 1680x1050,
   1600x1200, 1600x1024, 1600x900, 1440x900, 1366x768, 1360x768, 1280x1440,
   1280x1024, 1280x960, 1280x800, 1280x768, 1280x720, 1152x864, 1024x768,
   800x600.

   For the panel: 800x1280, 768x1280, 768x1024, 720x1280, 600x800.

2. **Scaling offered with each mode** (`MwSclDefault`, `display.c`
   EnumVidPnCofuncModality). The firmware's size: identity. A smaller mode
   of the screen's shape (within 1%, so 1366x768 counts as 16:9):
   stretched, which keeps the aspect ratio. Another shape:
   `D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX`, as large as it goes with black
   bars. With no source mode pinned yet all three are offered; once the
   path's scaling is pinned to identity, only the firmware's size is
   offered, and `IsSupportedVidPn` refuses a smaller mode with identity.
   Centered (one to one) is done when Windows asks for it but is not
   offered. The target keeps its one mode, the firmware's timing, so the
   monitor never resyncs and the refresh rate stays the EDID's.

3. **The window** (`scanout.c` `MwScanScale`, values from `MwSclWindow`).
   On a commit of a smaller mode, Esmart0 (VOP2 0x1800) gets, in one
   config-done:

   | Register | Offset | Value |
   |---|---|---|
   | `REGION0_VIR` | 0x181C | the framebuffer's pitch / 4 |
   | `REGION0_YRGB_MST` | 0x1814 | the framebuffer |
   | `REGION0_ACT_INFO` | 0x1820 | the source mode's size |
   | `REGION0_DSP_INFO` | 0x1824 | the size on the screen |
   | `REGION0_DSP_ST` | 0x1828 | where it starts (bars) |
   | `REGION0_SCL_FACTOR_YRGB` | 0x1834 | the vendor kernel's `vop3_scale_factor` for scale-up, the same numbers as mainline's `vop2_scale_factor` (the host test checks every pair up to 8x) |
   | `REGION0_SCL_CTRL` | 0x1830 | up mode, bicubic across, bilinear down: the vendor kernel's rk3576 Esmart0 filters |

   The framebuffer is cleared first. Only scale-up ever happens, so the
   gt2/gt4 bits stay 0 and `REGION0_CTRL` (enable, format, the bits the
   firmware set) is not written. No VOP2 register is read, as everywhere
   else in this driver. Bars are VP0's background, `DSP_BG` 0 (the
   firmware writes 0), so black.

   For 1920x1080 on 2560x1440: `ACT_INFO` 0x0437077F, `DSP_INFO`
   0x059F09FF, `DSP_ST` 0, `SCL_FACTOR` 0xBFF4BFF9, `SCL_CTRL` 0x59.
   For 1280x1024: `DSP_INFO` 0x059F0707 (1800x1440), `DSP_ST` 0x017C (x 380).

4. **Frames.** The window now reads the source mode's size, from the top
   left of the framebuffer or from a primary of that size, so both ways a
   frame reaches the screen work unchanged:
   - **Scan-out**: a primary of a listed size is entered in the VOP2 IOMMU
     like one of the firmware's size, and a flip to it is the address write
     plus, when its stride is not the framebuffer's, one `VIR` write.
     `MwScanWhy` scans out only a primary of the size the window reads now;
     anything else is copied.
   - **The copy** (no scan-out, an IOMMU fault, a primary of another size):
     the picture goes to the top left of the framebuffer as before, which
     is now the part the window stretches. Presents to the primary and the
     driver's rotation copy use the same size (`MwScrW`, `MwScrH`).

5. **Rotation.** Unchanged in principle: the source mode keeps the screen's
   orientation, DWM draws the desktop turned into the primary, and the
   window scales that. A primary of the turned size (which the driver turns
   on the CPU) is turned into the source mode's size in the corner.

6. **The bugcheck screen** is drawn at the source mode's size into the top
   left of the framebuffer (`SystemDisplayEnable` reports that size), which
   the window still scales; no register is written then beyond moving the
   window onto the framebuffer.

7. **Back to the firmware's window.** Committing the firmware's size again,
   the device stopping or handing the display back to Basic Display, and
   the adapter leaving D0 put back Esmart0 as the firmware set it (all at
   the screen's size, scaler 0). Back in D0 the scaled window is set again.
   If the window was never changed, none of this writes anything.

At `HdmiHpd` 3 none of this runs: `hpd.c` has its own VidPN and sets real
HDMI modes (below).

## Switches

A DWORD in `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters`,
read when the adapter starts.

| Name | Default | Meaning |
|---|---|---|
| `ScaledModes` | 1 | 0: the firmware's size only, as before this branch. |
| `ScanOut` | 1 | Unchanged. At 0 every frame is copied; smaller modes still work, the copy goes to the corner the window scales. |

Registry trace (`Device Parameters\maliwddm`):

| Value | Meaning |
|---|---|
| `ScaledModes` | 1 if smaller modes are offered (setting on, scan-out set up, `HdmiHpd` below 3) |
| `SourceModes` | how many source modes, the firmware's included |
| `SourceMode1`, `SourceModeLast` | the second and the last, width << 16 \| height |
| `Scl_Src`, `Scl_How` | the size the window was last set to and how (0 identity, 1 centered, 2 stretched, 3 aspect ratio kept) |
| `Scl_Dst`, `Scl_DstSt`, `Scl_Factor`, `Scl_Ctrl` | what was written to `DSP_INFO` (as width << 16 \| height), `DSP_ST`, `SCL_FACTOR_YRGB`, `SCL_CTRL` |
| `Scl_Applied`, `Scl_Native` | how many times the window was changed; the count when it was last put back to the firmware's |
| `T_SclSrc`, `T_SclFlips` | at each trace snapshot: the scaled size now (0 at the firmware's) and flips of a scaled primary scanned out |
| `Fail_ScanScale`, `Fail_CommitScale`, `Fail_UpdatePathScaling` | a size or scaling the window refused |

## Risks

- **The scaler has never run on this board.** The registers and values
  come from mainline and the vendor kernel; the window's size and stride
  registers were written by `hdmi.c`'s mode set on the board (HdmiHpd 3,
  2026-11-22), the scaler registers never. If a smaller mode shows black or
  garbage, Windows puts the old resolution back after 15 seconds unless
  "Keep changes" is pressed, and `ScaledModes` 0 removes the modes.
- The Esmart line buffer: the window reads at most 2560 pixels a line here,
  within the 4K line buffer Esmart0 has in every RK3576 line-buffer mode.
- A flip of a primary of the old size right after a commit is copied and
  cropped to the new size for that frame.
- The DSI panel's list is built by construction and checked on the host
  only (the ribbon is broken).

## Board test

Prerequisites: the package from this branch installed the usual way
(`pnputil /add-driver`, reboot), HDMI monitor, `HdmiHpd` as now (1). No new
setting is needed. After the reboot:

    reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" | findstr /i "ScaledModes SourceMode Scl_ EdidBytes ScanOut"

Expect `ScaledModes` 1, `SourceModes` about 20, `EdidBytes` 0x100.

1. **The list.** Settings > System > Display > Display resolution: enabled,
   2560 x 1440 (Recommended) first, then the list above (it depends on
   the monitor's EDID). Screenshot it.
2. **Scale.** Set 125%, then 150%, then back to 100%: text resizes each
   time, as before this branch.
3. **1920 x 1080.** Pick it. Within a second or two the whole screen shows
   the desktop at 1920x1080, stretched, with no bars and the mouse where
   it is drawn. Press "Keep changes". Move a window around for 10 seconds
   (scan-out). Check the trace: `Scl_Src` 0x07800438, `Scl_How` 2,
   `Scl_Dst` 0x0A0005A0, `Scl_Factor` 0xBFF4BFF9, `Scl_Ctrl` 0x59. If the
   screen is black or wrong: do not press anything, Windows reverts after
   15 s; then photograph and save the trace.
4. **1280 x 1024** (another shape). Black bars left and right, 380 pixels
   each, the picture 5:4. `Scl_How` 3, `Scl_Dst` 0x070805A0, `Scl_DstSt` 0x17C.
5. **Rotate** at 1920 x 1080: Display orientation Portrait, Keep, then
   Landscape again. The desktop turns and fills the screen each time.
6. **Back to 2560 x 1440.** Sharp again, full screen. `Scl_Src` 0x0A0005A0,
   `Scl_How` 0, `Scl_Ctrl` 0.
7. **Hot-plug** at 1920 x 1080 (pick it again first): unplug the HDMI
   cable for 5 s and plug it back. The picture comes back at 1920x1080
   stretched. Then the same at 2560 x 1440.
8. **Restart** at 1920 x 1080: the firmware's screen at boot is normal, and
   Windows comes back at 1920 x 1080.
9. **Regression** (optional): `ScaledModes` 0, reboot: one resolution again,
   as before.

## Phase 2: real modes and refresh rates (`HdmiHpd` 3)

Branch `display-retime`, off `test-2026-11-24`. Code: `hdmi_calc.h`
(`MwHdmiModes` and what it uses, host-tested by `tests/hdmi-host`), `hpd.c`
(the level 3 VidPN, the restore). Built by CI; not run on the board.

The report that started it: the refresh rate cannot be changed either.
Scaling cannot change it: the target keeps the firmware's timing. Only a
real mode set can, and `HdmiHpd` 3 has one (`hdmi.c` `MwHdmiModeSet`: VP0's
timing, DCLK_VP0, the HDPTX ROPLL, the DW-HDMI-QP controller, audio again;
run on this board on 2026-11-22 and 2026-11-23). What it lacked was modes.

### What is offered

Everything the EDID says the monitor takes, at every refresh rate it lists,
that the driver can drive. In this order (the first is the monitor's
preferred mode, the one Windows marks Recommended and the firmware picks):

1. Detailed timings: base block, CTA-861 extensions, DisplayID extensions
   (type I and type VII timings, where monitors put their 120 and 144 Hz
   modes).
2. CTA-861 short video descriptors: a VIC table of every progressive VIC up
   to 340 MHz (1, 2/3, 4, 16, 17/18, 19, 31 to 34, 41, 47, 60 to 64, 93 to
   95, 98 to 100), each 24, 30, 60 or 120 Hz VIC also at its 1000/1001
   rate (59.94, 119.88 Hz and so on; VICs 1 and 2 are listed at 59.94 and
   get 60).
3. Standard timings: the VESA DMT timing of that size and rate (32 DMT
   entries, Linux's `drm_dmt_modes` values), else CVT reduced blanking
   (Linux's `drm_cvt_mode` arithmetic; 2560x1440@60 comes out at 241.5 MHz,
   2720x1481, as Linux has it) but only when the EDID has a range limits
   descriptor and the mode is inside it.
4. Established timings (DMT).
5. 1920x1080@60 and 1280x720@60, as before.

Then, as before, the firmware's own timing first for the monitor it lit.

Filtered by what the hardware and this driver can drive (`MwModeAdd`):

| Limit | Value | Why |
|---|---|---|
| TMDS clock | at most 340 MHz | 8 bpc RGB with no scrambling (`SCRAMB_CONFIG0` 0, SCDC TMDS_CONFIG 0, as the firmware). The HDPTX PHY and the controller go to 600 MHz with HDMI 2.0 scrambling; mainline gets that with Ciocaltea's series (Linux 7.5). Not written here. |
| Sink TMDS clock | the HDMI VSDB's Max_TMDS_Clock when it gives one | below 340 MHz only (older HDMI 1.x sinks). A sink with no VSDB gets no lower cap, as in Linux. |
| PHY | `MwRopllFor` finds a ROPLL setting | the firmware's table, else its calculation |
| Width | a multiple of 4 | VOP2 widens other widths past the primary |
| Size | within VP0 (4096x2304) and `HdmiMaxPixels` (2560x1600 by default) | the reserved framebuffer |
| Scan | progressive only | |

Two entries of the same size with refresh rates within 0.05% count as one
(59.998 and 60.000 Hz are one choice to a user; 59.94 and 60 are two).

For the board's AOC (its `edid.bin` from the 2026-11-22 Linux session, run
through the same code on the host, not committed): 2560x1440 at 60 and 75
Hz, 1280x1440@59.91, 1920x1080 at 60, 59.94 and 50, 1280x720 at 60, 59.94
and 50, 1680x1050, 1440x900, 1280x1024 at 60 and 75, 1024x768 at 60, 70,
75 and 99.8, 800x600 at 56, 60, 72, 75 and 99.8, 720x576@50, 720x480 and
640x480 at several rates. 30 modes. **2560x1440@100 is not there:** it is
397.98 MHz, above the 340 MHz this driver can drive without scrambling.
That needs HDMI 2.0 scrambling (below).

### How Windows sees it

One target mode per timing, one source mode per size. A size at several
refresh rates is one entry in Settings > Display > Display resolution and
several in Advanced display > Choose a refresh rate. Only the monitor's
preferred mode is marked preferred. The primary's refresh rate is the
committed target's (`Hdmi.RefNum/RefDen`, the fix of 2026-11-22), so DWM
flips at 75 Hz in a 75 Hz mode.

### Robustness

- **A failed mode set** (the PHY does not lock, the lanes do not come up)
  now puts the previous timing back on the wire, on the framebuffer it had,
  before the commit fails (`Restore`). Windows keeps the old mode, and the
  monitor shows it again. Before, nothing was left on the wire. The
  firmware's own timing can be programmed again when it is a detailed
  timing of the EDID (the usual case) or a CEA-861 or DMT timing
  (`MwKnownTiming`; for an HDMI boot without EDID that is 1080p60). HDMI
  to HDMI only: the DSI panel path is unchanged.
- **Windows' 15 s revert** commits the previous mode, which is a full
  timing now, so the revert is a mode set like any other.
- **Hot-plug** re-reads the EDID over DDC on every plug (unchanged) and
  builds the list again; Windows asks for the new modes after the plug is
  reported.
- Trace: `Edid_M<i>`, `Edid_R<i>`, `Edid_C<i>` (each mode's size, refresh in
  1/100 Hz, clock in kHz, up to 24), `Hpd_Refresh`, `Mset_Status`,
  `Mset_Restore`, `Mset_RestoreMode`, `Hpd_RestoreOk`.

### Board result (2026-11-24)

TaishanPi 3M, AOC Q27B3S2 on HDMI, package 6.49.58.348 (`34aff0f`),
`HdmiHpd` 3 set by hand:

- Settings offered 60 and 75 Hz at 2560x1440.
- 75 Hz took: `Hpd_Refresh` 7500, and the monitor showed 75 Hz.
- It was kept across a reboot: Desktop mode and Active signal mode both
  2560x1440 at 75 Hz.
- HDMI hot-plug and the 15 s revert both worked (the user's report).

Merged to main as `d4b1e8f`, still behind the switch.

### Default: on for HDMI boots (branch `hdmi-retime-default`)

Without an `HdmiHpd` value the level is now 3 on an HDMI boot and 0 when the
firmware handed over the DSI panel (`MwHpdLevelFor`, `hdmi_calc.h`; the
panel is known by GPU0's second memory resource, which the firmware adds
only for a lit DSI panel, read before anything is mapped). A value, any
value, wins: 0 or 1 to go back. The trace says which it was:
`Hpd_LevelFrom` 0 from the value, 1 the HDMI default, 2 the panel's.

| Boot | No value | `HdmiHpd` set |
|---|---|---|
| HDMI | 3: the monitor's own modes and refresh rates | the value |
| DSI panel (with or without HDMI) | 0, as before | the value |

What changes on an HDMI boot with no value: the hot-plug thread runs, 16 MB
of contiguous memory is reserved at start for the driver's framebuffer,
the HDMI child is hot-plug aware, and Display resolution lists the
monitor's real modes instead of `ScaledModes`' scaled ones (`ScaledModes`
applies below level 3 only, so it stays for the panel and for anyone who
goes back to 0 to 2).

The board evidence is one install and one reboot, plus hot-plug and the
revert; the default was the user's decision on that. The first boot with
the value deleted is the check that the default itself is read the same
way as the value was.

**To go back** (administrator prompt, then reboot):

    reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiHpd /t REG_DWORD /d 0 /f

0 is the driver as it was before hot-plug existed (`ScaledModes` gives
smaller resolutions at the monitor's timing); 1 also logs HPD and the EDID.
With no usable picture: the same over SSH, or from the rescue Windows with
the main Windows' SYSTEM hive loaded. Deleting the value returns to the
default.

### Default: the largest size at 60 Hz (branch `default-60hz`)

The user's decision of 2026-11-25: the default HDMI mode is the monitor's
highest resolution at 60 Hz, not its highest refresh rate. Higher rates
(2560x1440 at 75 Hz on the AOC) stay in the list and selectable in
Settings > Display > Advanced display > Choose a refresh rate.

The default is the one target and monitor mode marked preferred, the
first of the list (`hpd.c`, `AddTargetModes` and
`MwHpdRecommendMonitorModes`). Windows takes it for a monitor it has no
saved arrangement for; a saved arrangement (a rate picked in Settings)
still wins, as before. `UpdateEdid` now moves to the front
(`MwHdmiDefaultFirst`, `hdmi_calc.h`):

- the largest size in the list (width times height),
- at the rate closest to 60 Hz that size is offered at: 60.000 Hz before
  59.94, and 75 Hz only when the size has nothing nearer 60;
- of two sizes with the same area, the one listed first.

The rest keep their order; nothing is added or dropped. So a monitor or
firmware timing of 2560x1440@75 that used to come first gives 2560x1440@60
the preferred mark, and a monitor whose largest size is listed only at
75 Hz keeps that size rather than dropping to 1920x1080@60. With no EDID
the safe list already starts at 1080p60. The DSI panel path is unchanged.

`HdmiDefault60` 0 (Device Parameters, read at start) gives the old order
back. The trace gets `Hpd_Default60`, and `Edid_M00` / `Edid_R00` show the
default (size, refresh in 1/100 Hz).

`tests/hdmi-host` checks it: 1440p75 put first comes back as 1440p60 first
with 1440p75 still there and every mode kept, 1080p's 60.000 before 59.94,
50, 100, 120 and 144, a monitor without 1440p60 keeping 1440p75, and the
no-EDID list.

Board test (TaishanPi 3M or CM5-IO on HDMI, the AOC or a 75/144 Hz
monitor):

1. Install the `default-60hz` package (full restart). The board has a saved
   2560x1440@75 arrangement from 2026-11-24, which would hide the default.
   Back it up and clear Windows' saved display arrangements (as
   administrator; Windows builds them again):

       reg export "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" gfx-backup.reg
       reg delete "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Configuration" /f
       reg delete "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Connectivity" /f

   then restart.
2. After login: Settings > Display shows 2560 x 1440 (Recommended), and
   Advanced display shows 60 Hz, with 75 Hz in the list. The monitor's OSD:
   2560x1440 60 Hz. Trace (`Device Parameters\maliwddm`): `Hpd_Default60`
   1, `Edid_M00` 0x0A0005A0, `Edid_R00` 6000.
3. Pick 75 Hz, Keep changes: OSD 75 Hz. Restart: still 75 Hz (the saved
   arrangement wins over the default).
4. Optional: `HdmiDefault60` 0, clear the arrangements again, restart: the
   monitor's own preferred timing is the default again (on the AOC also
   60 Hz, its first detailed timing; on a monitor whose first timing is
   its fastest, that rate).

### HDMI 2.0 scrambling (not done)

What 2560x1440@100 (398 MHz) and 1080p144 at more than 340 MHz would take:
the sink's HF-VSDB (SCDC present, Max_TMDS_Character_Rate), SCDC writes
over DDC (TMDS_Config: scrambling on, 1/40 clock ratio; then reading
Scrambler_Status back), the controller's `SCRAMB_CONFIG0`, the PHY's 1/40
TMDS clock pattern, and re-enabling it after every hot-plug, which sinks
forget. Mainline dw-hdmi-qp gets this in Linux 7.5 (Ciocaltea); port it
from there once it has landed.

### Board test for refresh rates

TaishanPi 3M, HDMI only, the AOC. The package from `display-retime`.

1. Install, then delete any `HdmiHpd` value (the default is 3 on an HDMI
   boot since `hdmi-retime-default`; before that, set it to 3):

       reg delete "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiHpd /f

   and reboot. `Hpd_LevelFrom` 1 says the default was used. The desktop comes up on HDMI as before.
   `reg query "...\Device Parameters\maliwddm" | findstr /i "Hpd_Level Hpd_Switch Edid_"`:
   `Hpd_Level` 3, `Hpd_Switch` 1, `Edid_Modes` about 30, `Edid_M00`
   0x0A0005A0 with `Edid_R00` 6000, and a 2560x1440 entry with `Edid_R` 7500.
   If the desktop does not come up: the rescue Windows, set `HdmiHpd` back
   to 1.
2. Settings > Display > Advanced display > Choose a refresh rate. Expect 60
   and 75 Hz at 2560x1440 (not 100: above 340 MHz). Screenshot.
3. Pick 75 Hz. The monitor goes dark for about 2 s and comes back. Check
   the monitor's OSD (input information): 2560x1440, 75 Hz. Press Keep
   changes. Trace: `Hpd_CommitMode` 0x0A0005A0, `Mset_Status` 0,
   `Hpd_Refresh` 7500. Drag a window for 10 s.
4. **Revert.** Pick 60 Hz and do not press anything: after 15 s Windows
   goes back to 75 Hz, the OSD says 75 Hz. Then pick 60 Hz and keep it.
5. Display resolution 1920 x 1080. OSD: 1920x1080 60 Hz. Choose a refresh
   rate now lists 60, 59.94 and 50. Pick 50; OSD 50 Hz. Back to 60.
6. **Hot-plug** at 1920x1080: unplug 5 s, plug back. The picture comes back
   at 1920x1080, OSD 60 Hz; `Hpd_Changes` up by 2, `Edid_Bytes` 0x100.
7. **Reboot persistence.** Set 2560x1440 at 75 Hz, restart. The firmware's
   boot screen is at 60 Hz (the firmware's choice); after login the OSD
   says 75 Hz again.
8. Back to 2560x1440 at 60 Hz.

A failed mode set shows as `Mset_Status` non-zero with `Mset_Restore` 0
and the old mode back on the monitor; nothing in this test forces one.
