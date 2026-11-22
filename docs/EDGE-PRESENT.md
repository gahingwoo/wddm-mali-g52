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

## Board test, one install

One variable: the UMD. Leave `UmdDdi` 101 and `UmdMsaa` unset.

Package: CI run 45004408487 (bba6a25), artifact
`wddm-mali-g52-driver-arm64`, DriverVer 11/22/2026,8.47.18.960, with the
UMD from Mesa run 45004408540 (patches 0068 and 0069). d3dprobe.exe is in
the `m3test-arm64` artifact of the same run. The KMD and the HDMI work
are hpd-edge's (c445902), unchanged.

1. Copy the package to `C:\mali\pkg-present\`, and d3dprobe.exe from the
   same CI run's `m3test-arm64` artifact. Make the names
   `C:\mali\fake\msedge.exe` and `C:\mali\fake\plainprobe.exe` from it.
2. `pnputil /add-driver C:\mali\pkg-present\maliwddm.inf` (stage only).
3. As SYSTEM (task `malireinst`), set `ConfigFlags |= 0x20` on
   `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0`, and poll until
   the value has 0x20 in it.
4. `mkdir C:\mali\log`, and delete `C:\mali\log\off` if it is there.
   Without the directory there is no UMD log at all. Then a clean restart.
5. After logon, in the user's session (`schtasks /it /ru woo` with a `.cmd`
   wrapper, not over SSH), run:
   - `C:\mali\fake\msedge.exe > C:\mali\present-msedge.txt`
   - `C:\mali\fake\plainprobe.exe > C:\mali\present-plain.txt`
6. Edge as on 2026-11-22: separate profile, `--enable-logging --v=1`, the
   WebGL page, then `edge://gpu`. Save the Edge log and the page.
7. `echo > C:\mali\log\off`, then collect `C:\mali\present-*.txt`, every
   `C:\mali\log\msedge.exe-*.txt` (one of them is the GPU process, the one
   with `CreateDevice` lines), the Edge log and the edge://gpu page.

What to read:

- present-msedge.txt, "edge path" lines: every step `device removed
  reason 0x00000000` and `edge path: device alive`. If a step says
  `<== REMOVED`, that is the call.
- `C:\mali\log\msedge.exe-*.txt`: `findstr /c:"ddi-error" /c:"dxgi-result"`.
  The `ddi-error (first)` line names the DDI and the code. With this
  package a code that used to remove the device shows as `reported as
  E_OUTOFMEMORY`, and Edge may then go further. A `dxgi-result: Present`
  line with a failed code is a `pfnPresentCb` result (the kernel's
  answer), and one of those can still remove the device.
- The Edge log: no `Present1 failed` and no `testDeviceLost`. edge://gpu:
  "Software Rendering: No".

If the device is still removed and the log has no `ddi-error` or failed
`dxgi-result` line before it, the removal did not come from a UMD error
report. Then send the full GPU-process log.

## Not known yet

- Which of the codes above Edge hit. This package logs it either way.
- Whether a call failing with E_OUTOFMEMORY, instead of removing the
  device, lets Edge render. That depends on which call it is: an
  OpenResource that fails would leave Chromium without that shared image.
