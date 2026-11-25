# Where a frame's time goes, and the plan to get it back

The cost model below was read from the code on 2026-11-13 and measured on
the TaishanPi 3M on 2026-11-14 (2560x1440, one frame is 14.7 MB). Line
numbers are those of `0a31314` and Mesa with patches up to 0040.

## Measured, 2026-11-14

`dwm.exe` with a window being dragged, from the UMD's `perf:` lines and the
kernel driver's counters. Each row adds one change to the row above.

| Path | Per present in the UMD | Presents in 5 s | Emulated vsync |
|---|---|---|---|
| as on 2026-11-13 (`primary-copy` and `rotate-copy` set) | readback 28 ms, copy to primary 2.7 ms, rotate 76 ms | 41 to 45 | not sampled |
| rotation by exchange | readback 30 ms, copy 3.3 ms, rotate 0.025 ms | 35 to 46 | not sampled |
| primaries rendered into | 0, 0, 0.025 ms | 46 to 55 | 30 to 36 a second, against 62 with the screen still |
| the flip copy on a thread | the same | 63 to 92 | 16.0 ms apart on average |
| scan-out by the VOP2 (`m5test scan 1`) | the same | 135 to 268 | 16.0 ms apart; no copy |

What the rows say:

- The rotate was the largest cost in the UMD, as read from the code, and
  exchange removes it. Removing 70 ms from each present did not change the
  rate: something else set it.
- That was the copy to the framebuffer in the vsync DPC. With a window
  moving the timer lost half its ticks. On a thread the copy measures
  27 ms on average (`T_FlipCopyUsAvg`), a flip reaches the screen 41 ms
  after it was made, and 1282 of the vsyncs in the test found the previous
  copy still running (`T_CopyDeferred`).
- 8139 jobs in that session, none failed. The picture was right through
  all four, by eye.

So the rate was set by a 27 ms copy that spans two vsyncs, plus the wait
for the GPU (a few to 25 ms a present). Stage 2 below removes the copy,
and was switched on the same day: 365 flips in 10.02 s with none copied
(`m5test perf 10`), up to 268 presents in 5 s, 11707 jobs and none failed,
the GPU busy 5% of the time. By eye the desktop keeps up with a drag about
as well as the CPU-rendered desktop does
([photo](results/tspi3m-2026-11-14/desktop-scanned-out.jpg)).

The first row is the state the 1 to 2 frames a second of 2026-11-13 was
seen in; 41 to 45 presents in 5 s is 8 to 9 a second while dragging a
window on an otherwise idle desktop. The lower figure was with the sign-in
screen and the full file log.

## One present today

| # | Where | What |
|---|---|---|
| 1 | `DxgiFns.cpp:71` | `pipe->flush`: submits the batches, does not wait |
| 2 | `d3d10_gdi.c:124` | maps Panfrost's buffer for reading, which waits for the GPU |
| 3 | `d3d10_gdi.c:131-141` | allocates 14.7 MB and copies the frame into it one pixel at a time |
| 4 | `malikm_drm.c:1308-1314` | locks the primary and copies the frame into it |
| 5 | `malikm_drm.c:1319` | `pfnPresentCb`; `MwSetVidPnSourceAddress` records the flip |
| 6 | `command.c:470-491` | in the vsync DPC, copies the whole primary to the firmware framebuffer row by row |
| 7 | `DxgiFns.cpp:249-289` | `RotateResourceIdentities`: a new temporary resource and N+1 `resource_copy_region` calls |

Step 7 is the largest. For Panfrost `resource_copy_region` is a CPU copy
between mapped buffers, and each map waits for the GPU. `dwm.exe` has three
primaries, so a rotate is four full-frame copies. With steps 3, 4 and 6
that is seven full-frame CPU passes per present.

Other facts read from the code:

- Primaries are already linear; no AFBC or tiling is involved.
- The firmware framebuffer is mapped write-combined. The memory segment is
  declared `CacheCoherent = FALSE`, so the user-mode views of buffers are
  probably write-combined too, which would make steps 3 and 7 read uncached
  memory. Not verified.
- The emulated vsync is a 16 ms timer. A flip completes at the tick after
  its copy, and the copy runs at DISPATCH in the same DPC.
- One job chain runs at a time for the whole machine. A logged session had
  56243 jobs for 16512 flips, about 3.4 per flip.
- Every submission lists every live buffer of the device, up to 256.

## Stage 1, done

- Timers: the UMD logs microseconds per stage per present every 5 s
  (`perf:` lines); the kernel driver keeps `T_FlipCopyUs*`, `T_FlipWaitUs*`,
  `T_JobUs*`, `T_VsyncGapUs*`; `m5test perf [seconds]` prints rates.
- The buffer Panfrost renders into is the primary: it is allocated through
  the runtime the way a shared surface is, and present hands it over with
  no pixel copy (0047).
- `RotateResourceIdentities` exchanges buffers and re-points the views on
  them instead of copying contents (0047).
- The copy to the framebuffer runs on a thread on the Cortex-A72 cores; a
  vsync reports a flip only once its pixels are shown.

Switches, read once per process, each restoring the old behaviour when the
file exists:

| File | Effect |
|---|---|
| `C:\mali\primary-copy` | copy the frame into a separate primary |
| `C:\mali\rotate-copy` | rotate by copying; implies `primary-copy` |
| `C:\mali\present-wait` | wait for the GPU before presenting a rendered-into primary |

None of the three was needed. No half-drawn frame was seen without
`present-wait`.

## Stage 2: the VOP2 scans out the primary

Written as a design on 2026-11-13 around a contiguous pool for primaries;
done on 2026-11-14 with the VOP2's IOMMU instead, which leaves the memory
model alone (`drivers/maliwddm/scanout.c`).

- A primary gets a fixed range of IO addresses. When VidMm maps it, its
  pages are entered in the VOP2 IOMMU's tables from the same page list the
  Mali's MMU is given.
- The firmware framebuffer is entered at its own physical address, so the
  picture survives translation being switched on, and the bugcheck screen,
  a blt present and a stop have something to return to.
- A flip is two writes: `ESMART0 YRGB_MST` (`0x1814`), then `REG_CFG_DONE`
  (`0x000`, value `0x00018001` for VP0). The VOP2's own registers are
  written and never read; on this SoC reading them has cost the HDMI signal
  before. The IOMMU's registers (`0x27D07E00`, `0x27D07F00`) are read.
- It was off after every boot until 2026-11-21. Since then it is on from
  the start unless the `ScanOut` setting is 0 (`drivers/maliwddm/INSTALL.txt`
  has the commands). `m5test scan 1` turns it on, `m5test scan 0` goes back
  to the copy, `m5test scan 2` also turns translation off, and `m5test scan`
  reports.

Measured when it was first turned on: both IOMMU instances report paging
enabled (status `0x19`), four primaries had been entered, none refused for
size, and no flip went back to the framebuffer.

Not done:

- The vsync is still the 16 ms timer, not the VOP2's. A flip is reported
  at the next tick, which is not the frame the VOP2 latched it on. No
  tearing was seen; none was looked for with a test pattern.
- It does not survive a reboot by itself, on purpose, until it has run for
  longer than one session.
- Nothing in Windows keeps the VOP2's clocks and power domain on.
- Display off and on, a mode change, sleep, and the bugcheck screen have
  not been tried with it on.
- Only the TaishanPi 3M at 2560x1440, the firmware's mode.

## The panel and rotation, 2026-11-21

On the TaishanPi 3M's 800x1280 DSI panel, turned to landscape, the stats
escape read `T_FlipCopyUsAvg` 24881: every flip was copied, about 25 ms a
frame, and YouTube in Edge stuttered. Scan-out was simply not on (it
needed `m5test scan 5`, and `INSTALL.txt` never said so). Rotation does
not stand in its way:

- The source mode, and so every shared primary, keeps the panel's size
  at every rotation; DWM draws the desktop turned into it. Such a primary
  is the panel's picture as it stands and the window shows it unchanged.
- The VOP2 cannot turn a picture by 90 or 270 degrees on the RK3576. In
  mainline `rockchip_vop2_reg.c` (`rk3576_vop_win_data`) the Esmart
  windows offer `DRM_MODE_REFLECT_Y` only and the Cluster windows
  `REFLECT_X | REFLECT_Y`; unlike the RK3588 there is no AFBC 90/270 in
  the list. Only a primary of the turned size (1280x800 here) would need
  turning, and Windows makes none while the source mode is the panel's;
  such a flip is still copied and turned on the CPU and counted
  (`T_CopyWhyRotated`, `T_RotFlipCopies`).
- The RK3576 has two RGA2E 2D engines (vendor `rk3576.dtsi`,
  `rga@27920000`), but mainline describes neither and the firmware powers
  neither; a GPU or RGA turn is only worth doing if those counters turn
  out not to be 0.

Changed on branch `present-fast`, not yet run on a board:

- Scan-out is on from the start; `ScanOut` = 0 turns it off.
- An IOMMU fault while a primary is shown turns it off and puts the window
  back on the framebuffer (`MwScanCheck`, every 8th vsync).
- The adapter leaving D0 turns translation off first, so a restart or the
  bugcheck screen finds the VOP2 reading physical addresses.
- While the source is invisible (display off) the window shows the
  cleared framebuffer, not the last primary; the primary comes back when
  the source is visible again.
- The stats escape counts, per flip, which way it went and why
  (`T_PresentPath`, `T_FlipsScanned`, `T_CopyWhy*`), the time between
  flips, flip to the vsync that showed it, the time in
  `SetVidPnSourceAddress`, and all the CPU time the copies took.

## Video in Edge, 2026-11-21

With every flip scanned out (167 flips in 8 s during YouTube, none copied,
GPU busy 4%), YouTube still dropped about 40% of its frames at 640x480,
with AV1 and with H.264 alike; decode is not the bottleneck.

Read from the code, not yet measured:

