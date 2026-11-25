# Hardware video decode (D3D11 video in the UMD, VDPU383)

Off by default: with `UmdVideo` absent or 0 every process gets the plain
D3D10.x driver. Settings: see [SETTINGS.md](SETTINGS.md). Chrome and
Edge have extra requirements: [CHROMIUM-VIDEO.md](CHROMIUM-VIDEO.md).

## 1. How it works

Every Windows player reaches a hardware decoder through
`ID3D11VideoDevice`: Edge and Chrome (`D3D11VideoDecoder`, never a
third-party MFT), Media Player, Movies & TV, Photos and Firefox (Media
Foundation's inbox DXVA decoders on the pipeline's D3D11 device), mpv,
FFmpeg and VLC (`d3d11va`, `d3d11va-copy`). The runtime offers a video
device only on a D3D11.1 device interface, so the UMD has one:

- The D3D11.1 table is the D3D10.x implementation behind new pointers.
  Hull, domain, compute and command-list entry points do nothing (unused
  at feature level 10_x). It is offered only in the processes `UmdVideo`
  names; dwm.exe, LogonUI.exe and consent.exe never get it.
- The video DDI (Mesa patch 0078 `Video.cpp`), the video processor and
  NV12 plane views (0079, V4), HEVC, VP9 and AV1 (0081), zero copy (0082).
- The backend is `video/vdec.c` ([video/README.md](../video/README.md)),
  using the VDPU383 register builders from woa-rk3576 copied into
  `video/`. The decoder needs firmware device `ACPI\RKCP0383`
  (edk2-rk3576, setup option *Video Decoder (VDPU383)*, `AcpiVdec`, off
  by default) and vdpu383.sys from woa-rk3576.
- NV12 is one R8 texture of 1.5 times the height: luma rows then chroma
  rows at one pitch, which is also the VDPU383's own layout. With
  `BIND_SHADER_RESOURCE` it also gets R8 and R8G8 plane textures that
  copies and decodes keep in step.

Who drives the decoder:

- **V3a (copy).** The UMD, in the player's process, opens `\\.\VDPU383`
  (`IOCTL_VDPU383_ALLOC`, one `IOCTL_VDPU383_RUN` per picture,
  synchronous). The decoder writes into surfaces of its own and
  `DecoderEndFrame` copies each picture into the application's texture.
  Opening the device needs vdpu383.sys `UserAccess` 1. Edge's GPU process
  (low integrity, restricted token) needs `UserAccess` 2, which lets any
  low-integrity process program the decoder: test boards only.
- **V3b (zero copy).** The decoder writes straight into the application's
  texture; section 8. Decoding from sandboxed processes through maliwddm
  is not started.

maliwddm.sys only passes the switches to the UMD
(`MW_UMD_ADAPTER_INFO.Video`) and, for V3b, exports allocations.

## 2. Settings

GPU0 Device Parameters, see [SETTINGS.md](SETTINGS.md): `UmdVideo`
(0 off; 1 the D3D11.1 interface and a video device with no profiles, in
mpv, ffmpeg, ffplay, ffprobe, vlc, vdectest and d3dprobe; 2 the same with
the decoder's profiles; 3 as 2 in every process except dwm, LogonUI and
consent), `UmdVideoCodecs`, `UmdAv1Synth`, `UmdVp9Mpp`,
`UmdVideoZeroCopy`. All need a full restart.

vdpu383.sys (`ACPI\RKCP0383\0\Device Parameters`): `UserAccess` 0, 1 or
2 as above.

UMD environment variables, for tests:

| Variable | Effect |
|---|---|
| `D3D10UMD_VDEC_FAKE=1` | build and check the register file, write a known pattern instead of decoding (CI) |
| `D3D10UMD_VDEC_REGLOG=<file>` | log every register file built |
| `D3D10UMD_VDEC_ZEROCOPY=0` | force the copy path |
| `D3D10UMD_UMDVIDEO=<n>` | `UmdVideo` when no KMD answers (CI) |

