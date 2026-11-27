# The Direct3D user-mode driver

The UMD is Mesa's d3d10umd frontend with Panfrost and the patches in
[mesa-patches/](../mesa-patches/). Settings: see
[SETTINGS.md](SETTINGS.md).

## What it offers

| Interface | Feature level | Who gets it |
|---|---|---|
| D3D10.0 DDI | 10_0 | every process with `UmdDdi` 100 |
| D3D10.1 DDI | 10_1 | every process with `UmdDdi` 101 (the default) |
| D3D11.1 DDI with the video DDI | 10_1 | only the processes `UmdVideo` names ([VIDEO-DECODE.md](VIDEO-DECODE.md)) |
| D3D11.1 DDI | 11_0, shader model 5 without hull, domain or compute shaders | only Chromium's GPU process (`--type=gpu-process`) with `UmdReportFL11` 1 ([CHROMIUM-VIDEO.md](CHROMIUM-VIDEO.md)) |

The D3D11 runtime drives the D3D10.x DDIs down-level. `ID3D11Device1`
is always there (the runtime offers it over any DDI); d3dprobe's "D3D11.1
DDI seen" line (`discard`, `copy/update flags`) shows whether the
runtime is really using the D3D11.1 DDI.

Feature level 11_0 for everything is out of reach: the G52 has no
tessellation, and Panfrost has no D3D11 compute model through this
frontend. 10_1 is what ANGLE needs for OpenGL ES 3.0, and so what Edge
needs ([EDGE-CREATEDEVICE.md](EDGE-CREATEDEVICE.md)).

Not implemented:

- `UmdDdi` 110 and 111 (D3D11.0 and D3D11.1 for every process). The UMD
  treats any value of 101 or more as 101.
- DXGI 1.2 DDI (`Present1` with dirty rects, `Blt1`, Offer/Reclaim).
  Without it, windowed presents are read back on the CPU and blitted by
  the KMD ([PERF.md](PERF.md)). `UmdDxgi12` is reserved for it and not
  read by the UMD.
- Shader model 4.1 opcodes `gather4`, `sample_pos` (and `lod`,
  `sample_info`) are parsed and logged as unsupported. ES 3.0 shaders do
  not use them.
- The D3DWDDM2_0 interface (GPU virtual addresses, MakeResident with a
  paging fence, monitored fences). The UMD keeps the WDDM 1.3 submit path
  at WDDM 2.0 in physical mode ([WDDM2.md](WDDM2.md)).

## How the KMD tells the UMD what to offer

At `OpenAdapter10_2` the UMD has the adapter callbacks but no device. It
calls `pfnQueryAdapterInfoCb` with `KMTQAITYPE_UMDRIVERPRIVATE`, and
maliwddm answers `MW_UMD_ADAPTER_INFO` (`include/maliwddm_abi.h`,
`UmdAdapterInfo` in `drivers/maliwddm/adapter.c`) from DWORDs in Device
Parameters. It reads them at each query, so most changes reach processes
started afterwards without a reboot or reinstall.

| Field | From | Meaning |
|---|---|---|
| `Magic` | | `MW_UMD_INFO_MAGIC`, 0x3149574D ('MWI1') |
| `Size` | | bytes filled in; fields are only added at the end |
| `Ddi` | `UmdDdi` | highest DDI: 100 D3D10.0, 101 D3D10.1, 110 D3D11.0, 111 D3D11.1 (`MW_UMD_DDI_*`) |
| `FeatureLevel` | `UmdFeatureLevel` | a D3D_FEATURE_LEVEL, default 0xa000; not read by the UMD |
| `Dxgi12` | `UmdDxgi12` | DXGI 1.2 DDI; not read by the UMD |
| `Video` | `UmdVideo` and, when it is not 0, `UmdVideoCodecs` (bits 8 to 15), `UmdAv1Synth` (bit 16), `UmdVp9Mpp` (bits 18 to 19), `UmdVideoZeroCopy` 0 (bit 20) | the video DDI ([VIDEO-DECODE.md](VIDEO-DECODE.md)) |
| `DecodeNode` | | `MW_UMD_NO_NODE` (0xFFFFFFFF): no decode engine node |
| `VirtualMode` | | 0: physical addressing. 1 would select D3DWDDM2_0 |
| `KmdBuild` | | the CI run that built the KMD |
| `Msaa` | `UmdMsaa` | who gets multisample answers ([EDGE-CREATEDEVICE.md](EDGE-CREATEDEVICE.md)) |
| `Redirect` | `UmdRedirect` | who gets DXGI redirection: 1 none, 2 browsers, 3 all; default 3 at WDDM 2.x and 2 at 1.3; 0 from an older KMD ([EDGE-PRESENT.md](EDGE-PRESENT.md)) |
| `ReportFl11` | `UmdReportFL11` | 1: feature level 11_0 for Chromium's GPU process |
| `Reserved[4]` | | zero |

A KMD that does not answer the query means `UmdDdi` 100, so a new UMD on
an old KMD offers the D3D10.0 DDI. On the CI runner, with no maliwddm,
the environment variables `D3D10UMD_UMDDDI`, `D3D10UMD_UMDMSAA`,
`D3D10UMD_UMDVIDEO` and `D3D10UMD_UMDFL11` stand in for the settings.
They are never read when the KMD answered.

The UMD log (`C:\mali\log`, [EDGE-PRESENT.md](EDGE-PRESENT.md#umd-log))
prints what it got: `OpenAdapter: KMD adapter info 0x..., magic ...,
UmdDdi ..., UmdMsaa ..., ... offering D3D10.<n>`.

## Checking it on the board

- `d3dprobe`: the Mali's feature level (10_1 by default), whether
  `ID3D11VideoDevice` exists, NV12 support, "D3D11.1 DDI seen".
- `edge://gpu`: GL_RENDERER `ANGLE (... Direct3D11 vs_4_1 ps_4_1, D3D11
  ...)`, "D3D11 Feature Level: D3D 10.1".
- `m5test perf`: per-process GPU work and present paths.
- To go back: `UmdDdi` 100 (or delete it), then restart the processes or
  reboot.

## Remaining work

- D3D11.0 and D3D11.1 DDIs for every process (`UmdDdi` 110, 111), with
  the 11.0 stages answered `E_NOTIMPL`.
- DXGI 1.2 and flip model (`UmdDxgi12`), plus `pfnBlt` on the GPU. This
  removes the CPU read-back of windowed presents.
- D3DWDDM2_0 once the KMD runs in virtual mode: residency per device and
  VAs from `pfnReserveGpuVirtualAddressCb` / `pfnMapGpuVirtualAddressCb`
  instead of the KMD's fixed addresses. The UMD picks it from
  `VirtualMode`, not from the WDDM version.
- A decode engine node (`DecodeNode`) if decode moves off the
  synchronous escape it uses now.
