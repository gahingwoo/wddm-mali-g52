# Direct3D 11 and WDDM 2.x: feasibility (superseded)

A study from 2026-11-13, written from source and documentation before any
of it was tried. The WDDM part is replaced by [WDDM2.md](WDDM2.md) and the
Direct3D part by [D3D11-UMD.md](D3D11-UMD.md). The conclusions that still
hold:

- **D3D11 does not need WDDM 2.0.** Every D3D11 feature level is supported
  on WDDM 1.x; Microsoft's render-only sample pairs a `D3DWDDM1_3` UMD with
  a 1.3 kernel driver.
- **Hardware limits of the G52:** no geometry or tessellation hardware, so
  geometry, hull and domain shaders need compute emulation (Mesa's
  `src/poly`, used so far only by Vulkan drivers). Panfrost forbids memory
  writes in vertex shaders, so full 11_1 (UAVs in every stage) is not
  reachable without running vertex shaders as compute.
- **Shader translation:** TGSI cannot carry SM5 and Mesa is removing it
  (upstream also removed the d3d10umd frontend in 2026-09). SM5 needs DXBC
  to NIR, directly or through SPIR-V.
- **PanVK plus DXVK is not a route on Bifrost:** PanVK on arch 6 and 7
  loads only with `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER`, reports Vulkan 1.0
  without geometry or tessellation shaders, and DXVK requires both.
- **Which programs need which level:** DWM and Direct2D run at 10_0. ANGLE
  gives WebGL 2 from 10_1. Windows Terminal's D3D backend needs 11_0 (else
  it uses Direct2D). Media playback needs the D3D11 video DDI. Most current
  games want 11_0.
- **WDDM 2.x does not explain slowness:** the time went to CPU copies
  ([PERF.md](PERF.md)).
