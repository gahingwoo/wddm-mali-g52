# Edge and Chrome on the Mali: presenting

Device creation (the multisample answers Edge needs) is in
[EDGE-CREATEDEVICE.md](EDGE-CREATEDEVICE.md). This page covers what
happens after: swap chains, DirectComposition and DXGI redirection.

Settings: see [SETTINGS.md](SETTINGS.md). The ones that matter here are
`UmdDdi`, `UmdMsaa` and `UmdRedirect`.

## State

- At WDDM 2.x every process gets redirection (`UmdRedirect` 3, the 2.x
  default since 2026-11-27). Blt model and flip model swap chains on a
  window reach the screen, and Edge renders on the Mali with
  DirectComposition and with `--disable-direct-composition`. Board
  results: "Board result, 2026-11-27".
- At WDDM 1.3 the browsers get redirection (`UmdRedirect` 2, the 1.3
  default), Edge renders on the Mali and DWM stays up.
- Without redirection at 2.x a D3D11 window shows nothing, and a
  composition swap chain's present removes the device. See "Without
  redirection".

## How it works

### Redirection

The UMD's CreateDevice answers either `DXGI_STATUS_NO_REDIRECTION` (DXGI
does not hand shared resources to DWM) or S_OK (redirection allowed).
Who gets S_OK, first match wins:

1. `C:\mali\noredirect` exists: no process.
2. `C:\mali\redirect` exists: every process.
3. `UmdRedirect`: 1 no process, 2 the browsers (msedge.exe, chrome.exe,
   msedgewebview2.exe, d3dprobe.exe, matched by process name), 3 every
   process. Absent: 3 at WDDM 2.x, 2 at 1.3. A KMD that predates the
   field answers 0, and the UMD then gives it to the browsers.

`UmdRedirect` is read at each adapter query, so a change applies to
processes started after it, with no reboot. The UMD log says which case
applied, for example `CreateDevice: done, DXGI_STATUS_NO_REDIRECTION (a
browser, but UmdRedirect 1)`.

### With redirection

- A blt model swap chain (DISCARD, SEQUENTIAL) keeps a private back
  buffer. Each present copies the frame into an allocation of the
  device's and hands it to `pfnPresentCb`; dxgkrnl has maliwddm blt it
  into the window's surface (`DxgkDdiPresent`, a CPU copy, one for each
  present).
- A flip model swap chain (FLIP_SEQUENTIAL, FLIP_DISCARD, and every
  composition swap chain) has shared buffers. A present names the
  buffer's own allocation, DWM opens the buffers and samples them, and
  `DxgkDdiPresent` is not called. RotateResourceIdentities exchanges the
  buffers, views and allocations of the resources (patch 0087).
- dxgkrnl gives DWM read-only access to those buffers. The shim lists a
  surface opened from another process as read (`WriteOperation` 0). See
  "The WDDM 2.0 session hang" for what a write did.
- The DXGI Blt DDI (a present between surfaces of different size, format
  or sample count) is carried out on the GPU (patch 0087).
- Present returns the result of `pfnPresentCb` for every kind of
  surface. With no surface to present from it returns E_OUTOFMEMORY,
  which the runtime accepts.

### Without redirection

At WDDM 2.0 a present from a window swap chain, blt or flip model,
returns S_OK and shows nothing: dxgkrnl does not call `DxgkDdiPresent`
and DWM is not told (presenttest, 2026-11-27: the window shows the
wallpaper, 0 driver blts). This was the white Edge window with
`--disable-direct-composition`.

A composition swap chain (what Chromium's `SwapChainImageBacking` and
DirectComposition use) can only present to DWM, through its shared
buffers. Without redirection `pfnPresentCb` refuses that present with
0x88760873 (D3DDDIERR_INVALIDHANDLE), the UMD passes the code back, and
the runtime removes the device with reason 0x887A0020
(DXGI_ERROR_DRIVER_INTERNAL_ERROR). Edge then logs `Present1 failed ...
(0x887A0005)`, `DComp commit failed: SolidColorSurfacePoolCreateSurface`
and ANGLE `testDeviceLost: HRESULT 0x887A0020`, and falls back to
software after three GPU process restarts.

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

With redirection, a flip model swap chain froze the session at WDDM 2.0
within seconds (Edge on 2026-11-25, `presenttest flipseq` on
2026-11-27): the application and dwm.exe could not be killed, DWM
stopped using CPU, `shutdown /r` never finished, and stopping an ETW
session on dxgkrnl hung too. There was no TDR: maliwddm was idle, its
last calls CloseAllocation and SetVidPnSourceVisibility(FALSE) from
dwm.exe.

