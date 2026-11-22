# Edge's D3D11 device on the Mali (branch edge-createdevice)

## The failure

TaishanPi 3M, 2026-11-22, package 6.27.50.355 (hdmi-hpd; its UMD is
d3d11 5e5f46b), `UmdDdi` 101, `UmdMsaa` not set (1). The same d3dprobe
binary, in the logged-in session:

- as `C:\mali\fake\msedge.exe`: the Mali `D3D11CreateDevice 0x887a0020`
  (DXGI_ERROR_DRIVER_INTERNAL_ERROR), the default hardware device too;
  WARP fine;
- as `d3dprobe0066.exe`: the Mali at 10_1, no multisample render target
  bit on any format.

The process name decides one thing only: whether the UMD gives its
multisample answers (Mesa patch 0066, `IsMsaaProcess` and
`EffectiveMsaa` in `Adapter.cpp`; `pDevice->msaa` is read nowhere but
`MsaaFormat` in `Device.cpp`). `GetCaps` answers zeros either way and the
DDI version comes from `UmdDdi` alone. So the runtime refused the device
for the multisample answers.

## The cause

`Device.cpp` in the UMD as of 0066 (`MsaaFormat`, and its callers
`CheckFormatSupport` and `CheckMultisampleQualityLevels`; in the Mesa
tree with 0001-0067 applied, `src/gallium/frontends/d3d10umd/Device.cpp`
around line 500 and 622) answered each format by its gallium translation
with `FormatTranslate(Format, false)`. A typeless format has none
(`PIPE_FORMAT_NONE`), so it answered 0 quality levels at every count,
while its typed members answered 1 at 4x.

