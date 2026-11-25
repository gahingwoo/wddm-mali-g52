# Hardware video decode in Windows players: design and board test

Written 2026-11-24, branch `video-decode`. Nothing here has run on the
board yet. CI on a92a4a9: Mesa run 45359774143 (vdectest on softpipe
through the D3D11.1 interface: device at 10_1, `ID3D11VideoDevice` yes,
H.264 profile, 12 of 12 frames through `ID3D11VideoContext` with the fake
decoder, MATCH; the triangle and present tests on the D3D11.1 interface
pass; `UmdVideo` 0 and an unlisted process get no video device), CI run
45359773995 (the backend host test, the package). Off by default: with `UmdVideo` absent or 0 every process gets
exactly the driver it gets on `main`.

What is already proven (TaishanPi 3M, 2026-11-24): firmware device
`ACPI\RKCP0383` (edk2-rk3576, now on `main`, setup option off by default),
the bring-up driver vdpu383.sys and `vdpu383test` from woa-rk3576
`vdpu383-v2`, and the H.264 conformance streams BA1_Sony_D, CABA1_SVA_B,
CABA3_SVA_B, MR2_TANDBERG_E, FRExt1_Panasonic and HPCAQ2LQ_BRCM_B decoded
bit-exact against FFmpeg, also with `--dxva-only` (the registers built
from DXVA picture parameters alone). That last run is the one this design
builds on: a player hands the driver DXVA picture parameters and nothing
else.

## 1. Which path each player takes (Windows 11 23H2, arm64)

| Player | How it decodes | Needs from us |
|---|---|---|
| Edge (Chromium) | `D3D11VideoDecoder` on its D3D11 device: `ID3D11VideoDevice`, H.264 only with `ConfigBitstreamRaw` 2 (Chromium `media/gpu/windows/d3d11_video_decoder_wrapper.cc`, read 2026-11-24). Never loads a third-party MFT. | the video DDI in the UMD |
| Media Player, Movies & TV, Photos | Media Foundation (Media Engine). The inbox H.264 decoder (`msmpeg2vdec.dll`; HEVC and VP9 from the Store extensions) is a DXVA client: given the pipeline's D3D11 device, it decodes through `ID3D11VideoDevice`. NV12 to RGB goes through the D3D11 video processor when the device has one. | the video DDI, then the video processor (V4) |
| mpv, FFmpeg, VLC | `d3d11va` / `d3d11va-copy`: `ID3D11VideoDevice` directly, the D3D11 device they create themselves | the video DDI |
| Firefox | Media Foundation's decoder with a D3D11 device, as above | the video DDI |

So every player on the list reaches a hardware decoder through
`ID3D11VideoDevice`, and only through it. On the board, 2026-11-21,
`d3dprobe` said `ID3D11VideoDevice: no` on the Mali: the D3D11 runtime
exposes a video device only on a driver whose device interface is
D3D11.1 or later, and maliumd offers D3D10.0 and D3D10.1.

## 2. Decision

**(a) D3D11 video decode in the display driver's UMD.** Not (b), a Media
Foundation decoder of our own:

1. (b) cannot serve Edge, mpv, FFmpeg or VLC at all; (a) serves them and,
   through Microsoft's own DXVA decoders, the Media Foundation players too.
2. A hardware MFT is picked ahead of Microsoft's decoders only with a
   merit Windows validates (an OPM certificate from Microsoft); without
   one the Media Foundation players keep decoding in software. Not tried;
   not needed once (a) works.
3. (b) still moves every frame through the CPU into the renderer's
   device; (a) can end with the decoder writing into the texture the Mali
   samples (V3b below).

**The risk to the display driver**, and how it is contained:

- The D3D11.1 interface (which the video DDI needs) is offered **only in
  the processes `UmdVideo` names** (Mesa patch 0078, `VideoEffective`).
  dwm.exe, LogonUI and the shell keep the D3D10.x interface of `main`, the
  answers to `GetCaps` are unchanged unless the interface is offered, and
  `UmdVideo` 0 (the default, also what a KMD that does not know the value
  gives) changes nothing anywhere.
- The D3D11.1 table is the D3D10.x implementation behind new pointers:
  the same functions where the signature did not change, thin wrappers
  where parameters were added (copy flags, constant buffer windows,
  unordered access views that are always empty), entry points that do
  nothing for hull, domain, compute and command lists (the runtime does
  not call them at feature level 10_x, and emulates deferred contexts).
- **maliwddm.sys is not changed** in this milestone. It already passes
  `UmdVideo` to the UMD (`MW_UMD_ADAPTER_INFO.Video`). WDDM 2.0 (the
  default) and the 1.3 fallback are untouched.

| `UmdVideo` (maliwddm Device Parameters) | Effect |
|---|---|
| 0, absent | as `main` |
| 1 | the D3D11.1 interface and a video device with **no** decoder profiles, in mpv.exe, ffmpeg.exe, ffplay.exe, ffprobe.exe, vlc.exe, vdectest.exe, d3dprobe.exe. Players fall back to software: checks the new interface alone |
| 2 | the same with the decoder's profiles (H.264, HEVC Main, VP9 profile 0, AV1 Main; section 7), when `\\.\VDPU383` opens |
| 3 | 2 in every process except dwm.exe, LogonUI.exe, consent.exe (Media Player, Edge) |

