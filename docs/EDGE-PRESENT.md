# Edge and Chrome on the Mali: presenting

Device creation (the multisample answers Edge needs) is in
[EDGE-CREATEDEVICE.md](EDGE-CREATEDEVICE.md). This page covers what
happens after: swap chains, DirectComposition and DXGI redirection.

Settings: see [SETTINGS.md](SETTINGS.md). The ones that matter here are
`UmdDdi`, `UmdMsaa` and `UmdRedirect`.

## State

- At WDDM 1.3 with redirection for the browsers (`UmdRedirect` 2, the
  1.3 default), Edge renders on the Mali and DWM stays up.
- At WDDM 2.x (`UmdRedirect` 1, the 2.x default), Edge's GPU process
  gets the Mali at feature level 10_1, but its first present to a
  composition swap chain removes the device. After three GPU process
  restarts Edge falls back to software. The desktop is not affected.
- At WDDM 2.x with redirection for the browsers (`UmdRedirect` 2), the
  whole session froze once. See "The WDDM 2.0 session hang".

## How it works

### Redirection

The UMD's CreateDevice answers either `DXGI_STATUS_NO_REDIRECTION` (DXGI
does not hand shared resources to DWM) or S_OK (redirection allowed).
Who gets S_OK, first match wins:

1. `C:\mali\noredirect` exists: no process.
2. `C:\mali\redirect` exists: every process.
3. `UmdRedirect`: 1 no process, 2 the browsers (msedge.exe, chrome.exe,
   msedgewebview2.exe, d3dprobe.exe, matched by process name), 3 every
   process. Absent: 1 at WDDM 2.x, 2 at 1.3. A KMD that predates the
   field answers 0, and the UMD then gives it to the browsers.

`UmdRedirect` is read at each adapter query, so a change applies to
processes started after it, with no reboot. The UMD log says which case
applied, for example `CreateDevice: done, DXGI_STATUS_NO_REDIRECTION (a
browser, but UmdRedirect 1)`.

A composition swap chain (what Chromium's `SwapChainImageBacking` and
DirectComposition use) can only present to DWM, through its shared
buffers. Without redirection `pfnPresentCb` refuses that present with
0x88760873 (D3DDDIERR_INVALIDHANDLE), the UMD passes the code back, and
the runtime removes the device with reason 0x887A0020
(DXGI_ERROR_DRIVER_INTERNAL_ERROR). Edge then logs `Present1 failed ...
(0x887A0005)`, `DComp commit failed: SolidColorSurfacePoolCreateSurface`
and ANGLE `testDeviceLost: HRESULT 0x887A0020`. Swap chains on a window
(FLIP_DISCARD, FLIP_SEQUENTIAL, DISCARD) present fine without
redirection. The UMD does not hide the failure: S_OK from a refused
present would give a blank Edge window instead of a software fallback.

### Errors the runtime accepts

`pfnSetErrorCb` accepts E_OUTOFMEMORY, DXGI_DDI_ERR_WASSTILLDRAWING,
DXGI_DDI_ERR_UNSUPPORTED, DXGI_DDI_ERR_NONEXCLUSIVEMODE and
D3DDDIERR_DEVICEREMOVED. Any other code, and any code a DXGI DDI makes
up itself, removes the device with 0x887A0020. So the UMD:

- logs every error as `ddi-error: <DDI> 0x<code>`, the first per device
  as `ddi-error (first)`; WASSTILLDRAWING at most four times;
- reports a code outside that set as E_OUTOFMEMORY and logs `a code the
  runtime removes the device for; reported as E_OUTOFMEMORY`;
- logs every non-S_OK result of Present, SetDisplayMode and
  RotateResourceIdentities as `dxgi-result: <DDI> 0x<code>`, including
  `pfnPresentCb`'s, which are passed on;
- logs a failed `pfnRenderCb` or `pfnLockCb` with its HRESULT;
- drops a frame (S_OK) when a present or mode change cannot be asked of
  the runtime at all, instead of removing the device.

### Other UMD rules Edge depends on

- A draw with no input layout (the runtime's own clear of a DComp
  surface region, ANGLE blits from SV_VertexID) binds an empty layout.
  Panfrost dereferences the bound vertex state at every draw.
- Shareable buffers never go into Panfrost's BO cache, and the copy
  temporary of RotateResourceIdentities is made without
  PIPE_BIND_SHARED, SCANOUT or DISPLAY_TARGET. Otherwise a later shared
  resource can get a cached buffer and fail with E_OUTOFMEMORY
  (`shared create: ... no buffer was made on the allocation`).
- At WDDM 1.3 a submission lists every live BO for residency, except
  runtime resources (shared, created or opened), which are listed only
  when the job names them. At 2.x residency comes from MakeResident.

## Known issues

- At 2.x, composition swap chains fail as described above, so Edge runs
  in software. `--disable-direct-composition` makes ANGLE present
  through a window swap chain instead; that is a test of this reading,
  not a fix for users.
