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

## Board result, 2026-11-23 (DriverVer 22.29.38.799)

DSI panel, HdmiHpd 1, logged in. Files in
`board-session-prep/edge3-22.29.38.799/`.

- 0070 and 0071 work: CreateSwapChainForComposition answers 0, and there
  is no CRASH line anywhere.
- 0072 and the ACL work: Edge's GPU processes wrote logs, first line
  `integrity 0x1000 (low), app container 0, restricted 1`.
- d3dprobe runs the edge path once per adapter. On adapter 0 (the Mali)
  it fails at `frame 0: Present1 0x887a0005, removed reason 0x887a0020`
  in both probe copies. The run that passes completely is adapter 1,
  Microsoft Basic Render Driver, so the failure is not intermittent: it
  happens on every composition present on the Mali.
- Edge: unchanged, `Present1 failed ... suspended`, testDeviceLost
  0x887A0020, three GPU-process restarts.

### What the logs say

Every Mali process logs the same thing at the composition swap chain's
first present:

```
Present: flags 0x1 interval 0, src primary 0x0, dst 0000000000000000 primary 0x0
present: shared allocation 0x40000540 -> 0x0, pfnPresentCb 0x88760873
dxgi-result: Present 0x88760873 (failed)
```

The flip-model swap chain on a window, a moment earlier in the same
process, presents its shared buffer the same way and pfnPresentCb answers
0. So the UMD makes the same call both times, and the error comes back
from the runtime callback, not from the UMD.

- 0x88760873 is MAKE_D3DHRESULT(2163), D3DDDIERR_INVALIDHANDLE (2160 is
  D3DDDIERR_DEVICEREMOVED, 0x88760870, already in State.h). From this
  package on, the build checks that value against the WDK header and the
  log prints the name.
- It is not maliwddm's. The KMD trace has `Enter_Present` and no
  `Fail_Present`, and MwPresent fails only with STATUS_INVALID_PARAMETER,
  on a blt without private data. dxgkrnl, or the runtime's side of
  pfnPresentCb, refuses the present before or without asking the KMD.
- The runtime then removes the device for it (0x887A0020), and Edge's
  next Present1 sees DEVICE_REMOVED.

### The likely cause: DXGI_STATUS_NO_REDIRECTION