## 3. Codec status

| Codec | Profile GUID | `ConfigBitstreamRaw` | State |
|---|---|---|---|
| H.264 VLD no FGT | `1b81be68-a0c7-11d3-b984-00c04f2e73c5` | 2 | board: FFmpeg `-hwaccel d3d11va` matches software decode |
| HEVC Main | `5b11d51b-2f4c-4452-bcc3-09f2a1160cc0` | 1 | board: matches software decode; vdpu383test 60 of 75 conformance streams bit-exact, the rest allocation failures fixed since by the driver's IOMMU, not rerun |
| VP9 profile 0 | `463707f8-a1d0-4585-876d-83aa6d60b89e` | 1 | board: matches software decode; 215 vectors checked off the board |
| AV1 Main, 8 bit | `b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a` | 1 | reference slot bug fixed (section 9), not rerun on the board; 21 streams checked off the board |
| 10 bit (HEVC Main 10, VP9 profile 2, AV1 10 bit) | | | not offered |
| V4 video processor and plane views | | | CI on softpipe only |

The board results are from FFmpeg on the TaishanPi 3M, 2026-11-25.
Profiles are listed in the table's order, NV12 output only, up to
4096x2304, 8-bit 4:2:0, no encryption. H.264 handles progressive and
field/MBAFF as the register builder does.

Slice and tile offsets are relative to the bitstream buffer of the same
`SubmitDecoderBuffers` call, so a frame split over several calls
(Chromium does this) works. AV1 texture indices must be below 32; a
higher index fails that frame.

Why no 10 bit: the decoder writes 10-bit pictures packed (4 samples in 5
bytes), the UMD has no P010 texture, and the HEVC builder is Main only.
`CheckVideoDecoderFormat` refuses P010, so players decode 10-bit streams
in software.

## 4. Tests and CI

- `tools/windows/vdectest/`: drives `ID3D11VideoContext` as FFmpeg's
  d3d11va-copy does and compares every frame with a reference.
  `vdectest hw --probe` lists the device and profiles; `--vp` tests the
  video processor and plane views (`vptest.cpp`); `--chromium` is the
  Chromium probe ([CHROMIUM-VIDEO.md](CHROMIUM-VIDEO.md)).
  `codec-conformance.ps1 -Kit <kit>` runs a conformance kit;
  `vdec-bench.ps1` measures speed, CPU and correctness per codec.
- `tests/vdec-host` (ci.yml): every stream in `tests/data` through the
  backend under ASan with the fake decoder, and its register log against
  vdpu383test `--dxva-only` (`tests/vdec-host/reglog.md5`) with both
  `UmdAv1Synth` forms; also the AV1 reference slot reading.
- mesa.yml: vdectest through the D3D11 runtime on softpipe for all four
  codecs with the fake decoder and the register log md5 (H.264 only on
  the host: vdectest pads the bitstream to 128 bytes like FFmpeg, so
  register 66 differs); the triangle and present tests on the D3D11.1
  interface as mpv.exe and ffplay.exe; and that `UmdVideo` 0, an unlisted
  process and a missing `\\.\VDPU383` give no decoder.

## 5. Board test: install, H.264, V4

Inputs: firmware with the decoder on; vdpu383.sys from woa-rk3576
(artifact `woa-drivers-signed-arm64`, folder `vdpu383`); the maliwddm
package `wddm-mali-g52-driver-arm64`; `vdectest-arm64` from the Mesa
workflow (vdectest.exe, `testsrc-*` streams, `codec-conformance.ps1`);
the H.264 conformance streams BA1_Sony_D, CABA3_SVA_B, MR2_TANDBERG_E,
FRExt1_Panasonic with FFmpeg references; FFmpeg for Windows on Arm
(BtbN `winarm64-gpl`) and mpv for aarch64 (note both file names); the
sample (section 6). Import the package's `.cer` into Root and
TrustedPublisher first: the CI test certificate changes per build.

