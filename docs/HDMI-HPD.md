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
| 1 | Sense only. HPD polled every 200 ms (debounced, below), the EDID read over the controller's DDC when a monitor appears and once at start. Everything goes to the registry trace. Windows is told nothing new. |
| 2 | Report. HDMI boot: the one child is hot-plug aware, Windows is told of plug and unplug, and the child's EDID is the one just read. No mode is ever set, so only a monitor that takes the firmware's timing gets a picture. A DSI boot behaves as 1. |
| 3 | Mode set. The driver programs VOP2 VP0, the DW-HDMI-QP controller and the HDPTX PHY itself. HDMI boot: a different monitor gets its own preferred timing. DSI boot (TaishanPi 3M panel): a second child, HDMI; Windows can show one or the other (Win+P), not both. HDMI audio is set up after each mode set. |

Other DWORDs, all optional:

| Name | Default | Meaning |
|---|---|---|
| `HdmiHpdSource` | 0 | Which bit is HPD: 0 `IOC_HDMI_HPD_STATUS` bit 3 (LEVEL_INT, what mainline and the firmware's HPD wait read), 1 bit 6 (vendor's HPD_PORT_LEVEL), 2 the GPIO4_PC1 pad. |
| `HdmiHpdPollMs` | 200 | Poll period, 50 to 2000. |
| `HdmiHpdDebounceMs` | 1000 | A level counts only after it has held this long with no edge, 300 to 10000. |
| `HdmiAudio` | 1 | 0: no audio setup after a mode set. |
| `HdmiMaxPixels` | 4096000 (2560x1600) | Size of the driver's own framebuffer, allocated at start at level 3. Modes larger than this are not offered. |

## Linux on the same board (2026-11-22 reference)

Armbian 7.3.5-edge through our UEFI, mainline dw-hdmi-qp and hdptx, DSI
panel and a 2560x1440 monitor attached, one unplug and replug with
drm.debug (files in `~/Desktop/board-session-prep/hdmi-ref/`, not in the
repo). What it settles and what the driver now does about it:

- HPD bit. Mainline's `dw_hdmi_qp_rk3576_read_hpd` returns connected iff
  `IOC_HDMI_HPD_STATUS & BIT(3)` (LEVEL_INT). Its interrupt path is only a
  trigger: the hard IRQ takes any non-zero status word, masks
  (`HPD_INT_MSK`), the thread writes `HPD_INT_CLR` 1 and queues the work
  150 ms later (`HOTPLUG_DEBOUNCE_MS`), and the work calls read_hpd. So
  the `hpd_notify status=1/2` lines and the sysfs status are all bit 3,
  which is `HdmiHpdSource` 0, the default. The driver never writes
  `IOC_MISC_CON0`; the firmware leaves `HPD_INT_CLR` 1 and `MSK` 0, which
  is what mainline's setup_hpd writes too.
- Bounce. Replug: connected at 224.559 s, disconnected at 225.023,
  connected at 225.223 (the 0.2 s sysfs poll caught it as 2 high, 1 low).
  Unplug was clean. Hence the 1 s debounce on a 200 ms poll: a level is
  used only after 1 s without an edge, so this replug is reported once, at
  about 226.2 s. `Hpd_Edges` counts the raw edges; on that replug it would
  go up by 3 while `Hpd_Changes` goes up by 1.
- Timing of the rest. Linux re-read the EDID 3.05 s after the unplug
  notify and 3.7 s after the replug; the compositor's mode set came 3.6 s
  after the replug. The driver reads the EDID right after the debounce,
  so Windows should hear about a monitor in about 1.2 s.
- Modes. Our parser on the same `edid.bin`: 2560x1440@60 241.70 MHz
  (+H +V, preferred), 2560x1440@75 302.13 MHz (+H -V), 1280x1440 156 MHz,
  then the safe 1920x1080@60 (VIC 16) and 1280x720@60 (VIC 4); sink HDMI,
  SCDC, basic audio, no QS. These are Linux's DTD modes with the same
  syncs. 2560x1440@100 (397.98 MHz) is left out (above 340 MHz, no
  scrambling here); Linux offers it with the scrambling patch. Linux's
  extra modes from the established and standard timing blocks
  (1680x1050, 1024x768 and so on) are not offered: only DTDs and the safe
  pair.
