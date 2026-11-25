# Hardware video decode in Chrome and Edge

What Chromium needs from a D3D11 decoder, what is missing, and how to
test it. The decoder itself is in [VIDEO-DECODE.md](VIDEO-DECODE.md).
Settings: see [SETTINGS.md](SETTINGS.md). Nothing here has run on the
board.

## What the RK3576 decodes

One block, RKVDEC (Rockchip calls it VDPU383), at 0x27B00000 (64 KiB),
interrupts `irq_rkvdec_dec` and `irq_rkvdec_mmu`. JPEG is a separate
block. The public TRM V1.2 does not describe the decoder; the codec list
comes from Rockchip MPP (`osal/mpp_soc.c`, `vdpu383_init_ctrl_regs()`).

| Codec | Mainline Linux | Rockchip MPP |
|---|---|---|
| H.264 up to High, 8 bit | yes (`rkvdec-vdpu383-h264.c`) | yes |
| HEVC Main, Main 10 | yes (`rkvdec-vdpu383-hevc.c`) | yes |
| VP9 profile 0, 2 | no | yes (`hal_vp9d_vdpu383.c`) |
| AV1 Main | no | yes (`hal_av1d_vdpu383.c`) |

So VP9 and AV1 have MPP as the only reference. What this driver decodes
today: [VIDEO-DECODE.md](VIDEO-DECODE.md) section 3.

YouTube sends VP9, or AV1 when the decoder offers it, and H.264 only when
neither is decoded in hardware. Bilibili sends H.264 by default, HEVC or
AV1 when chosen.

## What Chromium asks of a D3D11 decoder

From Chromium `media/gpu/windows` (shared by Chrome and Edge):

| Step | Needs |
|---|---|
| Device | the ANGLE device (render and decode on one device) with `GetFeatureLevel() >= 11_0`; below that no decoder at all (`kUnsupportedFeatureLevel`) |
| Interfaces | `ID3D11VideoDevice1`, `ID3D11VideoContext1` |
| Profiles | H.264 VLD no FGT, HEVC Main/Main10, VP9 profile 0/2, AV1 profile 0; H.264 is listed up to 1920x1088 whatever the driver says |
| Sizes | `GetVideoDecoderConfigCount` at growing sizes with NV12 (P010 for 10 bit) |
| Config | `ConfigBitstreamRaw` 2 for H.264, 1 for HEVC, VP9, AV1 |
| Output texture | NV12 or P010 array, `BIND_DECODER | BIND_SHADER_RESOURCE`, MiscFlags `SHARED | SHARED_NTHANDLE` (default), else `SHARED` |
| Display | the planes as R8/R8G8 views of the texture or of a copy opened from its NT handle, or a video processor copy |
| Process | all in the GPU process: low integrity, restricted token |

## Steps and state

| | What | State |
|---|---|---|
| C0 | the facts above and the probe (`vdectest --chromium`, `chromeprobe.cpp`) | done |
| C1 | feature level 11_0 for Chromium's GPU process only (`UmdReportFL11`, Mesa 0080, `fl11test`) | CI only |
| C2 | decoding in the GPU process: V3a with vdpu383.sys `UserAccess` 2 on a test board; for real, decode work through maliwddm | V3a written; maliwddm path not started |
| C3 | shared NV12: one allocation with plane views aliasing it, so a copy opened from the handle and the decoder see the same pixels | not started |
| C4 | H.264 in Edge on the board (`edge://media-internals` says D3D11VideoDecoder) | needs C1, C2, C3 |
| C5 | VP9 profile 0 for YouTube | in the UMD (VIDEO-DECODE.md) |
| C6 | HEVC Main, AV1 Main, P010 for 10 bit | HEVC and AV1 8 bit in the UMD; no P010 |

If the runtime or ANGLE does not accept 11_0 without compute shaders, the
remaining path is compute shaders in the D3D11 frontend, weeks of work.