With `UmdVideo` set, three more Device Parameters (read only then; the KMD
packs them above `UmdVideo` in `MW_UMD_ADAPTER_INFO.Video`):

| Setting | Default | Effect |
|---|---|---|
| `UmdVideoCodecs` | 0 | which profiles are listed: bit 0 H.264, 1 HEVC Main, 2 VP9 profile 0, 3 AV1 Main; 0 all four. 1 is the H.264-only UMD of patch 0078 |
| `UmdAv1Synth` | 0 | how the AV1 tile data is wrapped for the decoder, which parses OBUs itself: 0 one `OBU_TILE_GROUP`, 1 an `OBU_FRAME` with 16 zero bytes of frame header. Which one the VDPU383 accepts is the first AV1 question for the board |
| `UmdVp9Mpp` | 0 | VP9 frame contexts: 0 as the specification says, 1 MPP's reset of all four on intra frames, 2 MPP's write of the adapted context into a context the frame does not keep, 3 both (vdpu383test `--vp9-mpp`) |

### Who owns the VDPU383

The decoder is its own IP block with its own ACPI device, MMIO, two
interrupts and its own IOMMU. Three stages:

- **V3a, this branch: vdpu383.sys keeps it.** The UMD, inside the player's
  process, calls the bring-up interface vdpu383test proved: contiguous
  decoder buffers (`IOCTL_VDPU383_ALLOC`), the register file built in user
  mode by the same register builder (copied unchanged into `video/`), one
  `IOCTL_VDPU383_RUN` per picture. The device opens for users only with
  vdpu383.sys's new `UserAccess` 1 (woa-rk3576 branch `vdpu383-d3d11va`,
  default 0). The decoder writes into one surface of its own per DXVA
  surface index (references are read from there, exactly as in
  vdpu383test), and `DecoderEndFrame` then copies the picture into the
  application's NV12 texture: one CPU copy per frame, about 3 MB at
  1080p. Works for ordinary desktop processes (mpv, FFmpeg). It does not
  reach Edge's GPU process (low integrity, restricted token) unless
  `UserAccess` is 2, which a test board may do and a product must not.
- **V3b: maliwddm drives it, zero copy.** Decode work goes through
  dxgkrnl like any other GPU work, so every process that has a D3D device
  can decode, sandboxed or not. maliwddm talks to vdpu383.sys through a
  private kernel interface (`IoGetDeviceInterfaces` plus
  `IRP_MN_QUERY_INTERFACE`), not by taking over its MMIO: the decoder's
  power, reset, interrupt and register-check code stays in the driver
  that ran it on the board, and the KMD gains a submission path, not a
  second hardware block. The UMD submits one private command per picture
  (the register image and the allocation list: bitstream, output and
  reference NV12, scratch), maliwddm turns the allocations into the
  decoder's IOMMU page tables (it already builds rockchip IOMMU tables for
  the VOP2 in scanout.c; the VDPU383's IOMMU is the same kind) and hands
  the run to vdpu383.sys. The decoder then writes into the texture
  itself. The NV12 layout chosen here, luma rows then chroma rows at one
  pitch, is the VDPU383's own (chroma at `dma + stride * rows`), so V3b
  changes no texture layout, only where the pixels come from. Whether
  this later becomes a `DXGK_ENGINE_TYPE_VIDEO_DECODE` node with its own
  fences (D3D11-UMD.md U5) or stays a synchronous escape is decided by
  what V3a measures: at 1080p30 a picture is a few milliseconds in the
  decoder, so a synchronous path may be enough.
- Merging the decoder's resources into GPU0's `_CRS` (the old U5 plan)
  is not needed for either; it stays an option if two drivers sharing the
  block turns out wrong.

## 3. Milestones

