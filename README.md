# wddm-mali-g52

A Windows (WDDM) driver for the Arm Mali-G52 GPU in the Rockchip RK3576.

The Mali-G52 has run its first job under Windows (M1, 2026-10-01). Nothing
is drawn on it yet. Windows on RK3576 boards
([woa-rk3576](https://github.com/gahingwoo/woa-rk3576)) draws its desktop on the
CPU today: there is no GPU driver, so DWM renders through WARP into the
framebuffer the firmware leaves behind. No Windows driver for any Mali GPU
exists as far as we can find; Arm and the SoC vendors ship Linux and Android
drivers only.

## Plan

The user-mode half comes from Mesa: the Panfrost Gallium driver and its
Bifrost shader compiler, under Mesa's D3D10 user-mode driver frontend. The
kernel-mode half is new: a WDDM driver that powers the GPU, manages its MMU,
submits jobs and scans out the desktop.

Milestones, the reasoning behind them, and the risks:
[docs/PLAN.md](docs/PLAN.md).

| | Milestone | State |
|---|---|---|
| M0 | Feasibility checks | done, see the plan |
| M1 | Mali runs a job under Windows (plain KMDF driver) | **done**: [result](docs/results/m1-windows-2026-10-01.txt) |
| M2 | WDDM driver skeleton, D3D runtime loads the UMD | next |
| M3 | Clear and copy | |
| M4 | First triangle | |
| M5 | DWM composites on the Mali | |

## Hardware

| | |
|---|---|
| GPU | Mali-G52 (Bifrost, job manager), `rockchip,rk3576-mali` |
| Registers | `0x27800000`, 128 KiB |
| Interrupts | job SPI 347, MMU SPI 348, GPU SPI 349 (ACPI GSIV 379, 380, 381) |
| Power | domain `PD_GPU`; supply `vdd_gpu_s0`, RK806 DCDC5 |
| Clock | `CLK_GPU`, set through TF-A's SCMI service on Linux |

Developed on the ArmSoM CM5-IO with firmware from
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576).

## License

GPL-2.0, see [LICENSE](LICENSE). Code taken from Mesa keeps its own licence,
mostly MIT.
