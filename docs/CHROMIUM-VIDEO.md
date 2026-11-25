# Hardware video decode for Chrome and Edge: what it takes

Written 2026-11-25 (offline, nothing here has run on the board). The
first player to make work is web video in Chrome and Edge (YouTube,
Bilibili), ahead of Media Player and mpv. This says what the hardware
does, what Chromium asks of a D3D11 driver, what blocks it today, and the
order of the work. The design of the decoder itself is in
[VIDEO-DECODE.md](VIDEO-DECODE.md).

## 1. What the RK3576 decodes, and where

One block, **RKVDEC**, called VDPU383 by Rockchip, at 0x27B00000 (64 KiB),
interrupts `irq_rkvdec_dec` and `irq_rkvdec_mmu` (TRM V1.2 Part 1, the
address map and interrupt table). JPEG has a block of its own.

| Codec | The hardware | Mainline Linux | Rockchip MPP | Here |
|---|---|---|---|---|
| H.264 (up to High, 8 bit) | yes | yes (`rkvdec-vdpu383-h264.c`) | yes | **decoded bit-exact on the board** (vdpu383test, 2026-11-24); D3D11 decoder in the UMD (V3a), not on the board yet |
| HEVC Main, Main 10 | yes | yes (`rkvdec-vdpu383-hevc.c`) | yes | register builder and parser written (woa-rk3576 `vdpu383-hevc`), 75 conformance streams pass off-board checks; not on the board |
| VP9 profile 0, 2 | yes | **no** (the VP9 driver in rkvdec is RK3399's) | yes (`hal_vp9d_vdpu383.c`) | not started |
| AV1 Main | yes | **no** (no AV1 for the RK3576 at all; the RK3588's AV1 decoder is a separate Verisilicon block the RK3576 does not have) | yes (`hal_av1d_vdpu383.c`) | not started |

Sources: mainline `drivers/media/platform/rockchip/rkvdec/rkvdec.c`
(`rockchip,rk3576-vdec` -> `vdpu383_variant`, coded formats HEVC_SLICE
and H264_SLICE only); MPP `osal/mpp_soc.c` ("rk3576 has codec: RK
H.264/H.265/VP9/AVS2/AV1 8K decoder", `vdpu383`, `cap_10bit`, `cap_8k`)
and `vdpu383_init_ctrl_regs()` (dec_mode 0 HEVC, 1 H.264, 2 VP9, 3 AVS2,
4 AV1), whose VP9 and AV1 HALs name only `ROCKCHIP_SOC_RK3576`. The
public TRM V1.2 does not describe the decoder (Part 2's codec chapter is
JPEG and the encoder): which codecs it has rests on Rockchip's own
software, not on a register manual. So VP9 and AV1 have one reference
(MPP) and no second one to check it against.

## 2. What Chromium asks of a D3D11 decoder

From Chromium's `media/gpu/windows` (main, read 2026-11-25). Chrome and
Edge share it.

| Step | Where | What it needs |
|---|---|---|
| Device | `d3d11_video_decoder_backend.cc` | **the ANGLE device** (the GPU process renders and decodes on one device) and `GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0`; below that, no decoder is offered at all (`kUnsupportedFeatureLevel`) |
| Interfaces | same, `d3d11_video_decoder_wrapper.cc` | `ID3D11VideoDevice1`, `ID3D11VideoContext1` |
| Profiles | `supported_profile_helpers.cc`, `d3d_decoder_configurator.cc` | `GetVideoDecoderProfileCount/Profile`; H.264 `{1b81be68-...}` (H264_VLD_NOFGT), HEVC `..._HEVC_VLD_MAIN/MAIN10`, VP9 `..._VP9_VLD_PROFILE0`, `..._VP9_VLD_10BIT_PROFILE2`, AV1 `DXVA_ModeAV1_VLD_Profile0`. H.264 is listed up to 1920x1088 whatever the driver says |
| Sizes | same | `GetVideoDecoderConfigCount` at growing resolutions with NV12 (P010 for 10 bit): the largest with a config is the limit |
| Config | `d3d11_video_decoder_wrapper.cc` | `ConfigBitstreamRaw` **2** for H.264 (short slices), **1** for HEVC, VP9, AV1 |
| Output texture | `d3d_decoder_configurator.cc` | NV12 (8 bit) or P010 (10 bit) array, `BIND_DECODER | BIND_SHADER_RESOURCE`, MiscFlags `SHARED | SHARED_NTHANDLE` (the shared image path, the default), else `SHARED` |
| Display | `d3d11_texture_selector.cc` | the planes as R8/R8G8 (R16/R16G16) shader resource views, of the texture or of it opened from its NT handle; or a copy through the video processor |
| Process | Chromium's sandbox | all of it runs in the **GPU process**: low integrity, restricted token |

What the board adds: Edge's GPU process works today only with maliwddm
on WDDM 1.3 (the fix for 2.0 waits for a board test), so every step here
is planned and tested on WDDM 1.3 first.

## 3. What blocks it today

Measured by `vdectest --chromium` (tools/windows/vdectest/chromeprobe.cpp)
in mesa.yml on softpipe; the result of the first run is in section 5.

1. **Feature level.** This UMD's devices are 10_1 (D3D11-UMD.md: the G52
   has no tessellation, and this frontend has no compute shaders, so 11_0
   was ruled out). Chromium will not decode on it. Nothing else matters
   until this is solved.
2. **Sandbox.** The V3a backend opens `\\.\VDPU383` from the process that
   decodes. For Chromium that is the GPU process: it needs vdpu383.sys
   `UserAccess` 2 (now also granting RESTRICTED and ALL APPLICATION
   PACKAGES, woa-rk3576 `vdpu383-d3d11va` 5b913dd), which lets any
   low-integrity process program the decoder. A test board only.
3. **Shared NV12.** Chromium's decoder texture is shared through an NT
   handle and sampled as planes, possibly through the opened copy. The
   plane views of V4 are textures of the Resource that made them; a
   Resource opened from the handle has none, and would not see decodes.
4. **Codecs.** YouTube sends Chrome VP9, or AV1 when the decoder says it
   has AV1, and H.264 only when neither is decoded in hardware (or with an
   extension such as h264ify). Bilibili sends H.264 by default, HEVC or
   AV1 when chosen. H.264 alone covers Bilibili's default and not
   YouTube's.

## 4. The order of the work

| | What | Needs | State |
|---|---|---|---|
| C0 | the facts above, and the probe | | done (this branch) |
| C1 | **feature level 11_0 for Chromium's GPU process only**: the 11.1 DDI already there, the device reports 11_0, hull, domain and compute shader creation fail as geometry shaders fail today; offered only where the process is `msedge.exe`/`chrome.exe` with `--type=gpu-process`, behind a new maliwddm switch (default 0), like `UmdVideo` | a check that ANGLE stays on OpenGL ES 3.0 (it offers 3.1 only with compute), and that the D3D11 runtime accepts 11_0 from this DDI; WDDM 1.3 | written: `UmdReportFL11`, Mesa 0080, `fl11test`; CI only, not on the board (section 6) |
| C2 | decoding in the GPU process: V3a with `UserAccess` 2 on the test board; for real, V3b (decode work through maliwddm, which the sandboxed process already reaches through dxgkrnl) | V3b of VIDEO-DECODE.md | V3a written |
| C3 | Chromium's textures: NV12 one allocation, the plane views aliasing it (also what V3b's zero copy needs), so an opened copy and the decoder see the same pixels; then the probe passes | C1 for the device | not started |
| C4 | H.264 in Edge on the board: `edge://media-internals` says D3D11VideoDecoder | C1, C2, C3 | |
| C5 | VP9 profile 0 (YouTube), from MPP: register builder, the four frame contexts, compressed header probability deltas, backward adaptation from the counts the decoder writes; the libvpx test vectors in vdpu383test first | | not started |
| C6 | HEVC Main in the UMD (Bilibili, `ConfigBitstreamRaw` 1); AV1 Main (YouTube's first choice), from MPP's VDPU383 AV1 HAL; P010 for 10 bit | the HEVC board test (woa README section 5) | HEVC builder written |

C1 is the step that decides whether Chrome and Edge are reachable on
this GPU at all. If the D3D11 runtime or ANGLE does not accept a device
at 11_0 without compute shaders, the remaining way is compute shaders in
the D3D11 frontend (Panfrost has them for OpenGL ES 3.1), which is weeks
of work before any video.

## 5. Probe results

First run: mesa.yml run 45385905364 (softpipe, `UmdVideo` 2, fake decoder),
2026-11-25.

| Step | Result |
|---|---|
| feature level | **10_1: blocked** (the only "blocked by") |
| `ID3D11VideoDevice1`, `ID3D11VideoContext1`, `ID3D11Device5` | all there (the runtime provides them over this DDI) |
| profiles, configs | H.264, `ConfigBitstreamRaw` 2, configs up to 4096x2304 |
| NV12 array, `DECODER | SHADER_RESOURCE`, no MiscFlags | created; output view and R8/R8G8 views of slice 3 work |
| the same with `SHARED` or `SHARED | SHARED_NTHANDLE` | 0x8007000e, as every shared resource in CI: this UMD makes shared resources only on the hardware runtime (no KMD in CI). Whether NV12 can be shared at all is a board question, and C3 holds either way (an opened copy has no planes) |

So past the feature level, what Chromium needs that is missing is the
shared NV12 texture (C3); the video interfaces and the decoder answers
are there. On the board: `vdectest hw --chromium` with `UmdVideo` 2,
on WDDM 1.3, gives the same table for maliwddm.

## 6. C1: feature level 11_0 for the GPU process (`UmdReportFL11`)

Written 2026-11-25, CI only. Nothing here has run on the board.

### What 11_0 changes for ANGLE, read from the source

ANGLE main, `src/libANGLE/renderer/d3d/d3d11/`:

- `renderer11_utils.cpp`, `GetMaximumClientVersion`: 11_0 gives OpenGL
  ES **3.0**, as 10_1 does ("3.1 could theoretically be supported on FL
  11.0+, but is not supported in ANGLE"). So ANGLE never creates hull,
  domain or compute shaders for Chromium's own work.
- `Renderer11.cpp`, `getMajorShaderModel`: 11_0 compiles every shader as
  **vs_5_0 / ps_5_0 / gs_5_0**. This is the real gap: the frontend's DXBC
  parser knew shader model 4 only. FXC's shader model 5 output for the
  same HLSL differs in a few ways, of which one breaks every texture
  shader: `sample_indexable`, `ld_indexable` and `resinfo_indexable` carry
  two chained extended opcode tokens (resource dimension, return type),
  where the old parser read one and took the second as an operand. Then
  `rcp` (FXC's 1/x at SM5), `deriv_rt{x,y}_{coarse,fine}`, the bit-field
  instructions FXC makes from shifts and masks (`ubfe`, `ibfe`, `bfi`),
  `countbits`, `firstbit_{hi,lo,shi}`, `bfrev`, and the geometry shader's
  `dcl_stream` / `emit_stream` / `cut_stream`.
- At 11_0 ANGLE also turns on `ANGLE_shader_pixel_local_storage`
  (unordered access views in the pixel shader), the 16384 texture limit,
  multiview with 16 viewports, conservative depth, and sample variables.
  These are extensions a page or Skia must ask for; Chromium's own
  compositor does not. `ClearView` and constant buffer offsets come from
  `D3D11_FEATURE_D3D11_OPTIONS`, which this UMD answers with zeros.
- Chromium (`d3d11_video_decoder_backend.cc`): the 11_0 test is the only
  feature-level test; `kD3D12VideoDecoder` is off by default, so the
  D3D11 path is the one used.

### Gaps against section 2, after this change

| Chromium needs | Here | State |
|---|---|---|
| ANGLE's device at 11_0 | `UmdReportFL11` 1, GPU process only | written, CI |
| shader model 5 from FXC (ES 3.0 shaders) | Mesa 0080: the list above | written, CI on softpipe; Panfrost compiles it on the emulated G52. `f32tof16` / `f16tof32` (GLSL `packHalf2x16`) are not done: logged, destination untouched. Any other SM5 opcode is logged (`shader: opcode N not carried out`) and skipped |
| no hull / domain / compute use | create fails with E_OUTOFMEMORY, device lives | CI (`fl11test`) |
| `ID3D11VideoDevice1`, `ID3D11VideoContext1` | the runtime's, over the D3D11.1 DDI | CI |
| H.264 profile, `ConfigBitstreamRaw` 2 | `UmdVideo` 2, and `\\.\VDPU383` opened from the GPU process | needs vdpu383.sys `UserAccess` 2 (woa-rk3576 `vdpu383-d3d11va`); a GPU process at low integrity cannot open it otherwise |
| NV12 array `DECODER | SHADER_RESOURCE`, `SHARED | SHARED_NTHANDLE` | created on softpipe without MiscFlags only | **unknown on the board** (C3). If it fails, Edge's media log says `kCreateDecoderOutputTextureFailed`; `--disable-features=D3D11VideoDecoderUseSharedHandle` makes Chromium ask for `SHARED` alone |
| plane views R8 / R8G8 of the decoder texture | V4 (0079), same device | CI; of a copy opened from its handle: no (C3) |
| presenting at WDDM 2.0 | none | blocked, independently of C1 (below) |

### What CI showed (mesa.yml runs 45535163559 to 45538667838)

- The D3D11 runtime refused the adapter (0x887A0020, no CreateDevice
  call, even for a caller asking for 10_1 only) whenever 11_0 was in the
  pipeline levels and `D3D11DDICAPS_SHADER` was 0. It accepts it with
  `D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X`
  set; doubles alone are refused too, free threading does not matter, and
  neither does the KMD's WDDM version. So the GPU process gets that bit,
  which describes 10_x devices only, and no doubles. Experiments on the
  software path: `D3D10UMD_FL11_EXP` (0080).
- FXC at vs_5_0/ps_5_0 emitted, for fl11test's shaders: `bfi` (from
  `(id << 1) & 2` already), `deriv_rt{x,y}_{coarse,fine}`,
  `sample_indexable`, `firstbit_hi`, `ubfe`, `countbits`, `firstbit_lo`,
  `bfrev`, `ibfe`; `1.0 / x` stayed a `div`. On softpipe every value
  matched (12 of 12), CreateComputeShader returned E_OUTOFMEMORY and the
  device lived.
- The same shaders went through tgsi_to_nir and the Bifrost compiler on
  the emulated G52 (MALIKM_FAKE: the pixels are not run, so not checked).
- `vdectest --chromium --fl11` as chrome.exe with `--type=gpu-process`:
  feature level 11_0, the H.264 profile, `ID3D11VideoDevice1` and
  `ID3D11VideoContext1` there; blocked only by the shared NV12 texture,
  which needs a KMD (as before at 10_1).
- Off, 0, another process name, msedge.exe without `--type` or with
  `--type=renderer`: 10_1, as before.

### The switch

maliwddm Device Parameters `UmdReportFL11` (DWORD, default 0, read at
every adapter query so a new GPU process sees a change without a reboot),
`MW_UMD_ADAPTER_INFO.ReportFl11`. With 1, and only in a process named
msedge.exe, chrome.exe or msedgewebview2.exe whose command line has
`--type=gpu-process`:

- the adapter offers the D3D11.1 interface (D3D11.cpp, as `UmdVideo`
  does) and reports pipeline levels 10_0, 10_1 and 11_0, and the shader
  4.x compute bit in its shader caps (above);
- `UmdVideo` 1 or 2 applies to that process too (it is not on the list of
  players otherwise);
- everything else is the same D3D10.x implementation behind the D3D11.1
  table, as for `UmdVideo`.

Off (absent or 0), and in every other process (the browser process,
renderers, utility processes, dwm.exe), nothing changes: the GetCaps
answers, the interface list and the device are as before. The parser
changes are inert below 11_0 (the runtime refuses shader model 5 code at
10_x, and no shader model 4 opcode lost its entry). CI checks: off, 0,
another process name, `msedge.exe` without `--type` and with
`--type=renderer` all get 10_1 (fl11test exit 2).

One side effect to watch: on the D3D11.1 interface the device also has the
DXGI 1.2 DDI (`D3D11FillDxgi12Funcs`: flip model presents are not really
carried out). `DCompositionCreateDevice3` failed with access denied in
Edge's GPU process on 2026-11-24, possibly because the device had no DXGI
1.2; at 11_0 it may succeed, and Chromium would then present through
composition swap chains, a path this UMD has failed before (EDGE-PRESENT.md,
run A). `--disable-direct-composition` keeps the path of round 6.

### WDDM 2.0 and the redirection default

At 2.x the browsers get no redirection (`UmdRedirect` 1, the session hang
of EDGE-PRESENT.md). Edge's GPU process then has no working present: the
HWND swap chains fail with 0x887A0022 ("Could not create additional swap
chains") and DirectComposition, the flip-model alternative, failed to
create (access denied). ANGLE loses its device and Chromium falls back to
software, where no D3D11 decoder exists. C1 cannot help there until one
of those works. Whether the freeze is specific to redirected (blt) presents
is not known: with flip model DWM also opens the browser's buffers, which
is what the hang analysis points at. So C1 is tested at WDDM 1.3
(`wddm-probe off`), where Edge rendered on this adapter (round 6), and 2.0
is one observation at the end (does DirectComposition now come up), with
`UmdRedirect` left at 1.

Flags, and what each isolates:

| Flag | Why |
|---|---|
| `--disable-direct-composition` | the round 6 present path (HWND swap chain, redirection at 1.3); first runs |
| `--disable-features=SkiaGraphite` | if Edge 158 uses Graphite on Dawn's D3D11 backend: that needs compute shaders, which 11_0 now seems to offer and does not have. A GPU process crash right after the switch, with Dawn in the log, is this |
| `--disable-features=D3D11VideoDecoderUseSharedHandle` | the decoder texture with `SHARED` instead of an NT handle (C3) |
| `--disable-gpu-sandbox` | diagnosis only: separates the sandbox (vdpu383 access) from the rest |
| `--use-angle=d3d11` | the default on Windows; only if edge://gpu shows another ANGLE backend |

### Board test (TaishanPi 3M, one variable at a time)

Package: this branch's CI (DriverVer in the run summary); `fl11test.exe`
from the Mesa run's `fl11test-arm64` artifact; `C:\mali\c1-edge.ps1` from
`tools/windows/c1-edge.ps1`; `bbb.mp4` (VIDEO-DECODE.md s5) in
`C:\mali\c1\`. UMD log on (`C:\mali\log`) for steps 2 to 5.

0. **Install, nothing set.** Import the package's new test certificate
   (the `.cer`, or from `maliwddm.cat` with `openssl pkcs7 -print_certs`)
   into Root and TrustedPublisher; `pnputil /add-driver ...\maliwddm.inf`
   (stage only, never a hot install); ConfigFlags 0x20 on
   `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0` as SYSTEM; reboot.
   Desktop as before, `m5test wddm` confirmed 0x2000. This is main with
   everything off.
1. **WDDM 1.3.** `wddm-probe off`, reboot; desktop as before.
2. **The device, without Edge.** In the user's session (schtasks /it):
   `copy fl11test.exe C:\mali\fake\msedge.exe`, then
   `C:\mali\fake\msedge.exe hw --type=gpu-process > C:\mali\c1\fl-off.txt`:
   expect "feature level below 11_0" (exit 2).
   `reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" /v UmdReportFL11 /t REG_DWORD /d 1 /f`
   (no reboot), the same command into `fl-on.txt`: expect 11_0, every line
   MATCH on the Mali, `CreateComputeShader` failing, `GetDeviceRemovedReason`
   0, PASS. `C:\mali\fl11test.exe hw --type=gpu-process` (not renamed):
   10_1 again. In `C:\mali\log\msedge.exe-*.txt`: `UmdReportFL11 1, here 1`,
   `3D pipeline levels`, and no `not carried out` lines. A DIFFER here
   stops the test: that instruction is wrong on Panfrost.
3. **Edge at 11_0, round 6's present path.** `UmdReportFL11` 1.
   `c1-edge.ps1 -Start -Flags "--disable-direct-composition"` in the user's
   session, then from SSH `c1-edge.ps1 -Read -Tag b`. Expect in
   `b-gpu.txt`: `GL_RENDERER` with `Direct3D11 vs_5_0 ps_5_0`, the D3D11
   feature level 11_0, Compositing and Canvas hardware accelerated, GPU
   process crash count 0; dwm.exe the same PID for 3 minutes, an ordinary
   page scrolls. For comparison the same with `UmdReportFL11` 0 first
   (`-Tag a`: `vs_4_1 ps_4_1`). If the GPU process crashes, read the
   profile's `chrome_debug.log` (`C:\mali\edgeprof-c1`): Dawn or Graphite
   there means the second flag of the table.
4. **DirectComposition.** The same without `-Flags` (`-Tag c`). Note
   whether the log still has `DCompositionCreateDevice3 failed`, and
   whether pages still draw. If they do not, keep
   `--disable-direct-composition` for step 5.
5. **H.264 decode.** Needs vdpu383.sys with `UserAccess` (woa-rk3576
   `vdpu383-d3d11va`) installed and `UserAccess` 2 on `ACPI\RKCP0383\0`
   (a test board only), firmware AcpiVdec on, then
   `reg add ...\maliwddm /v UmdVideo /t REG_DWORD /d 2 /f`. Restart Edge
   (`-Stop`, `-Start` with step 3's or 4's flags), `-Read -Tag d`. Expect
   in `d-media.txt` `kVideoDecoderName = D3D11VideoDecoder` (Chromium may
   call it `D3DVideoDecoder`) and `kIsPlatformVideoDecoder = true`; in
   `d-caps.txt` `powerEfficient: true`; edge://gpu's video acceleration
   list has H.264. A software decoder name with a D3D11 message before it
   says why: `kCreateDecoderOutputTextureFailed` is C3 (add the
   shared-handle flag, `-Tag e`), a failure to open the decoder is the
   sandbox (`UserAccess`). Then look at the picture: right colours, no
   green or garbage.
6. **Only at the end, WDDM 2.0** (`wddm-probe default`, reboot), step 3
   again with `UmdRedirect` left at 1: expected software fallback as on
   2026-11-24 unless DirectComposition now comes up. Do not set
   `UmdRedirect` 2 as part of this test.

Undo: `reg delete ...\maliwddm /v UmdReportFL11 /f` and `/v UmdVideo`;
both apply to the next GPU process.

**If the desktop freezes** (any step): from SSH, `c1-edge.ps1 -Stop`;
at 2.0 `reg query ...\maliwddm\Probe2000 /v Wd_Recent` and `/v Wd_Stall`
(EDGE-PRESENT.md, "The WDDM 2.0 session hang"); then one discriminating
step, `taskkill /f /im dwm.exe`, noting whether the desktop returns and
whether Edge's processes exit; a live dump only if its first bytes are
`PAGEDU64`. After a power cycle, before anything else: `reg query
...\Probe2000` and save every `Prev_*`, and copy `C:\mali\log\msedge.exe-*`
and `C:\mali\edgeprof-c1\chrome_debug.log`. Then delete `UmdReportFL11`.

### Odds (honest)

- fl11test PASS on the Mali (step 2): likely, about two in three. The
  runtime's side and the translation are proven on softpipe and Panfrost
  compiles the same TGSI; open are the hardware runtime (a real KMD)
  asking more at 11_0, and Bifrost's results for the new instructions.
- Edge drawing correctly at 11_0 (step 3): about even. Chromium's shaders
  may make FXC emit instructions fl11test does not cover (the UMD log
  names any it skips), and DirectComposition or Graphite may change path
  because of the new answers.
- Hardware H.264 in Edge in this round (step 5): roughly one in five. It
  also needs the sandboxed decoder access, the shared NV12 texture (C3,
  never tried on the board), and the decoded planes sampled correctly by
  ANGLE.
- YouTube needs VP9 or AV1 (C5, C6) as well; Bilibili's default H.264
  would follow from step 5.