| | Goal | State |
|---|---|---|
| V3a H.264 | `ID3D11VideoDevice` with H.264 VLD (no FGT), short slice format, NV12 decoder output, `DecoderEndFrame` decodes on the VDPU383; FFmpeg d3d11va(-copy) and mpv `--hwdec=d3d11va-copy` | **this branch**: Mesa 0078, `video/`, vdectest, CI; vdpu383.sys `UserAccess` on woa-rk3576 `vdpu383-d3d11va`. Not run on the board |
| V3a HEVC Main | the same for `D3D11_DECODER_PROFILE_HEVC_VLD_MAIN`, `ConfigBitstreamRaw` 1 | vdpu383test on the board 2026-11-25: 60 of 75 conformance streams bit-exact, the rest allocation failures (fixed by the driver's IOMMU, not yet rerun). In the UMD: Mesa 0081 (section 7), CI only |
| V3a VP9 profile 0 | `D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0`, `ConfigBitstreamRaw` 1 | register builder on woa-rk3576 `main` (the decoder parses the compressed header and adapts the probabilities itself; the builder keeps the frame contexts' bookkeeping); 215 test vectors checked off the board, none run on it. In the UMD: Mesa 0081, CI only |
| V3a AV1 Main | `DXVA_ModeAV1_VLD_Profile0`, 8 bit, `ConfigBitstreamRaw` 1 | register builder on woa-rk3576 `main` (the decoder parses OBUs and keeps the CDFs itself; DXVA has only the tiles, so they are wrapped in OBUs, `UmdAv1Synth`); 24 streams checked off the board, none run on it. In the UMD: Mesa 0081, CI only |
| 10 bit | HEVC Main 10, VP9 profile 2, 10-bit AV1 | not offered: the decoder writes 10-bit pictures packed (4 samples in 5 bytes), the UMD has no P010 texture, the HEVC builder is Main only (section 7) |
| V4 | NV12 as two shader views (R8 and R8G8 planes), the D3D11 video processor (NV12 and RGB in, RGB out, on the GPU): Media Player, Edge, mpv zero-copy rendering | **this branch**: Mesa 0079, `vdectest --vp`, CI on softpipe. Not run on the board |
| V3b | zero copy through maliwddm and the decoder's IOMMU; sandboxed processes | zero copy: branch `vdec-zerocopy` with woa-rk3576 `vdpu383-import` (section 8), CI only. Sandboxed processes: not started (still `UserAccess`) |

H.264 limits in V3a: 8-bit 4:2:0, progressive and field/MBAFF as the
register builder handles them, up to 4096x2304; no encryption.

## 4. What is on the branch

- `mesa-patches/0078-...`: `Adapter.cpp` (the interface list, `GetCaps`
  for the D3D11.1 interface only), `Device.cpp` (the D3D11.1 branch of
  `CreateDevice`), `D3D11.cpp` (the D3D11.1 table, the DXGI 1.2 table),
  `Video.cpp` (the video DDI), `VideoStubs.h` (the video entry points not
  carried out, generated from the WDK header by
  `tools/gen-video-stubs.py`), `Resource.cpp` (NV12 as one R8 texture of
  1.5 times the height, plane-aware copies), `Shader.cpp` (constant
  buffer windows).
- `mesa-patches/0079-...`: `VideoProcessor.cpp` (the video processor
  DDI, drawn in a pipe context of its own, and the NV12 plane textures),
  `VideoShaders.c` (the NV12 and RGB fragment shaders, TGSI built with
  ureg: luma filtered by the sampler, chroma interpolated from texel
  fetches, BT.601 or BT.709, studio or full range), `Resource.cpp` (an
  NV12 texture with `BIND_SHADER_RESOURCE` gets R8 and R8G8 plane
  textures; copies and updates keep them in step), `Shader.cpp` (an
  R8_UNORM or R8G8_UNORM view of NV12 is a view of a plane), `Video.cpp`
  (a decode writes the planes too), `Device.cpp` (video processor input
  and output in `CheckFormatSupport`).
- `mesa-patches/0081-...`: `Video.cpp` (HEVC Main, VP9 profile 0 and AV1
  Main next to H.264: the profiles, their configurations, the per-codec
  picture parameters, slice control and tile buffers, `UmdVideoCodecs`,
  `UmdAv1Synth`, `UmdVp9Mpp`), `meson.build` (the new register builders).
  Section 7.
- `video/vdec.c`: the backend (README in `video/`). With
  `D3D10UMD_VDEC_FAKE=1` it builds and checks the register file and
  writes a known pattern instead of decoding; with
  `D3D10UMD_VDEC_REGLOG=<file>` it logs every register file it builds.
- `tools/windows/vdectest/`: drives `ID3D11VideoContext` the way FFmpeg's
  d3d11va-copy does (texture array with `BIND_DECODER`, an output view
  per slice, picture parameters, scaling matrix, short slice control,
  bitstream padded to 128 bytes, copy to a staging NV12 texture, map) and
  compares every frame with a reference.
- `tools/windows/vdectest/vptest.cpp` (`vdectest --vp`): an NV12 picture
  with luma changing at every texel and steep chroma ramps, through
  `VideoProcessorBlt` to BGRA at 1:1, 2x and 0.73x with a source
  rectangle, with destination and target rectangles and a background,
  and BGRA in, each read back and compared with a model of the sampling;
  then the NV12 texture as R8 and R8G8 shader resource views, read by a
  pixel shader.
- CI: `tests/vdec-host` (ci.yml) runs a 12-frame High profile stream
  through the backend on Linux under ASan; mesa.yml runs vdectest on
  softpipe through the D3D11.1 interface with the fake decoder, the
  triangle and present tests on that interface (as mpv.exe and
  ffplay.exe), and checks that `UmdVideo` 0, an unlisted process and a
  missing `\\.\VDPU383` give no decoder.

## 5. Board test (TaishanPi 3M, HDMI)

One test, one install. Stage only, `ConfigFlags` 0x20, full reboot. Never
`pnputil /install` or `/restart-device` on the running display driver.

**Inputs**

- Firmware: edk2-rk3576 `main` (67f0a98 or later) with *Video Decoder
  (VDPU383)* enabled in setup.
- vdpu383.sys: woa-rk3576 `vdpu383-d3d11va`, WinPE workflow artifact
  `woa-drivers-signed-arm64` (the `vdpu383` folder), run 45360193024.
- maliwddm package: wddm-mali-g52 `video-decode`, CI artifact
  `wddm-mali-g52-driver-arm64` (run 45359773995 or later; its UMD is the
  one with the video patch, now 0078). Its DriverVer is newer than the board's, so it is the
  package Windows binds.
- vdectest.exe and `testsrc-176x144-high.264`: Mesa workflow artifact
  `vdectest-arm64` (run 45359774143). The conformance streams and
  FFmpeg references of 2026-11-24 (BA1_Sony_D, CABA3_SVA_B,
  MR2_TANDBERG_E, FRExt1_Panasonic).
- Player: FFmpeg for Windows on Arm (BtbN FFmpeg-Builds, a dated
  `winarm64-gpl` release, note the file name) and mpv for aarch64 (a
  dated release, note the file name).
- Sample: Big Buck Bunny 1080p30 H.264 from the Blender Foundation,
  `https://download.blender.org/demo/movies/BBB/bbb_sunflower_1080p_30fps_normal.mp4.zip`
  (the plain .mp4 URL is gone). Unzip and rename to `bbb.mp4`. Check both
  with `certutil -hashfile <file> SHA256` against section 6.

**Install (as administrator, clock synced first: `w32tm /resync /force`)**

```
pnputil /add-driver vdpu383\vdpu383.inf
pnputil /add-driver maliwddm\maliwddm.inf
```

Then, as SYSTEM, `ConfigFlags` |= 0x20 under both
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0` and the GPU0
instance key (`pnputil /enum-devices /class Display` gives its instance
id), and restart. After the boot: `pnputil /enum-drivers` shows both
packages bound; delete the packages they replaced (DriverStore cleanup).

**Step A, switches off (the regression check)**

Desktop, Edge and `m5test perf` as before. `vdectest hw --probe` must
fail to create a video device (exit 1). This is `main`'s behaviour.

**Step B, switches on**

```
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters" /v UserAccess /t REG_DWORD /d 1 /f
reg add "<GPU0 instance key>\Device Parameters" /v UmdVideo /t REG_DWORD /d 2 /f
mkdir C:\mali\log
```

Full restart. Then, as the normal user (not elevated):

1. `vdectest hw --probe`: `ID3D11VideoDevice: yes`, `decoder profiles: 1`,
   the H.264 GUID with `NV12 yes`; exit 0.
2. `vdectest hw testsrc-176x144-high.264`: 12 frames out,
   `output md5 28b5ac0bb9dbb31571f6692a27b3d31a`, `vdpu383.sys runs: +12`.
3. `vdectest hw BA1_Sony_D.jsv --ref BA1_Sony_D.yuv`, then CABA3_SVA_B,
   MR2_TANDBERG_E, FRExt1_Panasonic with their references: `reference:
   MATCH` each, 0 decode errors.
4. FFmpeg, bit-exactness on the real file, then speed:

   ```
   ffmpeg -hide_banner -i bbb.mp4 -t 20 -an -f framemd5 sw.md5
   ffmpeg -hide_banner -hwaccel d3d11va -i bbb.mp4 -t 20 -an -f framemd5 hw.md5
   fc sw.md5 hw.md5
   ffmpeg -hide_banner -benchmark -hwaccel none   -i bbb.mp4 -t 60 -an -f null -
   ffmpeg -hide_banner -benchmark -hwaccel d3d11va -i bbb.mp4 -t 60 -an -f null -
   ```

   `fc` finds no differences; the hardware run's `speed=` is at least
   1.0x and its `utime` well below the software run's. If FFmpeg picks
   another adapter, add `-hwaccel_device N` with the Mali's index from
   `vdectest hw --probe`.
5. mpv, the player test:

   ```
   mpv --hwdec=d3d11va-copy --gpu-api=d3d11 --d3d11-flip=no --length=60 ^
       --term-status-msg="hw ${hwdec-current} drop ${frame-drop-count} ddrop ${decoder-frame-drop-count}" bbb.mp4
   ```

   and the same with `--hwdec=no`. During each, in another window:

   ```
   typeperf "\Processor(_Total)\% Processor Time" "\Process(mpv)\% Processor Time" -si 1 -sc 60 -o cpu-<on|off>.csv
   ```

   Pass: `hw d3d11va-copy` in the status line, dropped frames near 0 at
   1080p30, total CPU clearly lower with the decoder on than with
   `--hwdec=no` (the copy-out costs some: expect a few percent of one
   core per 1080p30 stream, not zero), and the decoder's counter moved by
   about the number of frames shown (`vdpu383test info` as administrator
   prints `runs`, before and after). If mpv's own renderer fails on the
   D3D11.1 interface, repeat with `--vo=null`: that still measures the
   decoder, and says the renderer is the problem.

6. V4, the video processor and plane views (`UmdVideo` 2 as above):

   ```
   vdectest hw --vp
   vdectest hw testsrc-176x144-high.264 --vp
   mpv --hwdec=d3d11va --gpu-api=d3d11 --d3d11-flip=no --length=60 bbb.mp4
   ```

   `video processor tests: PASS` (filter precision on the Mali allows 3
   per channel when scaled), `decoded picture ... through the video
   processor: MATCH`, and mpv showing `hw d3d11va` (not `-copy`): that is
   mpv sampling the decoder's NV12 through R8 and R8G8 views. Then set
   `UmdVideo` 3 (every process but dwm, LogonUI and consent), full
   restart, and play `bbb.mp4` in Media Player: a picture, and in its UMD
   log (`C:\mali\log\Microsoft.Media.Player*` or the Media Foundation
   host's) `video: frame` lines and `video: blit` lines. Edge needs
   vdpu383.sys `UserAccess` 2 as well (its GPU process runs at low
   integrity; a test board only): `edge://media-internals` for a
   `<video>` with `bbb.mp4` says `D3D11VideoDecoder` and no fallback.
   Not checked in Chromium's source tonight: whether D3D11VideoDecoder
   accepts a device below feature level 11_0 (this driver's is 10_1). If
   media-internals names the feature level as the reason, that is it, and
   Edge waits for an 11_0 device (D3D11-UMD.md); Media Player and mpv do
   not depend on it.

**Send back**

All command output above; `cpu-on.csv`, `cpu-off.csv`; `C:\mali\log`
(the UMD logs of mpv.exe, ffmpeg.exe and vdectest.exe: the `video:` lines
say how many frames and errors, the process total and the driver's run
count); `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters"`
(breadcrumbs); `vdpu383test info`. On a bugcheck: the stop code from the
screen and the Device Parameters breadcrumbs after the next boot.

**Back-out**

`UmdVideo` 0 (or delete it) and `UserAccess` 0, full restart: the UMD is
then `main`'s in behaviour. Anything worse: firmware setup, *Video Decoder*
Disabled (no `RKCP0383`, nothing opens).

## 6. The sample

Hashed 2026-11-24 from the URL in section 5:

| File | Bytes | sha256 |
|---|---|---|
| `bbb_sunflower_1080p_30fps_normal.mp4.zip` | | `e320fef389ec749117d0c1583945039266a40f25483881c2ff0d33207e62b362` |
| `bbb_sunflower_1080p_30fps_normal.mp4` (inside) | 276134947 | `ae51005850b0ff757fe60c3dd7a12d754d3cd2397d87d939b55235e457f97658` |

## 7. HEVC, VP9 and AV1 (Mesa patch 0081)

Written 2026-11-25, branch `video-decode-codecs`. CI only; nothing of this
has run on the board through D3D11.

**What the UMD offers.** With `UmdVideo` 2 (or 3) and `\\.\VDPU383` open,
four profiles, in this order, all with NV12 output only:

| Profile | GUID | `ConfigBitstreamRaw` | Buffers the UMD reads |
|---|---|---|---|
| H.264 VLD no FGT | `1b81be68-a0c7-11d3-b984-00c04f2e73c5` | 2 | picture parameters, matrix, short slices (counted), bitstream |
| HEVC Main | `5b11d51b-2f4c-4452-bcc3-09f2a1160cc0` | 1 | `DXVA_PicParams_HEVC`, `DXVA_Qmatrix_HEVC` (when scaling lists are on), `DXVA_Slice_HEVC_Short` (where the data ends), bitstream |
| VP9 profile 0 | `463707f8-a1d0-4585-876d-83aa6d60b89e` | 1 | `DXVA_PicParams_VP9`, `DXVA_Slice_VPx_Short`, the whole frame |
| AV1 Main (8 bit) | `b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a` | 1 | `DXVA_PicParams_AV1`, `DXVA_Tile_AV1` per tile, the tile data |

Slice and tile offsets are taken relative to the bitstream buffer of the
same `SubmitDecoderBuffers` call, so a frame submitted in several calls
(Chromium does that when a bitstream buffer is full) still adds up.
Sizes up to 4096x2304, as H.264.

**Per decoder** (`video/vdec.c`, one `vdec` per `ID3D11VideoDecoder`):
HEVC keeps its reference picture set scratch table and sizes surfaces per
stream as H.264 does; VP9 keeps `V383_Vp9State`, the 640 KiB auxiliary
buffer with the four frame contexts and the segment maps, and two
co-located motion vector buffers, all sized once for the decoder's
`SampleWidth` x `SampleHeight`; AV1 keeps `V383_Av1State` and gives each
surface its co-located motion vectors and its CDF and segment map buffer.
Surfaces are allocated when a DXVA index is first used (current picture or
reference). AV1 texture indices must be below 32 (the state the builder
keeps per surface); a higher index fails that frame.

**10 bit is not offered.** The decoder writes 10-bit pictures in a packed
layout of its own (4 samples in 5 bytes, `vdpu383_vp9.h`), not P010; the
UMD has no P010 texture (Resource.cpp, the plane views and the video
processor are NV12 only); the HEVC builder is Main only. So HEVC Main 10
and VP9 profile 2 are not listed, and `CheckVideoDecoderFormat` says no to
P010 for AV1: a player decodes a 10-bit stream in software. P010 would be:
a P010 texture (R16 storage of 1.5 times the height, R16/R16G16 plane
views, a video processor input), unpacking in `CopyToTarget`, and the
HEVC builder's Main 10 path.

**AV1 references from FFmpeg (fixed 2026-11-25).** On the board, FFmpeg's
`-hwaccel d3d11va` matched software decode for H.264, HEVC and VP9 but
not for a 1080p libsvtav1 clip (hw md5 `2b66808f3b1342a415c9a2c9cc745fb0`,
software `30f47ce96ff1f56a402f02373b39f7a5`), while vdectest passed the
AV1 kit. The register builder read `frame_refs[i].Index` as a surface
index; the DXVA AV1 specification, FFmpeg's `dxva2_av1.c` and Mesa's d3d12
driver put the reference's slot there (`ref_frame_idx[i]`, 0 to 7), and
only vdpu383test's parser (copied into vdectest) wrote the surface. Off the
board, the backend's register log for that clip with FFmpeg's convention
differed from the parser's from the third picture on (297 of 300
pictures); with the builder reading a slot (woa-rk3576 c373611) the
reference registers built from DXVA alone equal those built with the
parser's own `ref_frame_idx` for all 300 pictures. `tests/vdec-host` checks
the slot reading with references whose slot numbers are other slots'
surfaces.

**Film grain.** The VDPU383 does not apply AV1 film grain (vdpu383test's
refs: a stream with grain matches dav1d's output without it). A player
that leaves grain to the hardware decoder shows those streams without it.

**What CI checks.** `tests/vdec-host` (ci.yml): every test stream of
`tests/data` through the backend with vdpu383test's parsers and the fake
decoder, and the backend's register log against vdpu383test
`--dxva-only`'s for the same stream (`tests/vdec-host/reglog.md5`,
`video/README.md`), with both `UmdAv1Synth` forms. mesa.yml: vdectest
through the D3D11 runtime and the UMD on softpipe, HEVC, VP9 and AV1, the
fake pattern on every shown frame and the UMD's register log against the
same md5 (H.264 is checked on the host only: vdectest pads its bitstream to
128 bytes as FFmpeg does, vdpu383test does not, so register 66 differs),
and four profiles listed. Off the board, the backend's register logs were
identical to vdpu383test's for all 8-bit streams of the three kits (75
HEVC, 215 VP9, 21 AV1).

**Board test** (after section 5's install; one driver package, stage only,
`ConfigFlags` 0x20, full reboot):

- Inputs: firmware with *Video Decoder (VDPU383)* on (setup option
  `AcpiVdec`); vdpu383.sys from woa-rk3576 `main` (Iommu 1 by default,
  `UserAccess` 1); the maliwddm package and `vdectest-arm64` (with
  `codec-conformance.ps1`) of the run that built this branch; the kits
  `woa-vdpu383-hevc-2026-11-25`, `-vp9-`, `-av1-` (streams and FFmpeg
  6.1.1 references). The CI test certificate changes per build: import the
  package's `.cer` into Root and TrustedPublisher first.
- Settings: `UmdVideo` 2 (3 for Media Player and other players not on the
  list), `UmdVideoCodecs` absent, `UmdAv1Synth` 0, `UmdVp9Mpp` 0.

1. `vdectest hw --probe`: `decoder profiles: 4`, each with `NV12 yes`.
2. Each test stream: `vdectest hw testsrc-176x144-main.hevc`,
   `vdectest hw testsrc-176x144-p0.ivf`, `vdectest hw
   testsrc-352x288-2tiles.av1.ivf`: 12 frames out, 0 decode errors, the
   `output md5` of `video/README.md`, and `vdpu383.sys runs` up by the
   pictures decoded (13 for AV1).
3. The kits, one codec at a time:
   `codec-conformance.ps1 -Kit <kit>`; every 8-bit stream MATCH. If VP9
   shows mismatches, run again with `UmdVp9Mpp` 3 (full restart: the
   setting is read when the adapter opens) and send both result sets. If
   AV1 frames fail or come out grey, run again with `UmdAv1Synth` 1.
4. FFmpeg against itself on the same stream:

   ```
   ffmpeg -hide_banner -i F -autoscale 0 -pix_fmt yuv420p -f md5 -
   ffmpeg -hide_banner -hwaccel d3d11va -i F -autoscale 0 -pix_fmt yuv420p -f md5 -
   ```

   the same MD5 (AV1 with film grain excepted). Then mpv with
   `--hwdec=d3d11va-copy` and `--hwdec=d3d11va` on a 1080p HEVC, VP9 and
   AV1 file (the status line says the decoder is used, frame drops near
   0), as section 5 step 5.
5. `UmdVideoCodecs` 1 (H.264 only), full restart: `--probe` lists one
   profile again: the back-out that keeps H.264.

Send back: every vdectest output, `codec-results\results.txt` per kit,
`C:\mali\log` (the `video:` lines name the codec, the AV1 stream form and
the VP9 options), `vdpu383test info`.

**Risks.** VP9 and AV1 have never run on this decoder outside MPP; the AV1
stream form is a guess between two (`UmdAv1Synth`); a 4K HEVC stream with
20 surfaces of vdpu383test's layout (room for 4:2:2, about 19 MB each) is
near vdpu383.sys's 384 MiB per handle; Chromium needs C1 (feature level
11_0) before it asks for any of this.