Probe result (mesa.yml on softpipe, `UmdVideo` 2, fake decoder): at 10_1
the feature level is the only blocker; the video interfaces, the H.264
profile, configs up to 4096x2304, and an unshared NV12 array with output
and R8/R8G8 views all work. `SHARED` and `SHARED | SHARED_NTHANDLE` fail
with 0x8007000e because CI has no KMD; whether shared NV12 works is a
board question, and C3 is needed either way (an opened copy has no
planes).

## C1: feature level 11_0 for the GPU process (`UmdReportFL11`)

`UmdReportFL11` 1 (GPU0 Device Parameters, default 0, read at every
adapter query, so a new GPU process sees a change without a reboot)
applies only in msedge.exe, chrome.exe or msedgewebview2.exe with
`--type=gpu-process` on the command line. There:

- the adapter offers the D3D11.1 interface, pipeline levels 10_0, 10_1
  and 11_0, and `D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X`
  in its shader caps (without that bit the runtime refuses the adapter
  with 0x887A0020 whenever 11_0 is listed; doubles alone are refused too);
- `UmdVideo` 1 or 2 applies to that process too;
- hull, domain and compute shader creation fail with E_OUTOFMEMORY and the
  device lives.

Everywhere else (browser process, renderers, utility processes, dwm.exe)
nothing changes. `D3D10UMD_FL11_EXP` (0080) is for experiments on the
software path.

What 11_0 means for ANGLE (`src/libANGLE/renderer/d3d/d3d11/`): still
OpenGL ES 3.0, so ANGLE never uses hull, domain or compute shaders, but
every shader is compiled as vs_5_0/ps_5_0/gs_5_0. Mesa 0080 teaches the
frontend's DXBC parser shader model 5: the two extended opcode tokens of
`sample_indexable`, `ld_indexable`, `resinfo_indexable`; `rcp`,
`deriv_rt{x,y}_{coarse,fine}`, `ubfe`, `ibfe`, `bfi`, `countbits`,
`firstbit_{hi,lo,shi}`, `bfrev`, and `dcl_stream`/`emit_stream`/
`cut_stream`. Not done: `f32tof16`/`f16tof32` (logged, destination
untouched). Any other SM5 opcode is logged as
`shader: opcode N not carried out` and skipped. ANGLE also turns on
extensions at 11_0 (pixel local storage, 16384 textures, multiview,
conservative depth, sample variables) that Chromium's compositor does not
use. `D3D11_FEATURE_D3D11_OPTIONS` is answered with zeros.

CI: fl11test's shaders match on softpipe (12 of 12) and compile through
the Bifrost compiler on the emulated G52 (not run). `vdectest --chromium
--fl11` as chrome.exe `--type=gpu-process` reaches 11_0 with the H.264
profile and the video interfaces, blocked only by shared NV12. Off, 0,
another process name, msedge.exe without `--type` or with
`--type=renderer` all stay at 10_1 (fl11test exit 2).

Side effect: on the D3D11.1 interface the device also has the DXGI 1.2 DDI,
whose flip-model presents are not carried out. If
`DCompositionCreateDevice3` now succeeds in Edge's GPU process, Chromium
presents through composition swap chains, which this UMD has failed
before ([EDGE-PRESENT.md](EDGE-PRESENT.md)). `--disable-direct-composition`
keeps the HWND swap chain path.

At WDDM 2.x the browsers get no redirection by default (`UmdRedirect`,
see EDGE-PRESENT.md, "The WDDM 2.0 session hang"), so Edge's GPU process
has no working present and falls back to software, where there is no
D3D11 decoder. C1 is therefore tested at WDDM 1.3.

Edge flags:

| Flag | Use |
|---|---|
| `--disable-direct-composition` | the HWND swap chain present path; first runs |
| `--disable-features=SkiaGraphite` | if the GPU process crashes with Dawn in the log (Graphite needs compute shaders) |
| `--disable-features=D3D11VideoDecoderUseSharedHandle` | decoder texture with `SHARED` instead of an NT handle (C3) |
| `--disable-gpu-sandbox` | diagnosis only: separates decoder access from the rest |
| `--use-angle=d3d11` | only if edge://gpu shows another ANGLE backend |

### C1 board test (TaishanPi 3M, one change at a time)

