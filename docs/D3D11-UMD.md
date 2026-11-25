# The D3D11.1 user-mode driver: plan

Written 2026-11-21. Nothing here has run. It is the UMD half of the work
the user approved that day: D3D11 with the video DDI, then WDDM 2.x. The
KMD half is `docs/WDDM2.md` on branch `wddm2-design` (P0 to P7); where the
two meet is said below. Hardware video decode itself comes from the
VDPU383 work on woa-rk3576 branch `vdpu383`; this plan only gives it a
place to plug in.

## Where the UMD is today

- Mesa 26.2's d3d10umd frontend with Panfrost, patches 0001 to 0062. It
  offers the **D3D10.0 DDI only**: `State.h` has `SUPPORT_D3D10_1 0` and
  `SUPPORT_MSAA 0`, so the D3D10.1 interface numbers are compiled out of
  `OpenAdapter10`, `GetSupportedVersions` and `CreateDevice`, although the
  10.1 code behind them (the 10.1 function table, per-render-target blend
  in `CreateBlendState1`, cube-array views in `CreateShaderResourceView1`)
  is there. A `SUPPORT_D3D11` switch is 0 too, with only two interface
  numbers behind it.
- The D3D11 runtime drives it down-level at feature level 10_0. The board,
  2026-11-21: `d3dprobe` gives the Mali at 10_0, `ID3D11Device1` yes (the
  runtime offers that over any DDI), `ID3D11VideoDevice` no, NV12 not
  supported.
- DXGI is the 1.0/1.1 DDI. Without the 1.2 DDI there is no flip model, so
  every windowed present is read back on the CPU and blitted by the KMD
  (PERF.md, "Video in Edge"). That makes U2 a performance milestone too,
  not only a feature one.
- Feature level: the G52 has no tessellation and Panfrost has no D3D11
  compute model through this frontend, so 11_0 is out. **10_1 is the
  target, and the first priority (U0)**: it is what turns on Edge.

## Why Edge renders on WARP, and U0

The board's `edge://gpu` (2026-11-21, maliwddm 10.26.16.996): two GPU
process launches each failed with `eglCreateContext ES 3.0 failed with
error EGL_BAD_ATTRIBUTE. ES version fallback is disabled`, then Edge fell
back to SwANGLE; "D3D11 Feature Level: Unknown", "Software Rendering:
Yes". `GetGpuDriverOverlayInfo: Failed to retrieve video device` is the
missing `ID3D11VideoDevice` (U4).

ANGLE's rule, read from its source (google/angle main,
`src/libANGLE/renderer/d3d/d3d11/renderer11_utils.cpp`,
`GetMaximumClientVersion`): feature levels 12_1 to 10_1 give OpenGL ES
3.0; 10_0 gives ES 2.0 unless the D3D workaround `allowES3OnFL100` is on,
and `GenerateWorkarounds` sets it to false. Nothing else limits the
version: `getMaxSupportedESVersion` returns that value as it is. Chromium
asks for ES 3.0 and no longer falls back to 2.0. So a device at 10_0
cannot run Edge's GPU process at all, and one at 10_1 can.

**Try first, no code:** start Edge with ANGLE's workaround forced, which
gives ES 3.0 on our 10_0 device, and see whether everything after context
creation works on the Mali:

    "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" --enable-angle-features=allowES3OnFL100

(close every Edge window first, including the background ones, or the
flag is ignored; `edge://gpu` then shows it under "Command Line"). If the
GPU process stays up and GL_RENDERER names the Mali, U0 is the only
blocker; if it dies later, its log says on what.

**The no-code test, run on the board 2026-11-21** (maliwddm
10.45.38.341, still 10_0): the ES 3.0 context was created on the Mali, so
the feature level was the first blocker. The GPU process then died three
times (exit 0x80000003) at `skia_output_surface_impl.cc:1228` with
`color_type=4 backend_format.isValid()=0`, after many "CopySharedImage:
Dest shared image is not writable" and "Unable to initialize SkSurface".
Skia found RGBA8 not renderable. The chain, read from the three sources:

1. ANGLE's DXGI support table (`dxgi_support_table_autogen.cpp`) makes
   R8G8B8A8_UNORM always texturable, renderable and filterable at 10_0
   and 10_1; only `MULTISAMPLE_RENDERTARGET` is asked of the driver.
   (B8G8R8A8 asks the driver for everything; d3dprobe showed it fine.)
2. `GenerateTextureFormatCaps` gives a format sample counts, starting
   with 1x, only when it is a multisample render target. Our
   `CheckFormatSupport` never says so (`SUPPORT_MSAA 0`) and
   `CheckMultisampleQualityLevels` always answers 0, so every format has
   no sample counts, and `GL_NUM_SAMPLE_COUNTS` is 0 (`queryutils.cpp`).
3. Skia's `GrGLCaps::setupSampleCounts` on ES 3.0 fills a format's
   sample counts, 1x included, only when `GL_NUM_SAMPLE_COUNTS` is not 0.
   With none, the format is not renderable, `defaultBackendFormat` is
   invalid, and Chromium's compositor stops.

So it was our multisample answers, not the feature level, and `UmdDdi`
101 alone would very probably not have fixed it: ANGLE's table asks the
driver for multisampling at 10_1 exactly as at 10_0.

**U0a, Mesa patch 0064 (done on this branch, not run):** with `UmdMsaa`
1 (the default) `CheckFormatSupport` adds `MULTISAMPLE_RENDERTARGET` to
every render-target format and `CheckMultisampleQualityLevels` answers 1
for one sample, 0 for more. ANGLE then gives each format the count {1},
Skia sees RGBA8 renderable, and nothing is ever rendered multisampled.
`UmdMsaa` 4 also offers 4x where Panfrost can render and sample it, and
`ResolveSubresource` now resolves on the GPU with a blit (it did nothing
before); 0 is the old behaviour. This is also what makes the no-code
flag worth trying again on the current feature level.

**On the board, 2026-11-21, 0064 broke DWM:** with `UmdMsaa` 1 in every
process, dwm.exe's render thread failed at start (dwmcore.dll, exception
0x8898008d, UCEERR_RENDERTHREADFAILURE; n=2 boots, Windows Update
servicing on both), the logon screen spun, and `m5test perf` showed no
flips, no renders and no UMD reports: DWM never got a working device on
the Mali. With `UmdMsaa` 0 the desktop came (n=1). Not established which
answer DWM's device fails on. Candidates: a format claiming multisample
rendering with no count above one, which the runtime's format checks may
hold inconsistent, or a format (64- and 128-bit, integer, depth) that
Panfrost reports renderable but D3D does not allow to be multisampled.

**U0b, Mesa patch 0065 (not run):** the answers are given only where they
are needed and checked:
- only in msedge.exe, chrome.exe, msedgewebview2.exe and d3dprobe.exe
  (`UmdMsaa` 1 and 4); 2 and 5 give them to every process, for trying;
- only for R8G8B8A8, B8G8R8A8 and B8G8R8X8, UNORM and SRGB;
- one quality level at one sample for those, and only when they are
  render targets; 4x only with `UmdMsaa` 4 or 5.
DWM, LogonUI and everything else see the answers they had before 0064.
d3dprobe prints the multisample bit and the quality levels at 1x to 8x
for each format, and since it is on the list it shows whether the
runtime still creates the device with them.

**On the board, 2026-11-22, 0065:** DWM fine (logon screen, desktop,
flips scanned out, GPU jobs; n=1). Edge with the ES3 flag: ANGLE's
`eglInitialize: No available renderers`, three times, then software; its
D3D11 renderer did not come up at all. 0064 in DWM and 0065 in Edge
failed the same way, with no device, and share one thing: the
multisample render target bit with no sample count above one. Before
0064 there was neither, and every device came up.

