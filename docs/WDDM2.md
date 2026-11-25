# WDDM 2.0 in maliwddm

maliwddm reports WDDM 2.0 by default and falls back to WDDM 1.3 by itself
when a 2.0 boot does not reach a confirmed desktop. This page says how the
2.0 driver works, how to force 1.3, what is known to be wrong, and how to
collect evidence when a 2.0 boot fails.

Settings: see [SETTINGS.md](SETTINGS.md). The WDDM version switches
(`WddmProbe`, `Wddm2*`, `Probe*`) live in
`HKLM\SYSTEM\CurrentControlSet\Services\maliwddm\Parameters`, not in the
device's Device Parameters key, and are read in DriverEntry, so each change
needs a full reboot. History: [history.md](history.md).

Section numbers are those of the original design study, because code
comments cite them; sections that were cut leave gaps.

## 1. Status and the options

| | State |
|---|---|
| WDDM 2.0, physical mode (`Wddm2Shape` 0) | the default since 2026-11-24. LogonUI and the desktop on HDMI, GPU jobs through WDDM, dxdiag "Driver Model WDDM 2.0", feature level 10_1, the Task Manager GPU page. Board-tested on the TaishanPi 3M |
| WDDM 1.3 | `WddmProbe` 0x1300, or after a fallback. No Task Manager GPU page |
| WDDM 2.0 with GpuMmu (`Wddm2Shape` 1) | built, never confirmed on a board. Page tables are written but no work runs in virtual mode |
| WDDM 2.1 to 3.1 | the probe can report them (`wddm-probe set 2100` ... `3100`); not a goal (section 3.3) |

The options the 2026-11-21 study weighed, and where each ended:

- **(a) IoMmu model:** not possible. It needs a system IOMMU with PASIDs.
  The RK3576 has no SMMU, the GPU node has no `iommus`, and the Mali has
  its own MMU.
