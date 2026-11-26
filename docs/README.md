# Documents

| File | What it covers |
|---|---|
| [SETTINGS.md](SETTINGS.md) | Every registry switch, with its default and meaning |
| [M3.md](M3.md) | Mesa and Panfrost on Windows, `malikm`, the shim, GPU bring-up, tests without a board |
| [M5.2-PLAN.md](M5.2-PLAN.md) | `maliwddm`'s memory and submission model |
| [WDDM2.md](WDDM2.md) | WDDM 2.0: how it works, the fallback to 1.3, debugging a failed start |
| [D3D11-UMD.md](D3D11-UMD.md) | What the Direct3D UMD offers and how the KMD tells it |
| [PERF.md](PERF.md) | The present path, the GPU clock, the thermal guard, TDRs |
| [PERFDATA.md](PERFDATA.md) | GPU temperature and clock in Task Manager, the "RK3576 GPU" counters |
| [HDMI-HPD.md](HDMI-HPD.md) | HDMI hot-plug, the driver's mode set, HDMI audio, trace values |
| [DISPLAY-MODES.md](DISPLAY-MODES.md) | Resolutions and refresh rates |
| [DUAL-DISPLAY.md](DUAL-DISPLAY.md) | Panel and HDMI at once (`HdmiExtend`); board test in [Chinese](DUAL-DISPLAY-TEST.zh.md) |
| [EDGE-CREATEDEVICE.md](EDGE-CREATEDEVICE.md) | Edge and Chrome creating a D3D11 device on the Mali |
| [EDGE-PRESENT.md](EDGE-PRESENT.md) | Edge and Chrome presenting; the UMD log |
| [VIDEO-DECODE.md](VIDEO-DECODE.md) | D3D11 video decode on the VDPU383 |
| [CHROMIUM-VIDEO.md](CHROMIUM-VIDEO.md) | What Chrome and Edge need for hardware decode |
| [VULKAN.md](VULKAN.md) | Vulkan through panvk: ICD registration, the board test (branch `vulkan-panvk`, CI only) |
| [OPENCL.md](OPENCL.md) | OpenCL through rusticl on Panfrost: the ICD, the board test (branch `opencl-rusticl`, CI only) |
| [FEASIBILITY-D3D11-WDDM2.md](FEASIBILITY-D3D11-WDDM2.md) | Superseded study, conclusions only |
| [history.md](history.md) | How it got here, a few lines per topic |
| [results/](results/README.md) | Board results, one folder per milestone |