Install as administrator after `w32tm /resync /force`:

```
pnputil /add-driver vdpu383\vdpu383.inf
pnputil /add-driver maliwddm\maliwddm.inf
```

Then, as SYSTEM, set `ConfigFlags` |= 0x20 on
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0` and on the GPU0
instance key, and do a full restart. Stage only: never
`pnputil /install` or `/restart-device` on the running display driver.
Delete the replaced packages from the DriverStore afterwards.

**A, switches off.** Desktop, Edge and `m5test perf` as before;
`vdectest hw --probe` exits 1 (no video device).

**B, switches on.** `UserAccess` 1 on `RKCP0383\0`, `UmdVideo` 2 on GPU0,
`mkdir C:\mali\log`, full restart. As the normal user:

1. `vdectest hw --probe`: `ID3D11VideoDevice: yes`, the profiles with
   `NV12 yes`, exit 0.
2. `vdectest hw testsrc-176x144-high.264`: 12 frames,
   `output md5 28b5ac0bb9dbb31571f6692a27b3d31a`, `vdpu383.sys runs: +12`.
3. `vdectest hw BA1_Sony_D.jsv --ref BA1_Sony_D.yuv` and the other three
   streams: `reference: MATCH`, 0 decode errors.
4. FFmpeg:
   ```
   ffmpeg -hide_banner -i bbb.mp4 -t 20 -an -f framemd5 sw.md5
   ffmpeg -hide_banner -hwaccel d3d11va -i bbb.mp4 -t 20 -an -f framemd5 hw.md5
   fc sw.md5 hw.md5
   ffmpeg -hide_banner -benchmark -hwaccel none    -i bbb.mp4 -t 60 -an -f null -
   ffmpeg -hide_banner -benchmark -hwaccel d3d11va -i bbb.mp4 -t 60 -an -f null -
   ```
   No differences; hardware `speed=` at least 1.0x. If FFmpeg picks
   another adapter, add `-hwaccel_device N` (index from `--probe`).
5. mpv, with `--hwdec=d3d11va-copy` and with `--hwdec=no`:
   ```
   mpv --hwdec=d3d11va-copy --gpu-api=d3d11 --d3d11-flip=no --length=60 ^
       --term-status-msg="hw ${hwdec-current} drop ${frame-drop-count} ddrop ${decoder-frame-drop-count}" bbb.mp4
   typeperf "\Processor(_Total)\% Processor Time" "\Process(mpv)\% Processor Time" -si 1 -sc 60 -o cpu-<on|off>.csv
   ```
   Pass: `hw d3d11va-copy`, drops near 0 at 1080p30, CPU clearly lower
   than `--hwdec=no`, decoder `runs` (`vdpu383test info` as administrator)
   up by about the frames shown. If mpv's renderer fails, repeat with
   `--vo=null` to separate the decoder from the renderer.
6. V4: `vdectest hw --vp` (`video processor tests: PASS`, 3 per channel
   allowed when scaled), `vdectest hw testsrc-176x144-high.264 --vp`
   (`MATCH`), and `mpv --hwdec=d3d11va ...` showing `hw d3d11va`. Then
   `UmdVideo` 3, full restart, play `bbb.mp4` in Media Player: a picture,
   and `video: frame` and `video: blit` lines in its UMD log.

Back-out: `UmdVideo` 0 or deleted and `UserAccess` 0, full restart. If
worse, set *Video Decoder* Disabled in firmware setup: no `RKCP0383`,
nothing opens it.

## 6. The sample

Big Buck Bunny 1080p30 H.264,
`https://download.blender.org/demo/movies/BBB/bbb_sunflower_1080p_30fps_normal.mp4.zip`
(the plain .mp4 URL is gone), unzipped and renamed `bbb.mp4`. Check with
`certutil -hashfile <file> SHA256`:

