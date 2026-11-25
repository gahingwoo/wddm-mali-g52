# Display resolutions and refresh rates

Two ways to offer more than one mode:

- `HdmiHpd` 3 (the default on an HDMI boot): real HDMI modes at every
  refresh rate the EDID lists. Works on the board (TaishanPi 3M, AOC
  Q27B3S2: 2560x1440 at 60 and 75 Hz, kept across a reboot, hot-plug and
  the 15 s revert work).
- Below level 3 and on the DSI panel: smaller source modes scaled up by
  the VOP2 window (`ScaledModes`), at the firmware's timing.

Code: `drivers/maliwddm/scale.h` (source mode list and window arithmetic,
`tests/modes-host`), `hdmi_calc.h` (`MwHdmiModes`, `tests/hdmi-host`),
`display.c`, `hpd.c`, `scanout.c`, `command.c`. Settings: see
[SETTINGS.md](SETTINGS.md). HDMI trace values:
[HDMI-HPD.md](HDMI-HPD.md).

## Real modes at HdmiHpd 3

The EDID comes from `hpd.c`'s DDC read and is re-read on every plug.
Offered, in this order:

1. Detailed timings: base block, CTA-861 and DisplayID extensions (type I
   and VII, where 120 and 144 Hz modes usually are).
2. CTA-861 VICs: every progressive VIC up to 340 MHz, each 24, 30, 60 or
   120 Hz VIC also at its 1000/1001 rate.
3. Standard timings: the VESA DMT timing of that size and rate, else CVT
   reduced blanking, only when the EDID has a range limits descriptor and
   the mode is inside it.
4. Established timings (DMT).
5. 1920x1080@60 and 1280x720@60.

On an HDMI boot the firmware's own timing comes first for the monitor it
lit. Modes are dropped unless:

| Limit | Value | Why |
|---|---|---|
| TMDS clock | at most 340 MHz | 8 bpc RGB, no HDMI 2.0 scrambling |
| Sink TMDS clock | the HDMI VSDB's Max_TMDS_Clock, when given | older HDMI 1.x sinks |
| PHY | `MwRopllFor` finds a ROPLL setting | table, else calculated |
| Width | multiple of 4 | VOP2 widens other widths past the primary |
| Size | within VP0 (4096x2304) and `HdmiMaxPixels` | the reserved framebuffer |
| Scan | progressive | |

Two entries of the same size within 0.05% in refresh rate count as one
(59.998 and 60 Hz are one; 59.94 and 60 are two).

Windows gets one target mode per timing and one source mode per size: a
size is one entry under Display resolution and several under Advanced
display > Choose a refresh rate. The primary's refresh rate is the
committed target's, so DWM flips at 75 Hz in a 75 Hz mode.

### Default: the largest size at 60 Hz