## 8. Zero copy (V3b), and where the CPU time went

Written 2026-11-25, branches `vdec-zerocopy` (this repository) and
`vdpu383-import` (woa-rk3576). CI only; nothing of it has run on the board.

**What the board measured** (TaishanPi 3M, 2026-11-25, driver
5.17.13.886, FFmpeg BtbN master winarm64, 1080p30 clips of 300 frames,
`UmdVideo` 2): H.264 with `-hwaccel_output_format d3d11` (frames stay on
the GPU) 6.43 s wall and 5.0 s user CPU, 47 fps; downloaded, 11.07 s and
8.7 s; software 1.35 s. HEVC on the GPU 7.53 s and 6.6 s, software 2.29 s.
The decoder itself does a 1080p HEVC picture in about 2.2 ms (vdpu383test,
100 pictures, 223.8 ms in the decoder calls): ~450 fps, MPP's class.

**Where 15 to 20 ms of CPU per picture went**, read from the code (V3a,
`Video.cpp` and `video/vdec.c`, per picture):

| Step | Work | Cost at 1080p |
|---|---|---|
| `SubmitDecoderBuffers` | the picture parameters, matrix, slice control and bitstream copied out of the runtime's staging buffers (uncached, maliwddm's segment is not cache coherent) into memory of the UMD | the bitstream: 30 to 40 KB a picture for these clips, well under 0.5 ms |
| `vdec_*_decode` | the bitstream zeroed and copied into the decoder's buffer (write-combined), the register file built, `IOCTL_VDPU383_RUN` (synchronous: the thread sleeps while the decoder works) | the build and the ioctl: tens of us of CPU; the 2 to 3 ms in the decoder are wall time, not CPU |
| `CopyToTarget` | 3.1 MB of NV12 read from the decoder's surface, which is write-combined (Normal non-cacheable: every load goes to DRAM, no prefetch, no line fill), uploaded with `texture_subdata` into the texture: a staging copy, then Panfrost's tiled store into the texture's uncached pages | **the 15 ms**: uncached reads run at a few hundred MB/s on the A72; the tiled store and the staging copy add a few ms |
| (planes) | with shader resource views (mpv, Chromium) the same picture written again into the R8 and R8G8 plane textures | another copy, of the same kind |
| download (`-hwaccel d3d11va`) | FFmpeg's copy into a staging texture and its CPU read of that staging texture, uncached again | the other 12 ms of the download run |

