# GPU temperature and clock: Task Manager and performance counters

Branch `gpu-perfdata` (off `wddm2x`, with `gpu-thermal-fallback` picked
in). Not run on the board yet.

**Short answer.** At the default WDDM 2.0 the driver now answers
dxgkrnl's adapter performance query with the GPU temperature from the
TSADC, and Task Manager's GPU page should then show "GPU Temperature".
No higher WDDM version is needed for that: neither Task Manager nor
dxgkrnl checks the version or "dedicated" on that path in the binaries
read below. Task Manager shows no GPU clock at all, whatever the driver
reports; the clock is there for tools that ask for it (`m5test temp`
does). WDDM 2.4 (`wddm-probe set 2400`) adds only what dxgkrnl itself
asks at 2.4: the temperature limits and the GPU version, and the same
data through `D3DKMTQueryStatistics`.

For PDH consumers (perfmon, typeperf, a tray monitor) the driver also
provides a performance counter set of its own, "RK3576 GPU", at every
WDDM version (section 6).

That is a prediction from disassembly plus one fact from the board (run
G asked the driver for exactly this query at 2.0). Microsoft's own
statement says "dedicated GPU" and "WDDM 2.4"; section 2 says why the
binaries disagree, and section 5 is the board test that settles it.

## 1. What asks for what

Read from `dxgkrnl.sys` 10.0.22621.2428 (the 22631 binary; amd64 build,
public symbols, `tools/re/dxgk-annotate.py --winbindex dxgkrnl.sys
10.0.22621.2428`) and `Taskmgr.exe` 10.0.22621.2361 (`--winbindex
taskmgr.exe 10.0.22621.2361`; 2428 is not in winbindex). Microsoft's
names are from the DDI reference and the WDK headers.

| DDI query (`DXGK_QUERYADAPTERINFOTYPE`) | Asked by dxgkrnl | Input | Output | WDDM version |
|---|---|---|---|---|
| 24 `DXGKQAITYPE_NODEPERFDATA` | `DXGADAPTER::GetNodePerfData`, for `D3DKMTQueryAdapterInfo(KMTQAITYPE_NODEPERFDATA = 61)` and for `D3DKMTQueryStatistics` node data | one ULONG: physical adapter in bits 31:16, node in 15:0 | `DXGK_NODE_PERFDATA`, 44 bytes | none on the 61 path; 2.4 (`m_WddmVersion` >= 2400) on the statistics path |
| 25 `DXGKQAITYPE_ADAPTERPERFDATA` | `DXGADAPTER::GetAdapterPerfData`, for `KMTQAITYPE_ADAPTERPERFDATA = 62` and for statistics | one ULONG: physical adapter | `DXGK_ADAPTER_PERFDATA`, 53 bytes | none on the 62 path; 2.4 on the statistics path |
| 26 `DXGKQAITYPE_ADAPTERPERFDATA_CAPS` | `ADAPTER_RENDER::Initialize`, once per physical adapter | one ULONG | `DXGK_ADAPTER_PERFDATACAPS`, 28 bytes | 2.4 and up only; a failure is only logged |
| 27 `DXGKQAITYPE_GPUVERSION` | the same, right after 26 | none | `DXGK_GPUVERSION`, 128 bytes | 2.4 and up only; a failure is only logged |

- The sizes are not the padded C sizes: `d3dkmdt.h` declares these
  structures inside `#pragma pack(push, 1)`. dxgkrnl passes 0x2C and
  0x35; a driver that insists on 48 or 56 bytes fails every call.
- dxgkrnl calls the driver for 24 and 25 at most once every 450 ms per
  node or physical adapter (`KeQueryPerformanceCounter`, 0x1C2 ms) and
  answers callers from its copy in between. It does not call while the
  adapter is in a D3 transition (`DXGADAPTER` +0xA0 = 1, set in
  `AcquireLocksForPowerStateD3transition`).
- `DxgkQueryAdapterInfoImpl` dispatches 61 to 64 through its jump table
  straight to the four `Get*` functions; neither they nor
  `DXGADAPTER::DdiQueryAdapterInfo` look at the WDDM or interface
  version. `DxgkQueryStatisticsInternal` does (`cmp [adapter+0xB04],
  0x960`, 2400).
