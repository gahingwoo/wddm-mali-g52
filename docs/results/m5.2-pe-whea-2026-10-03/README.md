# WinPE shutdown WHEA_INTERNAL_ERROR: not maliwddm (2026-10-03)

Every WinPE run of `m5-pe-test.cmd` that finished its tests ended in a
`WHEA_INTERNAL_ERROR` bugcheck at `wpeutil shutdown`, 5 of 5. The bugcheck
followed maliwddm into every run, so maliwddm was the first suspect. It
turned out to be the firmware's USB-C setup.

## What the bugcheck said

`kd-listen.py` (edk2-rk3576 `scripts/`, from `898c4c1`) now holds the
target at a bugcheck. It reads the register context and 48 KiB of stack,
then lets the target continue.

- `0x122 (9, 0x11, 0, 0)`: an uncorrected error from an invalid error
  source. Its type, 0x11, is `WheaErrSrcTypeSea`, a synchronous external
  abort. There is no HEST, so WHEA has no source to file it under.
- ESR `0x96000210`: a data abort at EL1, synchronous external abort, read.
  FAR is a device mapping + `0x430`, which is an xHCI port-2 `PORTSC`.
  The abort record also carries `0x23000000`, XHC0's base address.
- The stack runs `UsbHub3 -> ucx01000 -> USBXHCI`. maliwddm's image range
  appears nowhere on it.

## Cause

On 2026-10-01, edk2-rk3576 `5edeab0` enabled DRD0's U3 port at
ExitBootServices so that Windows would start XHC0 (USB-C). That fixed a
code 10. Nothing ever brought up the USB/DP combo PHY behind the port,
though. When Windows suspended the root hub, usbxhci read the SS port's
`PORTSC`, and the bus answered with an external abort.

| Firmware | XHC0 | Runs | WHEA |
|---|---|---|---|
| 652af82, fbc4eff | visible, U3 port on, no PHY | 5 | 5 |
| 52e19ac (test: XHC0 `_STA=0`) | hidden | 2 | 0 |
| 111f23f, a545f4c (USBDP USB3 bring-up) | visible | 5 | 0 |

Things that did not work, or did not apply:
- Setting the DWC3 `SUSPHY` bits to Linux's values (fbc4eff) changed nothing.
- `pnputil /disable-device ACPI\PNP0D10\0` is refused in WinPE ("critical
  system device"). That run is `fbc4eff-pnputil-refused.txt`.

## USB-C after the fix

The USBDP LCPLL lock depends on how a dock is plugged in when all four
lanes are muxed to USB:
- normal orientation: it locks; XHC0 is Started and the dock's hub
  enumerates at high speed only (`a545f4c-dock-normal.txt`);
- flipped: 3 boots of 3 did not lock (`0x38`), the firmware leaves the U3
  port off, and XHC0 shows code 10 with no bugcheck.

edk2-rk3576 `1fc5c03` reads the plug orientation from the board's FUSB302
and maps the lanes to match. It is untested.

## Separate finding: the two job faults per present.exe

Every present.exe run also showed two failed jobs: read translation faults
(`0x4002c3`), always at the same VA. The shim's `GEM_CLOSE` destroyed the
WDDM allocation immediately, while panfrost/Mesa expect the kernel to keep
a BO alive until its jobs are done. Fixed in `win32-compat/malikm_drm.c`
(deferred destroy) and not yet re-run.

## Files

One `m5-pe-out.txt` per run, named `<firmware>-<condition>.txt`.