- `CopySubresourceRegion` and `CopyResource` between textures have run on
  the GPU since patch 0054 (`pan_blit.c`, blitter, linear views). The
  22 to 26 ms CPU copies in the 2026-11-14 slow log were from before it.
  What 0054 leaves on the CPU: buffers, copies within one level of one
  texture, MSAA, compressed formats, and formats the frontend judged not
  compatible (B8G8R8A8 to its _SRGB went to the CPU; 0062 sends pairs
  with the same linear format to the driver's copy).
- A window's present (not DWM's own, which renders into the primary) is
  the expensive one. `d3d10_gdi.c` maps the back buffer for reading,
  which waits for the GPU to finish everything queued and then reads the
  frame from write-combined memory; it was then copied into a malloc'd
  buffer, copied again into the surface DXGI presents from, and maliwddm
  copies it once more on the CPU into DWM's redirection surface
  (`DoPresent`, a blt present). Four passes over the frame, two of them
  reads of uncached memory, and a GPU wait in Edge's GPU process for
  every frame it presents. 0062 removes one pass (one copy straight from
  the map). The rest needs the frame never to touch the CPU: a GPU copy
  into the presented surface with maliwddm doing blt presents behind the
  GPU queue instead of at submission, or DXGI flip model (DWM composing
  Edge's buffers directly), which needs the DXGI 1.2 DDI the UMD does not
  have.
- There is no video DDI in the UMD (no `ID3D11VideoDevice`), so Chromium
  decodes in software and uploads planes with `UpdateSubresource` or
  `Map`; `VideoProcessorBlt` and NV12 textures are never reached. YUV to
  RGB runs in Chromium's shaders on the GPU.
- `ResolveSubresource` and DXGI's `pfnBlt` do nothing; both are now
  counted, so a caller relying on either shows up.

Patch 0062 counts all of this per process (`m5test perf`, the UMD lines;
INSTALL.txt lists the kinds and the two switches).

## Also run that day

`gsso.exe hw nogs` on the Mali with patches 0041 to 0044: creating a
geometry shader, with or without stream output, returns `E_OUTOFMEMORY`;
the stream-output queries read zero; `DrawAuto` draws nothing; an ordinary
draw is right; the device is not removed; 0 FAIL. Before these patches
each of the creates called a NULL function pointer.

## Hitches: the slow-operation log, 2026-11-14

Not yet run on the board. The desktop stalls for a moment now and then;
the per-present averages above cannot say which operation stalls. The
UMD (patches 0050, 0051 and `win32-compat/malikm_drm.c`) times the
operations that can, and writes one line for each that takes longer than
a threshold.

To use it, on the board:

    mkdir C:\mali\slow
    echo 20 > C:\mali\slow-ms        (optional; 20 ms is the default)

Each process that has the UMD loaded writes `C:\mali\slow\<exe>-<pid>.txt`.
A running `dwm.exe` notices the directory within 10 s; no restart is
needed. A file named `off` in the directory, or removing it, turns the
log off again (the file stays open in a running process). A process in an
AppContainer (the Start menu, Search) logs only if the directory's ACL
lets it create files:
`icacls C:\mali\slow /grant "ALL APPLICATION PACKAGES:(OI)(CI)M"`.

A line:

    1234567  4321 shader-compile     31.402 ms fragment, 1184 bytes, binary hash 0x1c2d3e4f, key hash 0x9a8b7c6d, variant 1, shader 000001F2A3B4C5D0

is GetTickCount (the same clock as the entry-point log), the thread, the
operation, how long it took, and a detail. Operations nest: a `present`
line can come after the `flush-all`, `batch-submit`, `submit` and
`gpu-idle` lines that were inside it, each with its own time.

| Operation | Where | What it covers |
|---|---|---|
| `dxbc-to-tgsi` | frontend | DXBC to TGSI; type 0 pixel, 1 vertex, 2 geometry; the DXBC hash is the same in every process |
| `tgsi-to-nir`, `shader-create` | Panfrost | creating a shader CSO; `shader-create` includes the default variant when it is compiled then |
| `shader-compile` | Panfrost | one variant compiled on the CPU; variant 0 is the default, a higher one was compiled at a bind or draw |
| `create-resource`, `destroy-resource`, `map`, `unmap`, `copy-resource`, `copy-region`, `update-subres`, `present`, `rotate` | frontend | the DDI entry points |
| `transfer-map`, `transfer-unmap` | Panfrost | every map, including those inside a copy (`resource_copy_region` is a CPU copy here) |
| `pan-bo-create` | Panfrost | a BO, with where it came from: the cache, a new allocation, the cache after a wait |
| `bo-wait` | Panfrost | a wait for one BO, with its label and the timeout asked for: `timeout 0` is a busy check |
| `bo-cache-evict` | Panfrost | BOs older than 2 s freed from the BO cache |
| `batch-submit`, `flush-all`, `flush-writer`, `flush-users` | Panfrost | building and submitting job chains; the flushes carry Panfrost's reason |
| `bo-create`, `bo-map`, `bo-unmap` | shim | `pfnAllocateCb` with the address escape, `pfnLockCb`, `pfnUnlockCb` |
| `submit` | shim | `pfnRenderCb`, with how many BOs were listed |
| `gpu-idle`, `wait-bo`, `syncobj-wait`, `destroy-closed` | shim | a wait for the GPU and why; destroying closed allocations after it |
| `shim-lock` | shim | a wait for the shim's one lock, which another thread holds while it waits for the GPU |
| `present-cb`, `shared-destroy`, `primary-destroy` | shim | the runtime calls of a present and of a shared surface's release |

`C:\mali\wait-timeout` (read once per process): the shim keeps the
timeout of WAIT_BO and of syncobj waits instead of waiting for the GPU to
go idle. Panfrost asks with a timeout of 0 whether a buffer is busy before
it reuses a cached BO and before it gives a WRITE_DISCARD map a fresh
buffer; without the file each of those questions waits for everything
the device submitted. Off by default until it has run on the board; with
the log on, `wait-bo ... timeout 0 ns (not kept)` lines say how much the
questions cost.

`m5test mem` lists the GPU memory maliwddm has allocated, in total and by
the process that created it, with how much of it is resident, next to the
working set and private bytes Windows reports for each process.

## The GPU's clock, 2026-11-14

The firmware leaves the GPU at GPLL / 6 = 198 MHz, the rate the device tree
assigns before Linux's devfreq raises it; the operating points run from 300
to 950 MHz. Nothing in Windows raised it, so every figure above this section
was taken at 198 MHz.

`m5test clk <parent> <div> [keep]` sets it (parent 0 GPLL 1188 MHz, 1 CPLL
1000 MHz; at most 600 MHz, the highest operating point that asks for the
lowest supply voltage, because the driver does not touch the regulator).

GpuTest 0.7.0 FurMark through the OpenGL ICD, 1024x640 windowed, 60 s, one
run each:

| Clock | Score | Frames a second |
|---|---|---|
| 198 MHz | 106 | 1 |
| 594 MHz | 285 | 4 |

At 198 MHz the GPU was 97% busy with single jobs of up to 425 ms; at 594 MHz
the longest job in the run was 144 ms. 27784 jobs since that boot, none
failed. The Direct2D frame is byte for byte the same at 198, 396, 500 and
594 MHz. The supply voltage was not measured.


## Measure first: the second round, 2026-11-22

Branch `gpu-perf`, off `hpd-edge`. Everything new is either a counter or
a switch that was off by default, so the package with no settings changed
behaves as `hpd-edge` does. The board ran it on 2026-11-23 (see "Board
results, 2026-11-23" below); since then `UmdMergeSubmit` is on by default
(branch `merge-submit-default`).

### What was already measured, and what was not

Already in `m5test perf`: flips, copies against scan-outs and why, flip
to shown, vsync gaps, jobs and their time (the GPU is busy that long,
one chain runs at a time), submission to start, preemption, blt
presents, and per process the UMD's copies, uploads, maps, read-back
presents and waits for idle. The UMD's `perf:` lines and the slow log
time the stages of a present and every slow operation.

Not measured before this branch:

- the CPU time of the driver's own DDIs, and of the interrupt and DPC;
- what a render carries: the allocation list is every live BO of the
  device (up to 256) on every render, and VidMm references and patches
  each of them;
- chain run times by slot (fragment against vertex/tiler/compute) and
  their distribution, not only the sum;
- submission to completion as a latency, with its distribution;
- the driver's own bubbles: the GPU idle between a chain's end and the
  start of one that was already queued (interrupt latency, or the poll);
- whether the interrupt or the 1 ms poll timer collects the chains;
- how often the UMD renders, and what a render costs it;
- the GPU clock as the CRU has it now, and any temperature.

### The new counters

`m5test prof [seconds]` (default 5) reads `MW_ESCAPE_PERF` twice and
prints the interval:

| Line | Meaning |
|---|---|
| `GPU clock` | `CRU_CLKSEL_CON165` read now: parent and divider. 594 MHz is GPLL / 2. Any PVTPLL mux is not read |
| `temperatures` | TSADC channels 0 to 5, with the setting `PerfTemp` 1 only, without the OTP trim (a few degrees off). Channel 3 is the GPU by the TRM |
| `DDI CPU time` | calls a second, average and longest microseconds, and milliseconds of CPU a second, inside the driver only |
| `renders` | allocation-list entries and chains a render carried |
| `GPU page tables` | pages mapped a second (VidMm paging in) |
| `GPU busy` | by slot, with a histogram of chain run times |
| `submission to completion` | SubmitCommand to DMA_COMPLETED, submissions with jobs |
| `bubbles` | a chain whose submission was queued when the chain before it ended: the time from that end to its start |
| `collected by` | the interrupt (or a preemption request) against the poll timer, and how often the timer ran |
| `completion to DPC` | DMA_COMPLETED noticed to the DPC that tells dxgkrnl |
| `submissions` | how many were already queued in the driver when one arrived |

`m5test ring [csv]` lists the last 128 chains: fence, slot, flags, status,
and for each its submission, start and end in microseconds from the first,
its time queued and its run time. Flag 0x400 means the poll timer found
it, 0x800 that it was not its submission's first chain.

`m5test perf` gains a UMD kind, `submit (pfnRenderCb)`: renders a
second, BOs listed in each, and the CPU time in the runtime's render
call, by process. The UMD's `perf:` line gains `bo-wait`, the time in
waits on a BO's own submission.

The DDI timers cost two reads of the counter and three interlocked
operations a call; the ring and the histograms are written inside the
queue's interrupt-level code that was already running. Reading the
counters submits nothing.

### Benchmark plan

One package, then one change at a time. Each run: start the load, wait
10 s, then `m5test prof 10 > prof-<name>-<n>.txt` and
`m5test perf 10 > perf-<name>-<n>.txt` and `m5test ring csv > ring-<name>-<n>.csv`,
plus the load's own score. Three runs per configuration at least; the
desktop's numbers move by tens of percent between runs (see the table at
the top), so one or two runs prove nothing.

| Load | What it shows | Record besides the counters |
|---|---|---|
| idle desktop, 30 s | the floor: DDI rates with nothing happening, temperatures | nothing |
| dragging a window for 10 s | DWM alone: renders, flips, scan-out | presents in the interval |
| `present.exe hw` / `triangle.exe hw` (tests) | one D3D11 process, small frames | its frame count |
| `d2dtext.exe` and `d3dparts.exe` (tools/windows) | Direct2D and D3D11 paths, CPU heavy | run time |
| GpuTest 0.7.0 FurMark, OpenGL, 1024x640 windowed, 60 s | GPU-bound fragment load: clock and thermal | score, fps, temperatures every 10 s |
| Edge, WebGL Aquarium (webglsamples.org/aquarium), 500 fish | browser present path plus GPU | fps shown; edge://gpu says whether it is on the Mali or SwiftShader/WARP |
| Edge, YouTube 720p, 30 s | video: upload, readback present | dropped frames (Stats for nerds) |

What to read first, in this order: `collected by` (if the poll timer
collects most chains, interrupts are not arriving and every chain pays up
to a poll period); `GPU busy` (above 80% the GPU or its clock is the
limit, below 20% the CPU side is); `renders` and the `Render`, `Patch`
and `SubmitCommand` lines (the cost of each render); `submit` per
process; `bubbles` against the chain count (driver latency the GPU
waits through).

### Ranked tuning list

Gains are estimates from the code and the 2026-11-14 measurements, not
measurements; each item says what number will confirm it.

| # | Change | Expected gain | Risk | Verify with | State |
|---|---|---|---|---|---|
| 1 | Measure (this branch, defaults) | none: the baseline | none | the plan above | done in code |
| 2 | `UmdMergeSubmit` 1: a batch's vertex/tiler and fragment chains as one render | half the renders, events, signal calls, allocation lists, patches and SubmitCommands; a few percent of a core for DWM, more for apps with many batches | low: Panfrost submits the two back to back with out_sync 0 on the first exactly when the second follows; any other call sends a kept chain first | `submit` calls/s halves, `renders: 2.00 chains each`, `Render`/`Patch`/`SubmitCommand` ms per s fall, no change in jobs failed | done; board 2026-11-23: DWM submit CPU per job down about a third, 0 errors; on by default (0 turns it off) |
| 3 | `PollMs` 4 or 10, only if the interrupt collects nearly every chain | up to 1000 timer DPCs a second fewer while the GPU works | low: the timer is the fallback and the 2 s timeout; with TimeSlice set it also paces soft-stops | `PollRuns` per second falls, `bubbles` unchanged | done, at 1 |
| 4 | GPU clock 702 MHz from SPLL (mux 3, divider 1) | up to +18% on GPU-bound loads over 594 MHz | medium: above the 700 MHz operating point; mainline asks 775 mV from 701 to 800 MHz. Do only after the RK806 BUCK5 voltage has been read (firmware) and is at least 775 mV | FurMark score, `GPU busy`, temperature under FurMark below 85 C, JobsFailed 0 over 3 runs | done, off by default: `GpuClkHigh` 1 on the board 2026-11-24, FurMark 334/335/336 against 288/290 (+16%), 0 failed jobs; firmware 537ea09 raises the rail to 775 mV; temperature fallback on branch `gpu-thermal-fallback` |
| 5 | Windowed presents without the CPU: DXGI 1.2 flip model (`UmdDxgi12`, d3d11 branch) or a GPU copy into the presented surface with the KMD's blt behind the GPU queue | the largest for browsers and video: four passes over the frame, two of them reads of write-combined memory, and a GPU wait in the app's present | medium; another branch's work | `present, read back` falls to 0, `KMD blt presents` MB falls, YouTube dropped frames | not here |
| 6 | Fewer BOs per render: list the named ones plus the others only when the set changed or every Nth render | `Render`, `Patch` and the runtime's render time scale with the entries (up to 256 a render) | medium: an allocation not in a list may be evicted under memory pressure, and a job touching it faults (the 0x4002c3 faults of 2026-10-17) | `renders: entries each` falls; JobsFailed stays 0 under memory pressure (many Edge tabs) | idea, needs item 1's numbers |
| 7 | Mali performance counters (PRFCNT: GPU active cycles, L2 and external read/write beats) | instrumentation: says whether a GPU-bound load is shader, texture or bandwidth bound | low: GPU-side registers and a dump buffer, no clock or power change | counters against FurMark at two clocks | design only |
| 8 | A shader disk cache on Windows | the 40 to 50 ms compiles at every process start (the hitches) | medium: Mesa's meson refuses the shader cache on Windows; needs a Mesa patch and a writable, per-build directory | slow log: `shader-compile` lines at the second start of a process | not done |
| 9 | CPU-cached staging and read-back allocations with explicit cache maintenance | Map(READ) and read-back presents read write-combined memory today, several times slower than cached | medium to high: a missed clean or invalidate gives stale pixels or data, intermittently | `map for reading` us each, read-back present time | not done |
| 10 | Skip the start-of-chain cache flush when the previous chain's end flush has just run | microseconds a chain; only matters for many short chains | medium: stale GPU reads if anything else filled the L2 | ring: chains under 100 us | not done |
| 11 | Vsync from the VOP2 interrupt, not the 16 ms timer | pacing and latency, not throughput | medium: a VOP2 interrupt in the DSDT was followed by WHEA errors in October | flip to shown | not here |
| 12 | PVTPLL with the per-chip OTP correction (firmware, TF-A) | 800 to 950 MHz on the GPU's own PLL | high: a wrong ring length gives an unstable GPU, the suspected cause of Linux's job timeouts under this firmware | glmark2 on Linux 3 cold boots, then Windows FurMark | not here |
| 13 | Raising vdd_gpu_s0 (RK806 BUCK5) for 800 to 950 MHz | up to +60% over 594 MHz on GPU-bound loads | **can damage the chip.** Last. Limits: never above 850 mV (mainline's highest operating point) nor 950 MHz; steps of 25 mV; only with TSHUT armed and the temperature read; back to the PMIC default on any job fault | as item 4, plus the voltage read back from the PMIC | not here |

Not on the list: running a vertex/tiler chain beside a fragment chain on
the other slot. Panfrost shares one tiler heap between batches and
relies on no other tiler job running between a batch's tiler and
fragment jobs (`jm_submit_batch`, the submit lock); Linux's panfrost
orders them through the shared buffers too. The gain is not there to
take without changing how Panfrost uses the heap.

The voltage was the open fact behind items 4 and 13. Nothing in the
firmware or Windows had set BUCK5, so it was at the PMIC's power-on
default. The firmware's read on 2026-11-23 gave 750 mV (see "Board
results, 2026-11-23"), below the 775 mV that 702 MHz needs.

### Board tests, one at a time

The KMD changed, so the first test is a package install; the settings
after it are registry values.

1. Package: CI run (see the branch tip's commit message or the report),
   artifact `wddm-mali-g52-driver-arm64`. Copy it to `C:\mali\pkg-perf\`
   and `m5test.exe` from the same run's `m3test-arm64` artifact to
   `C:\mali\`. If the clock is wrong (no RTC battery), `Set-Date` first:
   the test certificate is new each run.
2. Stage it, do not install it: `pnputil /add-driver C:\mali\pkg-perf\maliwddm.inf`.
   Never install a display driver over the running one.
3. Set `ConfigFlags |= 0x20` on
   `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0` as SYSTEM (task
   `malireinst`); poll until the value has 0x20 in it; then a full
   restart.
4. After logon: `m5test` (no argument) for the build, then the benchmark
   plan with no setting changed. This is the baseline; it must look like
   `hpd-edge` in `m5test perf` (same flips, scan-outs, jobs failed 0).
5. `PerfTemp` 1 (no restart), `m5test prof 5`: the temperatures appear.
   Remove it if they read 100.0 C or nonsense.
6. `UmdMergeSubmit` 1 (no restart; running processes take it within a
   second or two, a new process from its first second on), the benchmark
   plan again. Back with `reg delete ... /v UmdMergeSubmit /f`. (On
   `merge-submit-default` and later the default is on; the baseline there
   needs `UmdMergeSubmit` 0 and the delete turns it back on.)
7. Only if step 4 showed `collected by the interrupt` for nearly all
   chains: `PollMs` 4, restart, the plan again.

Send the `prof-*`, `perf-*` and `ring-*` files and the scores. A bad
result in any step: delete that one value and restart; the package
itself changes nothing until a value is set.

## Board results, 2026-11-23

TaishanPi 3M on the DSI panel, package 23.47.19.748 from `gpu-perf`
(76f677b), the board tests above in order. Clock 594 MHz (GPLL / 2)
throughout.

Idle desktop, three samples: GPU 0% busy, 594 MHz.

FurMark (GpuTest, 1024x640 windowed, 60 s), two runs: 288 and 290
points, 4 frames a second (285 at the same clock on 2026-11-14).
`m5test prof`: GPU busy 87.9%, fragment chains 131 to 262 ms,
vertex/tiler chains 33 to 65 ms, the driver's own CPU time small, every
chain collected by the interrupt, bubbles 8 us. Bound by the GPU clock;
the CPU side has nothing left to give here.

Window drag, three runs each way:

| | flips a second | GPU busy | DWM renders a second | DWM submit CPU per job | errors |
|---|---|---|---|---|---|
| baseline (`UmdMergeSubmit` unset) | 40, 36, 59 | 12 to 13% | about 550 to 660, 290 to 330 us each, 220 to 240 BOs each | 0.29 to 0.33 ms | 0 |
| `UmdMergeSubmit` 1 (no restart) | 46, 57, 36 | about 15.5% | 284 to 386, 2 chains each | 0.20 to 0.23 ms | 0 jobs failed, 0 timeouts |

The drag is bound by the CPU's submit path, not the GPU. Merging the two
chains of a batch halved DWM's renders and cut its submit CPU per job by
about 35%. The flip rate moves too much between runs of the same setting
to show a change with three runs. The board kept `UmdMergeSubmit` 1, and
it became the default on `merge-submit-default`.

The GPU's supply, read by the firmware (`tspi3m-gpu-volt-read`, read only
at that point) on the serial log: `vdd_gpu_s0` is RK806 BUCK5, ON_VSEL
0x28 = 750 mV, the PMIC's default, which nothing sets. CRU_CLKSEL_CON165
0x5 at boot (GPLL / 6, 198 MHz until the driver raises it). OTP
specification 0x01 (plain RK3576), gpu-leakage 0x05, gpu-opp-info empty.
At 750 mV 702 MHz is outside both mainline's (775 mV) and the vendor's
slowest level (812.5 mV).

edk2-rk3576 537ea09 (same branch) then raises BUCK5 to 775 mV on the
TaishanPi 3M only, in four 6.25 mV steps each read back, never above
0x2C, and only with TSHUT armed. The board read the rail back at 775 mV.
702 MHz with `GpuClkHigh` has not been run yet ("Board steps" below).

Next, from the ranked list: fewer BOs per render (item 6), and the clock
(item 4, then 12 and 13).


## Above 600 MHz: the supply first, 2026-11-23

Branch `gpu-clk`, off `gpu-perf`; firmware branch `tspi3m-gpu-volt-read`
in edk2-rk3576. The firmware's rail reading and the raise to 775 mV have
run on the board ("Board results, 2026-11-23"); the driver's clock above
600 MHz has not. With no setting changed the driver behaves as `gpu-perf`
does: 600 MHz at most.

FurMark on 2026-11-23 (1024x640, 288 points, 4 frames a second at
594 MHz) had the GPU 87.9% busy with fragment chains of 131 to 262 ms and
every chain collected by the interrupt; on 2026-11-14 198 MHz gave 106
points and 594 MHz 285. The load is bound by the clock and nothing else.

### The operating points

Mainline `arch/arm64/boot/dts/rockchip/rk3576.dtsi` (linux-next, lines
347 to 389), `gpu_opp_table`, one voltage for every chip (target, min,
max):

| MHz | mainline | vendor, plain RK3576, slowest level (L0) | vendor, fastest level (L10) |
|---|---|---|---|
| 300 to 600 | 700 mV | 712.5 mV | 712.5 mV |
| 700 | 725 mV | 750 mV | 712.5 mV |
| 800 | 775 mV | 812.5 mV | 725 mV |
| 900 | 825 mV | 875 mV | 762.5 mV |
| 950 | 850 mV | no such point | no such point |

The vendor columns are the vendor kernel's `rk3576.dtsi` `gpu_opp_table`
(lines 2683 to 2839; the same numbers in `gpu_opp_table_mali`, 2865 on).
Its maximum is 875 mV and 900 MHz; the RK3576J/M points
(`opp-supported-hw` 0x06) ask 750 mV up to 700 MHz and 850 mV at 800 MHz,
and the RK3576S stops at 800 MHz (837.5 mV at its slowest).

How the vendor kernel picks a level (`drivers/soc/rockchip/
rockchip_opp_select.c`): `rockchip_get_pvtm_pvtpll` (line 1049) sets the
GPU to `rockchip,pvtm-freq` 800 MHz and the rail to `rockchip,pvtm-volt`
750 mV, waits `pvtm-sample-time` 1100 us, reads the GPU PVTPLL's
`GCK_CNT_AVG` at `pvtm-offset` 0x54 (a ring-oscillator count) and maps it
through `rockchip,pvtm-voltage-sel` (705 to 724 is L1, ..., 885 and up is
L10; a count below 705 is L0). Faster silicon counts higher and gets a
lower voltage. Then `rockchip_adjust_opp_by_otp` (line 1880) adds the
OTP's `gpu-opp-info` voltage (OTP 0x48: u16 min MHz, u16 max MHz, u8 mV,
u8 length) to every point between min and max, capped at 875 mV. The
variant comes from OTP 0x08 bits 4:0 (`rockchip_get_soc_info`, line 1321:
0x0D M, 0x0A J, 0x13 S, anything else the plain chip). The Rockchip
Devfreq guide (`Rockchip_Developer_Guide_Devfreq_EN.pdf`, the PVTM
section) describes the same mechanism.

The level cannot be measured from Windows or the firmware without
changing the clock and the rail, so only two columns are usable: mainline,
and the vendor's slowest level, which is right for any plain RK3576.

### The supply

`vdd_gpu_s0` is RK806 BUCK5 (`dcdc-reg5` under `pmic@23` on `&i2c1`) on
the TaishanPi 3M (mainline `rk3576-lckfb-tspi-3m.dts:429`), the CM5
(`rk3576-armsom-cm5.dtsi:269`) and the ROCK 4D (`rk3576-rock-4d.dts:478`):
`regulator-boot-on`, 550 to 900 mV, ramp 12500 uV/us. The vendor
`rk3576-rk806.dtsi` adds `regulator-init-microvolt = <750000>`, which only
vendor U-Boot applies; nothing in edk2-rk3576 before 537ea09 or in
Windows sets the rail, so it is the RK806's power-on value: 750 mV, read
on the TaishanPi 3M on 2026-11-23.

RK806 (mainline `include/linux/mfd/rk808.h`, `rk808-regulator.c`):
`BUCK5_ON_VSEL` 0x1E, `BUCK5_SLP_VSEL` 0x28, `BUCK5_CONFIG` 0x14,
`POWER_EN1` 0x01 bit 0; the selector is 500 mV + 6.25 mV per step up to
159 (`rk806_buck_voltage_ranges`), so 750 mV is 0x28, 775 mV 0x2C,
812.5 mV 0x32, 850 mV 0x38.

### The clock

`CRU_CLKSEL_CON165` (TRM V1.2 part 1, CRU chapter): [4:0] divider minus
one, [7:5] 0 GPLL, 1 CPLL, 2 AUPLL, 3 SPLL, 4 LPLL, [8] `clk_gpu_inner`:
0 that divider, 1 the PVTPLL; [9] the PVTPLL's source, 0 deep slow, 1 the
ring. The fixed PLLs: GPLL 1188, CPLL 1000, SPLL 702 (in the secure CRU;
mainline's `clock-spll` fixed clock, TF-A's `SPLL_RATE`), AUPLL 786.432
(audio's). Between 600 and 950 MHz a divider reaches only SPLL / 1 =
702 MHz and AUPLL / 1 = 786 MHz. The driver takes SPLL and never AUPLL,
which belongs to audio, nor LPLL, which belongs to the A53 cluster.

702 MHz is above the 700 MHz point, so the 800 MHz point governs it:
775 mV by mainline, 812.5 mV by the vendor's slowest level (plus any OTP
offset). With the rail at its 750 mV default it is allowed by neither;
at 775 mV (537ea09) mainline's point is met and the vendor's slowest
level is not, so `GpuClkHigh` 1 allows it and 2 does not.

The PVTPLL (GPU instance at 0x27268000, vendor DT `gpu_pvtpll`) is a ring
oscillator in the GPU's own supply. TF-A's `plat/rockchip/rk3576/scmi/
rk3576_clk.c` (`clk_gpu_set_rate`, line 849; table at 306) sets for a
rate a ring length (900 MHz 20, 800 21, 700 21, 600 23, 500 32, 400 48,
300 63; 200 MHz has length 0 and uses GPLL / 6), writes `GCK_LEN`,
`GCK_CAL_CNT` 24, starts it through `GCK_CFG`, and moves bits 9 and 8 of
CON165. The real frequency then follows the voltage: the length is the
request, not the result. The OTP correction is not in this TF-A:
`RK3576_GPU_OPP_INFO_OFFSET` 72 and `struct otp_opp_info` are defined and
never used (on the RK3568 the same TF-A does read OTP and lengthen the
ring, `rk3568_clk.c` 683 to 750). `pvtpll_volt_sel_adjust` (line 1331)
adds length per voltage level, but no GPU entry sets `volt_sel_thr`, so
it changes nothing. The board's BL31 is this TF-A (v2.14.0, de38734). The
NPU's PVTPLL route through SCMI was measured to complete no job
(edk2-rk3576 `Npu.c`), and Windows has no SCMI client. Item 12 stays as
it was.

### Temperature

TSADC at 0x2AE70000. The TRM (Table 19-1) and mainline's thermal zones
say channel 3 is the GPU and 5 the DDR; mainline's and the vendor's OTP
trim cells put the GPU trim (OTP 0x2C) on sensor 5, and the vendor's
`gpu-thermal` zone reads channel 5. So which channel is the GPU is not
settled; FurMark will show which one rises. Trips: mainline `gpu-alert`
85 C passive, `gpu-crit` 115 C; TSHUT 120 C (CRU reset). The firmware's
thermal-safety work is on edk2-rk3576 main (761a938): TSHUT armed at
120 C plus trim on every valid channel, ACPI `_CRT` 115 C on the CPU
zones. `m5test prof` with `PerfTemp` 1 shows all six channels.

### The steps

1. Firmware (`tspi3m-gpu-volt-read`, `GpuRail.c`): reads RK806 BUCK5 and
   the GPU OTP bytes at boot, logs them, and publishes the volatile
   runtime variable `GpuRail` (GUID 7701c75f-f454-4aab-9445-117f09fb87cf,
   32 bytes). It writes nothing. The log says which rates the rail meets:

       GpuRail: CRU_CLKSEL_CON165 0x00000005 (mux 0, div 6)
       GpuRail: OTP specification 0x00 (RK3576), gpu-leakage 0x.., gpu-opp-info min .. MHz max .. MHz volt +.. mV length ..
       GpuRail: RK806 BUCK5 ON_VSEL 0x.. SLP_VSEL 0x.. CONFIG 0x.. POWER_EN1 0x.. (on)
       GpuRail: vdd_gpu_s0 (RK806 BUCK5) ...... uV
       GpuRail: at ...... uV mainline's OPP table allows up to ... MHz; the vendor table's slowest level up to ... MHz
       GpuRail: 702 MHz (SPLL / 1) needs 775000 uV by mainline (...), 812500 uV by the vendor's slowest level (...)

2. The rates the reading allows, from the table above. 594 MHz needs
   700 (mainline) or 712.5 mV (vendor); 702 MHz needs 775 or 812.5 mV.
3. Only if the rail already meets it: `GpuClkHigh` in the driver.
4. Raising the rail: 537ea09 raises it to 775 mV on the TaishanPi 3M,
   approved for that board and that value only, and
   nothing higher. Anything beyond stays a design (item 13): never above
   850 mV (the vendor's own maximum is 875, mainline's 850) nor 950 MHz,
   6.25 mV selector steps written one at a time with the rail read back,
   never from Windows while the GPU runs, only with TSHUT armed and the
   temperature read, and the PMIC default back on any job fault.

### The driver switch

Settings in the device's `Device Parameters` key, read once per start:

| Setting | Effect |
|---|---|
| `GpuClkHigh` not set (default) | since `gpu702-default`: 1 when the firmware's own reading (GpuRail) is at least 775 mV on a plain RK3576, else 0; with it on and no `GpuClk` stored, 702 MHz at start ("702 MHz by default" below) |
| `GpuClkHigh` 0 | off: at most 600 MHz, whatever the firmware set |
| `GpuClkHigh` 1 | allows a rate above 600 MHz when the supply meets mainline's operating point at or above it |
| `GpuClkHigh` 2 | the same with the vendor's slowest-level voltages (stricter) |
| `GpuRailUv` | a supply in microvolts verified some other way (the firmware log, or Linux `/sys/class/regulator/regulator.N/microvolts` for `vdd_gpu_s0`); with the firmware's reading too, the lower counts |

The checks in `GpuClkWrite` (`drivers/maliwddm/adapter.c`), all of which
must pass for anything above 600 MHz: the setting is on; a supply is
known and inside 550 to 900 mV; the firmware did not report a J, M or S
variant; the rate is at most 950 MHz and from GPLL, CPLL or SPLL; the
required voltage (plus the OTP `gpu-opp-info` offset when the point is in
its range) is at most the supply; CON165 is not on the PVTPLL path. A
refusal is logged (`GpuClkRefusedHz`, `GpuClkNeedsUv`). Above 600 MHz any
job fault or timeout writes GPLL / 2 back from the interrupt
(`GpuClkFallbacks`); the adapter stopping does the same, so a later start
with the setting off never inherits 702 MHz.

`m5test clk` reports the setting, the supply and where it came from, the
highest point allowed now and what 702 MHz needs. `m5test clk 3 1` asks
for SPLL / 1 = 702 MHz.

### Board steps

1. Flash the `tspi3m-gpu-volt-read` firmware (needs the user's go-ahead;
   keep the nvdata sectors as for every reflash). Boot, and keep the
   serial log's `GpuRail:` lines and `CpuMaxFreq:` lines.
2. If `vdd_gpu_s0` reads below 775 mV, stop here: 702 MHz is not allowed
   without raising the rail, which is item 13.
3. Otherwise, with this branch's package staged and installed as in
   "Board tests" above: `m5test clk` (expect the supply from firmware and
   "allowed now: up to 700" or higher), then `GpuClkHigh` 1, restart,
   `m5test clk 3 1`, FurMark three runs with `m5test prof 10` (`PerfTemp`
   1), JobsFailed 0, `GpuClkFallbacks` 0, the hottest of channels 3 and 5
   below 85 C. Only then `m5test clk 3 1 keep`.
4. Back: `m5test clk 0 2 keep` (594 MHz) or delete `GpuClkHigh`.


## DWM's copies, 2026-11-24

Branch `umd-copy-cost`, off `wddm2x`. With no setting changed the package
behaves as before: the new switch is off unless set.

### What was measured

HDMI 2560x1440, 15 s of window dragging (`wddm2x-stepG` session):

| | total CPU (8 cores) | dwm.exe CPU | dwm copies on the GPU | dwm uploads |
|---|---|---|---|---|
| WDDM 1.3 | about 26% | 15.2 s | +5203 in about 18 s (289 a second), 6216 ms, 1195 us each | +3770, 422 ms |
| WDDM 2.0 | about 20% | 7.1 s | (since boot: 13914, 14788 ms, 1087 us each) | (8378, 2006 ms) |

In WDDM 1.3 that is 345 ms of CPU a second in copies alone, a third of
the core dwm.exe used. The run of 2026-11-23 (`gpuperf`, DSI panel) had
the same price per copy, 889 us, at 43 copies a second, and also gives
the price of one render: `submit (pfnRenderCb)` 421 us each with 253 BOs
listed, 380 renders a second, 1.6 s of CPU in 10 s.

### Where a copy's time goes

`CopySubresourceRegion` and `CopyResource` reach
`panfrost_resource_copy_region` (mesa-patches/0054), which did:

1. `panfrost_flush_all_batches`: every batch the context had open is
   submitted, which splits the frame DWM is drawing in two;
2. the blitter's draw from the source into the destination, a new batch;
3. `panfrost_flush_all_batches` again: that batch is submitted.

A submission is one `pfnRenderCb` (vertex/tiler and fragment chains
merged since `UmdMergeSubmit`) with every live BO in the allocation list,
a CPU event and a `SignalSynchronizationObject2`. So a copy cost two
renders, about 2 x 421 us, plus tens of microseconds for the draw: the
889 to 1195 us measured. Nothing in that path waits for the GPU: the
shim's SUBMIT does not wait for in_syncs (the KMD runs one queue in
order), no WAIT_BO or syncobj wait is made, and the UMD does no cache
maintenance (the job chains' own flush flags do it on the GPU). The time
is the submissions.

### What is copied, and by whom

The entry-point log of dwm.exe in the same session (`log/dwm.exe-1208.txt`)
has 1023 `ResourceCopyRegion`, no `ResourceCopy` and no
`ResourceResolveSubResource`. They come in twos, after the frame's draws
and right before `_Present`:

    SetRenderTargets, DrawIndexed x4, ResourceCopyRegion x2, _Present, _RotateResourceIdentities, Flush

This is DWM keeping its rotating back buffers whole: the regions that
changed in the previous frame are copied from the buffer last presented
into the one being finished. explorer.exe does the same (464 copies, 190
presents). The copies are regions, not whole screens: 5775 MB over 14415
copies is 0.4 MB each, about 400x260 pixels of BGRA8, not 14.7 MB. While
a window is dragged DWM makes several a frame, which is the 289 a second.

The new log line says it per process when `C:\mali\log` exists when the
process makes its first copy (every 5 s, plus the first 16 copies in
full: sizes, formats, bind flags, box, whether the destination is the
bound render target):

    copies: 1440 on the GPU in 5003 ms (0 batched), 1180 us each, 610.2 MB; sizes <16K .. >=4M ...;
    largest 2560x1440; whole subresource N, into the bound RT N, into a shared/scan-out resource N,
    dst screen-sized N, src screen-sized N; open batches at the copy N (x.xx each)

(the numbers above are the format, not a measurement). "open batches at
the copy" is how many batches the first flush submitted, or with the
switch on would have.

### The switch: `UmdCopyBatch`

Board result, 2026-11-24 (TaishanPi 3M, WDDM 2.0, HDMI 2560x1440): `copytest hw`
passed with the switch off and on; three automated 15 s drags each way cut
DWM's GPU copy time from about 1075 ms to 60 to 123 ms and its submits from
about 3050 to 1480, present time rose from about 100 to 340 ms, and no stale
or ghost rectangles were seen. Since then the switch is on unless set to 0.


mesa-patches/0075. With `UmdCopyBatch` 1 in Device Parameters (handed to
the UMD as `MW_UMD_FLAG_COPY_BATCH`, 0x8 in `m5test perf`'s switches),
neither flush is made. The blitter's draw is recorded like any other draw
and Panfrost's own resource tracking orders it: reading the source
submits the batch that writes it first; writing the destination submits
any other batch that reads or writes it first. That is the same tracking
every draw that samples a texture rendered a moment before relies on. A
copy into the render target DWM has bound joins the batch already drawing
it, so the frame stays one submission and the copy costs one draw. A copy
between two levels of one texture keeps both flushes. Off unless 1;
running processes take a change within a second or two. For one process,
`D3D10UMD_COPYBATCH=0` or `1` in its environment overrides the KMD.
It applies to every process with the Panfrost UMD, the OpenGL ICD
(maligl.dll, `glCopyImageSubData`) included, since both take the switches
from the same escape.

Expected, from the numbers above (estimates, to be confirmed):

- `copy on the GPU`: from about 1100 us to 50 to 150 us each;
- dwm.exe's `submit (pfnRenderCb)` calls a second fall by about one to
  two per copy that was made (up to 289 a second fewer at 1.3 on HDMI);
- the render that a copy used to force now happens at the next flush,
  so `present, no copy` may take longer each; judge by dwm.exe's total
  CPU and its renders a second, not by the copy line alone;
- dwm.exe: about 0.2 to 0.3 of a core less while dragging on 1.3 (15.2 s
  in 15 s towards 10 to 12 s), less in absolute terms on 2.0 where a
  render is cheaper (7.1 s towards 5 to 6 s);
- the GPU does the same work: the same pixels, in fewer chains.

What can go wrong: a stale region (a trail behind a dragged window, a
rectangle of an older frame) if some order is not tracked. The order
cases are checked by `copytest.exe` (tools/windows, artifact
`board-tests-arm64`), which runs on softpipe and WARP in CI with the
pixels checked and on Panfrost on the emulated driver both ways (pixels
not checked there: nothing runs the jobs).

### Not done here

- Under WDDM 2.x every BO is made resident by `MakeResident` when it is
  created, yet each render still lists every live BO (about 250) and the
  KMD patches each. Listing only the BOs a job names there would cut the
  421 us a render, for every render, not only copies. It touches the 2.x
  residency work on `wddm2x`, so it is left for that branch, as a switch.
- `upload (UpdateSubresource)`: 112 us each on 1.3, 240 us on 2.0. A
  texture a pending batch reads takes Panfrost's shadow path (wait for
  the BO, a new BO, a copy of the whole BO on the CPU). Smaller than the
  copies (23 ms a second on 1.3); next after them.
- The KMD's DPC, 39 ms a second during a drag (`prof-merge-*`): not
  dwm.exe's time, and not touched.

### Board test

The KMD changed (the new setting), so the package goes in as in "Board
tests, one at a time" steps 1 to 3 (stage with `pnputil /add-driver`,
`ConfigFlags |= 0x20`, full restart). Copy `copytest.exe` from the same
commit's `board-tests-arm64` artifact and `m5test.exe` from `m3test-arm64`
to `C:\mali\`. Measure with `C:\mali\log` absent (the entry-point log costs
CPU), and in an elevated PowerShell (dwm.exe's CPU time needs it).

1. Correctness, before DWM uses it, no setting needed:

       $env:D3D10UMD_COPYBATCH = '0'; C:\mali\copytest.exe hw > C:\mali\copytest-0.txt
       $env:D3D10UMD_COPYBATCH = '1'; C:\mali\copytest.exe hw > C:\mali\copytest-1.txt

   Both must end with `done: 0 check(s) failed`. A FAIL with 1 and not
   with 0 means stop: do not set `UmdCopyBatch`.
2. The same frame shape without DWM's noise, three runs each way:

       foreach ($cb in '0','1','0','1','0','1') { $env:D3D10UMD_COPYBATCH = $cb; C:\mali\copytest.exe hw bench 600 | Select-String bench }

   Compare `CPU .. ms a frame` and `Mcycles a frame`.
3. Baseline drag, `UmdCopyBatch` unset. Three times: run the line below,
   then drag a window in circles for the whole 15 s.

       $s = { (Get-Process dwm | % { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum }
       $a = & $s; C:\mali\m5test.exe perf 15 > C:\mali\perf-copy0-1.txt; $b = & $s; "dwm CPU $([math]::Round($b - $a, 2)) s" | Tee-Object -Append C:\mali\perf-copy0-1.txt

   (`-2`, `-3` for the next runs.) Keep Task Manager closed: it costs
   12 s of CPU in 15 s on its own.
4. On, no restart:

       reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v UmdCopyBatch /t REG_DWORD /d 1 /f

   Wait 5 s; `C:\mali\m5test.exe perf 2` must show the switches with 8 in
   them (0xf with the defaults). Then step 3 again three times, into
   `perf-copy1-<n>.txt`.
5. Look at the screen with it on: drag windows over each other and over
   the taskbar, open and close the Start menu, scroll a page in Edge,
   resize a window. Anything left behind (trails, old rectangles) is a
   failure: photo or screenshot it.
6. Optional, for the sizes: `mkdir C:\mali\log`, `taskkill /f /im dwm.exe`
   (Windows starts it again), drag 15 s, send `C:\mali\log\dwm.exe-*.txt`
   (the `copy N:` and `copies:` lines), then `rmdir /s /q C:\mali\log` and
   restart dwm.exe again before any measurement.
7. Back: `reg delete "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v UmdCopyBatch /f`.

Run steps 3 and 4 under WDDM 1.3 and again under 2.0 if both packages are
on the board. Send `copytest-*.txt`, the bench lines, and the six
`perf-copy*` files. Per file read: `copy on the GPU` us each and calls a
second, dwm.exe's `submit` calls a second, `present, no copy` us each,
and the `dwm CPU` line. Three runs each way, as always: the desktop moves
by tens of percent between runs of the same setting.

## 702 MHz on the board, 2026-11-24

TaishanPi 3M, firmware `tspi3m-gpu-volt-read` 537ea09 (serial: BUCK5
0x28 -> 0x2C, read back 775000 uV, TSHUT armed), driver package
7.31.29.680 (`gpu-clk`), `GpuClkHigh` 1, `m5test clk 3 1`, so SPLL / 1.

GpuTest 0.7.0 FurMark through the OpenGL ICD, 1024x640 windowed, 60 s:

| Clock | Runs | Scores | GPU busy |
|---|---|---|---|
| 594 MHz (GPLL / 2), 2026-11-23 | 2 | 288, 290 | 87.9% |
| 702 MHz (SPLL / 1) | 3 | 334, 335, 336 | about 87% |

+16% for a clock 18% higher; the GPU stays about as busy, so the load is
still bound by the clock. 46408 jobs over the three runs: none failed,
timed out, faulted or reset the GPU, and no `GpuClkFallbacks`. The
hottest die reading was 49.9 C, with the fan on. The clock is kept on
this board with `m5test clk 3 1 keep`.

With the fan off the board idles at 67 to 68 C already at 594 MHz. In
an extreme test (fan off, FurMark at 702 MHz and Prime95 with every
option at its maximum) the board froze about 90 s after Prime95 started:
the picture stopped, no bugcheck. The highest readings were 82 to 84 C,
on the little cores; the GPU read 78.5 C. One run; taken as an extreme
case of the CPU load, not of 702 MHz, and not repeated. The fallback
below would not have fired in it (78.5 C is under 85 C).

The firmware raises the rail on every TaishanPi 3M boot (default on,
the user's decision); `GpuClkHigh` stayed 0 by default in the driver
until `gpu702-default` (2026-11-25, last section).

### The temperature fallback (branch `gpu-thermal-fallback`)

With `GpuClkHigh` on, the driver reads TSADC channels 3 and 5 (the two
candidates for the GPU, see "Temperature" above) once a second from its
own 1 s timer, and takes the hotter one:

- at or above 85 C (mainline's `gpu-alert`) the clock goes to GPLL / 2 =
  594 MHz at once and is held there;
- it goes back to the clock that was asked for only below 75 C (since
  `gpu-clk-safe-switch`: below 75 C at every reading for 90 s, see the
  next section);
- three readings in a row with neither channel valid count as hot;
- a job fault still drops the clock and nothing brings it back by itself
  (changed on `gpu-clk-safe-switch`, next section);
- the adapter stopping drops it as before.

The decisions are `drivers/maliwddm/thermal_calc.h`, checked on the host
by `tests/thermal-host` (329 checks: the code table, the hotter channel,
the 85/75 band, unreadable sensors). The clock write of a drop or a
restore runs synchronised with the interrupt, where a job fault drops
the clock, so the two cannot cross.

The TSADC is now read without `PerfTemp` once the CRU says it runs
(gates `CLKGATE_CON(13)` bits 8 and 9 open, resets `SOFTRST_CON(13)` bits
8 and 9 released, `TSADC_AUTO_CON` bit 0 set, the way edk2-rk3576's
`Rk3576TsadcIsRunning` checks it). Without a running TSADC there is no
guard and `GpuClkWrite` refuses anything above 600 MHz
(`GpuClkRefusedNoTemp`). No OTP trim is applied, a few degrees, which the
10 C band covers.

`m5test clk` adds a line: whether the guard is on, the hotter reading,
whether the clock is held, and how often it was held and released.
`m5test prof` shows the temperatures without `PerfTemp` and the hold
count (`GpuClkThermalDrops`, the old `Reserved0` of the perf escape).

Not run on a board yet. To see a drop:

1. Stage and install the `gpu-thermal-fallback` package as in "Board
   tests" (`pnputil /add-driver`, `ConfigFlags |= 0x20`, full restart).
   `GpuClkHigh` 1 and the kept `GpuClk` (3 1) stay as they are.
2. `m5test clk`: "temperature guard: on", "not held", 702 MHz.
3. Fan unplugged: FurMark 1024x640, and `m5test clk` every 30 s (or
   `m5test prof 10` in a loop). Expect "HELD at 594 MHz now" and held
   1 time once the hotter channel reaches 85 C, `m5test prof` showing
   594 MHz and the FurMark frame rate falling to the 594 MHz rate. Stop
   at once if any channel passes 95 C or the board stops answering:
   TSHUT is at 120 C, but the freeze on 2026-11-24 came at 84 C.
4. Fan back on, FurMark stopped: within a minute or two of the hotter
   channel falling below 75 C, "not held", back 1 time, 702 MHz again.
5. Fan on, FurMark 60 s three times: the scores of 2026-11-24 (334 to
   336), held 0 times, JobsFailed 0.

## The fallback on the board, and the switch between job chains (2026-11-24)

TaishanPi 3M, the `main` package of 01dc1bf/d4b1e8f (which carries
`gpu-perfdata` and the fallback), `GpuClkHigh` 1, firmware at 775 mV, fan
unplugged, FurMark. The tray log
(`board-session-prep/gpu-thermal-fallback-2026-11-24.log`, every 8 s):

    20:30:52 gpu 79C 702MHz throttled=0 busy=90%
    20:31:00 gpu 83C 702MHz throttled=0 busy=80%
    20:31:08 gpu 79C 594MHz throttled=1 busy=15%
    20:31:17 gpu 80C 594MHz throttled=1 busy=10%
    ...
    20:32:00 gpu 73C 594MHz throttled=0 busy=11%

The drop came as designed, at 85 C between two log lines. At 20:31:11
Windows logged Display event 4101 (TDR, "display driver stopped
responding and has recovered"). The driver's counters: one job failed,
one timed out, two GPU resets. Afterwards FurMark stuttered and "% busy"
stayed at 10 to 15%. With the fan back on the GPU cooled (66 C, not held)
and the clock stayed at 594 MHz: a job fault cleared the clock asked for,
so the TDR that the switch itself caused lost 702 MHz until the next
start.

### What was wrong with the switch

The drop was one register write, `(0xFF << 16) | GPLL << 5 | (2 - 1)`:
mux and divider together, from the 1 s DPC, while a job chain ran.

- **The order.** Nothing says the mux and the divider of
  `CLKSEL_CON165` take effect on the same edge. From SPLL / 1 to GPLL / 2
  the state "new mux, old divider" is GPLL / 1 = 1188 MHz, on a rail set
  for 702. Mainline changes this clock in two writes:
  `clk_composite_set_rate_and_parent` (drivers/clk/clk-composite.c; the
  RK3576's `clk_gpu_src_pre` is a `COMPOSITE` in clk-rk3576.c) writes the
  divider first when the old divider on the new parent would be faster
  than the target, else the mux first. TF-A's SCMI GPU clock
  (`plat/rockchip/rk3576/scmi/rk3576_clk.c`, `clk_gpu_set_rate`), which
  mainline's rk3576.dtsi uses for the GPU's assigned rate, writes the
  divider, then the mux, each with its own mask.
- **The moment.** Neither mainline (panfrost's devfreq calls
  `dev_pm_opp_set_rate` while jobs run) nor TF-A waits for an idle GPU.
  So the order alone may have been the fault. One run cannot tell which,
  and waiting is cheap here, so both are fixed.

Which of the two caused this TDR is not known; the fix does not depend on
it. A board run of the fixed package with the same load answers whether
the switch was the whole story (no 4101).

### The fix (branch `gpu-clk-safe-switch`)

- `drivers/maliwddm/gpuclk_calc.h`, `MwClkSteps`: the writes from one
  mux/divider to another, one field per write under its own mask, in the
  order whose halfway state is the slower (never faster than the faster
  end: the two halfway rates multiply to the product of the end rates).
  702 to 594: divider first (SPLL / 2 = 351 MHz on the way), as TF-A.
  594 to 702: mux first (SPLL / 2 again). `tests/gpuclk-host` checks every
  pair of GPLL, CPLL and SPLL with every divider (78727 checks).
- **Only between job chains.** Every clock change (the drop, the
  restore, `m5test clk`, the stored `GpuClk` at start, a fault) asks
  through `ClkRequest` at the interrupt's level: written at once when no
  chain runs, else by `Kick` (command.c) after the running chain ends and
  before the next starts. The driver runs one chain at a time, so this
  holds nothing back, and a chain is bounded by the 2 s job timeout, which
  bounds the wait. `m5test clk` counts the switches and how many waited.
- **The restore waits for a cool GPU.** Below 75 C at every reading for
  `GpuClkRestoreHoldSec` (default 90 s, at most 3600; a DWORD in Device
  Parameters, read at start). One reading at or above 75 C, or an
  unreadable one, starts the hold over. The drop at 85 C stays immediate.
- **The fault policy.** A job fault, a job timeout or a TDR while above
  600 MHz drops to 594 MHz at once, as before, but keeps the clock asked
  for: it comes back at the next reading (1 s) unless heat holds it. Two
  such faults within 10 minutes lock 594 MHz until the next start. A fault
  at 594 MHz does not count, so a fault and the TDR of the same hang count
  once (the first drop already made it 594). `tests/thermal-host` checks
  the band, the hold, the fault policy and a replay of the board log
  (2738 checks).
- **Why the clock is held.** `Throttled reason` in the performance
  counters and in `m5test clk`: 0 none, 1 thermal, 2 faults. The registry
  trace (`Device Parameters\maliwddm`) gets `GpuClkThrottleReason`,
  `GpuClkFaultLocked`, `GpuClkHighFaults`, `GpuClkThermalDrops`,
  `GpuClkThermalRestores`, `GpuClkSwitches` and `GpuClkDeferred` from a
  work item each time one changes, and `GpuClkRestoreHoldMs` at start.

### "% busy" after the TDR

"% busy" is the time job chains ran (`SlotTicks`), which grows only when
a chain ends; a reset does not add to it. So 10% with FurMark stuttering
means chains really ran about 100 ms a second, and "10 while idle" means
something kept the GPU busy that long. Not explained yet. Two candidates,
each checkable with `m5test prof 10` after a TDR: DWM compositing with
the GPU (its process line in the UMD section, and flips copied instead of
scanned out), or FurMark's context recovering slowly. The next board run
should not have a TDR; if one happens, `m5test prof 10` right after it.

### Board test of the fix

1. Install the `gpu-clk-safe-switch` package as usual (full restart).
   `GpuClkHigh` 1, the kept `GpuClk` 3 1.
2. `m5test clk`: 702 MHz, guard on, throttled reason 0, back after 90 s.
3. Fan unplugged, FurMark, `maliwddm-counters check` or the tray tool.
   Expect at 85 C: 594 MHz, Throttled 1, reason 1, **no event 4101**,
   "% busy" staying high (about the 702 MHz figure) at 594 MHz, and
   FurMark's frame rate falling to the 594 MHz rate without stutter.
   `m5test clk`: switches 2 or more (the start, the drop), some of them
   "waited for a running job chain".
4. Fan back on: after the hotter channel has stayed below 75 C for 90 s,
   702 MHz again, Throttled 0. `GpuClkThermalRestores` 1 in the trace.
5. The event log: `Get-WinEvent -FilterHashtable @{LogName='System';Id=4101}
   -MaxEvents 5` shows nothing newer than the start.
6. If a TDR still comes: the clock comes back after 1 s, and a second one
   within 10 minutes locks 594 MHz (reason 2, `GpuClkFaultLocked` 1). Then
   the switch was not the whole cause; keep `m5test prof 10` and the
   event, and stop raising the clock.

## TDRs with no job fault (board retest, 2026-11-24)

Package 7.42.44.26 (`gpu-clk-safe-switch`), `HdmiHpd` 3, fan off,
FurMark, then fan on (`board-session-prep/gpu-thermal-fallback-2-2026-11-24.log`).

**The switch passed.** The drop at 85 C (21:06:16) to 594 MHz was smooth:
"% busy" stayed at 88 to 91% and FurMark did not stutter. `m5test clk`:
5 clock switches, 3 of them waited for a running job chain. Below 75 C
from 21:07:07, 702 MHz again at 21:08:40: the 90 s hold works.

**Two TDRs (event 4101) anyway**, neither at a switch:

| Time | Clock | Before it | Counted as |
|---|---|---|---|
| 21:03:29 | 702 MHz, about 45 s into FurMark | the 8 s line before reads 15% busy | one fault above 600 MHz; 702 MHz back 1 s later |
| 21:08:23 | 594 MHz (held, cooling) | that line reads 73% | nothing (at 594) |

The job layer saw nothing: jobs done 25366, failed 0, timed out 0, JS
status 0, no MMU fault, resets 2. So both resets were dxgkrnl's
(`DxgkDdiResetEngine` or `DxgkDdiResetFromTimeout`, which run `SyncReset`),
not the driver's. dxgkrnl's rule ("TDR in Windows 8 and later"): a packet
or a preemption that does not complete within `TdrDelay` (2 s). The
driver's own chain timeout is also 2 s, so a chain that really hung would
show the same way: dxgkrnl gets there first and the driver counts nothing.

What the driver kept until now does not tell these apart: the boot
journal stops at the confirm (120 s), and the chain ring keeps chains,
not the state at the reset. So this branch adds a snapshot.

### The TDR snapshot

At every `ResetEngine` (node 0 or 1) and `ResetFromTimeout`, before
anything is reset, `TdrSnap` (command.c) records, at the interrupt's
level; a work item writes it to `Device Parameters\maliwddm` as
`Tdr<n>_<field>` (n = 0 to 3, the last four) and `TdrCount`:

| Field | Meaning |
|---|---|
| `Node` | 0 3D, 1 the copy node, 0xFFFFFFFF adapter-wide |
| `Running`, `Slot`, `RunMs` | a chain on the GPU, its slot, for how many ms |
| `JsStatus`, `JsHeadLo`, `JsTailLo`, `JobRaw`, `JobPending` | the slot's JS_STATUS and JS_HEAD/JS_TAIL, JOB_INT_RAWSTAT, and what the ISR collected but nothing reaped yet |
| `QCount`, `HeadFence`, `HeadNext`, `HeadCount`, `HeadStarted`, `HeadAgeMs` | the driver's queue and its head submission (chains started of how many, since when) |
| `LastSubmitted`, `LastCompleted` | the fences |
| `SinceDoneMs`, `SinceChainEndMs`, `SinceIsrMs`, `SincePollMs`, `SinceSubmitMs` | ms since the last DMA_COMPLETED, chain end, GPU interrupt, poll and submission |
| `PreemptPending`, `PreemptMs`, `StopAsked`, `StopMs` | an unanswered preemption request or soft-stop, and its age |
| `GpuClk`, `MilliC`, `JobsDone`, `ClkPending` | the clock (mux * 256 + divider), the temperature, jobs done, a clock change waiting |

How to read it:

- `Running` 1, `RunMs` about 2000, `JsStatus` 0x08 (active), `JobRaw`
  without the slot's bit: **the GPU is really on one chain for 2 s**.
  Either it hung (then it hangs at 594 MHz too, so not the clock) or a
  FurMark chain is that long (then `RunMs` is large on most snapshots, and
  the answer is longer chains in the UMD, not a driver fix).
- `Running` 1 but `JsStatus` 0x01 (done) or the slot's bit set in
  `JobRaw` or `JobPending`: **a completion was missed**; `SinceIsrMs` and
  `SincePollMs` say whether the interrupt and the 1 ms poll were running.
- `Running` 0 and `QCount` above 0: **the queue stalled** with work in it
  (nothing called `Kick`).
- `Running` 0, `QCount` 0, `LastCompleted` = `LastSubmitted`: **the
  driver finished everything** and dxgkrnl did not see it: a lost
  DMA_COMPLETED or DPC (`SinceDoneMs` against 2000).
- `PreemptPending` 1 with `PreemptMs` near 2000: **a preemption not
  answered**.
- `Node` 1: the copy node, which completes inside `SubmitCommand`.

A finding to check against the dump, not changed here: after a reset the
driver drops its whole queue but reports `LastAbortedFenceId` =
`LastCompleted`. Microsoft's text says it should be the fence of the
packet aborted in the middle of execution (the head), and that the
scheduler resubmits packets submitted later. Whether dxgkrnl resubmitted
what the driver dropped is in the dump's recovery context.

### What to collect on the next run

1. The same run (fan off, FurMark, then fan on), with the tray log.
2. After each 4101, at once: `m5test ring csv > ring-<n>.csv` (the last
   128 chains with their times) and `m5test prof 5`.
3. At the end: `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" > devkey.txt`
   (the `Tdr*` values), `m5test clk`, and the System log's 4101 events
   (`Get-WinEvent -FilterHashtable @{LogName='System';Id=4101} | Format-List *`).
4. The LiveKernelReport: copy everything in
   `C:\Windows\LiveKernelReports\WATCHDOG\` (the `.dmp` of each TDR,
   0x117 VIDEO_TDR_TIMEOUT_DETECTED). From it `!analyze -v` and `dt
   dxgkrnl!_TDR_RECOVERY_CONTEXT` give the node, the TDR reason, the fence
   dxgkrnl waited for and what it resubmitted; the 21:08:37 one from this
   run as well.
5. Optional, best evidence: a DxgKrnl ETW capture over the run
   (`wpr -start tools\windows\dxgkrnl.wprp -filemode`, then `wpr -stop
   tdr.etl` after a 4101): per-node packet submit and complete times show
   which packet was outstanding for 2 s and whether the driver ever
   completed it.

### The cause: a forced preemption the driver never answered (2026-11-25)

Board run of 2026-11-25 evening (`session-2026-11-25-night/results-tspi`):
package 3.58.41.641, WDDM 2.0, 702 MHz, fan on (55 C max), FurMark 5
minutes, four 4101 events (17:54:33, 17:55:05, 17:55:31, 17:58:16). All
four snapshots read the same:

| Field | Tdr0 | Tdr1 | Tdr2 | Tdr3 |
|---|---|---|---|---|
| `LastSubmitted` = `LastCompleted` | 0x1f42 | 0x221b | 0x240b | 0x31c7 |
| `SinceSubmitMs` | 2351 | 2480 | 2469 | 2467 |
| `SinceDoneMs` = `SinceChainEndMs` = `SinceIsrMs` | 2199 | 2328 | 2289 | 2289 |
| `SinceFlipMs` | 2366 | 2649 | 2483 | 2482 |
| `SinceDpcMs`, `SinceVsyncMs` | 4 | 6 | 11 | 8 |
| `LastNotifiedNode`, `LastNotifiedFence` = `CopyFence` | 1, 0xac4 | 1, 0xc82 | 1, 0xe88 | 1, 0xf3a |

`Running` 0, `QCount` 0, `NotifyPending` 0, `FlipsUnreported` 0,
`PreemptPending` 0 everywhere. So the last node-0 submission (a FurMark
frame, about 150 ms of GPU work at 5.5 frames a second) was submitted,
ran, ended on its interrupt and was notified; after it dxgkrnl gave node 0
nothing for 2.3 s, while the DPC, the vsync and the copy node all kept
going, and then reset node 0. dxgkrnl stopped feeding the node and then
timed it out. That is what a preemption request waiting for its answer
looks like from the driver's side.

`PreemptPending` 0 did not rule that out: with `TimeSlice` 0 (the
default) `MwPreemptCommand` returned at once without recording the call
(`T_PreemptRequests` counts only requests that reach `SyncPreempt`), and
`Enter_PreemptCommand` is never written because the DDI runs at
DISPATCH_LEVEL. The driver was written on the assumption that the INF's
`EnablePreemption` 0 means the scheduler never asks.

It does ask. dxgmms2.sys of the board (22621.2428, the file and PDB in
`board-session-prep/wddm2x-stepF`), read from the disassembly:

- `VidSchiReadGlobalConfiguration` reads `EnablePreemption` into bit 0
  of the scheduler's flags (VIDSCH_GLOBAL +0x9e8).
- `VidSchiCheckPreemptionPolicy` tests that bit and, with it clear,
  refuses (reason 3). That is the normal priority preemption, and it is
  the only thing the registry value turns off.
- When the policy refuses, `VidSchiSwitchContextWithCheck` sets a
  deadline on the node (+0x80, and the context at +0x88).
- `VidSchiCheckHwProgress`, the TDR watchdog, checks each node's deadline
  first. When it has passed, `VidSchiNeedToForcePreemptNode` returns TRUE
  if the node still has packets in its hardware queue (+0xb48), no
  preemption is already in flight (+0xb50), and the running context is the
  same one (+0x60 = +0x88). The watchdog then calls
  `VidSchiSubmitPreemptionCommand`. The flag is not checked anywhere on
  that path, and `VidSchiSubmitPreemptionCommand` (which
  `VidSchiSwitchFromSuspendedDevices`, `VidSchiResetEngine` and
  `VidSchiPreemptEngineNodes` also call) does not check it either. It
  queues a preemption packet that ends in `DxgkDdiPreemptCommand`.
- `VidSchiCheckNodeTimeout` then times out the node when the packet at the
  head of its hardware queue does not change for `TdrDelay`. A preemption
  packet that is never answered is such a packet.

FurMark holds node 0 with long frames, and dwm wants it for every
composition. Each time a FurMark packet was still running when the switch
deadline passed, the request went unanswered and node 0 was reset about
2 s later. A short run can miss the window, which fits the 60 s run with
none. At 1.3 the same dxgmms2 code runs, so this is not WDDM 2.0's.

### The fix (branch `tdr-node0-notify`)

- `MwPreemptCommand` with `TimeSlice` 0 now answers. The request goes
  through `SyncPreempt` as with a time slice. No chain is soft-stopped
  (`MaybeStop` returns at `TimeSlice` 0), and the queue is not cut short:
  `Kick` answers only once nothing is running and nothing is queued. By
  then every DMA buffer submitted before the request has run to its end
  and been notified, so DMA_PREEMPTED carries `LastCompletedFenceId` =
  the last fence and nothing is handed back to be resubmitted. That is
  preemption at the DMA-buffer boundary, which is what the adapter
  reports (dxdiag: "Graphics Preemption: DMA"). dxgkrnl submits nothing
  on the node while its preemption packet waits, so the drain is bounded
  by what was already queued (a FurMark frame or two). With the driver
  idle, the request is answered on the spot.
- `TimeSlice` set: unchanged (soft-stop and resume).
- Setting `PreemptAnswer` (Device Parameters, default 1): 0 drops node-0
  requests at `TimeSlice` 0 as before, for an A/B run. Read at start,
  logged as `PreemptAnswer`.
- The copy node already answered its requests (wddm2x.c); it now counts
  them.

New snapshot fields (`Tdr<n>_*`):

| Field | Meaning |
|---|---|
| `PreemptCalls`, `PreemptAnswered` | node-0 PreemptCommand calls (with any setting) and node-0 DMA_PREEMPTED sent |
| `SincePreemptMs`, `LastPreemptFence` | the last node-0 request: its age and its fence |
| `CopyPreempts` | node-1 requests |
| `Node0Submits`, `Node0Notifies` | node-0 SubmitCommand calls queued, and node-0 DMA_COMPLETED sent |
| `S<k>Fence`, `S<k>Info`, `S<k>AgeMs`, `S<k>DoneMs` | the last eight node-0 submissions, k = 0 newest: fence; chains (bits 0-7), 0x100 present, 0x200 paging, 0x400 no private data, present kind in bits 16-23; ms since submitted; ms since notified complete (0xFFFFFFFF not notified) |

The other suspects from the same day, read through again in the source
and not the cause:

- Fences per node: node 0's come from `NotifyCompleted` (NodeOrdinal 0)
  and only for submissions of the node-0 queue. Node 1's come from
  `SyncCopyDone` (NodeOrdinal 1), with the fence dxgkrnl gave that
  submission. The two are separate sequences, as dxgkrnl numbers them per
  node, and the snapshots agree: 0x1f42 against 0xac4.
- Submissions completed without a notify: none. Presents, paging
  buffers, empty buffers and anything without our private data go through
  the same queue with `Count` 0. `Kick` completes them in order with a
  notify. UmdCopyBatch and UmdMergeSubmit only make fewer submissions in
  the UMD, and every one that reaches SubmitCommand is notified.
  `SubmitCommandVirtual` on node 0 goes through `MwSubmitCommand`.
- The clock switch between chains (`MwGpuClkBetweenChains`) runs inside
  `Kick` and does not touch the queue. `ClkPending` was 0 in all four.
- Flips: `FlipsUnreported` 0 and the vsync ran to the last moment. The
  flips stopped because FurMark's next frame was never submitted.

### Board test

Stage and reboot as usual (new test certificate per package). Fan as in
the failing run.

1. `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" /v PreemptAnswer`
   should read 1 (the trace value).
2. FurMark through the GL ICD, 1024x640, 702 MHz, 5 minutes, twice.
   Expected: no 4101, a score like the 60 s run's (338).
   `m5test prof 60` during the run (its "preemption:" line) or the
   `T_Preempt*` trace values after it: the request count is now above 0, which shows dxgkrnl asking with
   `EnablePreemption` 0. The longest wait for an answer (`T_PreemptUsMax`)
   is at most about one FurMark frame (200 ms), far below 2 s.
3. A/B, once: `reg add ...\Device Parameters /v PreemptAnswer /t REG_DWORD /d 0 /f`,
   reboot, 5 minutes FurMark. Expected: 4101s come back. In every `Tdr<n>_*`,
   `SincePreemptMs` should be about 2000 or more with `PreemptAnswered`
   unchanged, and `S0DoneMs` < `SincePreemptMs` < `S0AgeMs` when the
   request came during the last frame. Then delete the value and reboot.
4. If a 4101 still comes with `PreemptAnswer` 1, the new fields say where:
   `PreemptCalls` > `PreemptAnswered`, or `Node0Submits` against
   `Node0Notifies`, and the eight `S<k>*`.

## 702 MHz by default where the firmware set 775 mV (2026-11-25)

Branch `gpu702-default`, off `edge-wddm2` (which carries
`gpu-clk-safe-switch` and `main`). The user's decision of 2026-11-25:
with the temperature guard, the fault policy and the switch between job
chains in place, 702 MHz becomes the default, but only on a board whose
firmware has put `vdd_gpu_s0` at 775 mV.

- **`GpuClkHigh` not set** (no value in Device Parameters): the driver
  takes 1 (mainline's operating points) when the firmware's `GpuRail`
  report says the rail is valid and at least 775 mV, on a plain RK3576;
  otherwise 0. Only the firmware's reading counts here: `GpuRailUv` can
  still only lower the rail the checks use, never turn the default on.
  edk2-rk3576 537ea09 (on `main` since 2026-11-24) does the raise on the
  TaishanPi 3M only. The CM5-IO and the ROCK 4D leave BUCK5 at the PMIC's
  750 mV (or report nothing): no clock above 600 MHz there. With no
  `GpuClk` stored they now start at GPLL / 2 = 594 MHz instead of the
  firmware's GPLL / 6 = 198 MHz (the user's decision of 2026-11-25; until
  then a fresh install ran the GPU at 198 MHz unless `m5test clk 0 2 keep`
  had been run). 594 MHz is within mainline's 600 MHz operating point
  (700 mV), below the PMIC's 750 mV default.
- **`GpuClkHigh` 0** turns it off as before; 1 and 2 as before.
- **The start clock.** A stored `GpuClk` (`m5test clk <p> <d> keep`) wins
  whatever it is, so a board kept at 594 MHz (`m5test clk 0 2 keep`) stays
  at 594. With none stored and `GpuClkHigh` on (by default or set), the
  driver asks for SPLL / 1 = 702 MHz, else GPLL / 2 = 594 MHz; 702 only
  when the checks allow it:
  the supply meets what 702 needs and the temperature guard runs (TSADC
  started by the firmware). `GpuClkWrite` checks everything again and
  logs a refusal as before. `m5test clk forget` now means "back to the
  default" (702 MHz here) from the next start.
- **Protection, unchanged** (`gpu-clk-safe-switch`): 594 MHz at once at
  85 C, back after 90 s below 75 C (`GpuClkRestoreHoldSec`); a job fault,
  timeout or TDR above 600 MHz drops to 594 MHz and two within 10 minutes
  lock 594 MHz until the next start; every switch between job chains,
  one field per write; the adapter stopping drops to 594 MHz.
- **What tells them apart.** `m5test clk` prints "not set: the default"
  or "set in Device Parameters" after `GpuClkHigh` (escape `RailSource`
  bit 0x200); the registry trace gets `GpuClkHighDefault` (1 default, 0
  set) and, when the default clock is used, `GpuClkStartDefault` (0x301).

The decisions are `MwClkHighDefault` and `MwClkStartWant` in
`drivers/maliwddm/gpuclk_calc.h`, checked by `tests/gpuclk-host` (rails
from 0 to 900 mV, plain or not, every set value 0 to 5, the stored clock
winning).

Not run on a board yet. Board steps (TaishanPi 3M, firmware with 537ea09):

1. Before installing, delete `GpuClkHigh` and `GpuClk` from Device
   Parameters (the board has had `GpuClkHigh` 1 and `GpuClk` 3 1 since
   2026-11-24, which would hide the default), or keep them and check
   only that nothing else changed.
2. Install the package as usual (full restart). `m5test clk`: GpuClkHigh 1
   "not set: the default", supply 775000 uV from firmware, 702 MHz
   written by this driver, guard on.
3. `GpuClkHigh` 0, restart: GpuClkHigh 0 "set", 594 MHz written by this
   driver (no `GpuClk` stored), nothing above 600 MHz.
4. On a CM5-IO or a ROCK 4D, if one is at hand: GpuClkHigh 0 "not set",
   594 MHz written by this driver, nothing above 600 MHz.

