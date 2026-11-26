# Settings

Every switch the driver reads. All are `REG_DWORD`. A value that is not
there means the default. Other docs link here instead of repeating this.

## Where they live

| Key | Path | Read |
|---|---|---|
| Device Parameters | `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters` | at adapter start, unless the table says otherwise |
| Parameters | `HKLM\SYSTEM\CurrentControlSet\Services\maliwddm\Parameters` | once, in DriverEntry: a change needs a full reboot |

The subkey `Device Parameters\maliwddm` holds the driver's trace values
(`Hpd_*`, `Tdr<n>_*`, `Fail_*`, `Probe2000\Wd_*` and so on). It is for
reading. A setting written there has no effect.

Example:

```
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters" /v HdmiHpd /t REG_DWORD /d 3 /f
```

To go back to the default, delete the value (`reg delete ... /v HdmiHpd /f`).

## Device Parameters

### User-mode driver

Read at each adapter query (new processes see a change, no reboot) unless
noted. Details in [D3D11-UMD.md](D3D11-UMD.md).

| Name | Default | Meaning |
|---|---|---|
| `UmdDdi` | 101 | 100: D3D10.0 DDI, feature level 10_0. 101: D3D10.1 DDI, 10_1. 110 and 111 are defined but not implemented (treated as 101). |
| `UmdFeatureLevel` | 0xa000 | Passed to the UMD; not used by it. |
| `UmdDxgi12` | 0 | Reserved for the DXGI 1.2 DDI; not used. |
| `UmdMsaa` | 1 | Multisample answers. 0: none. 1: only in msedge.exe, chrome.exe, msedgewebview2.exe, d3dprobe.exe. 2: every process. 4 and 5 act as 1 and 2. Reboot to change. |
| `UmdRedirect` | 1 at WDDM 2.x, 2 at 1.3 | DXGI redirection. 1: no process. 2: the browsers above, by name. 3: every process. At 2.x, 2 froze the session ([EDGE-PRESENT.md](EDGE-PRESENT.md#the-wddm-20-session-hang)). The files `C:\mali\noredirect` and `C:\mali\redirect` override it. |
| `UmdReportFL11` | 1 | 0: off. 1: feature level 11_0 and the D3D11.1 interface for Chromium's GPU process only (`--type=gpu-process`). See [CHROMIUM-VIDEO.md](CHROMIUM-VIDEO.md). |
| `UmdCopyOnGpu` | 1 | Texture copies on the GPU. 0: on the CPU. |
| `UmdReadbackOneCopy` | 1 | A read-back present copies the frame once. |
| `UmdMergeSubmit` | 1 | A batch's vertex/tiler and fragment chains go out as one render. |
| `UmdCopyBatch` | 1 | A GPU copy becomes a draw in the open batch, with no flush. |

The last four reach running processes within about a second.

### Video decode

A change needs a full restart. Details in
[VIDEO-DECODE.md](VIDEO-DECODE.md).

| Name | Default | Meaning |
|---|---|---|
| `UmdVideo` | 2 | 0: off. 1: D3D11.1 interface and a video device with no profiles in mpv, ffmpeg, ffplay, ffprobe, vlc, vdectest, d3dprobe. 2: as 1, with the decoder's profiles when `\\.\VDPU383` opens. 3: as 2 in every process except dwm, logonui, consent. Above 3 counts as 0. |
| `UmdVideoCodecs` | 0 | Profiles offered, bit 0 H.264, 1 HEVC Main, 2 VP9 profile 0, 3 AV1 Main. 0: all four. 1: H.264 only. Read only with `UmdVideo` set. |
| `UmdAv1Synth` | 0 | AV1 tile wrapping. 0: one OBU_TILE_GROUP. 1: an OBU_FRAME with a 16-byte zero frame header. |
| `UmdVp9Mpp` | 0 | VP9 frame contexts. 0: as the spec says. 1: reset all four on intra frames (as MPP). 2: write the adapted context (as MPP). 3: both. |
| `UmdVideoZeroCopy` | 1 | The decoder writes into the application's texture. 0: copy every picture. |
| `UmdVideoKmd` | 1 | The decoder's calls go through maliwddm ([CHROMIUM-VIDEO.md](CHROMIUM-VIDEO.md), C2): no process opens `\\.\VDPU383`. 0: the UMD opens it itself, which needs vdpu383.sys's `UserAccess`. |

The decoder driver `vdpu383.sys` (woa-rk3576) has its own key,
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters`:
`UserAccess` 0 (leave it: players go through maliwddm; 1: desktop processes
may open `\\.\VDPU383`, 2: also low-integrity and app-container processes,
both test boards only, with `UmdVideoKmd` 0) and `Iommu` 1 (the maliwddm
path needs it).

### Display

Read at adapter start. Details in [HDMI-HPD.md](HDMI-HPD.md),
[DISPLAY-MODES.md](DISPLAY-MODES.md) and [DUAL-DISPLAY.md](DUAL-DISPLAY.md).

| Name | Default | Meaning |
|---|---|---|
| `ScanOut` | 1 | The VOP2 reads primaries directly. 0: every frame is copied into the firmware framebuffer. |
| `ScaledModes` | 1 | Below `HdmiHpd` 3: smaller source modes, upscaled by the VOP2. 0: the firmware's size only. |
| `RotateFlips` | 0 | At 180 degrees, the driver also rotates flips of the screen's own size. |
| `HdmiHpd` | 3 on an HDMI boot, 0 on a DSI panel boot | 0: nothing. 1: poll HPD and read the EDID, trace only. 2: report plug, unplug and the EDID to Windows, no mode set. 3: the driver's own HDMI mode set and HDMI audio. A value set by hand wins over the boot default. |
| `HdmiHpdSource` | 0 | HPD bit. 0: `IOC_HDMI_HPD_STATUS` bit 3. 1: bit 6. 2: the GPIO4_PC1 pad. |
| `HdmiHpdPollMs` | 200 | HPD poll period, 50 to 2000. |
| `HdmiHpdDebounceMs` | 1000 | A level counts after holding this long, 300 to 10000. |
| `HdmiAudio` | 1 | 0: no audio setup after a mode set. |
| `HdmiDiag` | 0 | 1: VP0 colour bars after the mode set. 2: eight bars and a white frame in the driver's framebuffer. |
| `HdmiExclusive` | 1 | Level 3 on a DSI boot: the panel is reported gone while HDMI is connected. 0: report both (HDMI stays black with one source). Ignored with `HdmiExtend` 1. |
| `HdmiDefault60` | 1 | Level 3: the preferred mode is the largest size at the rate nearest 60 Hz. 0: the monitor's preferred timing. |
| `HdmiExtend` | 0 | 1, on a DSI boot with `HdmiHpd` 3 set by hand: panel and HDMI as an extended desktop. Not run on a board. |
| `HdmiMaxPixels` | 4096000 (2560x1600) | Size of the level 3 framebuffer; larger modes are not offered. 1280x720 to 4096x2304. |

### GPU clock, temperature, scheduling

Read at adapter start. Details in [PERF.md](PERF.md) and
[PERFDATA.md](PERFDATA.md).

| Name | Default | Meaning |
|---|---|---|
| `GpuClkHigh` | not set: 1 if the firmware reports the GPU rail at 775 mV or more, else 0 | 0: never above 600 MHz. 1: mainline operating points (702 MHz allowed). 2: the vendor's voltages for each level. |
| `GpuRailUv` | 0 | The rail in microvolts, measured another way (550000 to 900000). With a firmware reading too, the lower wins. It never turns `GpuClkHigh` on. |
| `GpuClk` | 0 | The stored start clock, `(parent << 8) \| divider`, written by `m5test clk <p> <d> keep`. 0: 702 MHz with `GpuClkHigh` on, else 594 MHz. |
| `GpuClkRestoreHoldSec` | 0 (90 s) | Seconds below 75 C before the clock comes back after a thermal drop, at most 3600. |
| `PerfTemp` | 0 | 1: `m5test prof` reads the temperature sensors even when the guard has not mapped them. |
| `PerfData` | 1 | 0: do not answer Task Manager's temperature and clock queries (WDDM 2.x). |
| `TimeSlice` | 0 | Time slice in ms with soft-stop preemption, at most 1000. 0: preempt only between DMA buffers. |
| `PollMs` | 1 | Job status poll period while work is in flight, 1 to 50. |
| `PreemptAnswer` | 1 | 0: ignore node-0 preemption requests at `TimeSlice` 0. For A/B tests only; it causes TDRs. |

## Parameters (service key)

Read once per boot. Details in [WDDM2.md](WDDM2.md).

| Name | Default | Meaning |
|---|---|---|
| `WddmProbe` | not set (WDDM 2.0) | 0x1300: force WDDM 1.3. 0x2000 to 0x3100: a one-shot probe at that version (0x2400 is the Task Manager probe). The driver deletes it when a probe falls back. |
| `WddmProbeKeep` | 0 | 1: no fallback for a probe. |
| `Wddm2Shape` | 0 | 0: physical mode with a copy paging node. 1: also GpuMmu caps and DDIs. |
| `Wddm2Budget` | 1 | 0: no segment budget group or application-target flags. |
| `Wddm2LocalBudget` | 1 | 0: the segment goes in the non-local budget group (bugcheck 0x7E). |
| `Wddm2PowerDdis` | 1 | 0: leave out CalibrateGpuClock and SetStablePowerState (dxgkrnl refuses the start). |
| `Wddm2RotOffset` | 1 | 0: leave out rotation Offset0 (bugcheck 0x113 or 0x1C). |
| `Wddm2AcquireHandle` | 1 | 0: use DxgkCbGetHandleData (black screen at 2.0). |
| `GpuMmuPtSegment` | 1 | Segment id for the page tables with `Wddm2Shape` 1. |
| `PagingMarker` | 1 | An 8-byte marker in paging DMA buffers at 2.x. 0: off. |
| `ProbeWatchdog` | 0 | 1: run the watchdog at 1.3 too (it always runs at 2.x). |
| `ProbeUart` | 0 | 1: one watchdog line a second on the UART. |
| `ProbeUartBase` | 0x2AD40000 | The UART's physical base. |
| `ProbeJournalMax` | not set | Lowers the journal limit (400 events, 1500 on a probe; never above 1500). |

The `Wddm2*`, `GpuMmuPtSegment` and `PagingMarker` switches exist to bisect
a failing start; leave them at their defaults.

The driver writes these itself; do not set them: `WddmProbeArmed`,
`WddmProbeConfirmed`, `WddmProbeFellBack`, `WddmProbeFellBackBuild`,
`WddmProbeFellBackBoot`, `WddmProbeFellBackReason`, `WddmProbeStage`,
`WddmProbeHardMisses`, `WddmProbeSoftMisses`, `WddmProbeLastMiss`,
`WddmProbeArmFailed`, `WddmProbeUnknown`, `ProbeBoots`, and the journal
under `Services\maliwddm\Probe\Boot<n>`.

## Set by the INF

`HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler`,
`EnablePreemption` = 0. The INF also writes the UMD and OpenGL ICD names
(`UserModeDriverName`, `OpenGLDriverName` and their `Wow` forms) in the
adapter's key.

## Environment and files

For CI and debugging. When the kernel driver answers, the `D3D10UMD_UMD*`
variables are not read.

| Name | Meaning |
|---|---|
| `D3D10UMD_UMDDDI`, `D3D10UMD_UMDMSAA`, `D3D10UMD_UMDVIDEO`, `D3D10UMD_UMDFL11` | Stand-ins for the settings above when no kernel driver answers (CI). |
| `D3D10UMD_COPYBATCH` | Per-process override of `UmdCopyBatch`. |
| `D3D10UMD_VDEC_FAKE=1` | Build the decoder's register file and write a known pattern instead of decoding. |
| `D3D10UMD_VDEC_REGLOG=<file>` | Log every register file built. |
| `D3D10UMD_VDEC_ZEROCOPY=0` | Force the video copy path. |
| `D3D10UMD_VDEC_PATH` | `direct`: open `\\.\VDPU383` and not maliwddm; `kmd`: maliwddm only. Unset: maliwddm, else the device. |
| `D3D10UMD_NV12_ALIAS=0` | NV12 plane views are copies of the texture, not aliases of it. |
| `C:\mali\vdec-shared-copy` | Shared decoder textures (Chromium's) are copied into, not decoded into. |
| `MALIKM_FAKE=1` | No device: the shim answers every call itself ([M3.md](M3.md)). 2 also logs each call. |
| `MALIKM_TRACE=1` | The shim logs every driver call. |
| `C:\mali\log` | The UMD log directory ([EDGE-PRESENT.md](EDGE-PRESENT.md#umd-log)); a file named `off` in it stops new logs. |