- With redirection on, DWM's device was removed by the user-mode runtime
  about 30 ms after DWM opened a composition buffer a browser had just
  presented (WDDM 1.3). dxgkrnl's trace showed no device error. DWM then
  rebuilt its device; after too many losses dwmcore fails fast with
  0x8898008D (MILERR_NO_HARDWARE_DEVICE). The cause is not known. The
  `removed:` and `recent:` lines in dwm.exe's UMD log are where to read
  it.
- Edge's own GPU process at 2.0 once logged `DCompositionCreateDevice3
  failed: Access is denied (0x80070005)` and a window swap chain failing
  with 0x887A0022, which `d3dprobe sandbox` does not reproduce even with
  a restricted token. Chromium's sandbox locks down further (job object,
  desktop and handle policy). `--disable-gpu-sandbox` separates the two.
- edge://gpu shows `VENDOR=0x50434b52, DEVICE=0x32303437`: DXGI builds
  them from the ACPI ID `RKCP7402`. They are not PCI IDs, so Chromium
  keeps GPU rasterization off for an unknown vendor ("Software only" is
  expected). `DRIVER_VERSION=0.0.0.0` is because the Mesa DLLs have no
  version resource.
- FurMark TDRs (event 4101) are a separate problem:
  [PERF.md](PERF.md), "TDRs with no job fault".

## Edge at WDDM 2.0

`d3dprobe sandbox` runs Edge's GPU process steps on adapter 0 in three
child processes, one per token: `medium`, `low` (low integrity) and `gpu`
(low integrity, a restricted token with Everyone, Users, RESTRICTED and
the logon SID as restricting SIDs, no privileges). Each child runs:

- `DCompositionCreateDevice3`, as Chromium calls it;
- FLIP_DISCARD, FLIP_SEQUENTIAL and DISCARD swap chains on a window:
  create, three presents, ResizeBuffers, release and remake on the same
  window (ANGLE's `SwapChain11::reset`), and a second one at the same
  time;
- the edge path (below).

Every step prints its HRESULT and the device's removed reason. Each
child's exit code is its count of failed steps. On the board at 2.0 with
no redirection, all three children pass everything except the edge
path's first composition Present1 (`0x887a0005`, reason `0x887a0020`).
With redirection (at 1.3, `C:\mali\redirect`) the whole edge path
passed. With `UmdRedirect` 2, `msedge.exe sandbox` should exit 0 and
`plainprobe.exe sandbox` should still fail the composition present
(exit 4), since the switch goes by process name.

The edge path, also run without `sandbox` after the swap chain test:

1. CreateSwapChainForComposition (FLIP_SEQUENTIAL, BGRA8, two buffers,
   premultiplied alpha);
2. a DirectComposition device, target, visual with the swap chain as
   content, Commit;
3. three frames: clear, Present1, the last two with a dirty rect;
4. a DComp surface: CreateSurface, BeginDraw, clear, EndDraw, Commit;
5. a texture shared by NT handle with a keyed mutex: CreateSharedHandle,
   OpenSharedResource1, AcquireSync, ReleaseSync.

The first step marked `<== REMOVED` is where to read the UMD log. It
ends with `edge path: device alive` when everything passed.

## The WDDM 2.0 session hang

With redirection for the browsers at WDDM 2.0, Edge froze the session
within seconds: the debug port never answered, Edge processes could not
be killed, DWM stopped using CPU, GPU counter queries hung, `shutdown /r`
never finished. There was no TDR and no LiveKernelReport.

A live dump showed maliwddm idle (no thread inside a DDI, every fence
completed, no fault), so dxgkrnl had nothing to time out. dxgkrnl's last
calls were OpenAllocation, Escape, Render, a paging buffer,
DestroyAllocation, CloseAllocation, SetPointerPosition and
SetVidPnSourceVisibility(FALSE) within 14 ms; after that only vsync DPCs.
37 GUI threads waited in win32k on one ERESOURCE whose owner was not in
the dump. The reading that fits, not proven: DWM lost its device right
after opening a browser's buffer, let go of the screen, and a thread in
the DWM restart path holds the win32k lock.

What the driver does about it:

- `UmdRedirect` defaults to 1 (no redirection) at 2.x.
- The KMD notes every DDI entered below DISPATCH_LEVEL with the caller's
  image name, pid and tid. Every second the watchdog writes, under
  `Device Parameters\maliwddm\Probe2000`: `Wd_Recent` (last twelve:
  sequence, DDI, image, pid.tid, ms ago), `Wd_Visible`, `Wd_SinceDdiMs`
  and `Wd_Stall` ("screen invisible and no DDI for N s", set at 10 s;
  also in the boot journal and on the UART). The next boot copies them
  to `Prev_*` first.
- The package carries `maliwddm.pdb` in `symbols\`.

If it happens (for example while testing `UmdRedirect` 2): over SSH,
`reg query ...\Probe2000 /v Wd_Recent` and `/v Wd_Stall`; then
`taskkill /f /im dwm.exe` and note whether the desktop comes back and
Edge exits. After a power cycle, save every `Prev_*` value before
anything else. Undo with `reg delete ... /v UmdRedirect /f`.

## Collecting evidence

### UMD log

The UMD writes `C:\mali\log\<exe>-<pid>.txt` when `C:\mali\log` exists
and has no file named `off` in it (`echo > C:\mali\log\off` stops new
logs). Edge's GPU process runs at low integrity with a restricted token
and cannot create files in a directory an administrator made. Set the
directory up once:

```
icacls C:\mali\log /grant *S-1-1-0:(OI)(CI)M *S-1-5-12:(OI)(CI)M *S-1-15-2-1:(OI)(CI)M *S-1-15-2-2:(OI)(CI)M
icacls C:\mali\log /setintegritylevel (OI)(CI)low
```

S-1-1-0 is Everyone, S-1-5-12 RESTRICTED, S-1-15-2-1 and S-1-15-2-2 all
application packages and all restricted ones. Each log starts with the
token (`log on: <path>, integrity 0x1000 (low), app container 0,
restricted 1` for Edge's GPU process). A process that cannot open its
log says so once through OutputDebugString (`maliumd: no log, ...
(error 5)`); DebugView with "Capture Global Win32" shows it.

Lines to search for:

- `ddi-error`, `dxgi-result`, `CRASH`;
- `CreateDevice: done` and `OpenAdapter: ... UmdRedirect`: who got
  redirection and why;
- `being removed`, `removed:`, `recent:`. When the runtime removes a
  device (it calls `RelocateDeviceFuncs`), the UMD logs `the runtime
  moved the function table; the device is being removed`, then `removed:`
  lines (the device's live BOs and its last eight submissions) and up to
  32 `recent:` lines (the process's last entry points). The last
  `recent:` line before `RelocateDeviceFuncs1` is the call the removal
  came in. The first `removed:` line says whether it came `inside a call
  that holds the shim lock`.

```
findstr /c:"being removed" /c:"removed:" /c:"recent:" C:\mali\log\dwm.exe-*.txt
```

A crash address in `maliumd` symbolizes with the package's PDB:
`llvm-symbolizer --obj=maliumd_arm64.dll --relative-address 0x...`, with
the DLL renamed to `libgallium_d3d10.dll` beside the `.pdb`.

### d3dprobe

From the `m3test-arm64` artifact of the same CI run. The UMD treats
processes by name, so copy it twice: `C:\mali\fake\msedge.exe` and
`C:\mali\fake\plainprobe.exe` (the control). Run it in the user's
session (`schtasks /it` with a `.cmd` wrapper works). Over SSH there is
no interactive desktop and every swap chain fails with 0x887A0022.

```
C:\mali\fake\msedge.exe > C:\mali\present-edge.txt
C:\mali\fake\msedge.exe sandbox > C:\mali\sandbox.txt
```

### Edge

Close every Edge process first, background ones included, or new flags
are ignored. Use a fresh profile, `--enable-logging --v=1`, and save
edge://gpu. Useful flags: `--disable-direct-composition`,
`--disable-gpu-sandbox`, `--show-fps-counter`. Hardware rendering looks
like: Canvas, Compositing and WebGL "Hardware accelerated", GPU process
crash count 0, no `Present1 failed` or `0x887A0022` in the log.

### KMD and dxgkrnl

- `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm"`:
  `Fail_*` breadcrumbs, `Tdr<n>_*` snapshots, `Probe2000\Wd_*`.
