# GPU temperature and clock: Task Manager and performance counters

The driver reports the GPU temperature and clock two ways: through
dxgkrnl's adapter performance queries (Task Manager's GPU page) at WDDM
2.x, and through a performance counter set of its own, "RK3576 GPU", at
every WDDM version (section 6). Task Manager shows the temperature; it
has no field for a GPU clock. Settings: see [SETTINGS.md](SETTINGS.md).

## 1. What asks for what

Read from `dxgkrnl.sys` 10.0.22621.2428 and `Taskmgr.exe` 10.0.22621.2361
(`tools/re/dxgk-annotate.py --winbindex <file> <version>`).

| Query (`DXGK_QUERYADAPTERINFOTYPE`) | Asked for | Input | Output | WDDM version needed |
|---|---|---|---|---|
| 24 `NODEPERFDATA` | `D3DKMTQueryAdapterInfo` type 61, and `D3DKMTQueryStatistics` | ULONG: physical adapter in 31:16, node in 15:0 | `DXGK_NODE_PERFDATA`, 44 bytes | none on the 61 path; 2.4 for statistics |
| 25 `ADAPTERPERFDATA` | type 62, and statistics | ULONG: physical adapter | `DXGK_ADAPTER_PERFDATA`, 53 bytes | none on the 62 path; 2.4 for statistics |
| 26 `ADAPTERPERFDATA_CAPS` | once per adapter at start | ULONG | 28 bytes | 2.4 and up; a failure is only logged |
| 27 `GPUVERSION` | right after 26 | none | 128 bytes | 2.4 and up; a failure is only logged |

- The sizes are packed (`#pragma pack(1)` in `d3dkmdt.h`): dxgkrnl
  passes 0x2C and 0x35. A driver that insists on 48 or 56 bytes fails
  every call.
- dxgkrnl calls 24 and 25 at most once every 450 ms per node or adapter
  and caches the answer. It does not call during a D3 transition.
- Task Manager makes one `D3DKMTQueryAdapterInfo` call per GPU: type 62,
  64 bytes, `PhysicalAdapterIndex` 0. On failure, or a temperature below
  1.0 C, it hides the "GPU Temperature" row. It has no test for a
  dedicated or integrated adapter, despite Microsoft's 2019 announcement
  ("dedicated GPU", "WDDM 2.4"); what decides is whether the driver
  answers.

## 2. What the driver answers

`MwQueryPerfData` in `drivers/maliwddm/adapter.c`, decisions in
`perfdata_calc.h`, checked by `tests/perfdata-host`.

| Query | Field | Value |
|---|---|---|
| 25 | `Temperature` | the hotter of TSADC channels 3 and 5 in deci-Celsius (no OTP trim, a few degrees off) |
| 25 | `PowerStateOverride` | 1 while the GPU is up |
| 25 | the rest | 0 |
| 24 | `Frequency` | the clock `CRU_CLKSEL_CON165` gives now (198, 594 or 702 MHz), both nodes; 0 on the PVTPLL path |
| 24 | `MaxFrequency` | 594 MHz |
| 24 | `MaxFrequencyOC` | the highest rate the setting, supply and temperature guard allow: 594 or 702 MHz |
| 24 | `Voltage` | the supply in mV from the firmware's `GpuRail` reading or `GpuRailUv`; 0 if neither |
| 26 | `TemperatureWarning`, `TemperatureMax` | 85.0 C and 115.0 C (mainline gpu-alert and gpu-crit) |
| 27 | `BiosVersion`, `GpuArchitecture` | "none (SoC GPU)", "Mali-G52 MC3 (Bifrost)" |

- Answered at WDDM 2.x only. At 1.3 all four return
  `STATUS_NOT_SUPPORTED`. `PerfData` 0 turns them off at 2.x too.