The D3D11 runtime asks every format while it creates the device, the
typeless member of each family first. The UMD log of the CI run
(20eb638, run 44997756600, the runner's own D3D11 runtime) shows where it
stopped:

```
caps: CheckFormatSupport 1 -> 0x00000000 (msaa 1)      R32G32B32A32_TYPELESS
caps: CheckMultisampleQualityLevels 1 x2..x31 -> 0
caps: CheckFormatSupport 2 -> 0x0000001f (msaa 1)      R32G32B32A32_FLOAT
caps: CheckMultisampleQualityLevels 2 x2 -> 0
caps: CheckMultisampleQualityLevels 2 x3 -> 0
caps: CheckMultisampleQualityLevels 2 x4 -> 1
. DestroyDevice
```

That is, on the first typed format to report a sample count its typeless
format did not. The runtime returned 0x887A0020, which is what the board
showed in msedge.exe. In plainprobe.exe the UMD answers 0 everywhere, so
both members of each family agree and the device comes up at 10_1.

A multisampled resource is created typeless, with one sample count for
all its views, so a typeless family has one answer.

Two more rules came out of the CI runs once the first was met. Each time
the runtime destroyed the device at the answer named:

- 2311bc2: `CheckFormatSupport 21 -> 0x19`, which is
  R32_FLOAT_X8X24_TYPELESS claiming to be a multisample render target;
- 80783ef: the same format with 0x11, but `x4 -> 1`. A format with
  quality levels above 1x has to be a multisample render target, and a
  view of one plane of a depth format may not be one.

WARP, on the same runner, answers exactly this way: typeless formats
`q 1 1 1 1`, and R32_FLOAT_X8X24_TYPELESS and R24_UNORM_X8_TYPELESS
`sample ms-load q 1 0 0 0`. 0066 also split
families by bind: it asked D32_FLOAT as a render target, so the answer
was none, while R32_FLOAT got 4x; D16_UNORM and R16_UNORM split the same
way.

## The change

Mesa patch 0068 (UMD only; the KMD and the INF are untouched, so it
merges into d3d11 and into hdmi-hpd alike):

- a typeless family has multisample answers when every member that
  Panfrost renders or depth-tests also works at 4x (`MsaaFormat`,
  `FormatFamily`, `MsaaTarget`);
- then every member answers one quality level at 1x and at 4x, and that
  includes the typeless format;
- the four views of one plane of a depth format (R32_FLOAT_X8X24_TYPELESS,
  X32_TYPELESS_G8X24_UINT, R24_UNORM_X8_TYPELESS, X24_TYPELESS_G8_UINT)
  answer no multisample at all (`DepthPlaneView`);
- the render and depth target members also report the multisample render
  target bit, plus multisample load where Panfrost samples at 4x. Depth
  formats are asked with `PIPE_BIND_DEPTH_STENCIL`, and they get the
  bit without the render target bit;
- `R10G10B10_XR_BIAS_A2_UNORM` never answers multisample;
- still only in msedge.exe, chrome.exe, msedgewebview2.exe and
  d3dprobe.exe with `UmdMsaa` 1 (every process with 2). With `UmdMsaa` 0,
  and in every other process, nothing changes.

Mesa patch 0067 (diagnostics, in 20eb638):

- every `CheckFormatSupport` and `CheckMultisampleQualityLevels` answer,
  the first 1024 per device, goes into the UMD log (`C:\mali\log`), so a
  refused device still leaves a record of what it was refused for;
- on an adapter with no maliwddm behind it (the software driver path, CI),
  `D3D10UMD_UMDDDI` and `D3D10UMD_UMDMSAA` stand in for the Device
  Parameters values. They are never read when the KMD answered.

d3dprobe prints every format's support bits and its quality levels at 1x
to 8x. It also prints `MSAA CONSISTENCY` lines in two cases: a target
format whose multisample render target bit and sample counts disagree,
and any family member (except the depth plane views) whose quality levels
above 1x differ from its typeless format's, which is the runtime's
rule. When a device is refused,
it tries 10_1 and 10_0 alone.

## CI

mesa.yml, job "d3d10umd + softpipe + Panfrost (ARM64)", step "The
runtime's view of the multisample answers": d3dprobe runs on the emulated
G52 (`MALIKM_FAKE`) with `D3D10UMD_UMDDDI` 101 and `D3D10UMD_UMDMSAA` 1,
once as msedge.exe and once as plainprobe.exe, so the runner's D3D11
runtime checks Panfrost's own answers. The step fails unless both devices
come up, msedge.exe at 10_1 with no disagreement, and RGBA8 and BGRA8
report `ms-rt` and `q 1 0 1 0`. It also prints the runner's own adapters
(warpref.exe, WARP) for reference. The output and the UMD logs are in the
`panfrost-fake-trace` artifact.

Results, Mesa run 45000776389 (c725ccc), green:

- msedge.exe: `D3D11CreateDevice 0x00000000, feature level 10_1`;
  R8G8B8A8_UNORM, B8G8R8A8_UNORM and their sRGB formats `rt sample ms-rt
  ms-resolve ms-load q 1 0 1 0`; depth formats `ds ms-rt q 1 0 1 0`;
  `MSAA consistency: 0 disagreement(s)`;
- plainprobe.exe: 10_1, no multisample answers, as before (the default
  path is unchanged).

Runs that reproduced the refusal (0x887A0020): 44997756600 (0066 as it
was), 44999018908 (2311bc2), 44999855762 (80783ef).

## Board test, one install

One variable: the UMD. Leave `UmdDdi` 101 and `UmdMsaa` unset as they
are.

Package: CI run 45000776395 (c725ccc), artifact
`wddm-mali-g52-driver-arm64`, DriverVer 11/22/2026,8.0.56.668, with the
UMD from Mesa run 45000776389.

1. Copy the package (artifact `wddm-mali-g52-driver-arm64`) to the board,
   e.g. `C:\mali\pkg-edge\`, and d3dprobe.exe from the same CI run's
   `m3test-arm64` artifact to `C:\mali\pkg-edge\`. Make the two names:
   `copy d3dprobe.exe C:\mali\fake\msedge.exe` and
   `copy d3dprobe.exe C:\mali\fake\plainprobe.exe`.
2. Stage it, do not install it: `pnputil /add-driver C:\mali\pkg-edge\maliwddm.inf`.
3. Set `ConfigFlags |= 0x20` (CONFIGFLAG_REINSTALL) on
   `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0` as SYSTEM (task
   `malireinst`); poll until the value has 0x20 in it.
4. `mkdir C:\mali\log` (the UMD log; remove the `off` file if there is
   one), then a clean restart.
5. After logon, in the user's session (not over SSH: `schtasks /it /ru woo`
   with a `.cmd` wrapper), run, redirecting each to a file:
   - `C:\mali\fake\msedge.exe > C:\mali\edge-msedge.txt`
   - `C:\mali\fake\plainprobe.exe > C:\mali\edge-plain.txt`
6. Edge, with no flags, `edge://gpu`; save the page.
7. `echo > C:\mali\log\off` (stops new logs), then collect
   `C:\mali\edge-*.txt`, `C:\mali\log\msedge.exe-*.txt`,
   `C:\mali\log\plainprobe.exe-*.txt` and the edge://gpu page.

What to read:

- `Get-ItemProperty` of the adapter, or pnputil /enum-drivers: the new
  package is bound (DriverVersion as CI reports).
- edge-msedge.txt: adapter 0 (the Mali) `D3D11CreateDevice 0x00000000,
  feature level 10_1`; R8G8B8A8_UNORM and B8G8R8A8_UNORM `ms-rt`, `q 1 0 1
  0`; `MSAA consistency: 0 disagreement(s)`.
- edge-plain.txt: as before this change (10_1, no `ms-rt`), the control.
- If msedge.exe is still refused: its `feature level 10_1 alone` and
  `10_0 alone` lines, and the `caps:` lines of
  `C:\mali\log\msedge.exe-*.txt`, which hold every answer the runtime got
  before it refused; send those.
- edge://gpu: "Software Rendering: No", GL_RENDERER ANGLE on the Mali,
  "D3D11 Feature Level: D3D 10.1". If Edge's GPU process still dies, the
  FATAL line from the GPU process log.

To go back: `UmdMsaa` 0 in Device Parameters and a reboot turns the
multisample answers off in every process without reinstalling.

## Not known yet

- Whether the board's runtime checks exactly what the runner's does. The
  CI step runs the same Windows D3D11 runtime, but through the software
  driver path and with Panfrost on an emulated G52.
- Whether Edge then renders: CreateDevice is the first gate, and Skia's
  RGBA8 format and ANGLE's ES 3.0 come after it.
