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

## Phase 2: retiming the HDMI output from Windows (design only)

Scaling keeps the monitor at its native timing, which is what laptops and
most GPUs do by default. Real modes (the monitor resyncing to 1920x1080,
other refresh rates) already exist at `HdmiHpd` 3: `hdmi.c`'s
`MwHdmiModeSet` programs VP0's timing, DCLK_VP0, the HDPTX ROPLL and the
DW-HDMI-QP controller, moves Esmart0 to a framebuffer of the driver's own
(`MwHpdReserve`) and sets up HDMI audio again, all ported from
edk2-rk3576 and run on this board on 2026-11-22 and 2026-11-23. It offers
the EDID's progressive detailed timings up to 340 MHz and 1920x1080@60 and
1280x720@60. What making that the normal way to change resolution would
take:

1. **The board check first.** Level 3 has run on a DSI boot with HDMI
   plugged and on plug/unplug; a resolution change from Settings on an HDMI
   boot at level 3 has not been tried. That is one boot with `HdmiHpd` 3
   and steps 1, 3, 6 and 7 above (expect the monitor to resync, about 2 s).
2. **More real modes.** `MwHdmiModes` (hdmi_calc.h) knows detailed timings
   only. Add the CEA-861 short video descriptors (a VIC table: 720p, 1080p
   at 50/60, 4K at 24/25/30 within 340 MHz), the EDID's standard and
   established timings through a DMT table, and CVT reduced blanking for
   sizes in neither. Each is checked against the PHY (`MwRopllFor`
   must find a setting), the 340 MHz TMDS cap (no scrambling until Ciocaltea's series
   lands), 4-pixel alignment and `HdmiMaxPixels`.
3. **Refresh rates.** Several target modes of one size (2560x1440 at 60 and
   75 on the AOC): the target mode set lists them all, the monitor mode set
   too, and the primary's refresh rate must be the committed target's
   (`Hdmi.RefNum/RefDen`, the cause of the black desktop on 2026-11-22).
4. **Both at once.** Smaller source modes on top of a retimed target: the
   window arithmetic here needs the current target size instead of the
   firmware framebuffer's, and the reserved framebuffer instead of the
   firmware's. `scale.h` is already independent of where the sizes come
   from; `MwScanScale` would take them from `Hdmi`.
5. **Framebuffer size.** The reserved framebuffer is allocated once at
   start for `HdmiMaxPixels` (2560x1600 by default). 4K modes need 33 MB
   contiguous below 4 GB, at start, or are not offered.
6. **Default.** `HdmiHpd` 3 would become the default for HDMI boots only
   after (1) and a run of plug, unplug, sleep and restart. A failed mode set
   is a failed commit today, with the window left on the driver's
   framebuffer and no timing on the wire: it would need to program the
   previous timing again. Windows' 15 s revert covers a mode the monitor
   shows black but the hardware accepted.

The DSI panel has one timing and is not retimed; it gets scaled modes only.