Nothing else per picture is close: no allocation (surfaces are made once
per DXVA index), the register log and its hashing only with
`D3D10UMD_VDEC_REGLOG`, a log line only for the first three pictures and
every 300th.

**The design.**

1. A decoder output texture (NV12, `D3D10_DDI_USAGE_DEFAULT`, no shader
   resource views, no CPU access, not shared) is made linear
   (`PIPE_BIND_LINEAR`, Mesa patch 0082 `Resource.cpp`). Its BO is marked in
   the shim (`malikm_vdec_mark`) so that its export succeeds, which makes
   Panfrost treat it as shared: never swapped for a new BO when it is
   mapped for writing (the decoder would keep writing the old pages), never
   kept in the BO cache when freed (its destruction reaches the KMD at
   once). Its pitch and slice stride are read back from Panfrost.
2. At the first decode into it, the UMD asks maliwddm to export the
   allocation (`MW_ESCAPE_VDEC_EXPORT`, `include/maliwddm_abi.h`). maliwddm
   resolves the handle in the calling process, checks the allocation is
   resident with all its pages, copies the page frame numbers VidMm gave
   `MAP_APERTURE_SEGMENT` and returns a token, valid for that process only
   (`drivers/maliwddm/vdecexport.c`).
3. vdec hands the token to vdpu383.sys (`IOCTL_VDPU383_IMPORT`, interface
   version 3), which asks maliwddm for the pages kernel to kernel (the
   callback object `\Callback\MaliwddmVdecExport`,
   `video/vdpu383/vdpu383_import.h`; maliwddm checks token and process, so a
   process reaches only its own GPU memory and never names pages itself),
   and maps them at a free run of the decoder's IO addresses. Once per
   texture (all its array slices); freed when the texture is destroyed
   (`vdec_forget`).
