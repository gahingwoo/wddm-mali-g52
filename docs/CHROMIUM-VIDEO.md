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
| Output texture | NV12 (P010 for 10 bit), `BIND_DECODER | BIND_SHADER_RESOURCE`. `ConfigDecoderSpecific` bit 14 set: one texture per picture, MiscFlags `SHARED | SHARED_NTHANDLE`; clear (this UMD): one array, MiscFlags 0 with a decode swap chain, else `SHARED` (`--disable-direct-composition`) |
| Display | no copy for NV12: ANGLE samples the decoder's texture on the decoding device through R8_UNORM and R8G8_UNORM `Texture2DArray` views of one slice (EGL image per plane); another device (Dawn) opens the NT handle |
| Process | all in the GPU process: low integrity, restricted token |

## Steps and state

| | What | State |
|---|---|---|
| C0 | the facts above and the probe (`vdectest --chromium`, `chromeprobe.cpp`) | done |
| C1 | feature level 11_0 for Chromium's GPU process only (`UmdReportFL11`, Mesa 0080, `fl11test`) | CI only |
| C2 | decoding in the GPU process: V3a with vdpu383.sys `UserAccess` 2 on a test board; for real, decode work through maliwddm | V3a written; maliwddm path not started |
| C3 | shared NV12: plane views aliasing the texture, so a texture opened from the handle and the decoder see the same pixels; zero copy for shared decoder textures | written (Mesa 0085), CI only |
| C4 | H.264 in Edge on the board (`edge://media-internals` says D3D11VideoDecoder) | needs C1, C2, C3 |
| C5 | VP9 profile 0 for YouTube | in the UMD (VIDEO-DECODE.md) |
| C6 | HEVC Main, AV1 Main, P010 for 10 bit | HEVC and AV1 8 bit in the UMD; no P010 |

If the runtime or ANGLE does not accept 11_0 without compute shaders, the
remaining path is compute shaders in the D3D11 frontend, weeks of work.

Probe result (mesa.yml on softpipe, `UmdVideo` 2, fake decoder, 11_0):
for each codec the decoder is created as Chromium creates it (1920x1080,
`ConfigBitstreamRaw` 2 for H.264), Chromium's size probe reaches
4096x2304, and output and plane views of a slice work. A `SHARED` texture
fails with 0x8007000e because CI has no KMD; on the board it is made.

## C3: colours, speed and H.264 (board run 2026-11-26)

Edge 158 with `UmdVideo` 2, `UmdReportFL11` 1, vdpu383.sys `UserAccess` 2,
`--disable-direct-composition`, WDDM 2.0: VP9, AV1 and HEVC 1080p play
on the VDPU383 (HEVC proves it: Edge has no software HEVC).

**Wrong colours.** The bars screenshot fits one model exactly: the shader
got the Cr byte as Cb and 0 as Cr (grey turns teal, red blue, yellow
cyan, green stays green). So the R8G8 view returned (second byte, 0).
Definite: the texture is `SHARED`, so zero copy refused it, and each
picture was copied into the texture and again into two plane textures,
which Panfrost made AFBC. Suspected, not proven off the board: Panfrost's
AFBC R8G8 path (the staging blit into it, or sampling it) loses the
green channel. Mesa 0085 takes AFBC and the copies out of the path:

- The plane views are aliases: R8 and R8G8 textures over the NV12
  texture's own memory (new `pipe_screen::resource_alias`, Panfrost and
  softpipe), the R8G8 one starting at the first chroma row. NV12 textures
  with shader resource binding are linear. A texture opened in another
  device gets aliases of the same memory (`OpenResource` now knows NV12).
  Without an alias (another driver, `D3D10UMD_NV12_ALIAS=0`) the plane
  textures are copies as before, now linear.
- A write to the texture (decode, copy, update) first waits for the GPU's
  reads through the aliases.

**Slow playback** (1.5 to 2.4 s of video in 4 s). Per picture on the GPU
thread: the copy of 3.1 MB from the decoder's write-combined surface (15
to 20 ms measured with FFmpeg), a second read of it for the plane
textures, and Panfrost's write of one layer of an AFBC array (read the
layer back on the GPU, wait, write, blit back) twice. Now: no plane copy,
and shared decoder textures get zero copy (`C:\mali\vdec-shared-copy`
restores the copy). Zero copy needs rows that are a multiple of 16 (H.264,
HEVC) or 64 (VP9, AV1): Chromium's H.264 texture (1088 rows) qualifies,
its HEVC, VP9 and AV1 textures (1080 rows) do not and keep the one copy.