- PLL. 241.70 MHz is not a table entry (the table's QHD entry is 241.5);
  it is calculated, and the result is what mainline's
  `rk_hdptx_phy_clk_pll_calc` gives (Mdiv 101, SDM 14/39, SDC 5 3/13,
  checked in tests/hdmi-host). The firmware drives this rate the same way
  on the CM5-IO.
- Bit depth: a deliberate difference. Linux ran 2560x1440@60 at 10 bpc
  (`bpc=10`, TMDS 302.125 MHz = 241.7 x 1.25, `max_bpc = 10` in the
  Rockchip glue, sink 12 bpc capable) from a Cluster0 window in XR30.
  The driver, like the firmware, runs 8 bpc: RGB888 from an ARGB8888
  Esmart0 window, VO0_GRF_SOC_CON8 8 bpc, PLL_PCG_CLK_SEL 0, TMDS = pixel
  clock. That is the configuration that has put a picture on this board
  from the firmware, and Windows' primaries are 8 bpc anyway. If 10 bpc is
  ever added, the 340 MHz cap must apply to clock x 1.25, which drops
  2560x1440@75 (377.7 MHz).
- Pitch: Linux's 2560x1440 framebuffer has pitch 10240 = width x 4, as
  the driver's own framebuffer has.
- Ports. Linux runs the panel on VP2 (Esmart0, CRTC video_port2) and HDMI
  on VP0 (Cluster0) at the same time; the firmware and this driver put the
  panel on VP0. So an extended desktop is possible on this hardware by
  moving the panel to VP2 (its own dclk from CPLL, MIPI0_IF_CTRL MUX 2 with
  PCLK_DIV, VP2_CLK_CTRL 0, the vendor dump's 0x8010003b) and giving
  Windows a second VidPN source. Not done here: that is the second step,
  after single-output switching has been seen to work. It would also take
  the DSI-to-HDMI-and-back path out, which is the least certain part of
  this one.

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
2. Plug the monitor. Within ~1.5 s (1 s debounce, then the EDID): `Hpd_Changes 1`, `Hpd_State 1`,
   `Edid_Status 0`, `Edid_Bytes 256` (typical), `Edid_Id` = the monitor's
   manufacturer/product bytes, `Edid_Modes` > 0, `Edid_Mode0` = its native
   size as (W << 16 | H). `Hpd_Raw` shows the IOC status in bits 7:0, the
   pad in bit 8, IOC_MISC_CON0 in bits 23:16.
3. Unplug: `Hpd_Changes 2`, `Hpd_State 0`, about 1.2 s after the cable
   leaves. `Hpd_Edges` larger than `Hpd_Changes` is contact bounce that
   was filtered out, as intended.
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

## Board run of 0aabb35 (2026-11-22) and what the next one should split

Level 1 worked as designed on the TaishanPi 3M (DSI boot, AOC 2703): HPD
on bit 3, EDID 256 bytes in 296 ms, sink 7 (HDMI, SCDC, audio), unplug and
replug each reported once, 1.1 s after the raw edge.

Level 3, DSI boot with the monitor attached: Windows committed the HDMI
target by itself at boot. The mode set ran to the end (step 12, PLL 0x0C,
lanes 0x0E, SCDC 0, audio 1, `Scan_Switch` 2, our framebuffer at
0xFEA9C000) and the monitor locked at the right timing, but the picture
was black. `DisplaySwitch /internal` did not bring Windows back to the
panel (no commit followed). The dump mixed values from the level 1 boot
after it (T_* show an 800x1280 framebuffer, `CommitUnpinned` is the level 1
VidPN's), so they say nothing about the flips on HDMI.

Locked timing with a black picture is VP0 putting out its background:
either the window is not composited, or it is and shows nothing but our
framebuffer, which is black until a flip lands on the screen (scanned out
or copied into it). The new build tells these apart:

| Setting / value | Says |
|---|---|
| `HdmiDiag` 1 | VP0's own colour bars (`VP0_COLOR_BAR_CTRL` = 0x00640101, the firmware's urgency bits plus enable) after the mode set. Bars on the monitor: VP0, the port mux and all of the HDMI side work, and it is the window or its pixels. Still black: the HDMI side (data path, AVMUTE) or VP0 itself. |
| `HdmiDiag` 2 | Eight colour bars and a white frame drawn into our framebuffer before the window points at it, and flips kept off it (no scan-out, no copy) for the whole HDMI session. Bars: the window, its address, stride, size and the IOMMU entries of our framebuffer are right, and the black came from the flips. Black: the window does not show our framebuffer. |
| `Mset_W_*` | Every VP0 and Esmart0 value written: DSP_CTRL, the four timing words, post scaler, PRE_SCAN, line flag, CLK_CTRL, IF_CTRL, VIR, YRGB_MST, ACT_INFO (and `Mset_W_ColorBar`). Expected at 2560x1440@60: DspCtrl 0xF, HTotalHsEnd 0x0AA00020, HActStEnd 0x00700A70, VTotalVsEnd 0x05C90005, VActStEnd 0x002605C6, PreScan 0x05130020, ClkCtrl 1, IfCtrl 0x80000033, Vir 0xA00, YrgbMst = `Hpd_ReservePa`, ActInfo 0x059F09FF. |
| `Chk1_*` (+3 s), `Chk2_*` (+10 s) | After each mode set or panel restore: Flips (SetVidPnSourceAddress calls), Scanned (window pointed at a primary), Copied (copied into our framebuffer), NotMapped, Invisible, Mismatch and Refused (a primary of another size, W << 16 \| H), Maps (primaries entered in the IOMMU), Showing (bit 0 window on a primary, 1 scan-out on, 2 IOMMU paging, 3 source visible, 4 path active, 5 on our framebuffer, 15:8 rotation), ScanAddr (last flip's address), Fb (W << 16 \| H of A->Fb), Mmu0/Mmu1 status and Fault address (the IOMMU registers MwScanCheck already reads). |
| `Chk1_VpRaw` | The one VOP2 read: VP0's interrupt raw status (the firmware reads it at [12b]), 3 s after the mode set, with POST_BUF_EMPTY cleared just before standby was released. Bit 4 set: VP0 asked for pixels and got none (the window's fetch fails: address, IOMMU, bandwidth). |
| `Hpd_IsSup` | Last changed IsSupportedVidPn answer: target \| reason << 8 \| supported << 16 \| paths << 24. Reasons: 1 more than one path or not source 0 (clone/extend refused), 2 no modes, 3 the pinned source size is not one of the target's (`Hpd_IsSupSize`), 4 pinned target mode not ours, 5 sizes differ. |
| `Hpd_Enum` | Last changed cofunctional enumeration: target \| source pinned << 8 \| target pinned << 9 \| it is ours << 10 \| modes << 16 \| pinned source width / 8 << 20. |
| `Hpd_Commits`, `Hpd_CommitAsk`, `Hpd_CommitRot` | Every commit, before any decision: target << 28 \| W << 14 \| H, and the path's rotation. |

Next session, in this order, each from a clean trace (`reg delete
"...\Device Parameters\maliwddm" /f`) and a cold boot with panel and
monitor attached: `HdmiHpd` 3 and `HdmiDiag` 1, then `HdmiDiag` 2, then
`HdmiDiag` 0. Report the screen and every `Mset_W_*`, `Chk1_*`, `Chk2_*`,
`Hpd_IsSup`, `Hpd_Enum` and `Hpd_Commit*` value. With `HdmiDiag` 0, try
`DisplaySwitch /internal` and Win+P "PC screen only" and report
`Hpd_IsSup`/`Hpd_Enum`/`Hpd_Commits` before and after.

On `DisplaySwitch /internal`: the panel child is reported as
`D3DKMDT_VOT_INTERNAL` with `HpdAwarenessAlwaysConnected` (hpd.c
MwHpdQueryChildRelations), which is what makes it the internal display.
That no commit was logged means Windows either never built a panel-only
VidPN or it was refused before the commit; `Hpd_IsSup` reason 3 with the
HDMI size in `Hpd_IsSupSize` would mean dxgkrnl kept the 2560x1440 source
pinned for the panel's path.

## Board run of 9b164db (2026-11-22) and what changed

Results (DSI boot, AOC on HDMI, level 3): `HdmiDiag` 1 showed VP0's colour
bars, so VP0, the port mux and the HDMI side work. `HdmiDiag` 2 showed the
test pattern in our framebuffer during the boot spinner, so the window,
its address, stride, size and IOMMU entries work, and went black at the
logon chime. `HdmiDiag` 0 showed the boot spinner squashed, then black from
the logon chime. Every flip after the mode set was "not mapped" and copied,
and the primary refused at map time was 2560x1440 itself.

What that says, and what the build does about it:

- **The 2560x1440 primary refused.** `MwScanMap` entered a primary only if
  its size was A->Fb's at the moment VidMm mapped it. dxgkrnl creates and
  maps the primary of the new mode before it calls CommitVidPn, when A->Fb
  is still the panel's 800x1280, so the primary was refused once and never
  entered, and every flip went to the copy. At level 3 any unpadded primary
  that fits a slot is now entered, and `MwScanWhy` decides per flip whether
  its size is the one VP0 shows (new reason 4, "another size", copied).
- **Black at DWM start with `HdmiDiag` 2.** `SetVidPnSourceVisibility(FALSE)`
  clears A->Fb, which is our framebuffer: that erased the pattern, and Hold
  kept anything from being drawn again. With Hold it no longer clears.
- **Black at DWM start with `HdmiDiag` 0.** The same clear, followed by
  `Visible(TRUE)`: the current primary was put back only if it was scanned
  out; one that was copied stayed black until the next flip with new
  content. At level 3 a copied primary is now copied again at the next
  vsync. Whether this was the whole of it is what the next run says: the
  first blanking event after a mode set is now stamped and logged.
- **The squashed spinner** is Windows' boot animation drawn into the new
  2560x1440 primary at the boot screen's own aspect (the panel's), stretched:
  the driver scales nothing (post scaler identity, `ACT_INFO` = `DSP_INFO`),
  and the copy is 1:1. It ends when DWM takes over. Not a driver defect as
  far as can be seen; noted here so it is not chased.
- **DisplaySwitch /internal never reached the driver.** `Hpd_IsSup`
  0x2000100 says the only other topology Windows ever asked about was clone
  (both targets on source 0), refused. A topology change that is decided
  in user mode and never asks the driver cannot be seen in the trace, so
  `tools/windows/ccdtest.exe` (in the `m3test-arm64` artifact) lists what
  CCD has (paths, `outputTechnology`, `targetAvailable`) and calls
  `SetDisplayConfig` with a topology, printing its Win32 error. The panel
  child is reported `D3DKMDT_VOT_INTERNAL`, `HpdAwarenessAlwaysConnected`.

New trace values (all level 3):

| Value | Meaning |
|---|---|
| `Per_*`, `Per_N` | Every 5 s for 2 min after the last mode set, then every 30 s: the snapshot below. |
| `Blk_Why`, `Blk_EventMs`, `Blk_AfterMs`, `Blk_*` | The first event after a mode set that can blank the screen: 1 visibility off, 2 window moved off a primary onto A->Fb, 3 a commit with no path; when (ms since boot, and after the mode set), and the snapshot at the next poll. |
| snapshot `_Flips` .. `_Maps` | Since the mode set: flips, scanned, copied, not mapped, invisible, refused at map time; `_Refused` the last refused size (any time). |
| `_Showing` | bit 0 window on a primary, 1 scan-out on, 2 paging, 3 visible, 4 path active, 5 our framebuffer, 6 Hold, 15:8 rotation. |
| `_LastFlip`, `_LastWhy` | The last flip's primary size, and why it was (0) or was not scanned out: 1 off, 2 not entered, 3 rotated, 4 another size, 9 invisible. |
| `_Vis` | Visibility on << 16 \| off, since start. |
| `_Calls` | IsSupportedVidPn, EnumVidPnCofuncModality, RecommendFunctionalVidPn, QueryChildStatus, QueryDeviceDescriptor calls since start, 6 bits each from the low end. Unchanged across a `ccdtest internal` means CCD never asked the driver. |
| `_Mmu0`, `_Mmu1`, `_Fault`, `_Ms` | IOMMU status, fault address, time. |

### The next one-shot test

`HdmiHpd` 3, `HdmiDiag` 0, trace cleared, cold boot with panel and monitor:

1. Watch HDMI from power-on to two minutes after logon; note the time the
   chime sounds and whether the desktop appears, and photograph it.
2. In the user's session: `ccdtest.exe > c:\mali\ccd0.txt`, then
   `ccdtest.exe validate internal > c:\mali\ccd1.txt`, then
   `ccdtest.exe internal > c:\mali\ccd2.txt`. Note what the screens do.
3. Unplug HDMI, wait 15 s, note the panel; plug it back, wait 15 s.
4. Dump the trace (all values) and the three text files.

## Board run of 8bf90b7 (2026-11-22): what the numbers say

The 2560x1440 primary is now scanned out (Scanned 2, Refused 0), and the
picture is still black; the panel restore on unplug works and lights the
panel at once.

- **LastWhy 9** is "the flip came while the source was invisible".
  `Showing` 0x36 has bit 3 (visible) clear, in both runs.
- **Vis churn.** `Vis` packs on << 16 | off: 0x10a52 is one visibility-on
  and 2642 visibility-off calls; 0x52abb is 5 on and about 10,000 off, some
  50 a second. dxgkrnl keeps turning the source off and almost never on.
  Flips are only made to a visible source, hence one flip in 88 s, and
  `Maps` in the thousands is DWM recreating its primaries each time.
- **What dxgkrnl keeps asking for.** In the packed `Calls` value of that
  build, IsSupportedVidPn ran some 30 times during the black period
  (0x..63 -> 0x..7f, 6-bit field) and `Hpd_IsSup` never changed from
  0x2000100: target 0, reason 1, two paths, refused. `Hpd_IsSup` is written
  only on a change, so every one of those calls was the same two-path
  VidPN, refused for having two paths. The four calls during Win+P "PC
  screen only" were the same.
- **The likely cause.** With two connected targets on one VidPN source,
  the session's display configuration asks for a two-path topology
  (Windows' default for a second display is Extend, and clone is the other
  two-path one), the driver refuses it, and the retry loop toggles the
  source invisible and rebuilds DWM's swap chain, forever. On the panel
  alone (after unplug) there is one display, nothing two-path to ask for,
  and it works. Unproven: the next build counts it outright.
- **Vsync** cannot be judged from that build; it is now counted.

New in this build: snapshots carry separate full counters instead of the
packed `Calls`: `_IsSup`, `_Enum`, `_RecFunc`, `_Status`, `_Desc`,
`_Sup0`..`_Sup5` (IsSupportedVidPn answers by reason), `_Vsyncs` and
`_VsyncTgt` (vsync interrupts sent to dxgkrnl since the mode set, and the
target they name), `_IntOn`/`_IntOff` (vsync interrupt switches),
`_Presents`, `_PresentFlips`, `_Creates`, `_Destroys`, `_VisOff`,
`_VisOn`; `Showing` bit 7 is "vsync interrupt on". `Per_Sup2Path` is the
last two-path VidPN refused: (source << 8 | target) of the first path in
bits 31:16, of the second in 15:0. Source 0 on both is clone; sources 0
and 1 is extend. `Blk_VisOff1Ms`..`4Ms` are the first four visibility-off
times after the mode set.

And an experiment, off by default: `HdmiExclusive` 1 (DSI boot, level 3).
The panel child is reported hot-plug aware, and disconnected while a
monitor is on HDMI. Windows then never has two displays, so it has nothing
two-path to ask for. Plugging HDMI tells Windows "HDMI connected" then
"panel gone"; unplugging, "panel back" then "HDMI gone". `Hpd_IndicatePanel`
is the status of the panel's indication. The cost: the panel cannot be
used while HDMI is plugged in, as on a laptop docked with the lid shut.
A real extended desktop needs a second VidPN source and the panel on VP2,
as Linux runs it.

### The next one-shot test

Two cold boots, trace cleared before each, panel and monitor attached,
`HdmiHpd` 3, `HdmiDiag` 0:

1. `HdmiExclusive` 0. Wait 90 s after the logon chime. Dump the trace.
   Expected if the explanation holds: `Per_Sup1` climbing, `Per_Sup2Path`
   naming the two-path VidPN, `Per_VisOff` climbing, `Per_Vsyncs` climbing
   at about 60 a second with `Per_VsyncTgt` 1. If `Per_Vsyncs` stays near 0
   instead, it is the vsync.
2. `HdmiExclusive` 1. Expected: the desktop on HDMI, `Per_Flips` climbing,
   `Per_VisOff` small. Then unplug HDMI (the panel should come back) and
   replug (HDMI should take over again). Dump the trace.

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
- A swap of monitors faster than the 1 s debounce is not seen as an unplug.
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