4. The decoder's surfaces take the texture's layout: hor stride = the
   texture's pitch, ver stride = its luma rows (chroma at pitch x rows, the
   UMD's NV12 layout). Decided per stream geometry (H.264, HEVC) or at the
   first picture (VP9, AV1) from the first picture's texture; the surfaces
   of the decoder's own then hold only the co-located motion vectors (and
   AV1's CDFs). The picture's address is the import's IO address plus the
   slice's offset; references are read from wherever their index was
   decoded. Conditions: pitch a multiple of 64 and at least the copy path's
   (64-aligned width), rows at least the copy path's and a multiple of 16
   (H.264, HEVC) or 64 (VP9, AV1), slice offset 256-aligned. FFmpeg's
   1080p textures: 1920 x 1088 (H.264, VP9), 1920 x 1152 (HEVC, AV1: 128-row
   alignment), all inside.
5. Before the decoder writes a slice the UMD maps it for writing, 1 x 1
   texel: Panfrost then flushes and waits for every batch that reads or
   writes the texture (a shared BO is never replaced, so this waits). After
   the decode returns, any GPU job that samples the texture starts with an
   L2 clean and invalidate (maligpu.c), and the CPU's view of the pages is
   uncached: no cache maintenance is needed anywhere.
6. Pages taken back: VidMm may unmap (evict) a resident allocation, or it
   is destroyed. maliwddm calls vdpu383.sys's revoke routine right after the
   unmap's MMU section and before it returns (VidMm keeps the pages until
   then), or before a destruction; vdpu383.sys waits for a decode in
   progress (its decode gate), clears the mapping and marks the import dead.
   A decode naming it is refused before the decoder starts
   (`VDPU383_RUN_REVOKED`); vdec asks the UMD for a new export of the same
   texture, refreshes the import at the same IO addresses and runs the
   same register file again. An export racing an unmap is made under
   maliwddm's MMU lock, so it either precedes the unmap (and is ended by
   it) or sees the allocation unmapped.
