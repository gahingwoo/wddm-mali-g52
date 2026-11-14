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
- It is off after every boot. `m5test scan 1` turns it on, `m5test scan 0`
  goes back to the copy, `m5test scan 2` also turns translation off, and
  `m5test scan` reports.

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