| File | Bytes | sha256 |
|---|---|---|
| `bbb_sunflower_1080p_30fps_normal.mp4.zip` | | `e320fef389ec749117d0c1583945039266a40f25483881c2ff0d33207e62b362` |
| `bbb_sunflower_1080p_30fps_normal.mp4` | 276134947 | `ae51005850b0ff757fe60c3dd7a12d754d3cd2397d87d939b55235e457f97658` |

## 7. Board test: HEVC, VP9, AV1

After section 5's install, with vdpu383.sys from woa-rk3576 `main`
(`Iommu` 1 by default, `UserAccess` 1) and the kits
`woa-vdpu383-hevc-2026-11-25`, `woa-vdpu383-vp9-2026-11-25`,
`woa-vdpu383-av1-2026-11-25` (streams and FFmpeg 6.1.1 references).
`UmdVideo` 2 (3 for Media Player), `UmdVideoCodecs`, `UmdAv1Synth` and
`UmdVp9Mpp` absent.

1. `vdectest hw --probe`: `decoder profiles: 4`, each `NV12 yes`.
2. `vdectest hw testsrc-176x144-main.hevc`, `testsrc-176x144-p0.ivf`,
   `testsrc-352x288-2tiles.av1.ivf`: 12 frames, 0 errors, the `output md5`
   in [video/README.md](../video/README.md), `runs` up by the pictures
   decoded (13 for AV1).
3. `codec-conformance.ps1 -Kit <kit>` per codec: every 8-bit stream
   MATCH. VP9 mismatches: run again with `UmdVp9Mpp` 3. AV1 failures or
   grey frames: run again with `UmdAv1Synth` 1. Full restart after each
   change; send both result sets.
4. FFmpeg against itself, the same MD5 except AV1 with film grain:
   ```
   ffmpeg -hide_banner -i F -autoscale 0 -pix_fmt yuv420p -f md5 -
   ffmpeg -hide_banner -hwaccel d3d11va -i F -autoscale 0 -pix_fmt yuv420p -f md5 -
   ```
   Then mpv `--hwdec=d3d11va-copy` and `--hwdec=d3d11va` on 1080p HEVC,
   VP9 and AV1 files, as section 5 step 5.
5. `UmdVideoCodecs` 1, full restart: `--probe` lists H.264 only. This is
   the back-out that keeps H.264.

## 8. Zero copy (V3b)

On by default with `UmdVideo` on (`UmdVideoZeroCopy` 1). Needs
vdpu383.sys with interface version 3 (woa-rk3576 `vdpu383-import`;
`vdpu383test info` prints an `imports` line). With an older vdpu383.sys
(interface 2) the UMD sees no imports and copies. CI only.

How it works: a decoder output texture (NV12, default usage, no shader
resource views, no CPU access, not shared) is made linear and its BO
marked in the shim (`malikm_vdec_mark`) so Panfrost never replaces or
caches it. At the first decode the UMD asks maliwddm to export it
(`MW_ESCAPE_VDEC_EXPORT`, `drivers/maliwddm/vdecexport.c`): maliwddm
checks the allocation is resident and returns a token valid only in that
process. vdec passes the token to vdpu383.sys (`IOCTL_VDPU383_IMPORT`),
which gets the pages from maliwddm kernel to kernel (callback
`\Callback\MaliwddmVdecExport`, `video/vdpu383/vdpu383_import.h`) and maps
them in the decoder's IOMMU, once per texture until `vdec_forget`.

- Layout: the decoder uses the texture's pitch and luma rows. Conditions:
  pitch a multiple of 64, rows a multiple of 16 (H.264, HEVC) or 64 (VP9,
  AV1) and at least the copy path's, slice offset 256-aligned. FFmpeg's
  1080p textures (1920x1088 H.264 and VP9, 1920x1152 HEVC and AV1) fit.