**U0c, Mesa patch 0066 (not run):** the bit is given only where 4x is
real: every format Panfrost renders at 4x is a multisample render target
(and multisample load where it samples 4x), and answers one quality level
at 1x and at 4x, still only in the browsers and d3dprobe. The G52 has no
2x (`pan_screen.c`: 2x only from arch 12), and ANGLE's loop asks for 2x
first and stops, so ANGLE keeps the count {1}: nothing in Edge renders
multisampled, and Skia sees RGBA8 renderable. `UmdMsaa` 4 and 5 now mean
the same as 1 and 2. If the runtime still refuses the device, the bit
was not the cause, and d3dprobe says so before Edge is tried.

**U0, the smallest change to 10_1 (Mesa patch 0063, done on this
branch, not run):** `SUPPORT_D3D10_1 1`, with the D3D10.1 interface
offered only where the KMD's answer allows it (`UmdDdi` 101; the default
100 keeps the D3D10.0 DDI): `OpenAdapter10`, `GetSupportedVersions` and
`CreateDevice` check it. The D3D11 runtime then creates devices at 10_1.

What 10_1 asks for that this does not yet do, in the order it matters to
ANGLE's ES 3.0:

| 10_1 requirement | State | Matters for ES 3.0 |
|---|---|---|
| D3D10.1 DDI entry points (blend per target, cube-array views, `ResourceConvert`) | present in the frontend, now compiled in | yes |
| 4x MSAA on render-target formats, `CheckMultisampleQualityLevels`, `ResolveSubresource` | `SUPPORT_MSAA 0`; resolve does nothing | ES 3.0 wants `GL_MAX_SAMPLES` of 4; ANGLE does not lower the version for it, but Chromium's MSAA paths and WebGL antialias will fail or fall back. Next step (U0b): `SUPPORT_MSAA 1`, resolve through `pipe->blit`, behind `UmdMsaa` |
| Shader model 4.1 opcodes `lod`, `gather4`, `sample_pos`, `sample_info` | parsed, `TGSI_LOG_UNSUPPORTED` (`ShaderTGSI.c:175-178`) | not used by ES 3.0 shaders (gather is ES 3.1); map to TGSI `LODQ`, `TG4`, `TXQS` and a sample-position table in U0b |
| Cube map arrays | resource target already chosen (`Resource.cpp:290`) | not an ES 3.0 feature |
| Format support for 10_1 (BGRA as render target, R10G10B10A2, R11G11B10) | as at 10_0 | ANGLE builds its format table from `CheckFormatSupport`; missing formats only remove formats |

