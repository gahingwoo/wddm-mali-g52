# Edge's device removed at its first present (branch edge-present)

## The failure

TaishanPi 3M, 2026-11-22, maliwddm 8.16.32.821 (hpd-edge c445902, UMD of
Mesa patch 0068), `UmdDdi` 101, `UmdMsaa` not set. d3dprobe as
msedge.exe now gets the Mali at 10_1 (EDGE-CREATEDEVICE.md). Edge
157.0.4341.60 in the logged-in session, about 1.4 s after its GPU
process started:

```
dxgi_swap_chain_image_backing.cc:266 Present1 failed: ... suspended (0x887A0005)
dcomp_presenter.cc:234 DComp commit failed: SolidColorSurfacePoolCreateSurface, ... (0x887A0005)
Renderer11.cpp:2206 testDeviceLost: The D3D11 device was removed, HRESULT: 0x887A0020
Restarting GPU process due to unrecoverable error. Context was lost.  (exit_code=34)
```

That happened three times, and then Edge fell back to software. There was
no TDR and no LiveKernelEvent, and the KMD logged no `Fail_*` breadcrumbs.

## What it means

0x887A0005 (DXGI_ERROR_DEVICE_REMOVED) from Present1 says the device was
already removed, or was removed by that call. The reason, 0x887A0020
(DXGI_ERROR_DRIVER_INTERNAL_ERROR), is the runtime's answer to a driver
error code it does not accept. A reset or a hang would give
DEVICE_RESET or DEVICE_HUNG. `pfnSetErrorCb` accepts E_OUTOFMEMORY,
DXGI_DDI_ERR_WASSTILLDRAWING, DXGI_DDI_ERR_UNSUPPORTED,
DXGI_DDI_ERR_NONEXCLUSIVEMODE and D3DDDIERR_DEVICEREMOVED. For any other
code it removes the device, and a DXGI DDI function that returns a code
of its own making (rather than the HRESULT of a runtime callback) gets the
same treatment.

The UMD had these, as of hpd-edge c445902 (Mesa tree with 0001-0068):

