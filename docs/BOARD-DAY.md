# Next time the board is on

Everything below is built by CI and waiting for hardware. In this order,
because each step is the gate for the next. All three drivers bind the same
device (ACPI\RKCP7402); only one can be installed at a time.

## Before Windows

- Firmware: the `cm5io-mali-m1` branch of edk2-rk3576 (powers PD_GPU, adds
  GPU0 to the DSDT). Already on eMMC p1 since 2026-10-01; check the build
  stamp before trusting that.

## 1. M3.1: malikm on its own

Stick layout and script: `tools/windows/m3-test.cmd` (it removes m1probe
first). Artifacts: `malikm-arm64`, `m3test-arm64`,
`mesa-d3d10umd-panfrost-arm64`.

- `m3test` must print PASS: WRITE_VALUE on slot 0 and slot 1, WAIT_BO, and
  1000 jobs with the time per job.
- If not: `m3-out.txt` has the driver's registry values (`Step`,
  `JobsFailed`, `JobsTimedOut`, `LastJsStatus`, `LastMmuFault`,
  `LastFaultAddress*`, `Irqs`). `Irqs` = 0 with jobs done means the
  interrupt lines are not reaching us (polling still works).

## 2. M3.2 and M4: Mesa on malikm

Same script, continued: the triangle with `GALLIUM_DRIVER=panfrost`, again
with `PAN_MESA_DEBUG=sync`, then `present.exe`.

- Success is the triangle's `centre=0xff0000ff ... corner=0xffff0000 -> PASS`
  on Panfrost: the first triangle on a Mali under Windows.
- `present.exe` should flash red, green, blue in a small window.
- A GPU fault under `sync` names the job; save the output.

## 3. M5.1: malidod

Remove malikm (`pnputil /delete-driver <oemN.inf> /uninstall`), install
`malidod-arm64`. Basic Display should hand over the framebuffer.

- Success: the desktop still shows, and `Device Parameters` has
  `Started` = 1, the framebuffer size, and a growing `Presents`.
- If the screen goes black, the registry tells how far it got
  (`AcquirePostDisplay`, `CommitStatus`). Recovery: boot to safe mode or
  remove the driver from WinPE.

## After

- Linux: `grubby --remove-args="modprobe.blacklist=panfrost regulator_ignore_unused"`
  once the GPU work on Linux is done, and check a normal Linux boot with
  Panfrost on the `cm5io-mali-m1` firmware before that branch is merged.