d3d10umd's CreateDevice returns DXGI_STATUS_NO_REDIRECTION ("the driver
is requesting that the DXGI runtime not use shared resources to
communicate with the Desktop Window Manager"). It has done that in every
process since patch 0023 (every `CreateDevice: done` line in these logs
says so); only the file `C:\mali\redirect` gives S_OK instead. A
composition swap chain has nothing to present to except DWM, through the
shared buffers: the runtime made them SHARED (MiscFlags 0x20002) but was
told not to hand them to DWM, and the present of one then names a
handle the kernel has no composition binding for. A swap chain on a
window still has the window, which is why that present works. The Basic
Render Driver's composition presents work on the same board; whether it
allows redirection is not visible from here, but nothing else about its
run differs from the Mali's. This is the reading that fits every line in the logs; it has not
been tested. The comment at the return said NO_REDIRECTION was the honest
answer "while OpenResource is not implemented"; OpenResource has worked
since 2026-11-13 (DWM opens LogonUI's shared surfaces).

Changing the default changes every windowed application's present path
under DWM, so the board test below first switches redirection on only for
processes started after logon, with the existing `C:\mali\redirect` file
(dwm.exe and the shell keep the devices they made at boot). If that makes
the composition present work, the default follows in the next package.

## Board result, 2026-11-23 (DriverVer 0.3.59.412)

Files in `board-session-prep/edge4-0.3.59.412/`.

- Run A (no `C:\mali\redirect`): `DXGI_STATUS_NO_REDIRECTION`, and the
  composition present fails with `pfnPresentCb 0x88760873
  (D3DDDIERR_INVALIDHANDLE)`, as before.
- Run B (`C:\mali\redirect` present): `S_OK (redirection allowed)`. Both
  probe copies on the Mali pass the whole edge path: three Present1, the
  DComp surface, the shared NT-handle texture, the keyed mutex, `device
  alive`. Edge's log has no `Present1 failed` and no testDeviceLost.
  **NO_REDIRECTION was the cause of the composition present failure.**
- But dwm.exe crashed twice while Edge ran (1:23:41 and 1:23:54 pm):
  dwmcore.dll 10.0.22621.2215, exception 0x8898008D at +0x1620f8. The
  screen went black and came back each time.

### What DWM's logs say

0x8898008D is MILERR_NO_HARDWARE_DEVICE (winerror.h; d3dprobe now checks
the value at compile time). DWM raises it when it has lost its D3D device
too many times in a short while. The logs show exactly that:

- The runtime calls `RelocateDeviceFuncs1` when it removes a device: it
  follows the failed present in run A's probe log, and a device that lives
  never gets it. dwm.exe-5940 got it eight times in 4.5 s (162.0 to
  166.6 s). Each time DWM made a new device, opened the application
  surfaces again, and lost the new device within about 30 ms. Then
  dwmcore failed fast. dwm.exe-1184 made 16 devices: its boot device,
  then 15 more from 115.7 s on.
- The first loss is at 115.57 s, 10 ms after the run B probe presented
  its composition buffer (115.562 s) and DWM opened that buffer
  (allocation 0x40006b80). DWM had composed the shell's redirected
  surfaces all session without losing a device.
- No UMD line says why. There is no `ddi-error`, no failed `dxgi-result`
  and no CRASH line in any DWM log, and the KMD has no `Fail_` breadcrumb
  and no `Enter_ResetFromTimeout` (no TDR). maliwddm also never reports a
  DMA fault to dxgkrnl, so a GPU fault does not remove a device either.
  What is left is dxgkrnl putting the device into an error state on its
  own (VidMm or the scheduler), which the runtime finds at its next call.

### The change (352b667, patch 0073)

One code change, plus logging for the cause.

- Every submission listed every live BO for residency, as a write (Mesa
  expects a job to reach BOs it does not name). That included every
  application surface DWM had opened, in every DWM job, and in every
  application job the swap chain buffers DWM was composing. Primaries
  were already left out unless a job named them. The allocations of all
  runtime resources (shared, created or opened) now are too. They are
  pipe resources, so a job that samples or renders to one names it. This
  is the only behaviour change, and it applies with redirection off too
  (DWM opens LogonUI's and the shell's shared surfaces either way).
  Whether it stops the removals is what the test below shows.
- `RelocateDeviceFuncs` now logs `the runtime moved the function table;
  the device is being removed`, followed by `removed:` lines: the device's
  live BOs and its last eight submissions (BOs named and listed, which
  runtime resources were named, the callback's status, and how long
  before the removal).
- The board test records dxgkrnl's own events (etwrec) around the run:
  if a device is removed again, they say why.

## Board result, 2026-11-24 (DriverVer 2.40.7.189 = 352b667, patch 0073)

TaishanPi 3M, HDMI only, 2560x1440. Files in
`board-session-prep/edge5-2.40.7.189/`.

- Without redirection, Notepad was opened, typed in, moved and resized 20
  times, then closed. The desktop and dwm.exe were unchanged: the
  residency change (runtime resources listed only when named) did not
  break the desktop.
- With `C:\mali\redirect` on, the probe on the Mali passed the whole edge
  path. dwm.exe did not crash, but its log ends at
  `RelocateDeviceFuncs1: ... the device is being removed`, with no
  `removed:` line after it and nothing more from dwm.exe. Every process
  started afterwards hung (SearchHost.exe seven times,
  ShellExperienceHost.exe, the screenshot script), each with its log ending
  at a `Flush`. About 1.5 minutes after the switch was deleted the whole
  UI froze, mouse included, until dwm.exe was killed.

### Decoding the dxgkrnl trace

`tools/etw/dxgk-decode.py` with the board's dxgkrnl.sys gives field names
but no event names. The event, task and opcode names are in the
WEVT_TEMPLATE's EVNT, TASK and OPCO tables, which pyfwevt does not
expose. `tools/etw/dxgk-event-names.py dxgkrnl.sys [ids]` reads them
now. Read directly, the rare events are Device and Context start and
stop, PresentHistory, Fence and MonitoredFence. There are no
device-error or TDR events. Event 70 with Status 0xc0000001, the one the
decoder's filter shows, is VidMm's InnerIteration stop, 590 times through
the whole trace: normal paging, not a failure.

- The probe's device lives from 24180046330 to 24190620573 (100 ns
  units), about 1.06 s. DWM's removal was 0.36 s into it.
- DWM's context (0xffffa5861c9a95a0, 22887 events) runs to 24188310622
  and then stops. Its last packets complete normally (QueuePacket stop,
  not preempted, not timed out; DmaPacket with no fault address). After
  that dwm.exe never makes another device.

So dxgkrnl did not remove DWM's device. The removal came from the user-mode
runtime, and after it DWM stopped working instead of rebuilding its device
as it did in round 4.

### Why DWM stopped: the removal logging deadlocked it

`malikm_note_removed` (352b667) took the shim's lock with
AcquireSRWLockExclusive. The runtime moves a device's function table
wherever it notices the removal, and that can be inside a runtime callback
the shim makes while it holds that lock. An SRW lock taken twice on one
thread never returns. The log's last line is the one written just before
that acquire, and nothing follows from any DWM thread. So the logging that
was meant to explain the removal stopped DWM's render thread for good, and
everything waiting on DWM hung with it. Round 4 had no such call, and DWM
rebuilt its device eight to fifteen times before giving up with
0x8898008D.

