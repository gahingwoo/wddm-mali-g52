# Performance: the present path, the GPU clock and TDRs

How a frame gets to the screen, how the GPU clock is chosen and guarded,
what a TDR leaves behind, and the tools that measure all of it. Settings:
see [SETTINGS.md](SETTINGS.md). Measurement history is in
[history.md](history.md).

## Reference numbers

TaishanPi 3M, Windows 11 23H2. Use these to tell whether a build got
slower.

| Load | Result |
|---|---|
| GpuTest 0.7.0 FurMark, OpenGL ICD, 1024x640 windowed, 60 s, 594 MHz | 288 to 290 points, GPU 87.9% busy, every chain collected by the interrupt |
| The same at 702 MHz (SPLL / 1) | 334 to 336 points, GPU about 87% busy |
| The same at 198 MHz (the firmware's clock) | 106 points |
| Window drag, DWM | GPU 12 to 15% busy; bound by the CPU's submit path. DWM submit CPU per job 0.20 to 0.23 ms with `UmdMergeSubmit` on, 0.29 to 0.33 ms off |
| Scan-out on (default) | every flip scanned out, none copied; up to 268 presents in 5 s while dragging |

The desktop's numbers move by tens of percent between runs of the same
setting. Take three runs per configuration at least.

## How a present works

- The buffer Panfrost renders into is the primary itself: it is allocated
  through the runtime like a shared surface, and present hands it over
  without a pixel copy.
- `RotateResourceIdentities` exchanges buffers and re-points their views;
  it does not copy contents.
- Primaries are linear (no AFBC, no tiling). The memory segment is
  `CacheCoherent = FALSE`, so user-mode views are write-combined and CPU
  reads of them are slow.
- The vsync is a 16 ms timer, not the VOP2's interrupt. A flip is reported
  at the next tick.
- One job chain runs at a time on the whole machine. Each render lists
  every live buffer of the device (up to 256), and the KMD patches each.
- A window's own present (not DWM's) is a read-back present: the UMD maps
  the back buffer (a GPU wait and an uncached read) and maliwddm copies it
  on the CPU into DWM's redirection surface. `UmdReadbackOneCopy` removes
  one of those passes. The rest would need DXGI 1.2 flip model or a GPU
  copy with the blt behind the GPU queue; neither is written.
- `CopySubresourceRegion` and `CopyResource` between textures run on the
  GPU (blitter, mesa-patches/0054). Buffers, copies within one level of a
  texture, MSAA, compressed formats and incompatible format pairs still
  go to the CPU.
- `ResolveSubresource` and DXGI's `pfnBlt` do nothing; both are counted
  in `m5test perf`, so a caller that relies on either shows up there.

Per-process switches read once per process; each one restores old
behaviour when the file exists: `C:\mali\primary-copy` (copy the frame
into a separate primary), `C:\mali\rotate-copy` (rotate by copying,
implies `primary-copy`), `C:\mali\present-wait` (wait for the GPU before
presenting a rendered-into primary). None is needed today.

## Stage 2: the VOP2 scans out the primary

`drivers/maliwddm/scanout.c`. On from the start unless `ScanOut` is 0.

- Each primary gets a fixed range of IO addresses. When VidMm maps it,
  its pages go into the VOP2 IOMMU's tables from the same page list the
  Mali's MMU gets.
- The firmware framebuffer is entered at its own physical address, so the
  picture survives translation being switched on, and the bugcheck
  screen, a blt present and a stop have something to return to.
- A flip is two writes: `ESMART0 YRGB_MST` (`0x1814`), then
  `REG_CFG_DONE` (`0x000`, value `0x00018001` for VP0). The VOP2's own
  registers are written and never read (reads have cost the HDMI signal
  on this SoC). The IOMMU's registers (`0x27D07E00`, `0x27D07F00`) are
  read.
- An IOMMU fault while a primary is shown turns scan-out off and puts the
  window back on the framebuffer (`MwScanCheck`, every 8th vsync).
- Leaving D0 turns translation off first, so a restart or the bugcheck
  screen finds the VOP2 reading physical addresses.
- While the source is invisible (display off) the window shows the
  cleared framebuffer.
- Rotation: the source mode keeps the panel's size at every rotation and
  DWM draws the turned desktop into it, so scan-out works rotated. The
  RK3576 VOP2 cannot turn a picture by 90 or 270 degrees (Esmart windows
  offer `REFLECT_Y` only, Cluster windows `REFLECT_X | REFLECT_Y`). A
  primary of the turned size is copied and turned on the CPU and counted
  in `T_CopyWhyRotated` and `T_RotFlipCopies`.