- The temperature needs a TSADC that the CRU shows running (gates and
  resets clear, `TSADC_AUTO_CON` bit 0 set). Firmware that arms TSHUT
  starts it. Otherwise the answer is 0, Task Manager shows nothing, and
  the trace has `TsadcNotRunningGate` or `TsadcNotRunningAuto`. The driver
  never reads a TSADC the CRU has not confirmed (that is an SError on
  this SoC).
- Only the first query of each type goes into the boot journal and the
  registry trace (`QAI_24`, `QAI_25`, `PerfNodeIn`, `PerfAdapterIn`,
  `PerfNodeMhz`, `PerfTempDeciC`).

dxdiag reports "Dedicated Memory: 0 MB, Shared Memory: 1024 MB" because
the one memory segment is an aperture segment.

## 3. WDDM 2.4 as a probe

`WddmProbe` 0x2400 (interface 0x9006) uses the same arming and one-shot
fallback as 2.0 ([WDDM2.md](WDDM2.md) 16.13 and 16.14). It adds only what
dxgkrnl asks at 2.4: queries 26 and 27 at start, and the performance data
through `D3DKMTQueryStatistics`. Task Manager does not need it.

What dxgkrnl checks differently at 2.4 and the driver already handles:
`DXGK_NODEMETADATA.Flags` reserved bits must be clear (the driver zeroes
the structure); the brightness interface is asked as version 3 first
(the driver refuses it and dxgkrnl falls back to 2). Not examined:
`SetModeBehavior` and the newer VidPN path calls. A failure costs one
boot at 1.3, then the default returns.

## 4. How to check

Task Manager at the default 2.0:

1. No `WddmProbe` value (`wddm-probe default` if unsure), full reboot.
2. `C:\mali\m5test.exe temp 5` prints every second
   `ADAPTERPERFDATA 0x00000000: Temperature 6xx (6x.x C), Task Manager shows it`
   and one `NODEPERFDATA` line per node with clock, max, max OC and mV.
   `ADAPTERPERFDATA_CAPS` and `GPUVERSION` are empty at 2.0. Exit code 0
   when the temperature is valid.
3. Task Manager > Performance > GPU 0: a "GPU Temperature NN C" row and
   "NN C" next to GPU 0, moving with load. It should agree with
   `m5test clk` within about a degree.
4. `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm"`:
   `QAI_25` 0 (0xC00000BB means not supported), `PerfTempDeciC` the first
   reading. `TsadcNotRunning*` present means the firmware did not start
   the TSADC.

For the 2.4 probe: `wddm-probe set 2400`, reboot; `wddm-probe check`
shows `WddmVersion` 0x2400 and after 120 s `WddmProbeConfirmed` 0x2400;
the journal has `QAI 26 answered 0` and `QAI 27 answered 0`;
`m5test temp 3` shows `TemperatureMax 115.0 C, TemperatureWarning 85.0 C`
and the GPU version strings. On failure the next boot is 1.3 with
`WddmProbeFellBack` 0x2400; keep `Probe\Boot<n>` and the event 494 text.
`wddm-probe default` and a reboot go back.

## 5. Sources

- [DXGK_QUERYADAPTERINFOTYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype),
  [DXGK_ADAPTER_PERFDATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_adapter_perfdata)
  (temperature in 0.1 C),
  [DXGK_NODE_PERFDATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_node_perfdata)
  (frequencies in Hz, voltage in mV),
  [DXGK_ADAPTER_PERFDATACAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_adapter_perfdatacaps),
  [DXGK_GPUVERSION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_gpuversion).
