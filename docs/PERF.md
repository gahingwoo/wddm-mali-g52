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

So the rate is now set by a 27 ms copy that spans two vsyncs, plus the wait
for the GPU (a few to 25 ms a present). Stage 2 below removes the copy.

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

## Also run that day

`gsso.exe hw nogs` on the Mali with patches 0041 to 0044: creating a
geometry shader, with or without stream output, returns `E_OUTOFMEMORY`;
the stream-output queries read zero; `DrawAuto` draws nothing; an ordinary
draw is right; the device is not removed; 0 FAIL. Before these patches
each of the creates called a NULL function pointer.