**Cause.** The shim listed every allocation a job names with
`WriteOperation` 1. dxgkrnl gives the compositor read-only access to
the buffers of a flip model swap chain, and at WDDM 1.2 and later
`VIDMM_GLOBAL::ReferenceDmaBuffer` (dxgmms2 22621) answers a written
entry for such an allocation with STATUS_ACCESS_DENIED. So DWM's first
job that sampled an application's flip buffer failed in D3DKMTRender.
The runtime's `pfnRenderCb` returns S_OK for that and removes the
device, which is why no failed callback was ever in the log. DWM then
tore its device down. At 1.3 it built a new one and lost it again at the
next frame (the removals of 2026-11-23, and 0x8898008D after enough of
them); at 2.0 the teardown never returned.

**Evidence.** dwm.exe's UMD log of 2026-11-27 (package 2.20.40.786):
the removal is inside `Render`, right after it opened the two buffers,
every earlier callback S_OK, and dxgkrnl reports every device of the
process as active. With opened surfaces listed as read, the same test
shows the picture and DWM keeps its device: `presenttest`, seven
models, and Edge with DirectComposition.

**Fix.** The shim lists a surface opened from another process as read.
`C:\mali\open-write` brings the old list back, to compare (read once
a process, so reboot; it freezes the session at the first flip model
present).

What the driver records for the next removal or freeze:

- The shim keeps the last 48 runtime callbacks with their results. When
  the runtime removes a device, the UMD log gets `removed:` lines: the
  callback in flight, the callbacks before it, dxgkrnl's execution state
  of every device of the process, and, for a removal inside `Render`,
  dxgkrnl's answer to the same allocation list and to each allocation
  alone, written and read (0xC000000D means dxgkrnl accepted it).
- The KMD notes every DDI entered below DISPATCH_LEVEL with the caller's
  image name, pid and tid. Every second the watchdog writes, under
  `Device Parameters\maliwddm\Probe2000`: `Wd_Recent` (last twelve:
  sequence, DDI, image, pid.tid, ms ago), `Wd_Visible`, `Wd_SinceDdiMs`
  and `Wd_Stall` ("screen invisible and no DDI for N s", set at 10 s;
  also in the boot journal and on the UART). The next boot copies them
  to `Prev_*` first.
- The package carries `maliwddm.pdb` in `symbols\`.

Not known: what dwm.exe's teardown waits for at 2.0. Any other removal
of DWM's device while an application holds flip buffers may freeze the
session the same way. To go back to no redirection:
`reg add ...\Device Parameters /v UmdRedirect /t REG_DWORD /d 1`.

## Board result, 2026-11-27

TaishanPi 3M, WDDM 2.0, HDMI 2560x1440, `UmdRedirect` 3.
`tools/windows/presenttest.cpp`: one window cleared to a colour and
presented for some seconds; `sdc` is the centre pixel read from the
screen DC.

| Model | Device | Before (no redirection) | After |
|---|---|---|---|
| GDI control | none | yellow | yellow |
| D3D11CreateDeviceAndSwapChain, DISCARD | Mali | wallpaper | orange, 61 presents/s, one driver blt each |
| DISCARD | Mali | wallpaper | red, 61/s, one driver blt each |
| SEQUENTIAL | Mali | wallpaper | green, 63/s, one driver blt each |
| FLIP_SEQUENTIAL | Mali | wallpaper; with redirection the session froze | blue, 62/s, no driver blt |
| FLIP_DISCARD | Mali | wallpaper | cyan, 62/s |
| FLIP_SEQUENTIAL, WS_EX_NOREDIRECTIONBITMAP | Mali | wallpaper | purple, 62/s |
| FLIP_DISCARD, WS_EX_NOREDIRECTIONBITMAP | Mali | wallpaper | teal, 62/s |
| DISCARD, FLIP_SEQUENTIAL | WARP | red, blue | red, blue |

Every HRESULT was S_OK before and after; dwm.exe kept its PID.

Edge 158, a fresh profile, edge://gpu read over the debug port, then
https://example.com:

| | Direct composition | Compositing, WebGL, Canvas | Crash count | Page |
|---|---|---|---|---|
| `--disable-direct-composition` | false | Hardware accelerated | 0 | drawn |
| no flags | true | Hardware accelerated | 0 | drawn |

Both: ANGLE on D3D11, the Mali, feature level 11_0, Software Rendering
No.

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