- dxgkrnl ETW: `etwrec.exe start C:\mali\dxgkrnl.etl` and `etwrec.exe
  stop` (from `m3test-arm64`), or `wpr -start tools\windows\dxgkrnl.wprp`.
  Decode with `tools/etw/dxgk-decode.py` and the board's dxgkrnl.sys;
  `tools/etw/dxgk-event-names.py dxgkrnl.sys [ids]` gives the event, task
  and opcode names. Event 70 with Status 0xc0000001 is VidMm's
  InnerIteration stop, normal paging.
- Live dump of a hang: `Get-StorageDiagnosticInfo -IncludeLiveDump`.
  Check `Format-Hex -Count 8` shows `PAGEDU64` before copying it.
- TDR live dumps in `C:\Windows\LiveKernelReports\WATCHDOG` are
  throttled by Windows. See
  `HKLM\SYSTEM\CurrentControlSet\Control\CrashControl\FullLiveKernelReports`
  (`ComponentThrottleThreshold`, `SystemThrottleThreshold`; 0 is
  documented as no throttle on newer builds, not checked on 22621) and
  keep `TdrDebugMode` under `...\Control\GraphicsDrivers` absent or 2.

If the UI freezes with redirection on: `del C:\mali\redirect` (or `echo >
C:\mali\noredirect`) and `Stop-Process -Name dwm -Force` over SSH.