- [Windows 10 Insider Preview Build 18963](https://blogs.windows.com/windows-insider/2019/08/16/announcing-windows-10-insider-preview-build-18963/)
  (the "dedicated GPU" and "WDDM 2.4" statements).
- [microsoft/libdxg](https://github.com/microsoft/libdxg) headers
  (`d3dkmdt.h`, `d3dkmthk.h`).
- [Using kernel mode performance counters](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/using-kernel-mode-performance-counters)
  and the [Kcs sample](https://github.com/microsoft/Windows-driver-samples/tree/main/general/perfcounters/kcs);
  the [INF DDInstall section](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-section)
  has no counter manifest directive.
- mainline `rk3576.dtsi` gpu-thermal: gpu-alert 85000, gpu-crit 115000;
  `rockchip,hw-tshut-temp` 120000.

## 6. The "RK3576 GPU" performance counters (PCW)

A kernel-mode counter set for anything that reads PDH counters (perfmon,
typeperf, a tray monitor). It works at 1.3 and 2.x and does not depend on
dxgkrnl's queries; `PerfData` does not turn it off.

| PDH path | Type | Value |
|---|---|---|
| `\RK3576 GPU(gpu0)\Temperature (C)` | rawcount | the hotter of TSADC 3 and 5, whole degrees; 0 when the TSADC is not running |
| `\RK3576 GPU(gpu0)\Clock (MHz)` | rawcount | 198, 594 or 702; 0 on the PVTPLL path |
| `\RK3576 GPU(gpu0)\% busy` | 100nsec_timer | time job chains ran, both slots; a running chain counts when it ends |
| `\RK3576 GPU(gpu0)\Throttled` | rawcount | 1 while the guard holds 594 MHz |
| `\RK3576 GPU(gpu0)\Throttled reason` | rawcount | 0 not held, 1 thermal (85 C, back after `GpuClkRestoreHoldSec` below 75 C), 2 faults (two above 600 MHz within 10 minutes, until restart). [PERF.md](PERF.md#temperature-guard-and-fault-policy) |
| `\RK3576 GPU(gpu0)\WDDM version` | rawcount | 20, 13, or 24 for a 2.4 probe |
| `\RK3576 GPU(gpu0)\WDDM fallback reason` | rawcount | 0 not fallen back, 1 two boots without a frame, 2 the same ending in a TDR storm, 3 three unconfirmed boots, 4 an explicit `WddmProbe` never confirmed, 5 `WddmProbe` 0x1300. [WDDM2.md](WDDM2.md) 16.14 |

One instance, `gpu0`, while the adapter is started. Counter set GUID
`{382d7234-6ed6-420e-9aa8-ba6e5abec190}`, provider `maliwddm`, provider
GUID `{f5aa14ca-df31-4bf4-b28d-74063728197c}`.

How it is built: `drivers/maliwddm/maliwddm.man` is the manifest
(`providerType="kernelMode"`, `callback="custom"`,
`applicationIdentity="maliwddm.sys"`). The vcxproj's `Ctrpp` item
generates `maliwddmCounters.h` and `maliwddmCounters.rc` (names compiled
into `maliwddm.sys`). `pcw.c` registers the set in DriverEntry and
answers the callback, which runs only while a consumer collects: two
TSADC reads and one CRU read under a mutex the adapter's stop also takes.

Installing the names: PDH needs `lodctr /m`, and no INF directive does
that. The INF copies `maliwddm.man` next to `maliwddm.sys` in the driver
store. From an administrator prompt in the release folder,
`maliwddm-counters install` finds that folder from
`Services\maliwddm\ImagePath` and runs `lodctr /m`. Each package update
uses a new driver store folder, so run it again after every install
(it unregisters by GUID first). Without that the counters exist but PDH
has no names. `maliwddm-counters uninstall` runs `unlodctr /g:`.

Checking:

1. `maliwddm-counters check`: `ApplicationIdentity` points into the
   current driver store folder; `typeperf -qx "RK3576 GPU"` lists the
   paths; three samples follow, for example `"67","594","3.1","0"`.
2. Under load: `typeperf "\RK3576 GPU(gpu0)\% busy" -si 1 -sc 10` follows
   `m5test prof`'s GPU busy.
3. `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" /v PcwRegister`:
   0 means registered; anything else is the failing status. A failed
   registration changes nothing else.
