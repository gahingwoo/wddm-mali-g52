# Documents

| File | What it is | State |
|---|---|---|
| [PLAN.md](PLAN.md) | The goal, the milestones M0 to M5, and the risks | original plan, kept as written |
| [M2-PLAN.md](M2-PLAN.md) | Why Mesa's D3D10 frontend came before any WDDM code | carried out |
| [M3.md](M3.md) | Design of `malikm` and the shim under Panfrost | current |
| [M5-PLAN.md](M5-PLAN.md) | First plan for the WDDM driver, with sources | superseded after M5.1 |
| [M5.2-PLAN.md](M5.2-PLAN.md) | The full WDDM driver in steps (a) to (d) | carried out; step (d) reached 2026-11-13 |
| [PERF.md](PERF.md) | Where a frame's time goes and how to get it back | measured 2026-11-14; scan-out on by default since. The 2026-11-23 board numbers are in the README and on branch `merge-submit-default` |
| [HDMI-HPD.md](HDMI-HPD.md) | HDMI hot-plug, the driver's own mode set and HDMI audio | works on the TaishanPi 3M (`HdmiHpd` 3), 2026-11-22 and 2026-11-23 |
| [PERFDATA.md](PERFDATA.md) | GPU temperature and clock for Task Manager and the "RK3576 GPU" performance counters: what Task Manager and dxgkrnl ask, what the driver answers, the WDDM 2.4 probe | branch `gpu-perfdata`, 2026-11-24, not run on the board |
| [DISPLAY-MODES.md](DISPLAY-MODES.md) | Display resolutions: smaller source modes scaled by the VOP2, the switches, the board test, and a design for retiming HDMI from Windows | branch `display-modes`, built, not run on the board |
| [EDGE-CREATEDEVICE.md](EDGE-CREATEDEVICE.md) | Why Edge could not create a D3D11 device on the Mali, and the fix | Edge gets the Mali at 10_1 on the board |
| [EDGE-PRESENT.md](EDGE-PRESENT.md) | Edge's device removed at its first present; three UMD bugs | fixes merged; presenting to a composition swap chain still blocked |
| [D3D11-UMD.md](D3D11-UMD.md) | Plan for the D3D11.1 user-mode driver | plan, 2026-11-21 |
| [FEASIBILITY-D3D11-WDDM2.md](FEASIBILITY-D3D11-WDDM2.md) | What Direct3D 11 and WDDM 2.x would take | study, 2026-11-13 |
| [WDDM2.md](WDDM2.md) | WDDM 2.x and 3.x design study: GpuMmu on the Mali MMU, options, phased plan | study 2026-11-21, plan approved; implementation on `wddm2` (progress log, UMD contract) |
| [BOARD-DAY.md](BOARD-DAY.md) | Checklist for the October 2026 board sessions | historical |
| [results/](results/README.md) | What was measured, one folder per milestone | |

The code refers to `docs/M5.2-PLAN.md` by path, so the plans stay where they
are.