Test for U0a and U0, in this order:
1. U0a alone (the package's defaults: `UmdMsaa` 1, `UmdDdi` 100): Edge
   with `--enable-angle-features=allowES3OnFL100`. Expected: no FATAL
   at `skia_output_surface_impl.cc`, `edge://gpu` "Software Rendering:
   No", GL_RENDERER ANGLE on the Mali with `vs_4_0 ps_4_0`.
2. U0: `reg add "...\Device Parameters" /v UmdDdi /t REG_DWORD /d 101 /f`,
   end every Edge process (or reboot), then Edge without any flag:
- `d3dprobe`: the Mali at feature level **10_1**.
- `edge://gpu`: GL_RENDERER `ANGLE (Rockchip, ... Direct3D11 vs_4_1 ps_4_1,
  D3D11 ...)`, "D3D11 Feature Level: D3D 10.1", "Software Rendering: No",
  no "ES version fallback is disabled" in the log.
- `m5test perf` during YouTube: an msedge.exe line with GPU work.
- `UmdDdi` 100 (or deleting it) goes back.

## How the KMD tells the UMD what to offer

At `OpenAdapter10_2` the UMD has the adapter callbacks, not a device, so
the escape the shim uses later is not available. It asks with
`pfnQueryAdapterInfoCb` for `KMTQAITYPE_UMDRIVERPRIVATE`, which maliwddm
answers with `MW_UMD_ADAPTER_INFO` (`include/maliwddm_abi.h`, done on this
branch: `UmdAdapterInfo` in `adapter.c`). It carries
the highest DDI the UMD may offer, the feature level, whether the video
DDI is exposed, the node ordinal of a decode
engine if there is one, and whether the adapter runs WDDM 2.x in virtual
mode. The KMD fills it from DWORD settings in Device Parameters, so each
step can be turned back without reinstalling:

| Setting | Default | Meaning |
|---|---|---|
| `UmdDdi` | 101 (since 2026-11-25; was 100) | 100: the D3D10.0 DDI (feature level 10_0); 101: the D3D10.1 DDI (10_1), board-tested on the TaishanPi 3M; 110: D3D11.0; 111: D3D11.1 (not offered) |
| `UmdDxgi12` | 0 until U3 has run | the DXGI 1.2 DDI and flip model |
| `UmdVideo` | 0 | 0 none; 1 the video DDI with no profiles; 2 with the decoder's profiles |
| `UmdMsaa` | 1 | 0: no multisampling, as before 0064; 1: every format Panfrost renders at 4x is a multisample render target at 1x and 4x, in the browsers and d3dprobe only (0066; ANGLE uses 1x); 2: the same in every process; 4 and 5: as 1 and 2 |
| `UmdFeatureLevel` | 0xa000 (10_0) | the most the UMD reports |
| `UmdRedirect` | 1 at WDDM 2.x, 2 at 1.3 | which processes get DXGI redirection by default (patch 0077): 1 none; 2 the browsers (0076); 3 every process. `C:\mali\redirect` and `C:\mali\noredirect` override it. EDGE-PRESENT.md, "The WDDM 2.0 session hang" |
| `UmdReportFL11` | 0 | 1: Chromium's GPU process (msedge.exe, chrome.exe, msedgewebview2.exe with `--type=gpu-process`) gets the D3D11.1 DDI at feature level 11_0, shader model 5 (the subset FXC emits for ANGLE's ES 3.0 shaders), no hull, domain or compute shaders; with it, `UmdVideo` 1 and 2 apply to that process too (patch 0080, CHROMIUM-VIDEO.md C1). Read at each adapter query: new GPU processes see a change without a reboot |

A KMD that does not answer the query means `UmdDdi` 100 (an older KMD), so a new UMD on
an old KMD behaves as today. The structure only grows at the end and says
its own size.

**For the `wddm2` branch:** keep `DXGKQAITYPE_UMDRIVERPRIVATE` answered
with this structure, and set `VirtualMode` to 1 once the adapter runs in
virtual mode (WDDM2.md P2), and `DecodeNode` to the ordinal of the decode
node once it exists (U5). The UMD picks the D3DWDDM2_0 interface from
`VirtualMode`, not from the WDDM version, so P1 (2.x in physical mode)
changes nothing for it.

## Milestones

Each is a branch commit series, built by CI, installable on its own, and
has a board test. Times are for one person; they add up to the 12 to 24
person-weeks WDDM2.md gives option (c), decode excluded.

### U1: the D3D11.0 DDI at feature level 10_1 (2 to 4 weeks)

- `OpenAdapter10_2` accepts `D3D11_0_DDI_INTERFACE_VERSION` and the 11.0
  build numbers; `GetSupportedVersions` lists them when `UmdDdi` allows.
- A `D3D11DDI_DEVICEFUNCS` table: the D3D10.1 entries where the signature
  is the same, the 11.0 forms where it changed (`CreateResource2`,
  shaders with signatures, `CheckFormatSupport`, `CalcPrivate*Size`), and
  the 11.0 stages (hull, domain, compute, UAVs, deferred contexts) as
  entries that set `E_NOTIMPL`; at 10_0 the runtime does not call them.
- `GetCaps`: threading caps with no deferred-context support, the 10_1
  feature level, the formats as today.
- Test: `d3dprobe` shows the Mali at 10_1 as after U0, and "D3D11.1 DDI
  seen" still 0 0. The UMD log (`C:\mali\log`) says it opened the 11.0 interface.
  dwm, the desktop, FurMark through the GL ICD, `gsso.exe hw` and the
  triangle tests behave as on the previous package. `UmdDdi` 101 restores
  the old interface.

