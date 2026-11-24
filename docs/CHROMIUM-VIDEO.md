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
| C1 | **feature level 11_0 for Chromium's GPU process only**: the 11.1 DDI already there, the device reports 11_0, hull, domain and compute shader creation fail as geometry shaders fail today; offered only where the process is `msedge.exe`/`chrome.exe` with `--type=gpu-process`, behind a new maliwddm switch (default 0), like `UmdVideo` | a check that ANGLE stays on OpenGL ES 3.0 (it offers 3.1 only with compute), and that the D3D11 runtime accepts 11_0 from this DDI; WDDM 1.3 | not started |
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
