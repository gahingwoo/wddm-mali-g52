# The D3D11.1 user-mode driver: plan

Written 2026-11-21. Nothing here has run. It is the UMD half of the work
the user approved that day: D3D11 with the video DDI, then WDDM 2.x. The
KMD half is `docs/WDDM2.md` on branch `wddm2-design` (P0 to P7); where the
two meet is said below. Hardware video decode itself comes from the
VDPU383 work on woa-rk3576 branch `vdpu383`; this plan only gives it a
place to plug in.

## Where the UMD is today

- Mesa 26.2's d3d10umd frontend with Panfrost, patches 0001 to 0062. It
  answers `OpenAdapter10_2` with the D3D10 and D3D10.1 DDI only
  (`Adapter.cpp:113-131`). A `SUPPORT_D3D11` switch exists in `State.h`
  and is 0; behind it there are only the two interface numbers, no table.
- The D3D11 runtime drives a D3D10.1 DDI driver down-level, so D3D11
  applications get feature level 10_0 today, but no `ID3D11Device1`, no
  `ID3D11VideoDevice` and no DXGI 1.2 (`d3dprobe`, branch `present-fast`).
- DXGI is the 1.0/1.1 DDI. Without the 1.2 DDI there is no flip model, so
  every windowed present is read back on the CPU and blitted by the KMD
  (PERF.md, "Video in Edge"). That makes U2 a performance milestone too,
  not only a feature one.
- Feature level: the G52 has no tessellation and Panfrost has no D3D11
  compute model through this frontend, so 11_0 is out. 10_0 is the
  target; 10_1 (cube arrays, per-sample shading, 4x MSAA everywhere) is a
  stretch to try once U2 runs.

## How the KMD tells the UMD what to offer

At `OpenAdapter10_2` the UMD has the adapter callbacks, not a device, so
the escape the shim uses later is not available. It asks with
`pfnQueryAdapterInfoCb` for `KMTQAITYPE_UMDRIVERPRIVATE`, which maliwddm
answers with a small versioned structure (new, `include/maliwddm_abi.h`):
the highest DDI the UMD may offer, the feature level, whether the video
DDI is exposed and which decode profiles, the node ordinal of a decode
engine if there is one, and whether the adapter runs WDDM 2.x in virtual
mode. The KMD fills it from DWORD settings in Device Parameters, so each
step can be turned back without reinstalling:

| Setting | Default | Meaning |
|---|---|---|
| `UmdDdi` | 10 until U1 has run, then 11, then 111 | 10: D3D10.1 DDI as today; 11: D3D11.0; 111: D3D11.1 |
| `UmdDxgi12` | 0 until U3 has run | the DXGI 1.2 DDI and flip model |
| `UmdVideo` | 0 | 0 none; 1 the video DDI with no profiles; 2 with the decoder's profiles |
| `UmdFeatureLevel` | 0xa000 (10_0) | the most the UMD reports |

A KMD that does not answer the query means `UmdDdi` 10, so a new UMD on
an old KMD behaves as today.

## Milestones

Each is a branch commit series, built by CI, installable on its own, and
has a board test. Times are for one person; they add up to the 12 to 24
person-weeks WDDM2.md gives option (c), decode excluded.

### U1: the D3D11.0 DDI at feature level 10_0 (2 to 4 weeks)

- `OpenAdapter10_2` accepts `D3D11_0_DDI_INTERFACE_VERSION` and the 11.0
  build numbers; `GetSupportedVersions` lists them when `UmdDdi` allows.
- A `D3D11DDI_DEVICEFUNCS` table: the D3D10.1 entries where the signature
  is the same, the 11.0 forms where it changed (`CreateResource2`,
  shaders with signatures, `CheckFormatSupport`, `CalcPrivate*Size`), and
  the 11.0 stages (hull, domain, compute, UAVs, deferred contexts) as
  entries that set `E_NOTIMPL`; at 10_0 the runtime does not call them.
- `GetCaps`: threading caps with no deferred-context support, the 10_0
  feature level, the formats as today.
- Test: `d3dprobe` shows the Mali at 10_0 as before, `ID3D11Device1` still
  no. The UMD log (`C:\mali\log`) says it opened the 11.0 interface.
  dwm, the desktop, FurMark through the GL ICD, `gsso.exe hw` and the
  triangle tests behave as on the previous package. `UmdDdi` 10 restores
  the old interface.

### U2: the D3D11.1 DDI (2 to 4 weeks)

- `D3D11_1_DDI_INTERFACE_VERSION`, and `D3DWDDM1_3_DDI_INTERFACE_VERSION`
  for the WDDM 1.3 KMD we have, with `D3D11_1DDI_DEVICEFUNCS`: the `*1`
  forms of copy and update (flags), `DiscardResource` and `DiscardView`,
  `Flush1`, blend and rasterizer state 1, `SetConstantBuffers` with
  offsets (or the cap off), `CheckDirectFlipSupport`.
