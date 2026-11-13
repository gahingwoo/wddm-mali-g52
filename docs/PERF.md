# Where a frame's time goes, and the plan to get it back

Written 2026-11-13 from the code. Nothing here has been measured on a
board yet; the branch `perf-primary` adds the timers that will measure it.
Line numbers are those of `main` at `0a31314` and Mesa with patches up to
0040.

The desktop runs at 1 to 2 flips a second with content on screen and 5 to 6
when idle, at 2560x1440. One frame is 14.7 MB.

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

## Stage 1, on the branch `perf-primary`, not yet run on a board

- Timers: the UMD logs microseconds per stage per present every 5 s
  (`perf:` lines); the kernel driver keeps `T_FlipCopyUs*`, `T_FlipWaitUs*`,
  `T_JobUs*`, `T_VsyncGapUs*`; `m5test perf [seconds]` prints rates.
- The buffer Panfrost renders into is the primary: it is allocated through
  the runtime the way a shared surface is, and present hands it over with
  no pixel copy. The kernel driver does not change.
- `RotateResourceIdentities` exchanges buffers and re-points the views on
  them instead of copying contents.

That removes steps 2, 3, 4 and 7 if it holds. Step 6 stays.

Switches, read once per process, each restoring the old behaviour when the
file exists:

| File | Effect |
|---|---|
| `C:\mali\primary-copy` | copy the frame into a separate primary |
| `C:\mali\rotate-copy` | rotate by copying; implies `primary-copy` |
| `C:\mali\present-wait` | wait for the GPU before presenting a rendered-into primary |

What only hardware can answer: whether the scheduler holds a flip until the
render that precedes it has finished (if not, half-drawn frames, and
`present-wait` is the fallback); whether the runtime re-binds views after a
rotation; the real vsync period.

## Stage 2, design only: scan out the primary

Step 6 goes away when the display controller reads the primary directly.

A flip is two writes to VOP2 (`0x27D00000`): `ESMART0 YRGB_MST` (`0x1814`)
with the primary's physical address, then `REG_CFG_DONE` (`0x000`). The
shadow registers latch at frame start, so the flip does not tear. A real
vsync interrupt exists (VP0, SPI 379).

What stands in the way:

- `YRGB_MST` is a 32-bit physical address and no IOMMU is on. The primary's
  pages today are scattered and can lie above 4 GB. Either the VOP IOMMU is
  programmed from the same page list the Mali MMU gets, or primaries come
  from a contiguous pool below 4 GB. Both change the kernel driver's memory
  model.
- The VOP registers belong to the `DSP0` ACPI device, not the GPU, and the
  interrupt was left out of its description after bugchecks in WinPE.
- Nothing in Windows keeps the VOP clocks and power domain on.
- On this SoC, reading VOP2 registers and incremental writes have killed
  the HDMI signal before. The flip must be write-only.
- The bugcheck screen and the hand-back to the firmware display must point
  `YRGB_MST` at the firmware framebuffer again.