- Coherency: before a decode the UMD maps the slice for writing (1x1), so
  Panfrost waits for batches using it; GPU jobs that sample it start with
  an L2 clean and invalidate; the CPU view is uncached.
- Eviction: maliwddm calls vdpu383.sys's revoke routine before VidMm
  unmaps or destroys the allocation. A decode naming a revoked import is
  refused (`VDPU383_RUN_REVOKED`) and vdec re-exports and runs again.
- Falls back to the copy path for: textures with shader resource views
  (mpv `d3d11va`, Chromium), layouts outside the conditions, textures
  made before the decoder could import, a refused export (retried every
  64 frames) or import, interface 2, `UmdVideoZeroCopy` 0,
  `D3D10UMD_VDEC_ZEROCOPY=0`. A single frame that does not fit is decoded
  into the decoder's own surface and copied.

Why: with the copy path, 1080p30 on the board cost 15 to 20 ms of CPU
per picture, nearly all of it reading 3.1 MB of NV12 from the decoder's
write-combined surface and storing it tiled into the texture. The
decoder itself does a 1080p HEVC picture in about 2.2 ms. Measured
2026-11-25 with FFmpeg and `-hwaccel_output_format d3d11`: H.264 6.43 s
wall and 5.0 s user CPU for 300 frames (47 fps), HEVC 7.53 s and 6.6 s;
software decode took 1.35 s and 2.29 s.

Board test: firmware with the decoder on, vdpu383.sys from
`vdpu383-import`, the current maliwddm package, FFmpeg (BtbN winarm64),
1080p30 clips of each codec. Install as section 5, `UmdVideo` 2,
`UserAccess` 1, `UmdVideoZeroCopy` absent. As the normal user, with
vdpu383test.exe next to the script:

```
powershell -ExecutionPolicy Bypass -File vdec-bench.ps1 -Clips D:\clips -Ffmpeg D:\ffmpeg\bin\ffmpeg.exe
```

Pass: every clip MATCH; `gpu` at 300 fps or more per codec with user
time a small part of the wall time; decode count up by the frames
decoded; imports made and none failed. Then `UmdVideoZeroCopy` 0, full
restart, the same script for the copy-path numbers. This is also the
back-out.

## 9. Known issues

- AV1 film grain is not applied by the VDPU383; streams with grain show
  without it.
- AV1: FFmpeg's `-hwaccel d3d11va` mismatched software decode on a 1080p
  libsvtav1 clip because the builder read `frame_refs[i].Index` as a
  surface index; DXVA puts the reference slot (0 to 7) there. Fixed in
  the builder; needs a board rerun.
- The AV1 stream form the VDPU383 accepts is not known (`UmdAv1Synth` 0
  or 1). VP9 and AV1 have only MPP as a reference.
- A 4K HEVC stream with 20 surfaces (about 19 MB each) is close to
  vdpu383.sys's 384 MiB per handle.
- Zero copy: the VDPU383 has not written with a ver stride other than the
  16 or 64-aligned height MPP uses. If HEVC and AV1 (1152 rows) mismatch
  while H.264 and VP9 match, `target_fits` in `video/vdec.c` must require
  the copy path's rows. The revoke path has not run: VidMm has never
  evicted an allocation on this board.
- Chromium needs feature level 11_0 and more before it decodes:
  [CHROMIUM-VIDEO.md](CHROMIUM-VIDEO.md).

## 10. Evidence to send back

All command output; `cpu-on.csv`, `cpu-off.csv`;
`codec-results\results.txt` per kit; `vdec-bench\bench.txt` per run;
`C:\mali\log` (the UMD logs of each player: the `video:` lines give
frames, errors, codec, AV1 stream form, VP9 options and the driver's run
count); `vdpu383test info`;
`reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters"`.
On a bugcheck: the stop code and the Device Parameters values after the
next boot.