**H.264 fails at once** ("Format error", readyState 0, no decode). Not
explained yet. Off the board nothing in the UMD's answers stops Chromium:
the probe sets H.264 up as Chromium does and the decoder is created, and
Chromium's size probe lists it up to 4096x2304 (Chromium's fixed
1920x1088 entry would mean it never found our H.264 GUID). readyState 0
means the pipeline failed to start: the demuxer or a decoder's
initialisation, the audio decoder included. `tools/windows/cdp-media.ps1`
prints the media log that says which.

**`UserAccess` 2.** Nothing here removes it: with zero copy too, the GPU
process opens `\\.\VDPU383` and runs every decode itself. Only C2's real
path, decode work submitted through maliwddm (which already hands
vdpu383.sys pages kernel to kernel, `\Callback\MaliwddmVdecExport`),
would.

### C3 board test

Inputs: this branch's maliwddm package and `vdectest-arm64`; vdpu383.sys
with imports (`vdpu383test info` prints an `imports` line); the clips in
`C:\mali\edgehw` (`t-h264.mp4`, `t-hevc.mp4`, `t-vp9.webm`, `t-av1.mp4`,
`play.html`, `edgehw.cmd`, `cdp2.ps1`, `cdpcall.ps1`, `hevc.js`) and
`cdp-media.ps1`; `t-h264.264` made with
`ffmpeg -i t-h264.mp4 -c copy -bsf:v h264_mp4toannexb t-h264.264`.
Registry as on 2026-11-26 (`UmdVideo` 2, `UmdReportFL11` 1, `UserAccess`
2, WDDM 2.0). Install as VIDEO-DECODE.md section 5 (stage, `ConfigFlags`
0x20, full restart) and check the DriverVer.

1. Plane views on the hardware, as the normal user, `C:\mali\log` on:
   - `vdectest hw testsrc-176x144-high.264 --srv`: `through the plane
     views (made before decoding): MATCH, 12 frames`.
   - `vdectest hw t-h264.264 --chromium-tex --open -n 120`: `MiscFlags
     0x2: 0x00000000`, both plane view lines MATCH (this device and the
     second one). The UMD log has `its shader resource views alias it`
     and `video: NV12 decoder textures: N for the decoder to write`.
   - The same with `set D3D10UMD_NV12_ALIAS=0` (linear copies). A
     MISMATCH here but not above points at the copies.
2. Colours: `edgehw.cmd` from a scheduled task (`/IT`, the desktop user),
   then `cdp2.ps1 -Match play.html -ExprFile hevc.js` and
   `cdpcall.ps1 -Match play.html -Method Page.captureScreenshot -OutFile hevc2.png`.
   Pass: red, green, yellow, blue, magenta, cyan bars as `hevc-ref.png`;
   the grey bar grey, the time code orange.
3. Speed: `cdp-media.ps1 -Match play.html -Src t-hevc.mp4 -Seconds 20`
   and the same for `t-h264.mp4`, `t-vp9.webm`, `t-av1.mp4`. Pass:
   `result` with `t` near 20 and `frames` near 600, `dropped` near 0;
   `kVideoDecoderName` D3D11VideoDecoder. The UMD log
   (`C:\mali\log\msedge.exe-*`) has the decoder's `video: NV12 decoder
   textures` line: H.264 counted as for the decoder to write.
4. H.264: the `cdp-media.ps1` output of step 3 for `t-h264.mp4` (every
   `ERROR` and `error` line, the decoder properties), `ffprobe t-h264.mp4`,
   and the same run on `ffmpeg -i t-h264.mp4 -an -c copy t-h264-na.mp4`.
5. Back-outs, one at a time, each with step 2: `C:\mali\vdec-shared-copy`
   (copy for shared textures); `UmdVideoZeroCopy` 0.

Send back: the vdectest output, `hevc2.png`, the `cdp-media.ps1` outputs,
`C:\mali\log\msedge.exe-*` and `vdectest-*`, `vdpu383test info`.

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
