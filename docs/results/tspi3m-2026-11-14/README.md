# 2026-11-14: from a frame or two a second to a desktop that keeps up

Board: LCKFB TaishanPi 3M (RK3576, 4 GB), Windows 11 23H2 on the eMMC,
2560x1440 over HDMI, firmware edk2-rk3576 `00b82c4`. Everything here ran on
that board that day, in the order given. The measurements and how to repeat
them are in [docs/PERF.md](../../PERF.md); this page is what was found.

![The desktop, scanned out by the VOP2](desktop-scanned-out.jpg)

## Speed

`dwm.exe` with a window being dragged, presents in 5 s:

| Change | Presents in 5 s |
|---|---|
| as on 2026-11-13 | 41 to 45 |
| the rotate exchanges buffers instead of copying them | 35 to 46 |
| Panfrost renders into the primary | 46 to 55 |
| the copy to the framebuffer on a thread, not in the vsync DPC | 63 to 92 |
| the VOP2 scans out the primary; no copy | 135 to 268 |

Two of these did nothing by themselves, and that was the finding. The
rotate cost 76 ms a present and removing it left the rate where it was,
because the rate was being set somewhere that had not been measured: the
copy to the firmware framebuffer in the timer's DPC, which halved the
emulated vsync whenever something moved.

Scan-out uses the VOP2's own IOMMU, so a primary's pages stay wherever
VidMm put them. It is off after every boot; `m5test scan 1` turns it on.

## Memory

`m5test mem`, new that day, gives GPU memory by process.

- Panfrost makes a 128 MB tiler heap for every screen. The frontend has
  made a screen for every Direct3D device since the explorer fix of
  2026-11-13, and the shim allocates the heap whole. `dwm.exe` held
  171.6 MiB and `LogonUI.exe` 163.5, 128 of each the heap.
- With 32 MB, `dwm.exe` held 119.2 MiB, `explorer.exe` (five devices) 210,
  and 24408 jobs ran with none failed. 32 MB is now the default.
- Committed memory on the desktop went from 4760 MB to 3233 MB between the
  morning and the afternoon. That is one comparison, with different things
  open.

## Hitches

The desktop stalled for a moment now and then. A log of every operation
over 20 ms (`C:\mali\slow`) said what it was.

- Panfrost asks with a timeout of 0 whether a buffer is busy before it
  reuses one from its cache. The shim ignored the timeout and waited for
  the whole queue with its process-wide lock held: 38 waits of up to 95 ms
  in 605 s of `dwm.exe`, nearly all for a 64 KB pool buffer. With the
  timeout kept there were none in 67 s. Kept by default now.
- Left over 20 ms: creating shaders when a process starts (40 to 50 ms
  each, no cache on disk), and `copy-region`, which Panfrost does on the
  CPU (22 to 26 ms each at this size). Neither is fixed.

## Bugs found on the way

1. **The elevation prompt crashed, so nothing that needs elevation could
   start.** `consent.exe` and `LogonUI.exe` create a device, destroy it and
   unload the driver. The exception handler the UMD's log installs was
   never removed, and the next exception ran unloaded code
   (`maliumd.dll_unloaded` in the Application log). The DLL now pins itself
   in the process. Introduced on 2026-11-13 with the crash reporter.
2. **A page fault in the VOP2's IOMMU, and a screen of flickering lines.**
   Releasing a primary pointed the window back at the framebuffer and
   cleared the primary's entries at once, but the window moves at the next
   frame start. Found by ending `dwm.exe` a dozen times in a row; a sign-out
   or a shutdown takes the same path. The entries now stay until the slot
   is reused. After the fix: three restarts of `dwm.exe`, a sign-out, and a
   restart of Windows with scan-out on, no fault.
3. **My own:** ending `dwm.exe` repeatedly to rename a log directory made
   Windows give up restarting it (seven restarts), and the session had to
   be signed out. The log is now turned off with a file.

## The driver as a package

For the first time the kernel driver and the UMD were installed from one
signed package (`wddm-mali-g52-driver-arm64` from CI), with the DLL copied
by setup. `Get-AuthenticodeSignature` on the installed DLL says Valid, and
Windows now shows a chip type and a DAC type for the adapter.

## What dxdiag says

Feature levels 10_0, 9_3, 9_2, 9_1; driver model WDDM 1.3; 0 MB of display
memory and 1024 MB shared; DirectDraw acceleration not available; the
monitor is "Generic Non-PnP Monitor".

## Not done, and known wrong

- The monitor's EDID does not reach Windows. The firmware side
  (edk2-rk3576 branch `acpi-edid`) and the driver side are written; the
  firmware has not been flashed, and the driver behaves as before without
  it.
- One display mode, the firmware's. Smaller modes scaled by the VOP2 are
  designed and sit behind a setting that is off; one scaler register's
  reset value is not known, and nothing is written to it yet.
- Scan-out is not on by default, the vsync is still a timer, and display
  off and on, sleep and the bugcheck screen have not been tried with it.
- x86 and x64 programs under emulation: whether they can use the Mali has
  not been looked at. GPU-Z starts and shows nothing, which is its own
  hardware database and kernel driver, not something this driver decides.
- `copy-region` on the CPU, and no shader cache.
- Geometry shaders and stream output fail cleanly instead of crashing;
  they are still not implemented.
- Not run on the CM5-IO since 2026-10-17.