| where | code | when |
|---|---|---|
| `Resource.cpp` ResourceMap, `SetError(hDevice, E_FAIL)` | E_FAIL | the pipe map returned no pointer (a lock that failed: `pfnLockCb` error, not logged) |
| `Resource.cpp` OpenResource, two `SetError(hDevice, E_INVALIDARG)` | E_INVALIDARG | not one allocation, or no description of this driver's in the private data (a surface the runtime or another driver made) |
| `OutputMerger.cpp` CreateRenderTargetView, `SetError(hDevice, E_NOTIMPL)` | E_NOTIMPL | a render target view of a buffer |
| `malikm_drm.c` malikm_present_allocation, `hr = E_FAIL` | E_FAIL from Present | no pfnPresentCb could be called (device not bound, or no DXGI callbacks) |
| `malikm_drm.c` malikm_set_display_mode, `hr = E_FAIL` | E_FAIL from SetDisplayMode | the same |
| `DxgiFns.cpp` RotateResourceIdentities | E_FAIL | a rendered-into primary (dwm's full-screen chain only) |

None of them names the DDI in the log: SetError wrote `SetError 0x80004005`
and nothing more, and a failed `pfnRenderCb` or `pfnLockCb` was only
visible with the stderr trace. The board session collected no UMD log, so
which of them Edge hit is not known yet. Edge's own log shows only the
consequence.

## The change

Mesa patch 0069 and `win32-compat/malikm_drm.c`, UMD only. The KMD and
the INF are untouched.

- `SetError` (State.h) is now a macro that passes `__func__`. Every error
  goes into the UMD log as `ddi-error: <DDI> 0x<code>`, and the first of
  each device is marked `ddi-error (first)`. WASSTILLDRAWING, which is an
  answer and not a failure, is logged four times at most, on a count of
  its own.
- A code outside the accepted set is logged as `a code the runtime
  removes the device for; reported as E_OUTOFMEMORY`, and it is reported
  as E_OUTOFMEMORY. The call fails, which the application sees as
  E_OUTOFMEMORY from the API, and the device lives.
- Present, SetDisplayMode and RotateResourceIdentities log every result
  that is not S_OK as `dxgi-result: <DDI> 0x<code>`, including the
  HRESULTs of `pfnPresentCb`, which are passed on as before.
- `malikm_drm.c`: a failed `pfnRenderCb` or `pfnLockCb` is logged with its
  HRESULT (`ddi-error: pfnRenderCb ...`). A present or a display mode
  change that cannot be asked of the runtime is logged and returns S_OK,
  so the frame is dropped instead of the device being removed.

d3dprobe has a new "edge path" section, run after the swap chain test.
It repeats Chromium's GPU process steps on the device and prints the
device's removed reason after each step:

1. CreateSwapChainForComposition (FLIP_SEQUENTIAL, BGRA8, two buffers,
   premultiplied alpha);
2. DirectComposition: a device on the D3D device, a target on a window,
   a visual with the swap chain as content, Commit;
3. three frames: clear, then Present1, the last two with a dirty rect;
4. a DComp surface: CreateSurface, BeginDraw, clear, EndDraw, Commit;
5. a texture shared by NT handle with a keyed mutex: CreateSharedHandle,
   OpenSharedResource1, AcquireSync and ReleaseSync.

The first step marked `<== REMOVED` is the one to read the UMD log at.

## CI

Runs on bba6a25, green: Mesa 45004408540 and CI 45004408487. The step
"The runtime's view of the multisample answers" in mesa.yml now also
fails unless msedge.exe and plainprobe.exe both end the edge path with
`edge path: device alive`.

On the runner the UMD is a software driver with no maliwddm behind it,
so this checks the probe and the logging rather than the board's path:

- CreateSwapChainForComposition answers 0x887A0001 (DXGI_ERROR_INVALID_CALL),
  as it does for any software driver;
- DComp CreateSurface answers 0x80070057;
- the shared textures answer E_OUTOFMEMORY, logged as
  `ddi-error (first): CreateResource 0x8007000e`, because there is no KMD
  to allocate them. The device stays alive.

WARP on the same runner passes every step, swap chain included.

## Board result, 2026-11-23 (DriverVer 22.7.16.475)

TaishanPi 3M, HDMI only, 2560x1440, logged-in session. Package of CI
45075872745, UMD of Mesa 45004408540 (patches 0001-0069). Files in
`board-session-prep/edge2-22.7.16.475/`.

- d3dprobe as msedge.exe and as plainprobe.exe, identical: DISCARD and
  FLIP_SEQUENTIAL swap chains create and present. On the edge path
  CreateSwapChainForComposition failed with 0x8007000e (UMD log:
  `ddi-error (first): CreateResource 0x8007000e`), the device stayed
  alive, DComp device, target, visual, Commit, CreateSurface and BeginDraw
  all answered 0, and the first draw after BeginDraw killed the process:
  `CRASH code 0xc0000005 at maliumd+0x19c100, reading address 0x344`.
- Edge: `RENDERER=WebGL2 ANGLE (... Mali-G52 (WDDM) ... Direct3D11 vs_4_1
  ps_4_1, D3D11)`, so ANGLE now has a 10_1 hardware device and WebGL2.
  The GPU process still lost the device at its first Present1, as on
  11-22, three times, and Edge gave up. That process wrote no UMD log.

## Three UMD bugs

### The crash: a draw with no input layout (patch 0070)

Symbolized with the package's PDB (`llvm-symbolizer --obj=maliumd_arm64.dll
--relative-address`, the DLL renamed to libgallium_d3d10.dll beside the
.pdb):

```
+0x19c100 panfrost_emit_vertex_data   pan_cmdstream.c:2343  (so->nr_bufs)
          panfrost_update_state_3d    pan_cmdstream.c:3301
          panfrost_draw_vbo           pan_cmdstream.c:3703
+0x19c02c panfrost_update_state_3d    pan_cmdstream.c:3264  (lr)
+0x1377c  Draw                        Draw.cpp:198 (util_draw_arrays)
```

`so` is `ctx->vertex`, the bound vertex elements state, and it was NULL.
The draw is the D3D11 runtime's own: ClearRenderTargetView of a view whose
clear is limited to the BeginDraw region of a DComp atlas surface (1024x1024
here) becomes a scissored triangle strip of four vertices from SV_VertexID,
with `IaSetInputLayout(NULL)` and no vertex buffer. d3d10umd's
`update_velems` bound nothing when the layout was NULL, and the device had
never had a layout (`velems_changed` starts false, so even that was
skipped). Panfrost dereferences the bound state at every draw. softpipe
does not, which is why no host test saw it.

0070 binds a layout of no elements when the application sets none, and
marks the vertex elements changed at device creation, so the first draw
always binds something. Chromium's solid colour surfaces (the
`SolidColorSurfacePoolCreateSurface` in Edge's log) are DComp surfaces
cleared the same way, and ANGLE draws on SV_VertexID alone for some blits.

### CreateSwapChainForComposition: a cached buffer for a shared resource (patch 0071)

The failing resource: 128x96 B8G8R8A8, BindFlags 0xa8, MiscFlags 0x20002
(SHARED and the DXGI composition bit), the same description as the two
FLIP_SEQUENTIAL buffers made a moment before, which worked. The log says
`shared create: ... no buffer was made on the allocation`: Panfrost issued
no CREATE_BO while the resource was made, so malikm_drm.c had nothing to
put on the runtime's allocation and the resource was failed with
E_OUTOFMEMORY.

Panfrost took the buffer from its BO cache. The cache refuses BOs that
were exported (PAN_BO_SHARED), but a BO made shareable (PAN_BO_SHAREABLE,
from PIPE_BIND_SHARED) and never exported went in like any other, and a
fetch matches on flags and size. One came from `RotateResourceIdentities`:
the FLIP_SEQUENTIAL buffers are not primaries, so they rotate by copy, and
the copy's temporary was made from the first buffer's template,
PIPE_BIND_SHARED included. It was never exported, it was freed into the
cache, and the next shared resource in its size bucket got it. That
happens after the first present of any flip-model swap chain, which is
where Edge loses its device: every later shared resource of a size up to
the swap chain's (DComp surfaces, shared images) can fail. Whether the
runtime removes Edge's device for one of these failures has not been seen
in a log yet (the GPU process had none); the timing fits.

0071 keeps shareable BOs out of Panfrost's cache, exported or not, and
makes the rotation's temporary without PIPE_BIND_SHARED, SCANOUT or
DISPLAY_TARGET.

### No log from Edge's GPU process (patch 0072, and an ACL)

The GPU process runs with a restricted token at low integrity (Chromium's
GPU sandbox). `C:\mali\log` made by an administrator has a medium label,
so a low process cannot create a file in it, and the restricted SIDs need
a grant of their own. The UMD gave up silently. Set the directory up once:

```
icacls C:\mali\log /grant *S-1-1-0:(OI)(CI)M *S-1-5-12:(OI)(CI)M *S-1-15-2-1:(OI)(CI)M *S-1-15-2-2:(OI)(CI)M
icacls C:\mali\log /setintegritylevel (OI)(CI)low
```

S-1-1-0 is Everyone, S-1-5-12 RESTRICTED (the restricted SID Chromium's
tokens carry), S-1-15-2-1 and S-1-15-2-2 all application packages and all
restricted ones, for an AppContainer GPU process. `C:\mali\log\off` still
stops new logs. 0072 writes the token as the first line of every log
(`log on: <path>, integrity 0x1000 (low), app container 0, restricted 1`
for Edge's GPU process), and a process that cannot open its log says so
once through OutputDebugString (`maliumd: no log, ... (error 5)`), which
DebugView with "Capture Global Win32" shows.

## CI for these

- `triangle.exe <dll> nolayout`: the triangle from SV_VertexID with no
  input layout or vertex buffer ever set. mesa.yml runs it on softpipe
  (pixels must be right) and on Panfrost with MALIKM_FAKE=2 (the process
  must not die). Pushed first without the fix (2c70ec9, Mesa run 45077326907): softpipe passed, Panfrost died with 0xc0000005 (`nolayout: exit code -1073741819`), as on the board.
- "The UMD log from a low-integrity process": triangle.exe copied to
  lowtri.exe with a low label (so it starts at low integrity) must get a
  log once `C:\mali\log` has the two icacls lines above, and the log's
  first line must say `integrity 0x1000 (low)`.
- Not on the host: 0071. The runner has no KMD, so shared resources fail
  there before Panfrost is asked for a buffer.

Runs on 2d73ab5, green: Mesa 45077995748 and CI 45077995719. On the runner
`nolayout: exit code 2` (alive, pixels arbitrary); the low-integrity
process had no log with the directory as made and one after the setup,
first line `log on: C:\mali\log\lowtri.exe-3512.txt, integrity 0x1000
(low), app container 0, restricted 0`. WARP (warpref.exe) passes every
edge-path step on the same runner, so the probe itself is sound.

## Board test, one install

One variable: the UMD (patches 0070-0072). The KMD and the INF are as in
22.7.16.475. Leave `UmdDdi` 101 and `UmdMsaa` unset.

Package: CI run 45077995719 (2d73ab5), artifact
`wddm-mali-g52-driver-arm64`, DriverVer 11/22/2026,22.29.38.799, UMD from
Mesa run 45077995748 (patches 0001-0072). Test certificate SHA1
6A15491315E605638E1E9F3077CAE3044D7602DD (new for this run). d3dprobe.exe
is in the `m3test-arm64` artifact of the same CI run.

1. Copy the package to `C:\mali\pkg-edge3\` and d3dprobe.exe from the
   same run's `m3test-arm64` artifact; make `C:\mali\fake\msedge.exe` and
   `C:\mali\fake\plainprobe.exe` from it (replace the old copies).
2. Import the package's new test certificate into both stores, as
   administrator:
   `certutil -addstore -f Root C:\mali\pkg-edge3\wddm-mali-g52-test.cer` and
   `certutil -addstore -f TrustedPublisher C:\mali\pkg-edge3\wddm-mali-g52-test.cer`.
3. `pnputil /add-driver C:\mali\pkg-edge3\driver\maliwddm.inf` (stage only).
4. As SYSTEM (task `malireinst`), set `ConfigFlags |= 0x20` on
   `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0`; poll until the
   value has 0x20 in it.
5. Log directory: `mkdir C:\mali\log` if missing, delete
   `C:\mali\log\off` if present, delete the old `*.txt` in it, then the
   two `icacls` lines above. Then a clean restart.
6. After logon, in the user's session (`schtasks /it /ru woo` with a `.cmd`
   wrapper, not over SSH):
   - `C:\mali\fake\msedge.exe > C:\mali\present-msedge.txt`
   - `C:\mali\fake\plainprobe.exe > C:\mali\present-plain.txt`
7. Edge as before: separate profile, `--enable-logging --v=1`, the WebGL
   page, then `edge://gpu`. Save the Edge log and the page.
8. `echo > C:\mali\log\off`, collect `C:\mali\present-*.txt`, every
   `C:\mali\log\*.txt`, the Edge log and the edge://gpu page.

What to read:

- Driver version first: the `log on:` line exists in every UMD log of the
  new package (absent: the old UMD is still loaded).
- present-msedge.txt, edge path: CreateSwapChainForComposition 0, the
  three frames and Present1 0, `DComp view, clear, EndDraw` reached (no
  CRASH line in the log), and `edge path: device alive`. If
  CreateSwapChainForComposition still fails, the log line after
  `shared create:` says why.
- `C:\mali\log\msedge.exe-*.txt`: now including the GPU process's, whose
  first line says `integrity 0x1000 (low) ... restricted 1`. In it
  `findstr /c:"ddi-error" /c:"dxgi-result" /c:"CRASH" /c:"no buffer was made"`.
  If there is no low-integrity msedge log at all, the ACL did not take
  (`icacls C:\mali\log` shows it).
- The Edge log: no `Present1 failed`, no `testDeviceLost`; edge://gpu
  "Software Rendering: No".

## Not known yet

- Whether Edge's removal at its first Present1 is the 0071 failure. The
  timing fits (the first rotation puts the shareable temporary in the
  cache); this package's GPU-process log settles it either way.
- Whether Present1 on a composition swap chain works on maliwddm at all:
  d3dprobe never got that far on the board.