### U2: the D3D11.1 DDI (2 to 4 weeks)

- `D3D11_1_DDI_INTERFACE_VERSION`, and `D3DWDDM1_3_DDI_INTERFACE_VERSION`
  for the WDDM 1.3 KMD we have, with `D3D11_1DDI_DEVICEFUNCS`: the `*1`
  forms of copy and update (flags), `DiscardResource` and `DiscardView`,
  `Flush1`, blend and rasterizer state 1, `SetConstantBuffers` with
  offsets (or the cap off), `CheckDirectFlipSupport`.
- Test: `ID3D11Device1` is not the check: the D3D11.1 runtime offers it
  over today's D3D10.1-DDI driver too (board, 2026-11-21: "yes"). The
  check is d3dprobe's "D3D11.1 DDI seen" line, `discard 1, copy/update
  flags 1`, which the runtime sets only for a D3D11.1-DDI driver, 0 0
  today. In `edge://gpu`, GL_RENDERER is ANGLE on the Mali at 10_1. The
  same regression set as U1.

### U3: DXGI 1.2 and flip model (2 to 3 weeks)

- `DXGI1_2_DDI_BASE_FUNCTIONS`: `Present1` with dirty rects, `Blt1`,
  `OfferResources` and `ReclaimResources`, `GetMultiplaneOverlayCaps`
  answering none. Back buffers of a flip-model swap chain are shared
  resources on runtime allocations (the path LogonUI already uses), so a
  present hands DWM the buffer and nothing is read back.
- Also implements `pfnBlt` (stretch and format conversion on the GPU
  through the blitter) and `ResolveSubresource`, which are counted as not
  carried out today.
- Test: `d3dprobe`, run in the user's session (not over SSH, where every
  swap chain fails with 0x887a0022, no interactive desktop), creates a
  `FLIP_SEQUENTIAL` swap chain and presents with `0x00000000`. During YouTube, `m5test perf` shows `present, read
  back` at about 0 for msedge.exe and `KMD blt presents` at about 0 MB,
  and stats-for-nerds dropped frames well below the 40% of 2026-11-21.
  `UmdDxgi12` 0 brings the read-back present back.

### U4: video DDI scaffolding (2 to 3 weeks)

**2026-11-24:** superseded in part by [VIDEO-DECODE.md](VIDEO-DECODE.md)
(branch `video-decode`): the D3D11.1 interface is offered only in the
processes `UmdVideo` names, instead of after U1 and U2 for every process,
and the video DDI with H.264 is written (milestone V3a). The decode node
of U5 becomes V3b there.

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

- Decided 2026-11-21: decode lives inside maliwddm, because Edge decodes
  only on its D3D11 device. maliwddm takes the VDPU383's MMIO, interrupt
  and IOMMU from ACPI (resources on GPU0 or a device it also drives, as
  the VOP2 already is) and gains a second node,
  `DXGK_ENGINE_TYPE_VIDEO_DECODE`, with its own queue and fences, next to
  the Mali's node 0. The standalone KMDF driver on woa-rk3576 `vdpu383` is
  the bring-up rig (register replay, checking the MPP-based builder, a
  console harness); its register-builder library is reused here.
- Where the register building happens: in the UMD, from the DXVA picture
  parameters and slice control the runtime hands `SubmitDecoderBuffers`,
  as MPP builds them in user mode on Linux. The UMD submits on node 1 one
  private command holding the register image and the allocations it
  references (bitstream, the output and reference NV12 surfaces, the
  scratch buffers), all in the allocation list. The KMD only patches
  addresses into the image (IOVAs of the decoder's IOMMU, from the same
  table code as scanout.c), checks that every address it writes belongs to
  an allocation in the list, writes the registers and completes the fence
  from the interrupt. The KMD never parses a bitstream, so a broken stream
  cannot do more than a wrong picture or a timeout.
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