Inputs: the current maliwddm package; `fl11test.exe` (Mesa artifact
`fl11test-arm64`); `C:\mali\c1-edge.ps1` from `tools/windows/c1-edge.ps1`;
`bbb.mp4` (VIDEO-DECODE.md section 6) in `C:\mali\c1\`; UMD log on
(`C:\mali\log`) for steps 2 to 5.

0. Import the package's test certificate into Root and TrustedPublisher,
   `pnputil /add-driver ...\maliwddm.inf` (stage only), `ConfigFlags`
   0x20 on `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0` as SYSTEM,
   reboot. Desktop as before, `m5test wddm` confirmed 0x2000.
1. `wddm-probe off`, reboot: WDDM 1.3, desktop as before.
2. In the user's session (`schtasks /it`):
   `copy fl11test.exe C:\mali\fake\msedge.exe`, then
   `C:\mali\fake\msedge.exe hw --type=gpu-process > C:\mali\c1\fl-off.txt`
   (expect "feature level below 11_0", exit 2). Set `UmdReportFL11` 1
   (no reboot) and run it again into `fl-on.txt`: 11_0, every line MATCH,
   `CreateComputeShader` failing, `GetDeviceRemovedReason` 0, PASS.
   `C:\mali\fl11test.exe hw --type=gpu-process` (not renamed) gives 10_1.
   `C:\mali\log\msedge.exe-*.txt` shows `UmdReportFL11 1, here 1`,
   `3D pipeline levels`, and no `not carried out`. A DIFFER stops the
   test: that instruction is wrong on Panfrost.
3. `c1-edge.ps1 -Start -Flags "--disable-direct-composition"` in the
   user's session, then `c1-edge.ps1 -Read -Tag b` from SSH. Expect in
   `b-gpu.txt` `GL_RENDERER` with `Direct3D11 vs_5_0 ps_5_0`, feature
   level 11_0, compositing and canvas hardware accelerated, GPU process
   crash count 0; dwm.exe keeps its PID for 3 minutes and a page scrolls.
   Compare with `UmdReportFL11` 0 first (`-Tag a`: `vs_4_1 ps_4_1`). On a
   GPU process crash read `C:\mali\edgeprof-c1\chrome_debug.log`.
4. The same without `-Flags` (`-Tag c`): does the log still have
   `DCompositionCreateDevice3 failed`, and do pages draw? If not, keep
   `--disable-direct-composition`.
5. H.264: vdpu383.sys installed with `UserAccess` 2 on `ACPI\RKCP0383\0`
   (test board only), firmware `AcpiVdec` on, `UmdVideo` 2. Restart Edge
   (`-Stop`, `-Start`), `-Read -Tag d`. Expect in `d-media.txt`
   `kVideoDecoderName = D3D11VideoDecoder` (or `D3DVideoDecoder`) and
   `kIsPlatformVideoDecoder = true`, in `d-caps.txt`
   `powerEfficient: true`, and H.264 in edge://gpu's video acceleration
   list. `kCreateDecoderOutputTextureFailed` is C3 (add the shared-handle
   flag, `-Tag e`); failing to open the decoder is `UserAccess`. Check the
   picture: right colours, no green or garbage.
6. Last, `wddm-probe default`, reboot, step 3 with `UmdRedirect` left at
   its default: software fallback expected unless DirectComposition comes
   up. Do not set `UmdRedirect` 2 in this test.

Undo: delete `UmdReportFL11` and `UmdVideo`; both apply to the next GPU
process.

If the desktop freezes: `c1-edge.ps1 -Stop` from SSH; at 2.0
`reg query <GPU0 Device Parameters>\maliwddm\Probe2000 /v Wd_Recent` and
`/v Wd_Stall` (EDGE-PRESENT.md, "The WDDM 2.0 session hang"); then
`taskkill /f /im dwm.exe`, noting whether the desktop returns and Edge's
processes exit; a live dump only if its first bytes are `PAGEDU64`. After
a power cycle, before anything else, save every `Prev_*` value under
`Probe2000`, `C:\mali\log\msedge.exe-*` and `chrome_debug.log`, then
delete `UmdReportFL11`.
