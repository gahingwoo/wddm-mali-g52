# Plan

## Goal

Make DWM composite on the Mali-G52 so the Windows desktop on RK3576 stops
being drawn by the CPU. Running D3D applications on the GPU comes with that.

## Prior art

- **Phones running Windows** (Renegade Project, Snapdragon 845) have a
  working GPU because Qualcomm ships Adreno drivers for Windows on Snapdragon
  laptops. They reuse a vendor driver; nobody wrote one.
- **No Mali driver for Windows** turned up in any search. Panfrost and Panthor
  are Linux only.
- **Triton** (UTM, 2026) is a D3D11 WDDM driver for QEMU guests. Its
  user-mode driver forwards D3D calls to the host, so it compiles no shaders
  and drives no real GPU. Its authors say the WDDM documentation left gaps
  they filled from VirtualBox's driver.

## What Microsoft documents

The kernel-mode DDI (`d3dkmddi.h`, `DxgkDdiSubmitCommand` and the rest) is
documented, and WDDM 1.2 defines render-only and display-only drivers. The
user-mode D3D DDIs are documented at reference level only. Open-source
implementations to learn from: Mesa's D3D10 UMD frontend, VirtualBox's WDDM
driver, the Venus and Triton Windows drivers.

## Architecture

```
D3D11 runtime, feature level 10_0
  UMD   Mesa d3d10umd        DXBC SM4 -> TGSI
        Mesa Panfrost        TGSI -> NIR -> Bifrost compiler, command streams
        pan_kmod backend     new: WDDM instead of Linux DRM
  KMD   full WDDM driver     new: power, clocks, GPU MMU, job manager,
                             interrupts and fences, memory segments, TDR;
                             display: inherit the firmware's mode, flip by
                             changing the VOP2 window address
```

### Why the driver has to drive the display as well

A render-only Mali next to BasicDisplay would not speed up the desktop. DWM
composites on the display adapter, so it would keep using WARP. Windows'
hybrid mode does not help either: it is defined for an integrated GPU that
displays plus a discrete render-only GPU, both full WDDM drivers with
cross-adapter resource support, and BasicDisplay is neither.

The display half can stay small. The firmware has already set the mode, so
the driver reports that one mode and flips by pointing a VOP2 window at a new
primary allocation, with VOP2's vsync interrupt for timing. It never touches
the HDMI controller or the PHY PLL, which is where the firmware's display
bring-up spent weeks. VOP2 takes 32-bit addresses only, so primaries must be
allocated below 4 GB.

## M0 findings

1. **Mesa removed d3d10umd on 2026-09-30** (commit `1f9af627b1`, MR 44717):
   it still produced TGSI and nobody maintained it. Mesa 26.2.0 is the last
   release that has it. Pin 26.2, or carry the frontend in this repo; either
   way TGSI may follow it out of Mesa later.
2. **Panfrost can be built for Windows only with patches.** Meson's Windows
   default leaves it out, `-Dgallium-drivers=panfrost` can force it in, and
   `pan_bo.c`, `pan_device.c` and `pan_resource.c` include `xf86drm.h`
   directly and use dma-buf file descriptors and `mmap`.
3. **The kernel interface is already abstracted.** `pan_kmod_ops` has about
   20 operations (device, BO alloc/free/import/export/wait, VM, ...). The
   Linux Panfrost backend is about 1000 lines; a WDDM backend should be of the
   same order.
4. **GPU clock.** Linux sets `CLK_GPU` through TF-A's SCMI service (SMC
   `RK_SIP_SCMI_AGENT0`, PVTPLL on RK3576). For M1 the firmware sets clock and
   power domain before Windows starts, and the driver only uses them.
5. **Licence.** GPL-2.0 for this repo, so the Linux Panfrost kernel driver may
   be followed closely. Mesa code keeps MIT.

## M1: Mali runs a job under Windows

A plain KMDF driver, not WDDM yet, bound to a new ACPI device in the firmware.

1. Firmware: power `PD_GPU`, set `CLK_GPU`, publish the GPU in the DSDT
   (registers and the three interrupts).
2. Driver: soft-reset the GPU, read `GPU_ID` (expect a G52), power up the
   shader cores and L2.
3. Build one address space: a page table for MMU AS0 mapping a few pages.
4. Submit a WRITE_VALUE job on job slot 0 and wait for its interrupt.
5. Check that the value landed in memory.

Done when step 5 passes on hardware. Before writing the Windows driver, the
same sequence can be tried from Linux userspace through `/dev/mem` with the
Panfrost kernel driver unbound, where a mistake is cheaper to debug.

## M1 status (2026-10-01)

Done on Linux and on Windows. `tools/m1/m1_raw.c` runs a WRITE_VALUE job from
Linux userspace with no GPU driver loaded; `drivers/m1probe` does the same in a
KMDF driver on Windows 11 23H2, with edk2-rk3576 `778163b` powering the GPU and
publishing it as `ACPI\RKCP7402`. Both write 0xC0FFEE42 with no MMU fault; on
Windows the page tables and job sat above 4 GB (0x1_2E41A000). The sequence:

1. Supply: `vdd_gpu_s0` (RK806 DCDC5) must be on. Linux switches unused
   regulators off 30 s after boot, so Panfrost-less Linux needs
   `regulator_ignore_unused`; the firmware boots with it on.
2. Clock: `CLKSEL_CON(165)` = GPLL / 6 (198 MHz). The reset default selects
   AUPLL.
3. Power domain: ungate `CLK_GPU` and `PCLK_GPU_ROOT` (`CLKGATE_CON(69)` bits
   1, 3, 8) and the PMU clock ungate (0x140 bit 0); clear PMU `PWR_CON1` bit 9;
   wait for status bit 25 clear and repair bit 25 set; clear `REQ0` bit 0 and
   wait for `ACK0`/`IDLE0` bit 0 clear. Touching any GPU register before the
   ACK clears is an SError.
4. GPU: soft reset, then power the L2, tiler and shader cores.
5. MMU AS0: `TRANSTAB` = level-0 table | 0x7, `MEMATTR` = 0x888d88,
   `TRANSCFG` = 0, then `UPDATE`. Mali LPAE: tables `pa | 3`, leaves
   `pa | 0x2C5` (type 1 even at level 3, no AF).
6. Job slot 0: head, affinity = shader cores, config = AS 0 | priority 8 |
   flush on start and end, then START.

On Windows the firmware does steps 1 to 3 and the driver steps 4 to 6.

## Risks, in order

1. The display half: VOP2 flips from Windows, and how DWM's primary surfaces
   map onto them.
2. Feature level 10_0 is assumed to be enough for DWM. Not verified.
3. WDDM memory management (paging, residency, preemption, TDR) is large and
   hard to debug without a working display.
4. Keeping a frontend Mesa has dropped.
