# Edge and Chrome on the Mali: creating the device

Edge's GPU process gets the Mali at feature level 10_1 (`UmdDdi` 101,
the default). Presenting is in [EDGE-PRESENT.md](EDGE-PRESENT.md).
Settings: see [SETTINGS.md](SETTINGS.md).

## Why the multisample answers matter

Chromium asks ANGLE for OpenGL ES 3.0 and does not fall back to 2.0.
ANGLE gives ES 3.0 only at feature level 10_1 or higher, so the D3D10.0
DDI (`UmdDdi` 100, feature level 10_0) cannot run Edge's GPU process.

At 10_1 Skia still needs RGBA8 to be renderable, and it only is when
ANGLE reports at least one sample count. ANGLE gives a format sample
counts only when the driver calls it a multisample render target. So the
UMD reports multisample support where Panfrost really renders at 4x.
The G52 has no 2x, and ANGLE asks for 2x first and stops, so ANGLE keeps
the count {1} and nothing in Edge renders multisampled.

## The rules the D3D11 runtime enforces

The runtime asks every format while it creates the device, typeless
first, and refuses the device (0x887A0020,
DXGI_ERROR_DRIVER_INTERNAL_ERROR) when the answers disagree. The UMD
(`MsaaFormat`, `FormatFamily`, `MsaaTarget`, `DepthPlaneView` in
`Device.cpp`) answers:

- a typeless family has multisample answers when every member Panfrost
  renders or depth-tests works at 4x; then every member, the typeless
  format included, answers one quality level at 1x and at 4x;
- a format with quality levels above 1x is a multisample render target;
- the four single-plane views of depth formats
  (R32_FLOAT_X8X24_TYPELESS, X32_TYPELESS_G8X24_UINT,
  R24_UNORM_X8_TYPELESS, X24_TYPELESS_G8_UINT) answer no multisample;
- depth formats are asked with `PIPE_BIND_DEPTH_STENCIL` and get the
  multisample render target bit without the render target bit;
- multisample load is added where Panfrost samples at 4x;
- `R10G10B10_XR_BIAS_A2_UNORM` never answers multisample.

WARP answers the same way.

`UmdMsaa` decides who gets these answers: 1 (default) msedge.exe,
chrome.exe, msedgewebview2.exe and d3dprobe.exe only; 2 every process;
0 none; 4 and 5 mean the same as 1 and 2. The default is per process
because an earlier set of answers, given to every process, kept DWM from
getting a device on the board; 2 with the current answers has not been
run under DWM. `UmdMsaa` 0 and a reboot turns them off without
reinstalling.

## Collecting evidence

- The UMD log (`C:\mali\log`, see [EDGE-PRESENT.md](EDGE-PRESENT.md#umd-log))
  has a `caps:` line for each `CheckFormatSupport` and
  `CheckMultisampleQualityLevels` answer, the first 1024 per device. The
  last ones before `DestroyDevice` are what the runtime refused.
- d3dprobe prints each format's support bits and quality levels at 1x to
  8x, and `MSAA CONSISTENCY` lines where a target's multisample bit and
  sample counts disagree, or a family member's levels above 1x differ
  from its typeless format's. When a device is refused it tries 10_1 and
  10_0 alone. Run it as `C:\mali\fake\msedge.exe` and as
  `C:\mali\fake\plainprobe.exe`.
- Expected as msedge.exe: `D3D11CreateDevice 0x00000000, feature level
  10_1`; R8G8B8A8_UNORM and B8G8R8A8_UNORM (and sRGB) `rt sample ms-rt
  ms-resolve ms-load q 1 0 1 0`; depth formats `ds ms-rt q 1 0 1 0`;
  `MSAA consistency: 0 disagreement(s)`. As plainprobe.exe: 10_1, no
  multisample answers.
- edge://gpu: "Software Rendering: No", GL_RENDERER ANGLE on the Mali
  (`Direct3D11 vs_4_1 ps_4_1`), "D3D11 Feature Level: D3D 10.1".

## CI

mesa.yml, step "The runtime's view of the multisample answers", runs
d3dprobe on the emulated G52 (`MALIKM_FAKE`) with `D3D10UMD_UMDDDI` 101
and `D3D10UMD_UMDMSAA` 1 as msedge.exe and plainprobe.exe, so the
runner's D3D11 runtime checks Panfrost's answers. Those two environment
variables stand in for Device Parameters only when no maliwddm answers
(the software driver path). Output and UMD logs are in the
`panfrost-fake-trace` artifact.