7. Fallbacks, all to the V3a copy: a texture with shader resource views
   (mpv's `d3d11va`, Chromium; their plane textures are copies, so the
   picture is decoded into a surface of the decoder's own and copied as
   before), a layout outside the conditions, a texture made before the
   decoder's driver could import, an export maliwddm refuses (not resident:
   asked again every 64 frames), an import vdpu383.sys refuses, an older
   vdpu383.sys (interface 2), `UmdVideoZeroCopy` 0, `D3D10UMD_VDEC_ZEROCOPY=0`.
   Within a zero-copy stream, a frame whose texture does not fit is decoded
   into a picture of the decoder's own in the same layout and copied.

| Setting (GPU0 Device Parameters) | Default | Effect |
|---|---|---|
| `UmdVideoZeroCopy` | 1 | with `UmdVideo` on: maliwddm registers the export callback at start, the UMD makes decoder outputs linear and decodes into them. 0: neither, every picture copied as in V3a (full restart) |

With the copy gone, a picture costs the decoder's time plus the ioctl, the
register build and the small bitstream copies: at 2.2 ms in the decoder
(HEVC) that is about 400 fps in FFmpeg's single-threaded decode loop, and
user CPU a small fraction of the wall time. The download path keeps its
own copies (FFmpeg's staging read).

**AV1.** The mismatch of 2026-11-25 was not the copy: section 7, "AV1
references from FFmpeg".

**Board test.** Inputs: firmware with the decoder on; vdpu383.sys from
woa-rk3576 `vdpu383-import` (interface 3; `vdpu383test info` prints an
`imports` line); the maliwddm package of `vdec-zerocopy` (its CI
artifact); FFmpeg (BtbN winarm64); the four clips of 2026-11-25. Stage
both drivers only, `ConfigFlags` 0x20, full restart, as section 5.
`UmdVideo` 2 and `UserAccess` 1 as before; `UmdVideoZeroCopy` absent.
Then, as the normal user:

```
powershell -ExecutionPolicy Bypass -File vdec-bench.ps1 -Clips D:\clips -Ffmpeg D:\ffmpeg\bin\ffmpeg.exe
```

(`tools/windows/vdectest/vdec-bench.ps1`; vdpu383test.exe next to it for
the decoder's counters). Pass: every clip MATCH; `gpu` at 300 fps or more
for each codec with user time a small share of the wall; the decoder's
decode count up by the frames decoded; imports made, none failed. Then
`UmdVideoZeroCopy` 0, full restart, the same script: the V3a numbers
again (the comparison, and the back-out). Send `vdec-bench\bench.txt` of
both runs and `C:\mali\log`.

**Back-out.** `UmdVideoZeroCopy` 0 (copy path, same drivers), or the
previous vdpu383.sys (interface 2: the UMD sees no imports and copies).

**Risks.** The VDPU383 has not written pictures with a ver stride other
than the 16 or 64-aligned height MPP and vdpu383test use (HEVC and AV1 at
1152 rows); if those two mismatch while H.264 and VP9 match, that is it,
and the condition in `target_fits` (video/vdec.c) needs the rows equal to
the copy path's. VidMm under WDDM 2.0 has never evicted an allocation on
this board, so the revoke path has run only on paper.