- Test: `d3dprobe` says `ID3D11Device1 (D3D11.1 DDI): yes`. In
  `edge://gpu`, GL_RENDERER is ANGLE on the Mali and the "D3D11 feature
  level" line reads 10_0. The same regression set as U1.

### U3: DXGI 1.2 and flip model (2 to 3 weeks)

- `DXGI1_2_DDI_BASE_FUNCTIONS`: `Present1` with dirty rects, `Blt1`,
  `OfferResources` and `ReclaimResources`, `GetMultiplaneOverlayCaps`
  answering none. Back buffers of a flip-model swap chain are shared
  resources on runtime allocations (the path LogonUI already uses), so a
  present hands DWM the buffer and nothing is read back.
- Also implements `pfnBlt` (stretch and format conversion on the GPU
  through the blitter) and `ResolveSubresource`, which are counted as not
  carried out today.
- Test: `d3dprobe` creates a `FLIP_SEQUENTIAL` swap chain and presents
  with `0x00000000`. During YouTube, `m5test perf` shows `present, read
  back` at about 0 for msedge.exe and `KMD blt presents` at about 0 MB,
  and stats-for-nerds dropped frames well below the 40% of 2026-11-21.
  `UmdDxgi12` 0 brings the read-back present back.

### U4: video DDI scaffolding (2 to 3 weeks)

- `D3D11_1DDI_VIDEODEVICEFUNCS` filled in with `UmdVideo` 1 or 2:
  - `GetVideoDecoderProfileCount`, `GetVideoDecoderProfile`,
    `CheckVideoDecoderFormat`, `GetVideoDecoderConfigCount`,
    `GetVideoDecoderConfig`: from the KMD's list (H.264 VLD no-FGT first,
    then HEVC Main, VP9 profile 0 and AV1 profile 0 as the VDPU383 gains
    them). With `UmdVideo` 1 the list is empty, which Chromium treats as
    software decode, so turning it on cannot break playback.
  - `CreateVideoDecoder` and the output view: created, and any
    `DecoderBeginFrame` fails with `E_NOTIMPL` until U5.
  - The video processor entries enumerate one processor that converts
    NV12 to B8G8R8A8 on the GPU (a fragment shader through the blitter),
    which Chromium also uses for its overlay path.
- NV12 resources: Panfrost's planar NV12, with shader views of plane 0 as
  R8_UNORM and plane 1 as R8G8_UNORM, render targets the same, and
  `CheckFormatSupport(NV12)` reporting texture2d, sample, render target,
  decoder output.
- Test: `d3dprobe` says `ID3D11VideoDevice: yes`, prints the profile count
  (0 with `UmdVideo` 1) and NV12 as supported. `edge://gpu` still says
  "Video Decode: Software only" with `UmdVideo` 1. YouTube plays as in
  U3.

### U5: the decode hook (design now, code with the decoder)

- maliwddm gains a second node, `DXGK_ENGINE_TYPE_VIDEO_DECODE`, whose
  submissions carry one private command: the decoder's picture
  parameters, slice control, bitstream and output allocations, as the
  DXVA buffers the runtime hands `SubmitDecoderBuffers`. The UMD passes
  them on unparsed with the allocations in the list; the KMD hands them to
  the VDPU383 backend (the MPP-based register builder of branch
  `vdpu383`), which programs the block and signals the node's fence.
  Whether the backend lives in maliwddm or is the standalone decoder
  driver reached through a KMD-to-KMD interface is the open question to
  settle with that work.
- The output is an NV12 allocation the Mali samples (U4), so the decoded
  picture never touches the CPU. The VDPU383 has its own IOMMU, as the
  VOP2 has (scanout.c): the same table-building code, a third client.
- Test, once decode exists: `UmdVideo` 2, `d3dprobe` lists H.264,
  `edge://gpu` says "Video Decode: Hardware accelerated", and YouTube
  forced to avc1 plays 1080p with no dropped frames and the CPU idle.

### U6: the D3DWDDM2_0 interface (with WDDM2.md P2 and later)

- On a KMD that reports WDDM 2.x in virtual mode (the query above says
  so), the UMD offers `D3DWDDM2_0_DDI_INTERFACE_VERSION`: no allocation or
  patch lists, `pfnSubmitCommandCb` with GPU virtual addresses,
  `pfnMakeResidentCb` and `pfnEvictCb` with a paging fence instead of the
  shim listing every live buffer, monitored fences for waits, and
  `pfnReclaimAllocations3Cb`. In physical mode (WDDM2.md P1) it keeps the
  1.3 path.
- The shim's buffer model changes most: residency per device, VAs from
  `pfnReserveGpuVirtualAddressCb` and `pfnMapGpuVirtualAddressCb` instead
  of the KMD's fixed addresses.
- Test: as U3, on the `wddm2` KMD, plus Task Manager's GPU page showing
  per-process dedicated and shared memory.

## Order and dependencies

U1, U2, U3 and U4 need nothing from the KMD but the adapter-info query,
which is a few lines in maliwddm. U5 needs the decoder and the second
node. U6 needs WDDM2.md P2 to P4. U3 is the one users will feel first.