The mode marked preferred (what Windows takes for a monitor with no saved
arrangement) is the largest size in the list, at the rate closest to 60 Hz
that size has (60.000 before 59.94; 75 Hz only when nothing is nearer 60).
Of two sizes with the same area, the one listed first. Nothing else moves.
A saved arrangement (a rate picked in Settings) still wins. `HdmiDefault60`
0 gives the monitor's own preferred timing first. Trace: `Hpd_Default60`,
`Edid_M00`/`Edid_R00` (the default's size and refresh in 1/100 Hz).

To see the default on a board that has a saved arrangement, back it up
and clear it as administrator, then restart:

    reg export "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" gfx-backup.reg
    reg delete "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Configuration" /f
    reg delete "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Connectivity" /f

### Failures and revert

A failed mode set (PHY or lanes do not lock) puts the previous timing back
on the wire before the commit fails (`Mset_Status` non-zero, `Mset_Restore`,
`Hpd_RestoreOk`). The firmware's timing can be set again when it is an EDID
detailed timing, a CEA-861 or a DMT timing (1080p60 for an HDMI boot with
no EDID). Windows' 15 s revert is an ordinary mode set.

## Source modes scaled by the VOP2 (ScaledModes)

Below `HdmiHpd` 3 the target has one mode, the firmware's timing, so the
monitor never resyncs and the refresh rate cannot change. `ScaledModes`
(on by default, used only when scan-out is set up) adds smaller source
modes:

1. The list (`MwSclModes`): the firmware's size, then every progressive
   size the EDID names, then common desktop sizes from 3840x2160 down.
   Kept: no larger than the screen, at most 8 times smaller (the Esmart
   upscale limit), even sizes, not below 800x600, largest first, at most
   32. A portrait screen gets the same sizes turned. The panel gets
   800x1280, 768x1280, 768x1024, 720x1280, 600x800.
2. Scaling: the firmware's size is identity. A smaller size of the same
   shape (within 1%) is stretched; another shape is
   `D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX` with black bars. Centered is done
   when asked but not offered.
3. The window: on commit, Esmart0 gets `REGION0_VIR`, `YRGB_MST`,
   `ACT_INFO` (source size), `DSP_INFO` (size on screen), `DSP_ST` (bar
   offset), `SCL_FACTOR_YRGB` (the vendor kernel's `vop3_scale_factor`)
   and `SCL_CTRL` (up mode, bicubic across, bilinear down) in one
   config-done. The framebuffer is cleared first. Only scale-up happens.
   No VOP2 register is read. Bars are VP0's background, black.
4. Frames: a primary of a listed size is scanned out like one of the
   firmware's size; anything else is copied to the top left of the
   framebuffer, which is the part the window stretches. Rotation and the
   bugcheck screen work at the source mode's size.
5. Committing the firmware's size, stopping, handing back to Basic Display
   or leaving D0 restores Esmart0 as the firmware set it.

With `ScanOut` 0 every frame is copied and smaller modes still work.

Expected values for 2560x1440: 1920x1080 gives `ACT_INFO` 0x0437077F,
`DSP_INFO` 0x059F09FF, `DSP_ST` 0, `SCL_FACTOR` 0xBFF4BFF9, `SCL_CTRL`
0x59. 1280x1024 gives `DSP_INFO` 0x059F0707 (1800x1440), `DSP_ST` 0x017C.

Trace (`Device Parameters\maliwddm`):

| Value | Meaning |
|---|---|
| `ScaledModes` | 1 if smaller modes are offered |
| `SourceModes`, `SourceMode1`, `SourceModeLast` | count, second and last (W << 16 \| H) |
| `Scl_Src`, `Scl_How` | size last set and how (0 identity, 1 centered, 2 stretched, 3 aspect kept) |
| `Scl_Dst`, `Scl_DstSt`, `Scl_Factor`, `Scl_Ctrl` | values written to `DSP_INFO` (W << 16 \| H), `DSP_ST`, `SCL_FACTOR_YRGB`, `SCL_CTRL` |
| `Scl_Applied`, `Scl_Native` | window changes; the count when last put back |
| `T_SclSrc`, `T_SclFlips` | per snapshot: scaled size now, scaled flips scanned out |
| `Fail_ScanScale`, `Fail_CommitScale`, `Fail_UpdatePathScaling` | a size or scaling refused |
| `EdidEval`, `EdidBytes`, `EdidTiming` | the firmware's EDID (from `GPU0.EDID` in the DSDT) reached Windows; the target mode is its detailed timing |

At levels 0 and 1 the HDMI child is always connected, so a monitor swapped
while Windows runs keeps the boot monitor's EDID.

## Testing

Refresh rates (HDMI boot, no `HdmiHpd` value):

1. `reg query "...\Device Parameters\maliwddm" | findstr /i "Hpd_Level Hpd_Switch Edid_"`:
   `Hpd_LevelFrom` 1, `Hpd_Level` 3, `Hpd_Switch` 1, `Edid_Modes` about 30
   on the AOC, `Edid_M00` 0x0A0005A0 with `Edid_R00` 6000.
2. Choose a refresh rate lists 60 and 75 Hz at 2560x1440 (not 100: above
   340 MHz). Pick 75: the monitor blanks about 2 s, its OSD says 75 Hz,
   `Hpd_CommitMode` 0x0A0005A0, `Mset_Status` 0, `Hpd_Refresh` 7500.
3. Pick 60 and wait: after 15 s Windows reverts.
4. 1920x1080 lists 60, 59.94 and 50 Hz.
5. Hot-plug: unplug 5 s, replug; the same mode comes back, `Hpd_Changes`
   up by 2.
6. Restart at 75 Hz: the firmware boots at 60, Windows comes back at 75.

Scaled modes (`HdmiHpd` 0 or 1, or the panel): expect `ScaledModes` 1 and
`SourceModes` about 20. Pick 1920x1080: full screen, stretched,
`Scl_Src` 0x07800438, `Scl_How` 2, `Scl_Dst` 0x0A0005A0. Pick 1280x1024:
380-pixel bars, `Scl_How` 3, `Scl_DstSt` 0x17C. Back to 2560x1440:
`Scl_How` 0, `Scl_Ctrl` 0. If the screen is wrong, press nothing; Windows
reverts after 15 s.

### Going back

    reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiHpd /t REG_DWORD /d 0 /f

then reboot. 0 is the driver without hot-plug (`ScaledModes` gives smaller
resolutions at the monitor's timing); 1 also logs HPD and the EDID.
Without a picture: the same over SSH, or from the rescue Windows with the
main SYSTEM hive loaded. Deleting the value returns to the default.
`ScaledModes` 0 removes the scaled modes.

## Known issues

- 2560x1440@100 (398 MHz) and anything above 340 MHz need HDMI 2.0
  scrambling, not written: the sink's HF-VSDB, SCDC TMDS_Config over DDC
  with Scrambler_Status read back, the controller's `SCRAMB_CONFIG0`, the
  PHY's 1/40 clock pattern, and redoing it after every hot-plug. To port
  from mainline dw-hdmi-qp once Ciocaltea's series lands (Linux 7.5).
- The VOP2 scaler registers come from mainline and the vendor kernel and
  are checked on the host only; the panel's list too.
- A flip of a primary of the old size right after a commit is copied and
  cropped for that frame.