`m5test scan` reports; `m5test scan 1` turns it on, `m5test scan 0` goes
back to the copy, `m5test scan 2` also turns translation off. In a
healthy state both IOMMU instances report paging enabled (status `0x19`).
`m5test perf` shows per flip which way it went and why (`T_PresentPath`,
`T_FlipsScanned`, `T_CopyWhy*`). `T_FlipCopyUsAvg` near 25000 means every
flip is being copied, which is the symptom of scan-out being off.

## DWM's copies

DWM keeps its rotating back buffers whole by copying the regions that
changed in the previous frame from the last presented buffer into the one
being finished: two `ResourceCopyRegion` calls right before `_Present`,
about 0.4 MB each, several a frame while a window is dragged (about 290 a
second on HDMI 2560x1440). explorer.exe does the same.

Without batching each copy flushes every open batch, draws, and flushes
again: two renders, about 0.9 to 1.2 ms of CPU per copy. With
`UmdCopyBatch` on (default, mesa-patches/0075, `MW_UMD_FLAG_COPY_BATCH`,
0x8 in `m5test perf`'s switches) neither flush happens: the blitter's draw
is recorded like any other draw and Panfrost's resource tracking orders
it. A copy into the render target DWM has bound joins the batch already
drawing it. A copy between two levels of one texture keeps both flushes.
It applies to every process with the UMD, the OpenGL ICD
(`glCopyImageSubData`) included. For one process,
`D3D10UMD_COPYBATCH=0` or `1` in its environment overrides the KMD.

The failure to look for is a stale region: a trail behind a dragged
window, or a rectangle of an older frame. `copytest.exe` (tools/windows,
artifact `board-tests-arm64`) checks the ordering cases:

    $env:D3D10UMD_COPYBATCH = '0'; C:\mali\copytest.exe hw > C:\mali\copytest-0.txt
    $env:D3D10UMD_COPYBATCH = '1'; C:\mali\copytest.exe hw > C:\mali\copytest-1.txt
    C:\mali\copytest.exe hw bench 600 | Select-String bench

Both runs must end with `done: 0 check(s) failed`. With `C:\mali\log`
present when a process makes its first copy, its log gets a `copies:`
line every 5 s (count, time, sizes, how many hit the bound render target
or a shared resource) and the first 16 copies in full.

## The GPU clock

The firmware leaves the GPU at GPLL / 6 = 198 MHz. The driver sets it
through `CRU_CLKSEL_CON165`: [4:0] divider minus one; [7:5] parent, 0
GPLL 1188 MHz, 1 CPLL 1000, 2 AUPLL 786.432 (audio's), 3 SPLL 702, 4 LPLL
(the A53 cluster's); [8] 1 selects the PVTPLL; [9] the PVTPLL's source.
The driver uses GPLL, CPLL and SPLL only, never AUPLL, LPLL or the PVTPLL.
Between 600 and 950 MHz a divider reaches only SPLL / 1 = 702 MHz.

The supply `vdd_gpu_s0` is RK806 BUCK5 on the TaishanPi 3M, the CM5 and
the ROCK 4D (550 to 900 mV range, 500 mV + 6.25 mV per selector step:
750 mV is 0x28, 775 mV 0x2C). Its power-on value is 750 mV. edk2-rk3576
raises it to 775 mV on the TaishanPi 3M only, in 6.25 mV steps each read
back, only with TSHUT armed, and publishes the reading as the volatile
UEFI variable `GpuRail` (GUID 7701c75f-f454-4aab-9445-117f09fb87cf,
32 bytes); its serial log has `GpuRail:` lines with the rail, the OTP
bytes and which rates the rail meets. The CM5-IO and ROCK 4D leave the
rail at 750 mV.

Operating points (MHz: mainline mV / vendor slowest level mV): 300 to 600:
700 / 712.5; 700: 725 / 750; 800: 775 / 812.5; 900: 825 / 875; 950: 850 /
none. 702 MHz is above the 700 point, so the 800 point governs it: 775 mV
by mainline, 812.5 mV by the vendor's slowest level.

`GpuClkHigh` decides whether anything above 600 MHz is allowed: 0 never,
1 when the supply meets mainline's point, 2 when it meets the vendor's
slowest level (stricter). `GpuRailUv` can supply a voltage verified some
other way; with the firmware's reading as well, the lower counts.

`GpuClkWrite` (`drivers/maliwddm/adapter.c`) allows a rate above 600 MHz
only when all of these hold: the setting is on; a supply is known and
inside 550 to 900 mV; the firmware did not report a J, M or S variant;
the rate is at most 950 MHz from GPLL, CPLL or SPLL; the required voltage
(plus the OTP `gpu-opp-info` offset in its range) is at most the supply;
CON165 is not on the PVTPLL path; the temperature guard runs. A refusal
is logged as `GpuClkRefusedHz`, `GpuClkNeedsUv` or `GpuClkRefusedNoTemp`.

Every clock change (the start clock, `m5test clk`, the thermal drop and
restore, a fault) goes through `ClkRequest` at the interrupt's level and
is written only between job chains: at once when nothing runs, else by
`Kick` after the running chain ends. The mux and the divider are written
one field at a time, in the order whose halfway rate is the slower
(`MwClkSteps` in `gpuclk_calc.h`; 702 to 594 goes divider first through
SPLL / 2). The adapter stopping drops the clock to 594 MHz, so a later
start never inherits 702 MHz.

## 702 MHz by default

With `GpuClkHigh` not set, the driver takes 1 when the firmware's
`GpuRail` report is valid and at least 775 mV on a plain RK3576, else 0.
Only the firmware's reading turns the default on; `GpuRailUv` can only
lower the rail the checks use.

The start clock: a stored `GpuClk` (`m5test clk <parent> <div> keep`)
wins. With none stored the driver asks for SPLL / 1 = 702 MHz when
`GpuClkHigh` is on and the checks allow it, else GPLL / 2 = 594 MHz. In
practice the TaishanPi 3M starts at 702 MHz and the CM5-IO and ROCK 4D at
594 MHz. `m5test clk forget` returns to the default from the next start.

`m5test clk` prints "not set: the default" or "set in Device Parameters"
after `GpuClkHigh`; the registry trace has `GpuClkHighDefault` (1 default,
0 set) and `GpuClkStartDefault` (0x301) when the default clock was used.
The decisions are `MwClkHighDefault` and `MwClkStartWant` in
`gpuclk_calc.h`, checked by `tests/gpuclk-host`.

## Temperature guard and fault policy

With `GpuClkHigh` on, the driver reads TSADC channels 3 and 5 (the TRM
calls 3 the GPU; the OTP trim and the vendor's zone point at 5) once a
second and takes the hotter one. No OTP trim is applied, so readings are
a few degrees off.

- At or above 85 C the clock drops to 594 MHz at once.
- It returns to the requested clock after every reading has stayed below
  75 C for `GpuClkRestoreHoldSec` (default 90 s). One reading at 75 C or
  above, or an unreadable one, restarts the hold. Three unreadable
  readings in a row count as hot.
- A job fault, job timeout or TDR above 600 MHz drops to 594 MHz; the
  clock comes back at the next reading unless heat holds it. Two such
  faults within 10 minutes lock 594 MHz until the next start. A fault at
  594 MHz does not count.
- The TSADC is used only when the CRU shows it running (gates
  `CLKGATE_CON(13)` bits 8 and 9 open, resets `SOFTRST_CON(13)` bits 8
  and 9 released, `TSADC_AUTO_CON` bit 0 set). Firmware that arms TSHUT
  starts it. Without it there is no guard and nothing above 600 MHz.

`Throttled reason` in the performance counters and `m5test clk`: 0 none,
1 thermal, 2 faults. The registry trace (`Device Parameters\maliwddm`)
gets `GpuClkThrottleReason`, `GpuClkFaultLocked`, `GpuClkHighFaults`,
`GpuClkThermalDrops`, `GpuClkThermalRestores`, `GpuClkSwitches`,
`GpuClkDeferred` and, at start, `GpuClkRestoreHoldMs`. The decisions are
`thermal_calc.h`, checked by `tests/thermal-host`.

Thermal notes from the board: with the fan off the TaishanPi 3M idles at
67 to 68 C at 594 MHz. Fan off with FurMark at 702 MHz plus Prime95 froze
the board once at 82 to 84 C on the little cores (GPU 78.5 C), below the
85 C guard. With the fan on FurMark peaks near 50 to 55 C.

## TDRs with no job fault

A TDR (System event 4101, "display driver stopped responding and has
recovered") with jobs failed 0, timed out 0 and no MMU fault is a reset
by dxgkrnl, not by the driver: a packet or a preemption did not complete
within `TdrDelay` (2 s). The driver's own chain timeout is also 2 s, so a
really hung chain looks the same.

The known cause: dxgkrnl forces a preemption even with the INF's
`EnablePreemption` 0. That registry value only turns off priority
preemption. When a context switch deadline passes while the node still
has a packet running, the TDR watchdog (`VidSchiCheckHwProgress`) sends
`DxgkDdiPreemptCommand` anyway, and then times out the node when that
packet goes unanswered for `TdrDelay`. A long FurMark frame plus DWM
wanting the GPU triggers it, at WDDM 1.3 and 2.0 alike.

The fix: with `TimeSlice` 0, `MwPreemptCommand` answers. No chain is
soft-stopped; `Kick` answers once nothing is running or queued, so
DMA_PREEMPTED carries the last completed fence and nothing is handed back
for resubmission (preemption at the DMA buffer boundary, which matches
dxdiag's "Graphics Preemption: DMA"). With `TimeSlice` set, soft-stop and
resume as before. `PreemptAnswer` 0 restores the old behaviour (requests
at `TimeSlice` 0 dropped) for an A/B run. The copy node answers its own
requests.

### The TDR snapshot

At every `ResetEngine` and `ResetFromTimeout`, before anything is reset,
`TdrSnap` (command.c) records the state; a work item writes it to
`Device Parameters\maliwddm` as `Tdr<n>_<field>` (n = 0 to 3, the last
four) and `TdrCount`.

| Field | Meaning |
|---|---|
| `Node` | 0 3D, 1 the copy node, 0xFFFFFFFF adapter-wide |
| `Running`, `Slot`, `RunMs` | a chain on the GPU, its slot, for how many ms |
| `JsStatus`, `JsHeadLo`, `JsTailLo`, `JobRaw`, `JobPending` | JS_STATUS, JS_HEAD/JS_TAIL, JOB_INT_RAWSTAT, and what the ISR collected but nothing reaped |
| `QCount`, `HeadFence`, `HeadNext`, `HeadCount`, `HeadStarted`, `HeadAgeMs` | the driver's queue and its head submission |
| `LastSubmitted`, `LastCompleted` | the fences |
| `SinceDoneMs`, `SinceChainEndMs`, `SinceIsrMs`, `SincePollMs`, `SinceSubmitMs` | ms since the last DMA_COMPLETED, chain end, GPU interrupt, poll and submission |
| `PreemptPending`, `PreemptMs`, `StopAsked`, `StopMs` | an unanswered preemption or soft-stop, and its age |
| `GpuClk`, `MilliC`, `JobsDone`, `ClkPending` | clock (mux * 256 + divider), temperature, jobs done, a clock change waiting |
| `PreemptCalls`, `PreemptAnswered` | node-0 PreemptCommand calls, and node-0 DMA_PREEMPTED sent |
| `SincePreemptMs`, `LastPreemptFence` | the last node-0 request: age and fence |
| `CopyPreempts` | node-1 requests |
| `Node0Submits`, `Node0Notifies` | node-0 submissions queued, and DMA_COMPLETED sent |
| `S<k>Fence`, `S<k>Info`, `S<k>AgeMs`, `S<k>DoneMs` | the last eight node-0 submissions (k = 0 newest): fence; chains in bits 0-7, 0x100 present, 0x200 paging, 0x400 no private data, present kind in bits 16-23; ms since submitted; ms since notified (0xFFFFFFFF not notified) |

How to read it:

- `Running` 1, `RunMs` about 2000, `JsStatus` 0x08, slot bit clear in
  `JobRaw`: the GPU really ran one chain for 2 s (a hang, or a very long
  chain).
- `Running` 1 but `JsStatus` 0x01 or the slot's bit set in `JobRaw` or
  `JobPending`: a missed completion; `SinceIsrMs` and `SincePollMs` say
  whether the interrupt and the poll were running.
- `Running` 0 and `QCount` above 0: the queue stalled.
- `Running` 0, `QCount` 0, `LastCompleted` = `LastSubmitted`, and
  `SinceSubmitMs` above 2000: dxgkrnl stopped feeding the node, which is
  what an unanswered preemption looks like. Check `PreemptCalls` against
  `PreemptAnswered` and `S0DoneMs` < `SincePreemptMs` < `S0AgeMs`.
- `PreemptPending` 1 with `PreemptMs` near 2000: a preemption not
  answered.

Open question: after a reset the driver drops its whole queue but reports
`LastAbortedFenceId` = `LastCompleted`; Microsoft describes it as the
fence of the packet aborted mid-execution.

"% busy" counts the time chains ran and grows only when a chain ends; a
reset adds nothing. A low "% busy" after a TDR therefore means chains
really ran that little.

## Tools

All from `m5test.exe` (artifact `m3test-arm64`) unless noted. None of
them submits GPU work.

| Command | What it shows |
|---|---|
| `m5test` | the build |
| `m5test perf [s]` | flips, copies against scan-outs and why, flip to shown, vsync gaps, jobs and their time, preemption, blt presents, the switches, and per process the UMD's copies, uploads, maps, read-back presents, waits for idle and `submit (pfnRenderCb)` (renders a second, BOs in each, CPU in the runtime's render call) |
| `m5test prof [s]` | default 5 s: GPU clock from CON165; TSADC channels 0 to 5; CPU time in each driver DDI, the ISR and the DPC; allocation-list entries per render; GPU pages mapped a second; GPU busy by slot with a histogram of chain times; submission to completion; bubbles (GPU idle between a chain's end and the next queued one's start); collected by interrupt or poll timer; completion to DPC; queue depth at submission; the preemption line |
| `m5test ring [csv]` | the last 128 chains: fence, slot, flags (0x400 found by the poll timer, 0x800 not its submission's first chain), status, submit, start and end in us, queued and run time |
| `m5test clk` | `GpuClkHigh` and where it came from, the supply and its source, the highest point allowed, what 702 MHz needs, the guard (on, hotter reading, held, held/released counts), switches and how many waited for a chain |
| `m5test clk <parent> <div> [keep]` | set the clock (parent 0 GPLL, 1 CPLL, 3 SPLL); `keep` stores it as `GpuClk`; `m5test clk 0 2 keep` is 594 MHz, `m5test clk 3 1` is 702 MHz; `m5test clk forget` deletes the stored value |
| `m5test mem` | GPU memory maliwddm allocated, total and per process, resident share, next to Windows' working set and private bytes |
| `m5test scan [n]` | scan-out state; n is 0, 1 or 2 as in "Stage 2" |
| `m5test temp [s]` | the performance data dxgkrnl gets ([PERFDATA.md](PERFDATA.md)) |

What to read first in `m5test prof`: `collected by` (if the poll timer
collects most chains, interrupts are not arriving); `GPU busy` (above 80%
the GPU clock is the limit, below 20% the CPU side is); `renders` and the
`Render`, `Patch` and `SubmitCommand` lines; `submit` per process;
`bubbles` against the chain count.

The UMD writes `perf:` lines (microseconds per stage per present, every
5 s, including `bo-wait`) to its log when `C:\mali\log` exists.

### The slow-operation log

For hitches. `mkdir C:\mali\slow`, optionally `echo 20 > C:\mali\slow-ms`
(threshold, default 20 ms). Each process with the UMD writes
`C:\mali\slow\<exe>-<pid>.txt`; a running dwm.exe notices the directory
within 10 s. A file named `off` in it, or removing it, stops the log. For
AppContainer processes (Start menu, Search):
`icacls C:\mali\slow /grant "ALL APPLICATION PACKAGES:(OI)(CI)M"`.

A line is GetTickCount, thread, operation, duration and a detail:

    1234567  4321 shader-compile     31.402 ms fragment, 1184 bytes, binary hash 0x1c2d3e4f, key hash 0x9a8b7c6d, variant 1, shader 000001F2A3B4C5D0

Operations nest (a `present` line follows the `flush-all`, `submit` and
`gpu-idle` lines inside it). Frontend: `dxbc-to-tgsi`, `create-resource`,
`destroy-resource`, `map`, `unmap`, `copy-resource`, `copy-region`,
`update-subres`, `present`, `rotate`. Panfrost: `tgsi-to-nir`,
`shader-create`, `shader-compile` (variant 0 is the default),
`transfer-map`, `transfer-unmap`, `pan-bo-create`, `bo-wait` (`timeout 0`
is a busy check), `bo-cache-evict`, `batch-submit`, `flush-all`,
`flush-writer`, `flush-users`. Shim: `bo-create`, `bo-map`, `bo-unmap`,
`submit`, `gpu-idle`, `wait-bo`, `syncobj-wait`, `destroy-closed`,
`shim-lock`, `present-cb`, `shared-destroy`, `primary-destroy`.

`C:\mali\wait-timeout` (read once per process, off by default, not yet
board-tested) makes the shim keep the timeout of WAIT_BO and syncobj
waits instead of waiting for the GPU to go idle. Without it each of
Panfrost's zero-timeout busy checks waits for everything the device
submitted; `wait-bo ... timeout 0 ns (not kept)` lines in the slow log
show what that costs.

## How to collect evidence

Installing a test package: stage it with
`pnputil /add-driver C:\mali\pkg\maliwddm.inf` (never install a display
driver over the running one), set `ConfigFlags |= 0x20` on
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0` as SYSTEM, wait until
the value has 0x20, then do a full restart. Each CI run has a new test
certificate; on a board without an RTC battery, `Set-Date` first.

A benchmark run: start the load, wait 10 s, then

    m5test prof 10 > prof-<name>-<n>.txt
    m5test perf 10 > perf-<name>-<n>.txt
    m5test ring csv > ring-<name>-<n>.csv

plus the load's own score. Loads: idle desktop 30 s; dragging a window
10 s; `present.exe hw` and `triangle.exe hw` (tests); `d2dtext.exe` and
`d3dparts.exe` (tools/windows); FurMark as above; Edge WebGL Aquarium
(500 fish, check edge://gpu for the Mali); Edge YouTube 720p 30 s
(dropped frames from Stats for nerds). For dwm.exe's CPU time, use an
elevated PowerShell and keep Task Manager closed (it costs about 12 s of
CPU in 15 s):

    $s = { (Get-Process dwm | % { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum }
    $a = & $s; C:\mali\m5test.exe perf 15 > C:\mali\perf-1.txt; $b = & $s; "dwm CPU $([math]::Round($b - $a, 2)) s"

Measure with `C:\mali\log` absent; the entry-point log costs CPU.

After a TDR:

1. At once: `m5test ring csv > ring-<n>.csv` and `m5test prof 5`.
2. `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" > devkey.txt`
   (the `Tdr*` values), `m5test clk`, and
   `Get-WinEvent -FilterHashtable @{LogName='System';Id=4101} | Format-List *`.
3. Copy `C:\Windows\LiveKernelReports\WATCHDOG\` (0x117
   VIDEO_TDR_TIMEOUT_DETECTED dumps). `!analyze -v` and
   `dt dxgkrnl!_TDR_RECOVERY_CONTEXT` give the node, the reason, the
   fence dxgkrnl waited for and what it resubmitted.
4. Best evidence: `wpr -start tools\windows\dxgkrnl.wprp -filemode`, then
   `wpr -stop tdr.etl` after the 4101. Per-node submit and complete times
   show which packet was outstanding.

## Known issues and not done

- Vsync is a 16 ms timer, not the VOP2's interrupt; tearing has not been
  looked for with a test pattern. A VOP2 interrupt in the DSDT was
  followed by WHEA errors in October 2026.
- Nothing in Windows keeps the VOP2's clocks and power domain on.
  Display off and on, sleep and the bugcheck screen are not tested with
  scan-out on.
- Every render lists every live BO (about 250) and the KMD patches each;
  about 420 us per render. Listing only the named BOs would cut it but
  risks eviction faults under memory pressure.
- `UpdateSubresource` on a texture a pending batch reads takes Panfrost's
  shadow path (a wait, a new BO, a CPU copy): 112 us each at 1.3, 240 us
  at 2.0.
- Ideas not started: Mali performance counters (PRFCNT), a shader disk
  cache on Windows (the 40 to 50 ms compiles at process start), CPU-cached
  staging buffers with explicit cache maintenance, skipping a redundant
  start-of-chain cache flush.
- Not done on purpose: running a vertex/tiler chain beside a fragment
  chain (Panfrost shares one tiler heap between batches); the PVTPLL path
  (this TF-A ignores the OTP correction, and Windows has no SCMI client);
  raising `vdd_gpu_s0` above 775 mV for 800 to 950 MHz (can damage the
  chip; if ever done: never above 850 mV nor 950 MHz, 6.25 mV steps read
  back, TSHUT armed, the PMIC default back on any job fault).