### The change (632b8e1, patch 0074)

- wlock/wunlock keep a thread-local "held" flag. `malikm_note_removed`
  does not take the lock when its thread already holds it; otherwise it
  tries for 20 ms and gives up. The first `removed:` line says which case
  it was: `inside a call that holds the shim lock` means the runtime
  called RelocateDeviceFuncs from inside one of the shim's callbacks.
- d3d10umd keeps the last 32 entry points of the process after the
  entry-point trace has stopped (DWM's had, long before the removal), and
  RelocateDeviceFuncs writes them as `recent:` lines. The last one before
  `RelocateDeviceFuncs1` is the call the removal came in.

Still not known: why the runtime removes DWM's device about 30 ms after
DWM opens a composition buffer an application has just presented. The
`recent:` and `removed:` lines of the next run are where to read it.

## Board test, one install

Same as round 5. This package (logging only, plus the lock fix) should
leave DWM alive after a removal, rebuilding its device as in round 4, and
give the removal's call in the log.

Package: CI run 45322983394 (632b8e1), artifact
`wddm-mali-g52-driver-arm64`, DriverVer 11/24/2026,3.57.23.142, UMD from
Mesa run 45322983455 (patches 0001-0074). Test certificate SHA1
473F2837FD96CFD9A205BD7B7D6C482586249112 (new). d3dprobe.exe, etwrec.exe
and m5test.exe are in the `m3test-arm64` artifact of the same CI run.

1. Copy the package to `C:\mali\pkg-edge6\`; d3dprobe.exe (to
   `C:\mali\fake\msedge.exe` and `plainprobe.exe`), etwrec.exe and
   m5test.exe from the same run's `m3test-arm64` artifact to `C:\mali\`.
2. Import the new test certificate into Root and TrustedPublisher.
3. `pnputil /add-driver C:\mali\pkg-edge6\driver\maliwddm.inf` (stage only).
4. As SYSTEM (task `malireinst`), `ConfigFlags |= 0x20` on
   `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0`; poll until set.
5. `C:\mali\log`: delete `off` and the old `*.txt`; the ACL stays.
   `C:\mali\redirect` must NOT exist. Clean restart.
6. Without redirection: the Notepad round from round 5 (automated is
   fine).
7. As administrator: `C:\mali\etwrec.exe start C:\mali\dxgkrnl.etl`, then
   `echo > C:\mali\redirect`.
8. In the user's session, with redirection, one at a time, about 20 s
   each, watching the screen and noting the time of anything that goes
   black or stops:
   - `C:\mali\fake\plainprobe.exe > C:\mali\present-plain-D.txt`
   - Notepad: open, type, move, resize, close (a new process after the
     switch).
   - Settings (a WinUI window, D3D).
   - Edge (separate profile, `--enable-logging --v=1`, the WebGL page,
     edge://gpu with a screenshot), open 60 s.
9. As administrator: `C:\mali\etwrec.exe stop`,
   `C:\mali\m5test.exe mem > C:\mali\m5-mem.txt`, `del C:\mali\redirect`,
   `echo > C:\mali\log\off`. Then 3 more minutes of normal use (move a
   window, open Start), to see whether DWM stays responsive.
10. Collect as in round 5: `present-*.txt`, `m5-mem.txt`, `dxgkrnl.etl`,
    `dxgkrnl.sys`, all `C:\mali\log\*.txt`, app.evtx and sys.evtx, the
    Edge log, the screenshot, and the times noted.

If the UI freezes anyway: `del C:\mali\redirect` and
`Stop-Process -Name dwm -Force` over SSH, as in round 5. Note the time.

What to read:

- `findstr /c:"being removed" /c:"removed:" /c:"recent:" C:\mali\log\dwm.exe-*.txt`.
  After each `being removed` there must now be `removed:` lines (if not,
  the deadlock is still there) and up to 32 `recent:` lines. The first
  `removed:` line says whether the removal came inside a shim callback;
  the last `recent:` lines before `RelocateDeviceFuncs1` name the DDI it
  came in.
- No AppHang for processes started after the switch, and no UI freeze
  after it is gone. With the deadlock gone DWM may still lose its device
  and rebuild it. If that happens too often it fails fast with 0x8898008D
  as in round 4. That would mean the removal is still there, with the
  hang fixed.
- Edge: no `Present1 failed`, no testDeviceLost; edge://gpu "Software
  Rendering: No".

## Not known yet

- Why the runtime removes DWM's device soon after DWM opens a freshly
  presented composition buffer (rounds 4 and 5). Not dxgkrnl: its trace
  shows no device error. The next run's `recent:` lines name the call.
- Redirected presents of blt-model windowed applications: Notepad in
  round 5 ran only as a process started after DWM had already stopped.
