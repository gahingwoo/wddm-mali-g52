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
  the processes `UmdVideo` names** (Mesa patch 0076, `VideoEffective`).
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
| 2 | the same with the H.264 profile, when `\\.\VDPU383` opens |
| 3 | 2 in every process except dwm.exe, LogonUI.exe, consent.exe (Media Player, Edge) |

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
| V3a H.264 | `ID3D11VideoDevice` with H.264 VLD (no FGT), short slice format, NV12 decoder output, `DecoderEndFrame` decodes on the VDPU383; FFmpeg d3d11va(-copy) and mpv `--hwdec=d3d11va-copy` | **this branch**: Mesa 0076, `video/`, vdectest, CI; vdpu383.sys `UserAccess` on woa-rk3576 `vdpu383-d3d11va`. Not run on the board |
| V3a HEVC Main | the same for `D3D11_DECODER_PROFILE_HEVC_VLD_MAIN` | register builder and parser written (woa-rk3576 branch `vdpu383-hevc`, `lib/vdpu383_hevc.c`); 75 conformance streams pass the parser and the reference set checks on Linux, with the SPS's sets and with DXVA alone. Next: vdpu383test on the board (`hevc-conformance.ps1`), then the profile in the UMD |
| V3a VP9 profile 0 | `D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0` | not started. VDPU383 VP9 is in MPP only (`hal_vp9d_vdpu383.c`, no mainline driver). DXVA hands the decoder the frame header and the compressed header in the bitstream; the driver keeps the four frame contexts, decodes the probability deltas of the compressed header (a bool decoder) and adapts the probabilities from the counts the hardware writes after each frame (MPP `vp9d` does all three). Then vdpu383test with the libvpx test vectors, then the UMD |
| V4 | NV12 as two shader views (R8 and R8G8 planes), the D3D11 video processor (NV12 and RGB in, RGB out, on the GPU): Media Player, Edge, mpv zero-copy rendering | **this branch**: Mesa 0077, `vdectest --vp`, CI on softpipe. Not run on the board |
| V3b | zero copy through maliwddm and the decoder's IOMMU; sandboxed processes | not started |

H.264 limits in V3a: 8-bit 4:2:0, progressive and field/MBAFF as the
register builder handles them, up to 4096x2304; no encryption.

## 4. What is on the branch

- `mesa-patches/0076-...`: `Adapter.cpp` (the interface list, `GetCaps`
  for the D3D11.1 interface only), `Device.cpp` (the D3D11.1 branch of
  `CreateDevice`), `D3D11.cpp` (the D3D11.1 table, the DXGI 1.2 table),
  `Video.cpp` (the video DDI), `VideoStubs.h` (the video entry points not
  carried out, generated from the WDK header by
  `tools/gen-video-stubs.py`), `Resource.cpp` (NV12 as one R8 texture of
  1.5 times the height, plane-aware copies), `Shader.cpp` (constant
  buffer windows).
- `mesa-patches/0077-...`: `VideoProcessor.cpp` (the video processor
  DDI, drawn in a pipe context of its own, and the NV12 plane textures),
  `VideoShaders.c` (the NV12 and RGB fragment shaders, TGSI built with
  ureg: luma filtered by the sampler, chroma interpolated from texel
  fetches, BT.601 or BT.709, studio or full range), `Resource.cpp` (an
  NV12 texture with `BIND_SHADER_RESOURCE` gets R8 and R8G8 plane
  textures; copies and updates keep them in step), `Shader.cpp` (an
  R8_UNORM or R8G8_UNORM view of NV12 is a view of a plane), `Video.cpp`
  (a decode writes the planes too), `Device.cpp` (video processor input
  and output in `CheckFormatSupport`).
- `video/vdec.c`: the backend (README in `video/`). With
  `D3D10UMD_VDEC_FAKE=1` it builds and checks the register file and
  writes a known pattern instead of decoding.
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
  one with 0076). Its DriverVer is newer than the board's, so it is the
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