- **The board agrees.** Run G (WDDM 2.0, build 350, docs/WDDM2.md 16.12)
  left `QAI_25 = 0xC00000BB` (`STATUS_NOT_SUPPORTED`) and `QAI_25_Size =
  0x35` in `Device Parameters\maliwddm`
  (`board-session-prep/wddm2x-stepG/devkey-20261124-183425.txt`): dxgkrnl
  asked a 2.0 driver for the adapter performance data, 53 bytes, while
  Task Manager was open for `taskmgr.jpg`. No `QAI_24`: Task Manager never
  asks for nodes.

### Task Manager

`AtmGpuView::UpdateUtilization` runs for every GPU in the view's list,
with nothing that tests for a dedicated or integrated adapter:

1. `D3DKMTOpenAdapterFromLuid`, then the program's only
   `D3DKMTQueryAdapterInfo` call: type 0x3E (62, `ADAPTERPERFDATA`), 64
   bytes, `PhysicalAdapterIndex` 0.
2. On failure: the sidebar entry and the "GPU Temperature" row are
   hidden (`E_INVALIDARG` logged as harmless).
3. On success: `Temperature / 10`. If that is 1000 or more and the
   vendor is 0x1002 (AMD) it is divided by 100 again (a workaround for
   drivers that report centi-degrees). If the result is 0:
   `InvalidTemperatureUpdate`, both hidden.
4. Otherwise the value goes into the sidebar ("GPU 0 ... NN C") and the
   page's `gpuTemperatureValue`, and the `gpuTemperature` row is made
   visible.

So the condition is: the query succeeds and the temperature is at least
1.0 C. No memory frequency, power, fan or engine clock is read: Task
Manager has nothing that would show a GPU clock.

## 2. "Dedicated GPU only" and "WDDM 2.4"

The Windows Insider announcement of build 18963 (2019-08-16), where the
feature first appeared: "If you have a dedicated GPU card, we will now
bubble up the current temperature next to its listing in the Performance
Tab", "Showing the GPU temperature in Task Manager is currently only
supported if you have a dedicated GPU card", "one which supports version
2.4 (or higher) of WDDM is required".

Neither is a check in the 22621 binaries:

- WDDM 2.4 is where `DXGKQAITYPE_ADAPTERPERFDATA` entered the DDI
  (Windows 10 1803, the DDI reference), so a driver that answers it was
  written against 2.4 or later. dxgkrnl does not hold an older driver to
  that on the `D3DKMTQueryAdapterInfo` path (section 1).
- "Dedicated": in 2019 the drivers that answered the query were the
  discrete GPUs' drivers. Task Manager has no adapter-type test around the
  query. What decides is whether the driver answers.

What is not known: whether the Task Manager on the board is the same
code. Its version is in `(Get-Item C:\Windows\System32\Taskmgr.exe).VersionInfo`;
anything 10.0.22621.x is the same source line as the one read here. Also
not known: whether Task Manager filters its GPU list some other way
before step 1. It already shows this adapter, with utilisation and
memory (run G), so the view's list has it.

How this adapter is classed elsewhere, for completeness: dxdiag says
"Dedicated Memory: 0 MB, Shared Memory: 1024 MB" (run G), because the
one segment is an aperture segment; the budget group does not change
that. A segment that is not an aperture would make the 1 GB "dedicated"
and is a memory-model change, not something to do for a temperature.

## 3. What the driver answers

`drivers/maliwddm/adapter.c` (`MwQueryPerfData`), decisions in
`perfdata_calc.h`, checked by `tests/perfdata-host` in CI.