- **(a') WDDM 2.0 in physical mode:** what ships (P1).
- **(b) WDDM 2.x with GpuMmu on the Mali MMU:** phases P2 to P7, not done.
  Estimated 16 to 26 person-weeks.
- **(c) WDDM 1.3 plus a D3D11.1 UMD with the video DDI:** the UMD work,
  [D3D11-UMD.md](D3D11-UMD.md). Estimated 12 to 24 person-weeks, decode
  excluded. It does not need WDDM 2.x.
- **D3D12:** 50 to 100 person-weeks; no open D3D12 UMD exists to start
  from. Not planned.
- **WDDM 3.x:** no gain on this board. 23H2 knows at most 3.1, every 3.0
  and 3.1 feature is optional, and none has hardware here.

What 2.0 gives here, compared with 1.3: the Task Manager GPU page with
per-process GPU use, and the performance counters that need 2.4 queries
([PERFDATA.md](PERFDATA.md)). What only GpuMmu (b) would give: per-process
GPU isolation (today any process can read another's GPU memory by
address), 32 GB of GPU VA per process instead of about 4 GB shared, more
than 1 GB resident, and faults charged to one context instead of a GPU
reset for everyone. Neither changes frame rates much: GPU-bound work is
limited by the clock.

## 2. How the 2.0 driver works

The configuration is fixed in `drivers/maliwddm/wddm2plan.h` (`Mw2Plan`),
checked by `tests/wddm2-host` (section 15):

- **Version:** `DXGK_DRIVERCAPS::WDDMVersion` 0x2000 and DDI interface
  0x5023. At 0x1300 the driver is the old 1.3 driver: one node, segment
  flags 0x5, the 1.3 DDI table (interface 0x4002), no arming, no journal.
- **Memory model:** physical mode. Allocation and patch lists as at 1.3,
  `VirtualAddressingSupported` 0, one 1 GB aperture segment. The Mali uses
  one address space (AS0) and one VA range, 32 MB to 4 GB, for all
  processes.
- **Nodes:** two. Node 0 is the 3D node (the Mali). Node 1 is a COPY node
  and the paging node (`PagingNodeIndex` 1). Its paging buffers are done on
  the CPU while they are built, and its submissions complete at once.
- **Segment flags at 2.x:** 0x88005, that is aperture, CPU visible,
  application target and the local budget group. dxdiag then shows
  "Dedicated Memory 0 MB, Shared Memory 1024 MB"; the dedicated figure
  follows the segment type.
- **DDIs a 2.x driver must have:** `CalibrateGpuClock` (GPU time modelled
  as the performance counter) and `SetStablePowerState` (nothing to pin),
  `CollectDiagnosticInfo` (only registered at 2.6 and above),
  `SupportPerEngineTDR` with ResetEngine, QueryEngineStatus and
  QueryDependentEngineGroup, `CancelCommandAware`, `MultiEngineAware`.
- **Handles:** OpenAllocation and the ALLOC_INFO escape resolve
  allocations with `DxgkCbAcquireHandleData`/`ReleaseHandleData`. At 2.x
  the 1.x `DxgkCbGetHandleData` returns NULL.
- **Rotation:** every path updated in EnumVidPnCofuncModality carries
  Offset0 as well as the rotations (0x1F, not 0xF).
- **Residency:** the UMD makes every buffer resident as it creates or
  opens it (section 16.10).
- **Escape `MW_ESCAPE_WDDM`:** returns the WDDM version and
  `MW_WDDM_NEEDS_RESIDENCY` at 2.x. A 1.3 KMD refuses the code.

### The fallback

Decided in `Mw2BootDecide` (`wddm2plan.h`), run in DriverEntry:

- Every 2.x boot arms itself first: `WddmProbeArmed` is written, flushed
  and read back. If it does not read back, the boot is 1.3 and
  `WddmProbeArmFailed` says so.
- The watchdog thread confirms the boot at PASSIVE_LEVEL after 60 renders,
  10 flips or presents, 20 s up, no DDI stuck and fewer than three TDRs.
  It then clears `WddmProbeArmed` and writes `WddmProbeConfirmed`.
- An armed boot records how far it got in `WddmProbeStage`: 1 armed, 2
  started, 3 presented (10 flips or presents), 15 TDR storm (three TDRs
  before the confirm).
- The next boot judges an unconfirmed one by its stage. Stage 1 or 2 (no
  frame) and stage 15 count as failures (`WddmProbeHardMisses`). Stage 3
  (frames, then nothing: a power cut, a reset, a later crash) counts as a
  miss (`WddmProbeSoftMisses`). `WddmProbeLastMiss` holds the class: 1 no
  frame, 2 TDR storm, 3 frames then no confirm.
- **The default** falls back after two failures in a row, or three
  unconfirmed boots of any kind in a row. A confirmed boot clears both
  counts.
- **An explicit `WddmProbe`** is one-shot: one unconfirmed boot falls back
  whatever its stage, and the value is deleted.
- **A fallback sticks per build:** the default stays at 1.3
  (`WddmProbeResult` 6) until another package is installed or
  `wddm-probe default` is run.
- On a fallback the driver writes `WddmProbeFellBack` (the version),
  `WddmProbeFellBackBuild`, `WddmProbeFellBackBoot` (the journal to read)
  and `WddmProbeFellBackReason`:

  | Reason | Meaning |
  |---|---|
  | 0 | not a fallback |
  | 1 | two 2.0 boots in a row showed no frame |
  | 2 | two 2.0 boots in a row failed, the last a TDR storm |
  | 3 | three unconfirmed 2.0 boots in a row |
  | 4 | an explicit `WddmProbe` never confirmed |
  | 5 | not a fallback: `WddmProbe` 0x1300 asks for 1.3 |

`WddmProbeResult` (in the device trace): 0 1.3 wanted, 1 a 2.x version in
effect, 2 fell back, 3 a value that is no WDDM version, 4 no service key
path, 5 arming failed, 6 stayed at 1.3 after an earlier fallback with this
build.

Known limit: a bugcheck before the first frame and a power cut before the
first frame look the same, so two power cuts in the first seconds of two
boots fall back. `wddm-probe default` brings 2.0 back.

### Switching version

From an administrator prompt (`tools/windows/wddm-probe.cmd`, in the
package), then a full reboot:

| Command | Effect |
|---|---|
| `wddm-probe off` or `wddm-probe set 1300` | force 1.3 (`WddmProbe` 0x1300); `off` also removes the boot-time ETW capture |
| `wddm-probe default` | delete `WddmProbe` and the fallback values: back to the 2.0 default |
| `wddm-probe set 2000 [phys\|gpummu] [uart] [nobudget] [nonlocalbudget] [nopowerddi] [norotoffset] [gethandledata]` | a 2.0 probe with the full journal and a boot-time DxgKrnl ETW capture to `C:\mali\probe\` |
| `wddm-probe keep <v> ...` | the same without the one-shot fallback (`WddmProbeKeep` 1), only for a version already seen to start |
| `wddm-probe watch` | the watchdog on a forced 1.3, as a baseline (`Probe1300` key) |
| `wddm-probe check` | after the reboot: writes `C:\mali\probe\probe-<time>.txt` with everything below |

Or by hand:

```
reg add HKLM\SYSTEM\CurrentControlSet\Services\maliwddm\Parameters /v WddmProbe /t REG_DWORD /d 0x1300 /f
```

The `set` flags reproduce the failures of section 16: `nobudget`
(`Wddm2Budget` 0), `nonlocalbudget` (`Wddm2LocalBudget` 0, bugcheck 0x7E),
`nopowerddi` (`Wddm2PowerDdis` 0, start refused), `norotoffset`
(`Wddm2RotOffset` 0, bugcheck 0x113), `gethandledata`
(`Wddm2AcquireHandle` 0, black screen).

Never test with `pnputil /restart-device` or a hot install: the loaded
image is kept and dwm wedges.

## 3. Versions above 2.0

dxgkrnl does not use the version number to pick features, but it checks at
StartDevice that the caps and DDIs a reported version requires are present,
and fails the adapter otherwise (event 549 in the DxgKrnl Admin log names
the reason). On Windows 11 ARM64, `WDDMVersion` 0x2300 or higher makes
dxgkrnl tear down a lost VidPN source instead of pausing it, and 0x2200
hung at LogonUI on a G615 driver. 2.0 is the target.

### 3.3 WDDM 2.1 to 2.9

| Version | Documented mandatory | Relevance here |
|---|---|---|
| 2.1 | UMD callbacks (`pfnReclaimAllocations3Cb`, `pfnSyncTokenCb`), driver-store install, NV12 camera frame sharing | mostly UMD; NV12 sharing may block 2.1 |
| 2.2, 2.3 | none | none |
| 2.4 | none; GPU paravirtualization optional | dxgkrnl asks QAI 26 and 27 (performance caps, GPU version) from 2.4 on and validates node metadata flags from interface 0x9000: [PERFDATA.md](PERFDATA.md) section 4 |
| 2.5 | none documented; possibly a hidden GPU-PV rule | unknown |
| 2.6 | `DxgkDdiCollectDiagnosticInfo` | registered by the driver at 2.6 and above |
| 2.7 | none; HAGS optional | HAGS impossible: Bifrost's job manager has no scheduling processor |
| 2.8, 2.9 | none | none |

GPU-PV (WSL GPU, Sandbox) needs GpuMmu, Hyper-V and, for WSL, a D3D12 UMD.
None is available here.

## 4. GpuMmu on Bifrost (shape 1)

Hardware: `MMU_FEATURES` 0x2823 (35 VA bits, 40 PA bits), 8 address spaces
(`AS_PRESENT` 0xff), 3 job slots. The legacy LPAE format always walks four
9-bit levels; leaves are `pa | 0x2C5` (bit 54 for no-execute), tables
`pa | 3`. Linux uses the legacy format on RK3576; the AArch64 MMU mode is
unproven on this SoC.

Shape 1 (`Wddm2Shape` 1) reports:

- `VirtualAddressingSupported` and `GpuMmuSupported`;
- `GPUMMUCAPS`: 4 levels, 48 bits, CPU-virtual page table updates,
  read-only and no-execute supported, no cache-coherent or zero PTEs;
- `PAGETABLELEVELDESC`: 9 index bits, 4 KB tables, 4 KB alignment (0 makes
  dxgmms2 divide by zero), segment `GpuMmuPtSegment` (default 1);
- `InternalGpuVirtualAddressRangeStart/End` below 2^35, so VidMm's own
  addresses (monitored fences) stay in what the G52 translates;
- CreateProcess, DestroyProcess, SetRootPageTable, GetRootPageTableSize
  and SubmitCommandVirtual. `UPDATE_PAGE_TABLE` is written in the Mali
  format. The tables are not bound to an address space yet.

The design for P2 and later: `SubmitCommandVirtual` takes an AS slot for
the context's process (free, or the least recently used idle one, as Linux
`panfrost_mmu_as_get`), writes `TRANSTAB`/`MEMATTR`/`TRANSCFG`, and puts
the slot in `JS_CONFIG`. With 3 job slots at most 3 address spaces are live,
so 8 are enough. AS0 stays for physical-mode contexts. Paging operations
are recorded at build and run on the CPU at submit, in order with GPU work.
Monitored fences are written by the KMD in the job-done DPC. Primaries stay
in an aperture segment (`AccessedPhysically`) so `scanout.c` keeps getting
their MDL.

## 10. Phases

| Phase | What | State |
|---|---|---|
| P0 | host tests of the GpuMmu pieces | folded into `tests/wddm2-host` |
| P1 | the probe: report 2.0 in physical mode | done; the default |
| P2 | GpuMmu in the KMD with one test client (`m5test virt`); sets `VirtualMode` in the UMD adapter info | not started |
| P3 | isolation, residency and monitored fences | not started |
| P4 | the OpenGL ICD in virtual mode | not started |
| P5 | the D3D11 UMD on `D3DWDDM2_0` (after option c) | not started |
| P6 | primaries in the aperture, scan-out | not started |
| P7 | version number, `CollectDiagnosticInfo`, soak | not started |

## 12. What dxgkrnl requires of a 2.0 driver

Found on hardware by Jamie (`claude-g615`, Mali-G615) and mr4g
(`codex-g610`, Mali-G610 on RK3588) in
[Panorama](https://github.com/awesomefroggy29/Panorama), and confirmed or
added here. Both gave permission on 2026-11-22 to use their work in this
GPL-2.0 driver with credit. Nothing was copied; code their repository
inherits from `WoA-OpenMali/rk3588-GPU` was not used.

1. The paging node is a node of its own, a copy engine, below
   `NumExecutionNodes`. Naming the 3D node fails StartDevice; a node past
   the count makes a phantom node and a later 0xD1.
2. `BuildPagingBuffer` returns only `STATUS_SUCCESS` or
   `STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER`, otherwise bugcheck 0x10E.
   With no room for the marker it returns INSUFFICIENT_DMA_BUFFER before
   doing anything.
3. A GpuMmu driver's paging buffers arrive through `SubmitCommandVirtual`,
   which must not be a stub (0x119).
4. `PageTableAlignmentInBytes` is not 0 (0x7E).
5. `SchedulingCaps.MultiEngineAware` is checked for every WDDMv2 driver.
6. `PHYSICALADAPTERCAPS.DxgkPhysicalAdapterHandle` is dxgkrnl's
   `DeviceHandle` from StartDevice, not the driver's context.
7. QUERYSEGMENT4: the count pass is the one with `NbSegment` 0; the input's
   `PhysicalAdapterIndex` is 0. dxgkrnl 22631 passes the 26100 layout
   (0x28 bytes); QAI 15 is 0x14 bytes. Both are `C_ASSERT`s.
8. QAI 10 (HISTORYBUFFERPRECISION) is filled for every node.
9. At interface 0x5008 or later, `pfnCalibrateGpuClock` and
   `pfnSetStablePowerState` must be registered, or the start is refused
   with STATUS_INVALID_PARAMETER right after QAI 16.
10. At interface 0x4003 or later with test signing on, the primary clone
    path's rotation support needs Offset0 and no other offsets
    (0x113/0x1C otherwise).
11. `DxgkCbGetHandleData` returns NULL on a WDDMv2 adapter; use
    AcquireHandleData.
12. An allocation in a render's allocation list must be made resident
    first (section 16.10).
13. On an integrated adapter the local budget group must not be empty
    (section 16.11).

## 13. What the KMD expects of the UMD

### 13.1 Physical mode (today)

The UMD uses the same D3D10 DDI and physical-mode callbacks as at 1.3:
`pfnAllocateCb` with `MW_ALLOCATION_INFO` and the VA from
`MW_ESCAPE_ALLOC_INFO`, `pfnLockCb`, `pfnRenderCb` with `MW_COMMAND`s and
the allocation list, `pfnPresentCb`. It must not depend on the reported
version. The one 2.x addition is residency (16.10), turned on when
`MW_ESCAPE_WDDM` says `MW_WDDM_NEEDS_RESIDENCY`.

### 13.2 Virtual mode (P4 and P5, not implemented)

1. **Context:** `D3DKMTCreateContextVirtual` or `pfnCreateContextVirtualCb`,
   node 0, private data `MW_VCONTEXT_CREATE { Magic, AbiVersion }`.
2. **Allocations:** as today; the KMD reserves no VA and
   `MW_ESCAPE_ALLOC_INFO` returns `STATUS_NOT_SUPPORTED`.
3. **GPU VAs:** `pfnMapGpuVirtualAddressCb` or `D3DKMTMapGpuVirtualAddress`.
   Keep VAs in 32 MB to 4 GB (Panfrost's range); the hardware limit is
   2^35. Executable allocations must not cross a 16 MB boundary
   (`MK_EXEC_WINDOW`): reserve a 16 MB-aligned window and map them inside
   it. Wait for each map's paging fence before first use.
4. **Residency:** `pfnMakeResidentCb` after creating an allocation, wait on
   its paging fence; evict only on the runtime's trim or under budget.
5. **Submission:** `pfnSubmitCommandCb`/`D3DKMTSubmitCommand` with
   `Commands` = the GPU VA of a 4 KB per-context command allocation the KMD
   never reads. The work goes in private data:
   `MW_VSUBMIT { Magic, Count, MW_COMMAND[Count] }` (`MW_CMD_JOB`, `Slot` 0
   fragment or 1 vertex/tiler/compute, `Jc` the first job header's VA). No
   allocation or patch lists.
6. **Fences:** monitored fences (`pfnCreateSynchronizationObject2Cb`), a
   `pfnSignalSynchronizationObjectFromGpuCb` after each submit, CPU waits
   with `pfnWaitForSynchronizationObjectFromCpuCb`. Per-BO busy tracking
   replaces "wait for the device to go idle".
7. **CPU access:** `pfnLock2Cb`/`D3DKMTLock2`; the UMD waits on its own
   fences before reading GPU output.
8. **Primaries and shared surfaces:** created as today with
   `MW_ALLOC_PRIMARY`, kept in the aperture segment. Each process maps a
   shared allocation into its own VA space.
9. **Unchanged:** `MW_ESCAPE_GPU_INFO`, `MW_ESCAPE_STATS`, the other
   escapes, node 0.

## 15. The rules the host test checks

`make -C tests/wddm2-host run` (CI, Linux, ASan/UBSan) checks, for 1.3 and
2.x in both shapes and every budget setting:

- the paging node is a COPY node below `NumExecutionNodes`; the VPR paging
  node is a real node; one GpuMmu bit everywhere;
- `PageTableAlignmentInBytes` is not 0, VA bits = levels x index bits + 12,
  the internal VA is below 35 bits;
- the physical adapter handle is dxgkrnl's, and `(HANDLE)` of our context
  never comes back (source lint);
- one budget group; 1.3's plan is byte for byte the same whatever the 2.x
  knobs say, with no budget flag;
- the QUERYSEGMENT4 pass table, the 0x5008 power DDI rule, the rotation
  Offset0 rule;
- the fallback over every pattern of eight boots of four kinds (confirmed,
  no frame, TDR storm, cut): never more than two failed or three
  unconfirmed 2.0 boots in a row, never 2.0 again after a fallback with the
  same build, no fallback unless a limit was reached, the probe's one-shot,
  the stage steps and the confirm thresholds.

## 16. Collecting evidence, and the failures found

### 16.1 Installing a package for a WDDM test

Stage the package without hot-installing it:
`pnputil /add-driver C:\mali\pkg\maliwddm.inf`. Then, as SYSTEM, set
`ConfigFlags |= 0x20` (CONFIGFLAG_REINSTALL) on
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0`, wait until the value
has 0x20, and do a clean restart. For a baseline, `wddm-probe off`, reboot,
`wddm-probe check`: `WddmVersion 0x1300`, dxdiag WDDM 1.3.

### 16.2 Running a probe

1. `wddm-probe set 2000 phys uart`. If a serial logger is on UART0
   (1500000 8n1), start it before the reboot.
2. Clean restart.
3. If it hangs at the spinner, wait at least 3 minutes so the watchdog and
   the journal keep writing, then power off and on. The next boot is 1.3.
4. On whichever boot comes up, `wddm-probe check`, and keep
   `C:\mali\probe\` and the serial log.

What the output answers:

- **Accepted?** `WddmProbeConfirmed` against `WddmProbeFellBack`, the
  device status, and the DxgKrnl Admin log (event 549 names an
  adapter-start failure and its reason).
- **Where it stopped:** the journal (16.3 below for where it is). The last
  `> <DDI>` line without a matching `<`, any `watchdog ... still in` line,
  and the `QAI 15 ... handle` line.
- **The watchdog key** `Device Parameters\maliwddm\Probe<version>`, written
  every second for 5 minutes, then every 10 s or on change: `Wd_Seconds`,
  `Wd_Last` (last DDI entered), `Wd_Stuck`/`Wd_WorstStuck` (anything in
  progress over 2 s), `Cnt_<ddi>`, `Seq_<ddi>`, `Fails_<ddi>`,
  `LastFail_<ddi>`, `Pg_<op>`/`PgRefused_<op>` per paging operation,
  `QI_RefusedData1`, `AdapterLuidLow/High`, and in shape 1 `Wd_Processes`,
  `Wd_RootSets`, `Wd_PteWrites`, `Wd_VirtSubmits3d`. DDIs called above
  PASSIVE_LEVEL (CalibrateGpuClock, SubmitCommand, the ISR) appear only
  here.
- **The ETW capture:** `C:\mali\probe\etl-<time>\boot.etl.*`, one file per
  boot, flushed every second. Decode on Linux with
  `tools/etw/dxgk-decode.py` and an `events.json` made by
  `make-events-json.py` for the board's dxgkrnl.
- **If it came up:** dxdiag "Driver Model", `maliumd.dll` in `dwm.exe`, the
  `GPU Engine` counters by LUID (they exist at 1.3 too, so compare the
  LUID and whether 3D use moves), and Task Manager's Performance tab.

`ProbeUart 1` prints one watchdog line a second on UART0 (`ProbeUartBase`,
default 0x2AD40000). It needs neither the registry nor the file system.
Do not use it together with the kernel debugger on the same UART.

### 16.3 The boot journal, and reading it without Windows

`Services\maliwddm\Probe\Boot<n % 8>` (n = `Parameters\ProbeBoots`),
from DriverEntry on, only while 2.x is in effect:

- `E0001`, `E0002`, ...: `<ms since DriverEntry> <event>`. `> <DDI> #<call>
  seq <n>` is written and flushed before the DDI runs, `< <DDI> <status>`
  after it returns. QAIs 1, 10, 11, 13, 14, 15 and 16 are logged field by
  field. Also StartDevice's handle, CreateDevice and CreateContext flags,
  the first 64 BuildPagingBuffer operations, the first 8 unresolved
  OpenAllocation handles, RemoveDevice and the stop DDIs, the watchdog's
  "still in" lines, `confirmed`, and a once-a-second copy of the ISR, DPC
  and DMA-completed/preempted/vsync ring (first 32 of each).
- `Last` is the newest event, `Count` the number, `Probe\LastBootKey`
  names the newest boot's subkey.
- Limits: only at PASSIVE_LEVEL; the first 64 calls of each DDI plus every
  failure; 400 events on a default boot and none after the confirm, 1500
  on a probe. `ProbeJournalMax` can lower either, never above 1500.

If Windows does not come up at all, the journal is in the SYSTEM hive:

- **From TSPI-RESCUE** (the second Windows on the eMMC):

  ```
  reg load HKLM\OFF C:\Windows\System32\config\SYSTEM
  reg query HKLM\OFF\ControlSet001\Services\maliwddm\Probe /s
  ```

  with the main Windows' drive letter. To clear a probe from there:
  `reg add HKLM\OFF\ControlSet001\Services\maliwddm\Parameters /v WddmProbe
  /t REG_DWORD /d 0 /f`, then `reg unload HKLM\OFF`.

- **From Linux** (read only; do not write the hive from Linux):

  ```
  sudo mount -t ntfs3 -o ro,force /dev/mmcblk0pN /mnt/win
  mkdir ~/hive && cp /mnt/win/Windows/System32/config/SYSTEM{,.LOG1,.LOG2} ~/hive/
  sudo umount /mnt/win
  python3 -m venv ~/rv && ~/rv/bin/pip install regipy
  ~/rv/bin/regipy-process-transaction-logs ~/hive/SYSTEM -p ~/hive/SYSTEM.LOG1 \
      -s ~/hive/SYSTEM.LOG2 -o ~/hive/SYSTEM.clean
  hivexregedit --export ~/hive/SYSTEM.clean '\ControlSet001\Services\maliwddm\Probe'
  ```

  Replay the transaction logs: the newest flushed lines are in `SYSTEM.LOG1`
  and `LOG2`, and the main file lags. `hivexregedit` is in
  `libwin-hivex-perl`. `Select\Current` says whether the set is
  `ControlSet001`.

### 16.4 Back-out

- To 1.3 with the same package: `wddm-probe off`, clean restart.
- To an older package: find its `oem<N>.inf` with `pnputil /enum-drivers`
  (stage it again if gone), `pnputil /delete-driver oem<M>.inf` for the
  newer package without `/uninstall` (the higher-ranked package wins), then
  `ConfigFlags |= 0x20` as in 16.1 and a clean restart.
- `wddm-probe off` removes the boot-time capture. The `Services\maliwddm\Probe`
  key can be deleted once read.

### 16.6 Start refused after QAI 16, and the teardown bugcheck

Symptom: bugcheck 0x139 P1 3 (LIST_ENTRY corruption) a second after the
journal's last line, `< QueryAdapterInfo` for QAI 16.

Cause, read from dxgkrnl 10.0.22621.2428: at DDI interface 0x5008 or
later, `DXGADAPTER::Initialize` refuses an adapter without
`pfnCalibrateGpuClock` or `pfnSetStablePowerState`
(STATUS_INVALID_PARAMETER). The refusal led to
`StopDeviceAndReleasePostDisplayOwnership` and RemoveDevice, which freed
the adapter while the HPD and copy threads still waited on events inside
it: the 0x139 came from a timer on a freed wait block.

Fix: both DDIs registered at every 2.x interface; the stop DDI does all of
StopDevice's work, and RemoveDevice without a stop first stops the copy and
HPD threads and the backlight timer. `Wddm2PowerDdis` 0 reproduces the
refusal, which now ends in a clean stop, code 43 and Basic Display.

Tools: `tools/re/dxgk-annotate.py --winbindex dxgkrnl.sys <version>`
fetches a dxgkrnl build and its public symbols and disassembles it. To find
where a 0x113 subcode is raised, search its `dis.txt` for
`mov edx, 0x113` with `r8d` set to the subcode.

If a stack is needed, use the kernel debugger over UART0 with
`scripts/kd-listen.py` from edk2-rk3576, which saves the register context
and 48 KB of stack on a fatal error:

1. On the host, keep one reader on the debug probe:
   `scripts/kd-resilient.sh <raw-file> <log-file>` (1500000 baud), then
   stop the console logger.
2. On the board: `bcdedit /dbgsettings serial debugport:1 baudrate:1500000`,
   `bcdedit /debug on`. UART0 (`0x2AD40000`) is the DBG2 port.
3. Set the probe without `uart`.
4. Restart with the listener already running; without it the boot crawls.
5. Afterwards `bcdedit /debug off`. Copy `dxgkrnl.sys`, `dxgmms2.sys` and
   `ntoskrnl.exe` and name the return addresses with
   `tools/re/dxgk-annotate.py <dir> --local <file>`.

KD changes boot timing; on the CM5-IO it starved USB enumeration.

### 16.7 Bugcheck 0x113, subcode 0x1C

Raised in `DMMVIDPNPRESENTPATH::SetRotationSupport` when the driver
supports path-independent rotation (interface 0x4003 or later) and test
signing is on. The primary clone path needs Offset0 and no Offset90, 180 or
270 (P3 1 or 2 if broken); any other path needs at least one Offset bit
(P3 0). P2 is the bits reported. Fixed by adding Offset0 (0x1F);
`Wddm2RotOffset` 0 reproduces it.

### 16.8 Black screen: presents skipped, UMD on softpipe

Symptom at 2.0: `Escape(ALLOC_INFO): 0xc0000008`, Panfrost fails
`CREATE_BO`, every UMD runs on softpipe, presents counted as skipped.
Cause: `DxgkCbGetHandleData` returns NULL on a WDDMv2 adapter. Fixed with
AcquireHandleData; `Wddm2AcquireHandle` 0 reproduces it.

### 16.9 D3DKMTRender fails with 0xc0000001

Symptom: HDMI has a signal, `renders 0`, no `Cnt_Render`, m5test's render
fails in D3DKMTRender with 0xc0000001, the UMDs fall back to CPU paths.
`m5test renderprobe` runs D3DKMTRender calls that change one thing each
without a reboot:

| | variant |
|---|---|
| A | node 0, one allocation, locked before |
| B | node 0, no allocation list |
| C | one allocation, not locked before |
| D | one allocation, NullRendering |
| E | no allocations, one command the driver refuses (0xc000000d means DxgkDdiRender was reached) |
| F | node 1, the copy node |
| G | one allocation, a command the driver refuses (0xc0000001 expected at 2.0) |
| H | the same, made resident first (0xc000000d expected) |

A to D and F have no command, so dxgkrnl completes them without calling the
driver. Cause: section 16.10.

### 16.10 Residency, the 2.x rule

dxgmms2's `VIDMM_GLOBAL::ReferenceDmaBuffer` refuses a DMA buffer with a
command whose allocation list names an allocation that was not requested to
be resident ("Failed to reference DMA buffer: Allocation is not requested to
be resident", 0xc0000001), unless it is pinned or the adapter is in IoMmu
mode. WDDM 1.x paged in whatever the list named; 2.x needs MakeResident on
the rendering device first.

The fix (`win32-compat/malikm_drm.c`): when the KMD reports
`MW_WDDM_NEEDS_RESIDENCY`, every buffer the shim creates or opens is made
resident on the current device through the runtime's
`pfnCreatePagingQueueCb`/`pfnMakeResidentCb`, or the D3DKMT calls when not
bound to a runtime device. One paging queue per device, the paging fence is
waited for on the CPU, nothing is evicted. The UMD log
(`C:\mali\log\<process>-*.txt`; `mkdir C:\mali\log`, stop with
`echo > C:\mali\log\off`) has `residency: ...` lines: whether the escape
saw 2.x, the paging queue, and any MakeResident that failed. Plain `m5test`
prints `MakeResident: 0x00000000` and `PASS: the Mali ran a job submitted
through WDDM`.

### 16.11 Bugcheck 0x7E, divide by zero in dxgmms2

Symptom: GPU work runs at 2.0, then 0x7E with P1 0xC0000094 in dxgmms2 at
`VIDMM_COMMIT_TELEMETRY::LogMemoryState+0x160`. That function divides usage
by the local segment group's budget, and on an integrated adapter only the
local group. With the segment in the non-local budget group the local
budget was 0. It is reached from paging telemetry (long paging packet,
defrag pass, commit failures), so short boots do not hit it.

Fix: at 2.x the segment is in the local budget group (flags 0x88005). The
journal's plan line shows `local 1 (flags 0x88005)`. `Wddm2LocalBudget` 0
reproduces the bugcheck; `Wddm2Budget` 0 drops both groups. If VidMm ever
refuses an aperture in the local group, the next step is a second, memory
segment with `PopulatedFromSystemMemory` in the local group, as Intel and
AMD integrated drivers do.

### 16.12 The first 2.0 desktop: what was seen

2026-11-24, TaishanPi 3M, HDMI 2560x1440: LogonUI and the desktop,
`renders` above 12000 at 88 s, `m5test` PASS, Task Manager shows the GPU
with 1.0 GB, dxdiag Driver Model WDDM 2.0, feature levels 10_1 to 9_1. The
same package at 1.3 also boots. A 15 s window drag (n=1 per mode, not a
result): total CPU about 20% at 2.0 against 26% at 1.3. In both modes dwm
does thousands of GPU copies of about 1 ms each, a UMD cost.

Not yet tested at 2.0: a drag with n of at least 3 per mode, a long
session, suspend and resume, HDMI hot-plug, a forced TDR, 10 cold boots.

### 16.13 Checking the default, and going back to 1.3

On a board with no values set:

1. `wddm-probe default`, reboot. The desktop, dxdiag WDDM 2.0,
   `Device Parameters\maliwddm\WddmVersion` 0x2000, `WddmProbeResult` 1.
2. About 20 s after the desktop: `Parameters\WddmProbeConfirmed` 0x2000,
   `WddmProbeArmed` 0. `m5test wddm` prints the stage, the counts, the
   class of the last miss, and any fallback with its build, boot and
   reason.
3. `maliwddm-counters check`: `\RK3576 GPU(gpu0)\WDDM version` 20 (13 at
   1.3) and `WDDM fallback reason` 0.
4. `wddm-probe off`, reboot: WDDM 1.3 and the desktop.

At 0x1300 the driver is the 1.3 driver: one node, segment flags 0x5, the
1.3 DDI table, no arming, no journal, and no watchdog unless
`ProbeWatchdog` 1.

### 16.14 Telling a failure from a power cut

The fallback rules are in section 2. To check them on a board:

1. Install the package. `m5test wddm`: stage 3 or more within 20 s of the
   desktop, armed 0 and confirmed 0x2000 after about 20 s.
2. Cut the power at the desktop within 20 s of LogonUI and boot again: 2.0,
   `m5test wddm` shows one cut and the last miss "frames, then no
   confirm". Boot normally: the counts return to 0.
3. `maliwddm-counters check`: `WDDM version` 20, `WDDM fallback reason` 0.

Known issue at 2.0 outside this driver's start path: browser redirection
froze the session, so at 2.x no process gets it by default (`UmdRedirect`,
[EDGE-PRESENT.md](EDGE-PRESENT.md), "The WDDM 2.0 session hang").
