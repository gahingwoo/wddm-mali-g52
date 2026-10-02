# M5.2 step (b): a job through WDDM, 2026-10-02

**Passed** in WinPE (22621) on the CM5-IO, run 15
([output](m5.2-pe-run15-PASS.txt)): `tools/windows/m5test.c` created a device,
a context and one 4 KB allocation through the D3DKMT system calls, wrote M1's
WRITE_VALUE job into it, rendered one `MW_CMD_JOB`, and read back 0xC0FFEE52
written by the Mali. maliwddm: one aperture map (0x2000000, 1 page), one
interrupt, job done, no fault, no reset.

## Configuration that passed

- maliwddm as a **full** WDDM 1.3 adapter on GPU0 (`ACPI\RKCP7402`), alone:
  WinPE has no DWM, so the display half's missing Present does not matter.
  WARP named as UMD (copied into WinPE, which lacks it).
- Firmware: edk2-rk3576 `cm5io-mali-m1` with DSP0 (`ACPI\RKCP7403`) first and
  carrying the VOP2 registers, no interrupt (unverified on main; not merged).
- Test from `mali\m5-pe-autorun.cmd` on the woa-rk3576 WinPE stick.

## What it took (runs 1-15)

| Run | Failure | Cause / fix |
|---|---|---|
| 1-3 | code 43 after DRIVERCAPS | no user-mode driver named; run 2 was also the old image (`restart-device` keeps a loaded driver) |
| 4 | Windows stops at boot | full driver may not leave target-mode frequencies unspecified (KMDOD must) |
| 5 | SYSTEM_SERVICE_EXCEPTION | my trace wrapper treated OpenAllocation's hDevice as the adapter |
| 6 | desktop and RDP black | WARP cannot render for this KMD; DWM gave up |
| 7, PE 2-11 | render-only code 43 | dxgkrnl ETW: STATUS_GRAPHICS_INVALID_DRIVER_MODEL, adapter type 0x9 (render + POST), whatever the DSDT, class or caps; render-only abandoned |
| PE 5 | malidod no framebuffer | DSP0 not the POST device until it came first with resources (firmware) |
| PE 6 | malidod presents fail | CDD presents from kernel memory; probed as user memory |
| PE 1-12 | m5test never ran | WinPE gdi32 forwards D3DKMT to dxcore, absent; resolve NtGdiDdDDI* from win32u |
| PE 13-14 | job faults at its own address | the allocation was never mapped: WDDM 1.x VidMm pages in what the **patch list** references, and it was empty; Render now emits one patch location per allocation |

Instruments that made it tractable: the per-DDI `Enter_`/`Order_` registry
trace with flushes, the stats escape, and dxgkrnl's own ETW events recorded
with `etwrec` and decoded on Linux (`tools/etw`).

## Open

- WHEA_INTERNAL_ERROR at WinPE shutdown on the DSP0 firmware (every run since
  DSP0 got resources, with or without its interrupt).
- The eMMC Windows install no longer boots (spinner); parked.
- Next: step (c), the UMD (Panfrost behind a WDDM `pan_kmod` backend).