| Query | Field | Value |
|---|---|---|
| 25 | `Temperature` | the hotter of TSADC channels 3 and 5, deci-Celsius, rounded (`thermal_calc.h`'s reading, as the 85 C fallback uses it; no OTP trim, a few degrees) |
| 25 | `PowerStateOverride` | 1 while the GPU is up |
| 25 | the rest | 0: no memory clock, bandwidth, fan or power counter here |
| 24 | `Frequency` | the GPU clock as `CRU_CLKSEL_CON165` has it now (198 MHz as the firmware leaves it, 594 MHz by default since `gpu702-default` or with `GpuClk`, 702 MHz with `GpuClkHigh`, which is on by default where the firmware set the rail to 775 mV), both nodes; 0 on the PVTPLL path |
| 24 | `MaxFrequency` | 594 MHz, the highest rate at or below the 600 MHz default ceiling |
| 24 | `MaxFrequencyOC` | the highest rate the current setting, supply and temperature guard allow: 594 MHz, or 702 MHz with `GpuClkHigh` and the guard running |
| 24 | `Voltage` | the supply in mV as the firmware measured it at boot (`GpuRail`) or `GpuRailUv`; 0 if neither |
| 26 | `TemperatureWarning`, `TemperatureMax` | 85.0 C and 115.0 C, mainline rk3576.dtsi's gpu-alert and gpu-crit |
| 27 | `BiosVersion`, `GpuArchitecture` | "none (SoC GPU)", "Mali-G52 MC3 (Bifrost)" |

- Answered at WDDM 2.x only. At 1.3 (`WddmProbe` 0x1300) the four stay
  `STATUS_NOT_SUPPORTED`, as on `main`.
- `Device Parameters\maliwddm\PerfData` 0 (read at start) turns them off
  at 2.x as well.
- **The temperature needs a running TSADC.** It is read through the
  mapping the temperature guard makes once the CRU says the TSADC runs
  (`TsadcMapIfRunning`: gates and resets clear, `TSADC_AUTO_CON` bit 0
  set). Firmware that arms TSHUT (edk2-rk3576 761a938 and later; the
  board's 537ea09 says "TSHUT armed") starts it. Without that the answer
  is 0 and Task Manager shows nothing; the trace has
  `TsadcNotRunningGate` or `TsadcNotRunningAuto`. The driver never reads
  a TSADC the CRU has not confirmed (an SError on this SoC).
- 24 and 25 come twice a second while Task Manager is open, so only the
  first of each goes into the boot journal and the registry trace
  (`QAI_24`, `QAI_25`, `PerfNodeIn`, `PerfAdapterIn`, `PerfNodeMhz`,
  `PerfTempDeciC`). Before this change every one of them was a failed
  DDI: two registry writes with a flush and a journal line, twice a
  second, for as long as Task Manager stayed open.

## 4. WDDM 2.4 as a probe

`WddmProbe` 0x2400 already exists (`version.c` levels: caps 0x2400,
interface `DXGKDDI_INTERFACE_VERSION_WDDM2_4` = 0x9006), with the same
arming and one-shot fallback as 2.0. Nothing about the default changes.

What it buys: dxgkrnl asks 26 and 27 at start and keeps them
(`m5test temp` prints them), and `D3DKMTQueryStatistics` carries the
node and adapter performance data, which some monitoring tools read.
Task Manager does not need it.

What changes in dxgkrnl between interface 0x5023 and 0x9006, as far as
the disassembly shows (`cmp` against the interface or `m_WddmVersion`):

| Where | At | What | Here |
|---|---|---|---|
| `ADAPTER_RENDER::Initialize` | 0x9000 | `DXGK_NODEMETADATA.Flags` taken seriously: reserved bits 4 to 15 set fail the start ("Driver should not set reserved bits"); bit 0 (`ContextSchedulingSupported`) without GPU VA fails it | `MwGetNodeMetadata` zeroes the structure; no flag is set |
| `ADAPTER_RENDER::Initialize` | 2400 | QAI 26 and 27, failures logged only | answered (section 3) |
| `DpiBrightnessStartDevice` | 0x700A | asks `GUID_DEVINTERFACE_BRIGHTNESS_3` first, then 2, then 1 | `MwBacklightQueryInterface` refuses the unknown GUID; dxgkrnl goes on to 2 |
| `DXGADAPTER::Initialize` | 0x6002 / 0x7000 | `SetModeBehavior` from two driver-reported fields | not examined |
| `DMMVIDPNPRESENTPATH::SetTarget{Gamma,ContentType,AnalogCopyProtection}` | 0x700A | newer path calls | not examined |
| `DpiGetDriverDataSizeFromVersion` | each step | how much of `DRIVER_INITIALIZATION_DATA` it reads | the driver is built with the 26100 WDK: the whole structure is there |

Microsoft documents no mandatory DDI for 2.1 to 2.4 that this driver
lacks (docs/WDDM2.md 3.3); 2.1's UMD callbacks are the UMD's. The unknown
is undocumented validation, and the fallback covers it: a refused start,
a bugcheck or a black screen costs one boot at 1.3, then the default 2.0
returns (the fallback is sticky only for the default's own version).

Risk, in order: a refused start with event 494 text (cheap, the
fallback); a path call that only 2.2+ makes into a DDI that answers
differently (display: a black screen, the fallback); nothing at all.

## 5. Board test

The package from this branch's CI run. Do not combine with other probes.

### A. Default 2.0, Task Manager

1. Install the package as usual (docs/WDDM2.md 16.13). No `WddmProbe`
   value (`wddm-probe default` if unsure). Full reboot.
2. Desktop up. `C:\mali\m5test.exe temp 5`. Expect, every second:

       t=0s ADAPTERPERFDATA 0x00000000: Temperature 6xx (6x.x C), Task Manager shows it (6x C), PowerStateOverride 1
             node 0 NODEPERFDATA 0x00000000: 594 MHz, max 594 MHz, max OC 594 MHz, 775 mV
             node 1 NODEPERFDATA 0x00000000: 594 MHz, ...

   (Before `gpu702-default`, 198 MHz if no `GpuClk` was stored; now 594,
   or 702 where `GpuClkHigh` is on. Max OC 702 MHz with `GpuClkHigh` 1.)
   `ADAPTERPERFDATA_CAPS` and `GPUVERSION` print zeros and empty strings at
   2.0: dxgkrnl asks for them only from 2.4. Exit code 0.
3. Task Manager > Performance > GPU 0 ("Rockchip RK3576 Mali-G52 (WDDM)").
   Expect a "GPU Temperature  NN C" row under the GPU memory figures and
   "NN C" next to GPU 0 in the left column, within a second or two, and
   moving with load (FurMark, or a window drag).
4. Compare with `m5test clk` (thermal guard, hotter of TSADC 3/5): the
   same temperature within a degree or so.
5. Registry trace, `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm"`:
   `QAI_25` 0 (it was 0xC00000BB), `PerfAdapterIn` 0, `PerfTempDeciC` the
   first reading (0x29E, 670, at 67.0 C); after step 2 also `QAI_24` 0,
   `PerfNodeIn` 0 and `PerfNodeMhz`. `TsadcNotRunningGate` or
   `TsadcNotRunningAuto` present means the firmware did not start the
   TSADC: temperature 0, nothing in Task Manager, and that is the answer.
6. The journal (`wddm-probe check`, `Probe\Boot<n>`): on a default boot it
   ends at the confirm (120 s), so the QAI 25 lines are there only if Task
   Manager was opened before that: `QAI 25 asked: in 4 bytes, out 53
   bytes`, then `QAI 25 answered 0`, once.

If step 2 passes and step 3 shows no temperature, Task Manager decides by
something not read here: note its version (section 2), keep the photo,
and try B.

### B. WDDM 2.4 probe (optional)

1. `wddm-probe set 2400`, full reboot.
2. Desktop up: `wddm-probe check` shows `WddmVersion` 0x2400 and after 120 s
   `WddmProbeConfirmed` 0x2400. dxdiag: Driver Model WDDM 2.4.
3. The journal near the start: `QAI 26 asked: in 4 bytes, out 28 bytes`,
   `QAI 26 answered 0`, `QAI 27 asked: ... out 128 bytes`, `QAI 27
   answered 0`.
4. `m5test temp 3`: `ADAPTERPERFDATA_CAPS ... TemperatureMax 115.0 C,
   TemperatureWarning 85.0 C`, `GPUVERSION ... "none (SoC GPU)",
   "Mali-G52 MC3 (Bifrost)"`, the rest as in A.
5. Task Manager as in A.3, and the desktop, a window drag and `m5test`
   (the job run) as for any probe.
6. If the boot fails: the next boot is 1.3 with `WddmProbeFellBack`
   0x2400, `WddmProbe` deleted; the boot after that is 2.0 again. Keep the
   journal of the failed boot (`Probe\Boot<n>`, `WddmProbeFellBackBoot`)
   and the event 494 text.
7. Back: `wddm-probe default`, reboot.

## 6. The "RK3576 GPU" performance counters (PCW)

A kernel-mode counter set of the driver's own, for anything that reads
PDH counters. It does not depend on the WDDM version (1.3 and 2.x alike)
or on dxgkrnl's queries above, and the setting `PerfData` does not turn it
off.

| PDH path (English names) | Type | Value |
|---|---|---|
| `\RK3576 GPU(gpu0)\Temperature (C)` | `perf_counter_rawcount` | the hotter of TSADC channels 3 and 5, whole degrees (the reading of section 3, truncated as Task Manager truncates it); 0 when the TSADC is not running |
| `\RK3576 GPU(gpu0)\Clock (MHz)` | `perf_counter_rawcount` | the GPU clock `CRU_CLKSEL_CON165` gives now, rounded: 198, 594 or 702; 0 on the PVTPLL path |
| `\RK3576 GPU(gpu0)\% busy` | `perf_100nsec_timer` | the time job chains ran on the GPU (`SlotTicks`, both slots, in 100 ns), which PDH turns into a percentage of each sample interval. A chain still running counts when it ends |
| `\RK3576 GPU(gpu0)\Throttled` | `perf_counter_rawcount` | 1 while the guard holds 594 MHz (`GpuClkHigh` on: heat or faults), else 0 |
| `\RK3576 GPU(gpu0)\Throttled reason` | `perf_counter_rawcount` | why: 0 not held, 1 thermal (85 C; back after 75 C for `GpuClkRestoreHoldSec`, default 90 s), 2 faults (two job faults or TDRs above 600 MHz within 10 minutes; until restart). docs/PERF.md, "The fallback on the board" |
| `\RK3576 GPU(gpu0)\WDDM version` | `perf_counter_rawcount` | the WDDM version of this boot: 20 for 2.0, 13 for 1.3, 24 for a 2.4 probe |
| `\RK3576 GPU(gpu0)\WDDM fallback reason` | `perf_counter_rawcount` | why this boot is 1.3: 0 it is not, 1 two boots without a frame, 2 the same ending in a TDR storm, 3 three unconfirmed boots, 4 an explicit `WddmProbe` never confirmed, 5 `WddmProbe` 0x1300. docs/WDDM2.md 16.14 |

One instance, `gpu0`, while the adapter is started; none before or
after. Counter set GUID `{382d7234-6ed6-420e-9aa8-ba6e5abec190}`, provider
`maliwddm`, provider GUID `{f5aa14ca-df31-4bf4-b28d-74063728197c}`.

**How it is built.** `drivers/maliwddm/maliwddm.man` is the counter
manifest (`providerType="kernelMode"`, `callback="custom"`,
`applicationIdentity="maliwddm.sys"`). The WDK's ctrpp, run by the
vcxproj's `Ctrpp` item as in the WDK's Kcs sample, writes
`maliwddmCounters.h` (the `MwRegisterRk3576Gpu`, `MwAddRk3576Gpu` and
`MwUnregisterRk3576Gpu` helpers around `PcwRegister`, `PcwAddInstance`
and `PcwUnregister`) and `maliwddmCounters.rc` (the names, compiled into
`maliwddm.sys` through `maliwddm.rc`). `drivers/maliwddm/pcw.c`
registers the set in DriverEntry and answers PCW's callback.

**Cost.** None until a consumer collects: PCW calls the callback only
then, at PASSIVE_LEVEL. A collection reads two TSADC registers and one
CRU register, under a fast mutex the adapter's stop also takes (so a
collection never reads a mapping that is going away). Nothing is logged
per collection.

**If registration fails** the trace has `PcwRegister` with the status
(`Device Parameters\maliwddm`, written at every start; 0 = registered),
and nothing else changes. A DriverEntry whose `DxgkInitialize` fails
unregisters the set again, since no DriverUnload follows.

**Installing the names.** PDH lists a kernel provider only once its
manifest is installed with `lodctr /m`. There is no INF directive for a
counter manifest (the INF reference has `AddEventProvider` for ETW
manifests in `DDInstall.Events`, nothing for counters), and AddReg of
lodctr's registry layout would be undocumented. So:

- the INF copies `maliwddm.man` into the driver store next to
  `maliwddm.sys` (`MW_CopyFiles`, DIRID 13);
- `maliwddm-counters.cmd install` (in the release folder) finds that
  folder from `Services\maliwddm\ImagePath` and runs `lodctr
  /m:<folder>\maliwddm.man`; lodctr records `<folder>\maliwddm.sys`, the
  manifest's directory plus its `applicationIdentity`, as the file with
  the names;
- after every package update the driver store folder is a new one: run
  `maliwddm-counters install` again (it unregisters by GUID first). With
  the old folder deleted and no reinstall, the counters are still there
  but PDH has no names for them.

`maliwddm-counters uninstall` runs `unlodctr /g:` with the provider GUID.

### Board test

1. Install the package. From an administrator prompt in the release
   folder: `maliwddm-counters install`. Expect lodctr to succeed.
2. `maliwddm-counters check`: the registry lines show
   `ApplicationIdentity` in the current driver store folder;
   `typeperf -qx "RK3576 GPU"` lists the four paths of the table; three
   samples follow, for example `"67","594","3.1","0"`.
3. The same at 1.3: `wddm-probe off`, reboot, `maliwddm-counters check`
   gives the same four counters with values (`wddm-probe default` and
   reboot to go back).
4. Load: FurMark or a window drag, `typeperf "\RK3576 GPU(gpu0)\% busy"
   -si 1 -sc 10`: the busy share follows `m5test prof`'s GPU busy.
5. `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm" /v PcwRegister`: 0.
6. Optional, the cost: `m5test perf 10` with and without `typeperf ...
   -si 1` running; no difference beyond the noise of two runs is
   expected (state n).

## Sources

- [DXGK_QUERYADAPTERINFOTYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype):
  values 24 to 27, "Supported starting in Windows 10, version 1803".
- [DXGK_ADAPTER_PERFDATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_adapter_perfdata):
  "Temperature: The main temperature sensors reading in decimal Celsius.
  1 = 0.1C"; Power in tenths of a percent.
- [DXGK_NODE_PERFDATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_node_perfdata):
  Frequency, MaxFrequency, MaxFrequencyOC in Hz; Voltage in mV.
- [DXGK_ADAPTER_PERFDATACAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_adapter_perfdatacaps)
  and [DXGK_GPUVERSION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_gpuversion).
- [DXGKARG_QUERYADAPTERINFO](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_queryadapterinfo):
  does not document the input of 24 to 27; the input above is dxgkrnl's.
- [Announcing Windows 10 Insider Preview Build 18963](https://blogs.windows.com/windows-insider/2019/08/16/announcing-windows-10-insider-preview-build-18963/):
  the "dedicated GPU" and "WDDM 2.4" statements.
- WDK headers as published in [microsoft/libdxg](https://github.com/microsoft/libdxg)
  (`include/dxg/d3dkmdt.h`: the structures under `pragma pack(1)`;
  `d3dkmthk.h`: `KMTQAITYPE_NODEPERFDATA` 61 to `KMTQUITYPE_GPUVERSION` 64,
  `D3DKMT_*_PERFDATA` with `PhysicalAdapterIndex` and `NodeOrdinal`).
- [Using kernel mode performance counters](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/using-kernel-mode-performance-counters)
  (manifest, ctrpp, `lodctr /m`, how the binary path is formed) and the
  [Kcs sample](https://github.com/microsoft/Windows-driver-samples/tree/main/general/perfcounters/kcs)
  (the `Ctrpp` msbuild item this project copies);
  [INF DDInstall section](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-section)
  and [INF AddEventProvider directive](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addeventprovider-directive):
  no counter manifest directive.
- mainline `arch/arm64/boot/dts/rockchip/rk3576.dtsi`, gpu-thermal:
  gpu-alert 85000, gpu-crit 115000; `rockchip,hw-tshut-temp` 120000.
