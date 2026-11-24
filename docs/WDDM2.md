# WDDM 2.x and 3.x for maliwddm: design study

Written 2026-11-21 from the source of this repository (branch `next` at
`2bb2b52`), the Linux panfrost driver, Microsoft's compute-only sample, the
26100 WDK headers and Microsoft's documentation. Nothing in it was measured on
a board. It builds on [FEASIBILITY-D3D11-WDDM2.md](FEASIBILITY-D3D11-WDDM2.md)
(2026-11-13) and does not repeat that report's D3D11 feature-level analysis;
where the two disagree, this one says so and why.

> Status, 2026-11-21: the plan was approved the same day, going straight to
> WDDM 2.x with GpuMmu (the user: "I want to see this driver in Task
> Manager"). Implementation is on branch `wddm2`; section 12 is the progress
> log and section 13 is what the KMD expects of the UMD at each phase.
> Ownership: the KMD side (VA model, page tables, paging and residency,
> monitored fences, contexts, version reporting, Task Manager) is on
> `wddm2`; the UMD (D3D11.1, the `D3DWDDM2_0` interface, the video DDI) is
> on the present-path branch. P5 and P6 wait until `present-fast` is in
> `next`.

> Status, 2026-11-22 (branch `wddm2x`, on `hpd-edge`): the probe was ported
> onto the board line and its answers to QAI 11 and 15 corrected. Section 15
> is the decision and the fixes, section 16 the one-shot board test and how
> to read a hang. Without `WddmProbe` the driver is WDDM 1.3 as before.

> Status, 2026-11-23: two 2.0 boots bugchecked (0x139) after QAI 16. dxgkrnl's
> own code says why it refused the start: two DDIs every 2.x interface must
> fill in. The bugcheck was most likely our teardown after the refusal.
> Both are fixed; section 16.6 has the evidence and the next two boots.

The study below is kept as written.

Line numbers below are those of `next`. On `main` and `umd-file-log` the
same two lines are `adapter.c:263` (`caps->WDDMVersion = DXGKDDI_WDDMv1_3`)
and `adapter.c:574` (`init.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3`); on
`next` they are `:393` and `:816`.

## 1. Short answer

**Recommendation: (c) first, then (b). Do not do (a).**

1. Stay on WDDM 1.3 and build the D3D11.1 UMD with the video DDI (option c).
   It is needed for Edge video under any WDDM version. It is also the
   prerequisite of WDDM 2.x: a UMD on a WDDM 2.x virtual-addressing adapter
   very probably has to offer the `D3DWDDM2_0` D3D11 DDI (section 6.1), and
   that interface is an extension of the D3D11.1/`D3DWDDM1_3` tables.
2. Early and cheaply, run the probe ladder P1 (section 10): the same
   driver, reporting WDDM 2.0 and higher in physical mode. It answers whether
   dxgkrnl accepts this KMD as 2.x, whether today's D3D10-DDI UMD still
   loads, and whether the Task Manager GPU page appears. It is about one
   person-week (pw) and needs no new design.
3. Then WDDM 2.x with GpuMmu on the Mali MMU (option b). It removes the
   per-submit "list every live BO" workaround and the system-wide 4 GB GPU VA
   and 1 GB residency ceilings, and gives per-process GPU isolation. That
   last one matters for a browser: today any process can read any other
   process's GPU memory by address.
4. Do not chase WDDM 3.x. On Windows 11 23H2 the ceiling is 3.1. Every 3.0
   and 3.1 feature is optional and none is usable on this board (section 9).
   Report 3.x only if the probe ladder shows dxgkrnl accepts it for free.

**Option (a), the IoMmu model, is not available on this SoC.** It needs a
system IOMMU with PASIDs walking the CPU's page tables. The RK3576 has no
SMMU; the GPU node has no `iommus` property; the Mali has its own MMU
(section 4.4).

**Effort.** All figures are estimates for one person with the CI and board rig
as they are now, in person-weeks:

| Option | What | Estimate | Risk |
|---|---|---|---|
| (a) | IoMmu model | not possible | n/a |
| (a') | WDDM 2.0 in physical mode only (the probe, kept) | 1 to 3 | low; small gain |
| (b) | WDDM 2.x, GpuMmu on the Mali MMU | 16 to 26 (P2 to P7), after (c) for the D3D UMD | medium to high |
| (c) | WDDM 1.3 plus a D3D11.1 UMD with the video DDI | 12 to 24, excluding the VDPU383 decoder | medium |
| D3D12 | a D3D12 UMD for Mali | 50 to 100 | very high; not recommended |
| 3.x | WDDM 3.0/3.1 on top of a working 2.x | 1 to 3 to probe; more only for features this board cannot use | low; no gain found |

**Top risks.**

1. The D3D runtime may refuse a D3D10-DDI UMD on a WDDM 2.x adapter, or
   accept it only in physical mode. If so, (b) waits on (c). The docs do not
   say. P1 measures it.
2. The Mali's 35-bit VA space against VidMm's page-table rules: a 3-level
   description whose root uses 32 of its 512 entries, plus a private
   level-0 page per address space. The docs neither allow nor forbid a root
   that uses only part of its 4 KB (section 4.2). P2's first run settles it.
3. Paging operations done on the CPU at build time can race with GPU work
   still using the old mapping. The compute-only sample does it, but it has no
   real GPU (section 5.2).
4. The present and scan-out path (`scanout.c`, the KMD blt) depends on the
   MDL that `MAP_APERTURE_SEGMENT` hands over. Under GpuMmu, ordinary
   allocations stop coming that way. Primaries need an aperture segment with
   `AccessedPhysically`. This collides with the present-path work on
   `present-fast` unless it is sequenced (section 11).

## 2. What is already settled, and what this study adds

The 2026-11-13 report established:

- GpuMmu fits Mali's MMU in general.
- WDDM 2.0 residency matches Panfrost's map-at-creation model.
- WDDM 2.x does not fix the slowness measured then; the CPU copies did, in
  PERF.md stages 1 and 2.
- D3D11 does not need WDDM 2.0.

This study adds:

- the exact GpuMmu page-table rules, read from the docs and the 26100
  headers, and a concrete mapping onto Bifrost's legacy LPAE format and its
  35 VA bits (section 4). The 2026-11-13 report and M5-PLAN suggested 39 bits;
  this study corrects that;
- the address-space-slot design, and what happens past 8 processes;
- the mandatory sets per WDDM version, as far as Microsoft documents them,
  and the 3.x section asked for on 2026-11-21;
- HAGS, GPU-PV, user-mode submission and D3D12 against this hardware;
- Chromium and Edge, read from Chromium and Dawn sources;
- the coupling with the D3D11.1 work, which reverses the earlier "W0 then W1
  when convenient" ordering;
- a phased plan with a board measurement per step.

## 3. What each WDDM version requires

### 3.1 How dxgkrnl decides

From [WDDM 2.1 features](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-2-1-features):

> Dxgkrnl doesn't use the WDDMVersion cap as a way to determine which
> features are supported ... However, if the driver reports WDDM 2.1 support
> through the WDDMVersion cap, Dxgkrnl does validate that the caps or DDIs
> required by WDDM 2.1 are present and fail to create the adapter if they
> aren't.

So a version number is a promise that dxgkrnl checks at StartDevice, as it
checked `SupportPerEngineTDR` in WinPE run 10 ("Driver reports
SupportPerEngineTDR cap but does not fill in all of the required DDIs"). The
ETW text of event 494 is the oracle, as it was then. Microsoft does not
publish the full validation list per version; the documented items follow.

### 3.2 WDDM 2.0 (Windows 10 1507)

Sources: [GPU virtual memory](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0),
[driver residency](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/driver-residency-in-wddm-2-0),
[GpuMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model).

- **Engines run in physical or virtual mode.** "In physical mode, the
  scheduling model remains the same as it is with WDDM v1.x. The UMD
  continues to generate the allocation and patch location lists." Virtual
  mode: no lists, GPU VAs, `pfnSubmitCommandCb`. "A single GPU node can
  support both modes simultaneously."
- **Opt-in caps.** `DXGK_VIDMMCAPS::VirtualAddressingSupported` with
  `GpuMmuSupported` or `IoMmuSupported` (WDK `d3dkmddi.h:2269-2271`),
  repeated per physical adapter in `DXGK_PHYSICALADAPTERFLAGS`
  (`QUERYPHYSICALADAPTERCAPS`).
- **GpuMmu KMD DDIs.** `DxgkDdiCreateProcess`, `DxgkDdiDestroyProcess`,
  `DxgkDdiSetRootPageTable`, `DxgkDdiGetRootPageTableSize` (only for a
  2-level description), `DxgkDdiSubmitCommandVirtual`; QAI types
  `GPUMMUCAPS`, `PAGETABLELEVELDESC`, `PHYSICALADAPTERCAPS`, `QUERYSEGMENT4`;
  paging operations `UPDATE_PAGE_TABLE`, `FLUSH_TLB`,
  `COPY_PAGE_TABLE_ENTRIES`, `NOTIFY_RESIDENCY`, `UPDATE_CONTEXT_ALLOCATION`.
  The compute-only sample wires exactly the first five
  (`CosKmdGlobal.cpp:323-327`).
- **Residency is per device, not per DMA buffer.** "VidMm ensures that all
  allocations on a particular device residency requirement list are resident
  before any contexts belonging to that device are scheduled." The UMD calls
  `pfnMakeResidentCb` and `pfnEvictCb` and waits on a paging fence. The UMD
  also takes over allocation usage tracking and renaming synchronisation,
  and works to a per-process budget.
- **Monitored fences.** A fence object with a CPU VA and a GPU VA. The GPU
  writes the value and the KMD raises `DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED`.
- **New VidSch caps** (`d3dkmddi.h:2011-2012`): `No64BitAtomics` and
  `LowIrqlPreemptCommand`.
- **D3D12 needs it.** "The WDDM 2.0 driver model is a pre-requisite for
  D3D12" ([prior 2.x versions](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-prior-wddm-2-x-versions)).
  The reverse does not hold: WDDM 2.0 does not require a D3D12 UMD. Every
  D3D version from D3D9 to D3D12 is listed as supported.

Whether a driver may report 2.0 and leave `VirtualAddressingSupported` off is
not stated. The paragraph above allows physical-mode engines, and P1 tests
that combination.

### 3.3 WDDM 2.1 to 2.9 (Windows 10 1607 to 21H1 and Server 2022)

From the [prior 2.x versions page](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-prior-wddm-2-x-versions),
the [2.1 page](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-2-1-features)
and the 26100 headers. "Mandatory" means the page says so; "optional" means it
is opt-in by its own cap.

| Version | Documented mandatory | Optional additions that touch this driver | Relevance here |
|---|---|---|---|
| 2.1 | offer/reclaim improvements (`pfnReclaimAllocations3Cb`, UMD); present batching for GameDVR (`pfnSyncTokenCb`, UMD); driver-store install (the INF already copies into the store; `INSTALL.txt`); camera frame sharing (NV12 shared textures) | 64 KB pages, 2 MB PTEs, `AllowNonAlignedLargePageAddress`, `SysMem64KBPageSupported` | mostly UMD-side. Frame-server sharing wants NV12, which d3d10umd does not offer; that may block 2.1 |
| 2.2 | none documented | MPO3 DDIs, `ParavirtualizationSupported` (header field) | none |
| 2.3 | none documented | `HwQueuePacketCap` | none |
| 2.4 | none documented | GPU paravirtualization (needs GpuMmu and Hyper-V), `BeginExclusiveAccess`/`EndExclusiveAccess` for IOMMU domain switches | GPU-PV not usable (section 9.3) |
| 2.5 | none documented; a DirectX blog says "all WDDMv2.5 or later drivers have native support for GPU virtualization", which reads like a certification rule | display synchronization, `TypeVideoOutput` children with `DxgkDdiUpdateMonitorLinkInfo` | possibly a hidden PV requirement; P1 will show |
| 2.6 | `DxgkDdiCollectDiagnosticInfo` ("This new feature is a requirement in WDDM 2.6") | Super Wet Ink, VRS (D3D12), background processing (D3D12), driver hot update | the one cheap mandatory item: a diagnostics blob |
| 2.7 | none documented | hardware-accelerated GPU scheduling, `VirtualCopyEngineSupported` | HAGS impossible (section 5.5) |
| 2.8 | none documented | none relevant found | none |
| 2.9 | none documented | WSL GPU through GPU-PV, `MapAperture2Supported` (the ADL form of map-aperture), `SysMemLargePageSupported`, cross-adapter scan-out | none |

Reading: past 2.0, the documented mandatory items are few and small. They are
2.1's UMD callbacks and 2.6's diagnostics DDI. Undocumented validation is the
unknown. The method is to raise the number one step at a time and read
event 494.

### 3.4 What a 2.x driver must have, decided by the hardware

These are not version rules. Any 2.x KMD here needs them.

| Item | Today (1.3) | Under 2.x |
|---|---|---|
| Preemption caps | reported; soft-stop preemption behind `TimeSlice` (`adapter.c:431-434`, `command.c:15-40`) | unchanged. Graphics `PRIMITIVE_BOUNDARY` is what soft-stop gives for fragment chains |
| Per-engine TDR | `ResetEngine`, `QueryEngineStatus` and `QueryDependentEngineGroup` exist; the cap is only set in the render-only build | set it in the full build; dxgkrnl checks the DDIs (run 10) |
| `HistoryBufferPrecision`, `64BITONLYCAPS` | answered | unchanged |
| Node metadata | "Mali-G52 job manager" | add `GpuMmuSupported` to the node metadata, as the sample does (`CosKmdAdapter.cpp:1602`) |

## 4. GPU addressing: today, and under GpuMmu

### 4.1 Today

Read from source on `next`:

- **One address space for the whole machine.** `AsEnable` programs only AS0
  (`maligpu.c:115-128`), and `MkJobStart` puts AS0 in every `JS_CONFIG`
  (`maligpu.c:360`).
- **Legacy Mali LPAE format.** `TRANSCFG = 0` (`ADRMODE_LEGACY`), `TRANSTAB =
  L0 | 0x7`, `MEMATTR = 0x00888d88` (`maligpu.c:121-126`). Leaves are `pa |
  0x2C5`, plus bit 54 for no-execute; tables are `pa | 3` (`maligpu.h:143-145`).
- **One VA bitmap, 32 MB to 4 GB, shared by every process** (`MK_VA_START`,
  `MK_VA_END`, `maligpu.h:137-139`). `NewAllocation` reserves an allocation's
  VA at creation (`memory.c:209`), and the address never changes.
- **One aperture segment of 1 GB** (`MW_SEGMENT_SIZE`, `maliwddm.h:22`) with
  `CommitLimit` 1 GB. VidMm's `MAP_APERTURE_SEGMENT` hands over an MDL and
  the KMD writes the Mali PTEs on the CPU inside `BuildPagingBuffer`
  (`memory.c:391-430`).
- **Residency by allocation list.** WDDM 1.x pages in only what a DMA buffer
  lists, so `Render` patches every allocation (`command.c:112-150`). The shim
  lists every live BO of the device on every submit (`malikm_drm.c`, the
  2026-10-17 fix), up to 256 (`MaxAllocationListSlotId`).

Consequences that matter now:

- The total GPU address space of all processes together is about 4 GB.
- At most 1 GB is resident at once, on a board with 4 or 8 GB.
- Any process can read or write any other process's mapped allocations: DWM's
  primaries, a browser tab's textures. Today that is acceptable on a test
  board. It is not acceptable for a browser that sandboxes its GPU process.

### 4.2 The GpuMmu rules

From the 26100 `d3dkmddi.h`, [DXGK_GPUMMUCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_gpummucaps)
and [DXGK_PAGE_TABLE_LEVEL_DESC](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_page_table_level_desc):

- `PageTableLevelCount` is at least 2. "When PageTableLevelCount is greater
  than 2, all page table levels have a fixed size, which is described through
  DXGK_PAGE_TABLE_LEVEL_DESC::PageTableSizeInBytes." Only with exactly 2
  levels is the root resizable through `GetRootPageTableSize`.
- `PageTableSizeInBytes` "must be a multiple of the CPU page size". When the
  segment is system memory, "the page table size cannot be more than 4 KB".
  So with more than 2 levels in system memory every table is exactly 4 KB.
- "The number of entries in every page table is
  2^PageTableIndexBitCount", and VidMm "sets up the page table entries,
  assuming that each entry covers a 4 KB page" at the leaf.
- PTEs are handed over as `DXGK_PTE` (`d3dukmdt.h:395-420`): Valid, Zero,
  CacheCoherent, ReadOnly, NoExecute, Segment, LargePage,
  PhysicalAdapterIndex, PageTablePageSize, and a page address. With
  `DXGK_PAGETABLEUPDATE_CPU_VIRTUAL` the driver writes its own hardware
  format at a CPU address VidMm gives it (`UpdatePageTable.PageTableAddress.CpuVirtual`).
- `PageTableUpdateRequireAddressSpaceIdle`: when set, VidMm suspends every
  context sharing the address space for each update and TLB flush.
- `VirtualAddressBitCount`: "the number of bits in the GPU virtual address".
  VidMm picks some addresses itself. The UMD may let it pick, within bounds
  or not ([GpuMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)),
  and monitored fence GPU VAs are VidMm's. So the bit count must be the one
  the hardware really translates.
- Paging: the compute-only sample does `UPDATE_PAGE_TABLE` as a CPU copy of
  `DXGK_PTE` at build time and ignores `FLUSH_TLB` (`CosKmdAdapter.cpp:859-886`).
  It describes 2 levels of 10 bits with 16-byte PTEs (`CosKmdAdapter.cpp:1486-1530`).
  It is a WDDM 2.6 driver (`:571`) for an emulated engine.

### 4.3 How Bifrost maps onto it

The hardware facts:

- `MMU_FEATURES` = `0x2823` on this board (project dmesg): 35 VA bits (low
  byte `0x23`), 40 PA bits (`0x28`). `AS_PRESENT` = `0xff`: 8 address spaces.
  `JS_PRESENT` = `0x7`: 3 job slots.
- The legacy format always walks four 9-bit levels, whatever the VA width.
  Linux: "Mali seems to need a full 4-level table regardless of IAS"
  (`drivers/iommu/io-pgtable-arm.c:1232`, linux-next-v8). The legacy L3 leaf
  descriptor is type `0b01`, not `0b11` (`io-pgtable-arm.c:172`). That is why
  our leaves end in `...5`.
- The G52 also has `HW_FEATURE_AARCH64_MMU` (`panfrost_features.h:73-82`).
  Linux uses that mode (`TRANSCFG = ADRMODE_AARCH64_4K | INA_BITS(55 - ias)`,
  `panfrost_mmu.c:235-238`) only on MediaTek Valhall parts with
  `GPU_QUIRK_FORCE_AARCH64_PGTABLE` (`panfrost_drv.c`, mt8188, mt8192 and mt8370).
  RK3576 uses the legacy format in Linux, so the AArch64 mode is unproven on
  this SoC.
- Leaf and block sizes: 4 KB and 2 MB in Linux (`pgsize_bitmap = SZ_4K |
  SZ_2M`, `panfrost_mmu.c:812`).

**Proposed description (plan B, the legacy format):**

| VidMm level | Mali level | `PageTableIndexBitCount` | `PageTableSizeInBytes` | Covers |
|---|---|---|---|---|
| 0 (root) | L1 | 5 | 4096 (32 of 512 entries used) | VA bits 34:30 |
| 1 | L2 | 9 | 4096 | VA bits 29:21 |
| 2 (leaf) | L3 | 9 | 4096 | VA bits 20:12 |

The settings around it:

- `PageTableLevelCount = 3`, `VirtualAddressBitCount = 35`.
- `PageTableUpdateMode = CPU_VIRTUAL`, as the sample and as malikm do today.
- Page tables in system memory (segment 0), which the G52 reaches like any
  other memory (`SELECTED_COHERENCY` none, so the PTE writes need the same
  cache maintenance as today's `MkMmuMapPages`).
- A private level-0 page per process. The hardware needs an L0 above
  VidMm's root. The KMD keeps one 4 KB page per process whose entry 0 is
  `root | 3`; `SetRootPageTable` rewrites that one entry, and `TRANSTAB`
  points at the private page. This is M5-PLAN's idea, narrowed from 39 to
  35 bits so that VidMm can never place an address the hardware does not
  translate.
- PTE translation from `DXGK_PTE`:
  - Valid gives a leaf of `pa | 0x2C5`, or a table of `pa | 3`.
  - ReadOnly clears the write bit `0x80`.
  - NoExecute sets bit 54.
  - CacheCoherent is not supported: report `CacheCoherentMemorySupported = 0`.
  - Zero is not supported: report `ZeroInPteSupported = 0`.
  - LargePage at level 1 gives a 2 MB block (`pa | 0x2C5` at L2). Report
    `LargePageSupported = 0` at first. Root-level large pages would be 1 GB
    blocks, which Linux does not use.
- `ReadOnlyMemorySupported = 1`, `NoExecuteMemorySupported = 1`.
  `PageTableUpdateRequireAddressSpaceIdle = 0`, because the AS
  `LOCK` + `FLUSH_PT` sequence works on a live address space
  (`maligpu.c:287-291`, `panfrost_mmu.c:133-162`). Set it to 1 if the first
  board runs show stale translations; it costs throughput, not correctness.
- `DualPteSupported = 0` (no 64 KB leaf tables in the legacy format), and
  `LeafPageTableSizeFor64KPagesInBytes = 0`.

Open question MMU-1: does VidMm accept a root whose `PageTableIndexBitCount`
(5) implies 256 bytes of entries in a 4096-byte table? The docs neither allow
nor forbid it. The first P2 run answers it. If refused, plan B' describes
VidMm level 0 as 9 bits (39-bit VA) and relies on the UMD's bounds. That
leaves VidMm-chosen VAs (fences) above 2^35 able to fault, so B' is a
fallback with a known hole, not an alternative.

**Plan C (the AArch64 4K format):** `INA_BITS = 55 - 35 = 20`. The hardware
then starts its walk at level 1 with a 32-entry root, exactly VidMm's three
levels, and needs no private L0 page. The leaf, attribute and `MEMATTR`
encodings differ (AArch64 descriptor types, `AS_MEMATTR_AARCH64_*`,
`panfrost_mmu.c:45-63`). Prove it first with an M1-style raw test on Linux
(`tools/m1`) before it goes near Windows. Worth it only if plan B's private
L0 page causes trouble.

### 4.4 Address spaces: 8 slots against unlimited processes

Windows gives every process its own GPU VA space and root. The Mali has 8 AS
slots. This is the problem Linux panfrost already solves:
`panfrost_mmu_as_get` gives each file an AS, and when all 8 are taken it
reuses the least recently used idle one (`panfrost_mmu.c:262-327`).

Design:

- `CreateProcess` allocates the private L0 page. `SetRootPageTable` records
  the root and is documented to be called with the context idle.
- `SubmitCommandVirtual` acquires a slot for the context's process. It hits
  if bound; otherwise it takes a free slot or steals an idle one LRU. It
  writes `TRANSTAB`/`MEMATTR`/`TRANSCFG` and `AS_COMMAND_UPDATE`, then puts
  the slot number in `JS_CONFIG`. A slot is busy while any chain using it is
  on a job slot.
- Today one chain runs at a time for the whole GPU (PERF.md), so at most one
  AS is live at once and stealing is always possible. With 3 job slots
  running concurrently, at most 3 are live, and 8 is enough.
- `FLUSH_TLB` for a process that is not bound to a slot is a no-op, because
  binding flushes. For a bound one it is `LOCK` + `FLUSH_PT` on that slot.
- MMU fault IRQs now carry the slot. The fault handler maps slot to process
  to context and reports the fault against the right context, so only the
  faulting app loses its device. Today a fault resets everything.

What is lost: nothing measurable. More than 8 simultaneously *running*
processes cannot happen with 3 job slots.

### 4.5 The IoMmu model, and why option (a) is not available

The [IoMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-model):
"each process has a single virtual address space that is shared between the
CPU and GPU ... the GPU sends a data request to a compliant IOMMU. The
request includes a shared virtual address and a process address space
identifier (PASID)."

On the RK3576:

- The GPU node in `rk3576.dtsi` has no `iommus` property; the GPU sits
  directly on the bus.
- The SoC's IOMMUs are Rockchip's per-device units (`rockchip,rk3576-iommu`,
  for VOP2, VDPU and so on). They have one page table each, no PASID, no
  ATS, and no CPU page-table format.
- There is no Arm SMMU at all.

So there is nothing for IoMmu to use, and the Mali's own MMU is GpuMmu's
case by definition. The "physical addresses through the SoC IOMMU" idea also
does not hold for the GPU. What survives of option (a) is (a'): report 2.0
and stay in physical mode with today's aperture model. That is P1 below.

WDDM 3.0's [IOMMU DMA remapping](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-dma-remapping)
is a different thing: logical addresses for GPUs that cannot reach all
physical memory. "Implementing support on integrated parts is optional", and
it needs a system IOMMU behind the device. Not applicable either.

## 5. Residency, paging, fences, contexts, preemption, scheduling

### 5.1 Residency (MakeResident and Evict)

The UMD side:

- `pfnMakeResidentCb` at BO creation. Panfrost already assumes everything is
  mapped from creation on, the same contract as Linux, where `bo_handles`
  lists are not exhaustive (that was the 2026-10-17 bug).
- Wait on the returned paging fence before the first submit that uses the
  BO, either as a GPU wait on the context (`pfnWaitForSynchronizationObjectFromGpuCb`)
  or on the CPU.
- `pfnEvictCb` on trim. Budgets (`QueryVideoMemoryInfo`) matter only under
  pressure.
- Usage tracking for CPU maps: Panfrost has its own BO busy tracking
  (`bo-wait` in the hitch log). It needs per-BO last-write fences on the
  monitored fence instead of today's "wait for the device to go idle".
  PERF.md notes that today's busy checks wait for everything the device
  submitted; 2.x makes the right fix possible.

Removed:

- the per-submit list of every live BO;
- `PatchEveryAllocation`;
- `MaxAllocationListSlotId = 256` and the patch-location plumbing (`Patch`
  only records stats today);
- `MW_ESCAPE_ALLOC_INFO`: the VA comes from `pfnMapGpuVirtualAddressCb`,
  which is Panfrost's `AUTO_VA` contract.

### 5.2 Paging queue and paging operations

- Paging buffers are still built by `DxgkDdiBuildPagingBuffer` and run on a
  node in order with the paging fence. We have one node and no DMA engine. A
  software paging path, operations executed on the CPU, is the plan, as
  M5-PLAN said.
- **Execute at submit, not at build.** The sample writes PTEs inside
  `BuildPagingBuffer`. That is safe for an emulated engine. On a real GPU an
  unmap written at build time can land while an earlier chain still uses the
  mapping, because build runs ahead of the paging fence. The proposal is to
  record the operations in the paging DMA buffer at build, run them on the
  CPU when that buffer is submitted (in submission order with the GPU's
  work), then signal its fence at once. That costs one small interpreter
  over the existing `MkMmuMapPages`/unmap code.
- Operations to support:
  - `UPDATE_PAGE_TABLE`, including the `Repeat` and `InitialUpdate` flags;
  - `FLUSH_TLB`;
  - `COPY_PAGE_TABLE_ENTRIES`;
  - `NOTIFY_RESIDENCY`: no work;
  - `MAP_APERTURE_SEGMENT` and `UNMAP_APERTURE_SEGMENT`, still used for
    primaries, section 7;
  - `TRANSFER` and `FILL`: not supported, as now, which has worked with
    one aperture segment.
- Whether VidMm ever asks for TRANSFER on a system-memory-only GpuMmu
  adapter is not documented. The counters in the stats escape will show it.

### 5.3 Monitored fences

- The UMD signals after a submit with `pfnSignalSynchronizationObjectFromGpuCb`.
  The value must reach the fence's memory and the KMD must raise
  `DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED`.
- Two ways on Mali:
  1. The GPU writes it. Append a Bifrost WRITE_VALUE job (immediate 64,
     type 6/7) to the chain, aimed at the fence's GPU VA. That job type is
     proven since M1 and M3. Completion raises the job IRQ as now.
  2. The KMD writes it in the job-done DPC, from the CPU VA, before raising
     the interrupt.
- Start with (2). It needs no new job type, and its latency is the DPC
  latency the driver already pays for `DMA_COMPLETED`. Move to (1) only if
  measured latency matters.

### 5.4 Contexts, queues and preemption

- Virtual-mode contexts: `DxgkDdiCreateContext` with the virtual flag, or the
  runtime's `pfnCreateContextVirtualCb`. Submission is
  `DxgkDdiSubmitCommandVirtual` with `DmaBufferVirtualAddress` (a GPU VA in
  the process) and the DMA buffer's private data.
- **The Mali makes this easy.** A Mali job chain is started by writing its
  GPU VA into `JS_HEAD`. The KMD never needs to read the command buffer: the
  UMD puts the chain heads and their requirements (slot, the
  `MW_CMD_JOB` contents) into the private data, which the KMD gets in kernel
  memory. The sample does the same with `COSDMABUFINFO`
  (`CosKmdAdapter.cpp:1612-1660`). The DMA buffer itself can be a 4-byte
  marker.
- Preemption: unchanged. Soft-stop plus resume from `JS_TAIL` works per
  chain in either mode. `DXGK_INTERRUPT_DMA_PREEMPTED` is already raised
  (`command.c:375-410`).
- Hardware queues (`CreateHwQueue`, WDDM 2.2+) are only for HAGS drivers
  and are not needed without it.

### 5.5 Hardware-accelerated GPU scheduling (WDDM 2.7)

Microsoft: "With the right hardware and drivers, Windows can now offload most
of GPU scheduling to a dedicated GPU-based scheduling processor"
([DirectX blog](https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/)).
The driver advertises it through `HwSchSupported`
(`D3DKMT_WDDM_2_7_CAPS`, `DXGK_FEATURE_HWSCH`).

Bifrost's job manager has no scheduling processor. The command-stream
front end with an MCU arrived with Valhall CSF, which Linux drives with
panthor, not panfrost. HAGS on the G52 would mean the KMD's CPU code
pretending to be firmware, which is today's software scheduler under another
name. **Not possible honestly. No gain.**

## 6. The UMD side, and D3D12

### 6.1 Can today's UMD run on a WDDM 2.x adapter?

Today `maliumd.dll` is Mesa's d3d10umd: `D3D10_0_7_DDI_INTERFACE_VERSION`
(`0xa0009`) and the D3D10/10.1 DDI, with runtime callbacks used from the shim
(`malikm_drm.c:1122-1219`). Physical-mode callbacks only: Allocate, Lock,
Render and Escape.

Evidence, none of it a measurement:

- The virtual-mode callbacks (`pfnMakeResidentCb`, `pfnMapGpuVirtualAddressCb`,
  `pfnSubmitCommandCb` and the rest) live in `D3DDDI_DEVICECALLBACKS` under
  `D3D_UMD_INTERFACE_VERSION_WDDM2_0`. A UMD built against the current
  header sees the fields. Whether the runtime fills them for a UMD that
  negotiated a D3D10 interface is not documented.
- WDDM 2.0 moves trim and residency policy into the UMD through DDIs the
  runtime calls, such as `pfnTrimResidencySet`. As far as I recall the
  header, these sit in the `D3DWDDM2_0DDI` D3D11 device function table. A
  D3D10-DDI UMD has no slot for them. *Recalled, not re-read; check in
  `d3d10umddi.h` before P2.* If so, the runtime has no way to ask a D3D10-DDI
  UMD to trim, which suggests it will only run such a UMD in physical mode.
- The 2.1 table says WDDM 2.1 "supports" D3D9 to D3D12. It is silent on
  which DDI a 2.x virtual UMD must expose.

Conclusion: assume a WDDM 2.x virtual-mode UMD must be a `D3DWDDM2_0` D3D11
DDI UMD. That interface is the D3D11.1/`D3DWDDM1_3` tables plus a few
entries, so the D3D11.1 UMD from option (c) is the base it grows from. P1
tests the weaker case, a D3D10 UMD on a 2.x KMD in physical mode, because it
is free.

The OpenGL ICD (Mesa WGL on Panfrost, `mesa-patches/0060`) goes through the
same shim but calls D3DKMT directly, not runtime callbacks.
`D3DKMTMakeResident`, `D3DKMTMapGpuVirtualAddress`,
`D3DKMTCreateContextVirtual` and `D3DKMTSubmitCommand` exist for it. So the
GL ICD can be the first real client of virtual mode, before any D3D11 DDI
work, and FurMark (PERF.md) is the ready-made test.

### 6.2 D3D12

- **What it needs:** WDDM 2.0 (the docs, above). Feature level 11_0 at least
  (D3D12 has no 10_x levels). Shader model 5.1 or later delivered as DXBC or
  DXIL. Resource binding tier 1. A D3D12 DDI UMD (`d3d12umddi.h`), which is
  a much larger interface than D3D11's.
- **What exists:**
  - Mesa has no D3D12 UMD. Its `d3d12` Gallium driver runs on top of
    D3D12, the opposite direction; so do Dozen (Vulkan on D3D12) and the
    translation layers. D3D11On12 and D3D9On12 are Microsoft layers that
    need a D3D12 driver underneath; they cannot help a GPU without one.
  - vkd3d-proton is D3D12 on Vulkan as an application-side `d3d12.dll`,
    not a UMD. DWM and the system never load it. It also needs a far more
    capable Vulkan than PanVK gives on Bifrost: Vulkan 1.0 behind
    `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER`, no geometry or tessellation
    (FEASIBILITY section 2.3).
- **What building one would take:**
  - a D3D12 DDI layer over Gallium/Panfrost;
  - DXIL to NIR, through dxil-spirv then Mesa's `spirv_to_nir`, which is
    the pieces vkd3d-proton uses;
  - FL 11_0, which means geometry and tessellation emulation through
    `src/poly` (FEASIBILITY S0b and S2);
  - descriptor heaps over Panfrost's binding model;
  - WDDM 2.x virtual mode underneath.

  Estimate: 50 to 100 pw (1 to 2 person-years). Very high risk, because no
  open D3D12 UMD exists to learn from and the FL 11_0 emulation has no
  Gallium precedent.
- **What it would buy here:**
  - WebGPU in Edge: Dawn's D3D12 backend needs FL 11_1, or 11_0 with binding
    tier 2 ([Dawn support](https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7032/docs/support.md)).
    Its D3D11 backend refuses FL below 11_0
    ([Dawn 4622ac3](https://dawn.googlesource.com/dawn/+/4622ac3f1e21dc089f61dcea20edc6218a0da3a5)).
  - DirectML and Windows ML on the GPU.
  - D3D12 games.
  - WSL and Sandbox GPU, which also need GPU-PV (section 9.3).

  None of this is what the board is being used for, so it is not
  recommended.

## 7. Display side

WDDM 2.x full drivers keep the VidPN DDIs that `display.c` implements
(`IsSupportedVidPn` to `CommitVidPn`, `SetVidPnSourceAddress`). Nothing in
the 2.x pages makes a newer display model mandatory. The additions are
opt-in:

- MPO3 (2.2);
- `SetTimingsFromVidPn` and `UpdateMonitorLinkInfo` (2.5);
- Super Wet Ink scan-out transforms (2.6);
- the hardware flip queue (3.0).

What does change is memory for primaries:

- Under GpuMmu, ordinary allocations live in the implicit system-memory
  segment and get no `MAP_APERTURE_SEGMENT`. Their pages arrive only as PTEs.
- `scanout.c` and the KMD's CPU blt take a primary's pages from the MDL that
  `MAP_APERTURE_SEGMENT` hands over (PERF.md stage 2).
- The way to keep that is the IOMMU DMA remapping page's own recipe. Keep an
  aperture segment for primaries and mark primary allocations
  `AccessedPhysically` (`DXGK_ALLOCATIONINFOFLAGS_WDDM2_0`). Without that
  flag "any allocation that specifies an aperture segment in their supported
  segment set gets upgraded to the implicit system memory segment, which
  doesn't receive MAP_APERTURE calls".
- The IoMmu page also says "VidMm maps primary allocations into the aperture
  segment before being displayed". That is the same mechanism.
- So: two segments. Aperture (primaries and anything scanned out, mapped
  through the VOP2 IOMMU and the Mali, as today), plus system memory
  (everything else, through GpuMmu PTEs).

The 16 ms emulated vsync (`command.c:677-716`) is unchanged by 2.x. The
hardware flip queue (3.0) needs flip-queue hardware in the display
controller, and VOP2 takes one address per window with a "config done"
latch, so it is not possible.

## 8. What we gain on this board

| Gain | 1.3 today | 2.x (GpuMmu) | Evidence / note |
|---|---|---|---|
| Task Manager GPU page, per-process GPU use | absent | yes | "requires a GPU driver that supports WDDM version 2.0 or above" ([blog](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)). May also appear with (a') physical-mode 2.0; P1 shows it |
| Per-process GPU isolation | none: one AS, all VAs visible to all | yes, 8 hardware AS slots | section 4.4. The main security gain, relevant to Edge's sandboxed GPU process |
| GPU VA per process | about 4 GB for all processes together | 32 GB (35 bits) each | `MK_VA_START`/`MK_VA_END` |
| Resident GPU memory | at most 1 GB system-wide (`MW_SEGMENT_SIZE`) | budgeted from system RAM | `CommitLimit` 1 GB today |
| Submit cost | every live BO listed and patched per submit | no lists | the per-submit cost was never measured; PERF.md shows the GPU busy 5% with the copy gone, so expect little frame-rate change |
| Faults | a fault resets the GPU for everyone | the fault is charged to the faulting context; others continue | AS-slot fault routing |
| Panfrost busy checks | "wait for everything the device submitted" (PERF.md, `wait-timeout`) | per-BO monitored fences | needs UMD work, which 2.x enables |
| D3D12, WebGPU, WSL/Sandbox GPU | no | still no: needs a D3D12 UMD, and GPU-PV needs Hyper-V | sections 6.2, 9.3 |
| HAGS | no | no: no scheduling processor | 5.5 |
| Edge hardware video decode | no | no change: needs the D3D11.1 video DDI, which works on WDDM 1.2+ | other research, in flight |
| Frame rate | PERF.md figures | about the same | GPU-bound work is limited by the 198 to 594 MHz clock, not the driver model |

**Edge and Chromium, read from source:**

- ANGLE runs on D3D11 at 10_0 or higher. GLES 3.0 and WebGL 2 need 10_1
  (FEASIBILITY 2.4).
- DirectComposition in Chromium needs a D3D11 device and `dcomp.dll`
  (`ui/gl/direct_composition_support.cc`). There is no WDDM-version test.
  Overlay support is asked per output through DXGI's `CheckOverlaySupport`.
  MPO exists since WDDM 1.3.
- Dawn (WebGPU, and Skia Graphite where Chromium uses Dawn) uses D3D12 when
  available and D3D11 otherwise. Both need FL 11_0 or higher.
- I found no Chromium path that requires WDDM 2.x as such. What Edge cannot
  do today is WebGL 2 (needs 10_1), WebGPU and Graphite (need 11_0), and
  hardware video decode (needs the D3D11.1 video DDI). None is unlocked by
  WDDM 2.x.

**What cannot be used today because of WDDM 1.3 itself:** the Task Manager
GPU page, per-process isolation, more than 1 GB of resident GPU memory, and
anything that needs D3D12, which also needs a D3D12 UMD. Everything else on
the wish list is a UMD feature-level or DDI question.

## 9. WDDM 3.0 and 3.1, and whether 3.x is worth targeting

### 9.1 What 3.0 and 3.1 add

From [what's new for Windows 11 display drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-windows-11-display-and-graphics-drivers):
"WDDM 3.0 and later drivers can support the following features". Every item
is phrased as optional:

| Version | Feature | Needs | Here |
|---|---|---|---|
| 3.0 (21H2) | [D3D12 enhanced barriers](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/enhanced-barriers) | a D3D12 UMD | no |
| 3.0 | [D3D12 video encoding](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/video-encoding-d3d12) | a D3D12 UMD and an encoder | no |
| 3.0 | [DisplayPort over USB4](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-usb4) | USB4 | no USB4 on RK3576 |
| 3.0 | [Hardware flip queue](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/hardware-flip-queue) | display hardware that queues flips with target times | VOP2 has no flip queue |
| 3.0 | [IOMMU DMA remapping](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-dma-remapping) | a system IOMMU; "optional but recommended" for integrated GPUs, required for discrete ones for 3.0 certification | no SMMU (4.5) |
| 3.0 | [Signaling a CPU event from KMD](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/signaling-cpu-event-from-kmd) | nothing special | possible, no consumer here |
| 3.1 (22H2) | [Sharing the backing store with KMD](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/sharing-backing-store-with-kmd) | nothing special | no use found |
| 3.1 (header) | `NativeGpuFence` and `OptimizedNativeFenceSignaledInterrupt` VidSch caps (`d3dkmddi.h:2018-2022`), `CachedPageTables`, the GPU VA IOMMU flags, `AlwaysPoweredVRAM` | native fences are documented as 3.2 (24H2) work | beyond the OS ceiling in practice |

I found no Microsoft statement of what dxgkrnl *validates* for 3.0 or 3.1.
The published 3.0 certification rule found is the discrete-GPU DMA
remapping one, which does not apply to an integrated GPU.

### 9.2 The OS ceiling

- Windows 11 22H2 is WDDM 3.1. For 23H2 Microsoft writes: "The WDDM version
  didn't change." 24H2 is 3.2.
- This board runs 23H2 at most: 24H2 needs ARMv8.1 and the Cortex-A72 is
  ARMv8.0 (project memory `project_armv81_on_a72`). **So 3.1 is the highest
  version this board's Windows knows.** 3.2 features (native fences,
  user-mode submission, which is "still under development as of Windows 11,
  version 24H2") cannot be had at all.

### 9.3 Which 3.x-era features matter on a single-board ARM64 device

- **GPU-PV** (2.4+; WSL GPU 2.9; Sandbox memory sharing 2.5): the
  [GPU-PV page](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-paravirtualization)
  says "The driver must support GpuMmu" and "VM creation fails if the
  driver doesn't support IoMmu isolation".
  - It needs Hyper-V on the board. The RK3576's GIC is v2 (project memory);
    Hyper-V on ARM64 is, as far as I know, only shipped on GICv3 platforms.
    *Recalled, unverified.*
  - WSL's GPU path also needs a D3D12 UMD on the host.
  - **Useless here.**
- **User-mode submission:** a 3.2-era, in-development feature built on
  doorbells and a hardware scheduler. Bifrost has neither, and 23H2 lacks
  it. **Useless here.**
- **Hardware flip queue, DMA remapping, USB4:** no hardware for them.
  **Useless here.**
- **Enhanced barriers, D3D12 video encode:** need D3D12. **Useless here.**

### 9.4 Effort from 2.x to 3.0 or 3.1, and what reporting it unlocks

- Once a 2.x driver works, raising the number is the same probe ladder as
  P1: report 3.0, then 3.1, read event 494, and implement only what dxgkrnl
  says is missing. Estimate 1 to 3 pw if dxgkrnl asks for nothing
  substantial. Any real 3.x feature from the table costs more, and none of
  them is usable here.
- What reporting 3.x unlocks:
  - Windows 11 itself asks for "DirectX 12 or later with WDDM 2.0 driver"
    ([specs](https://www.microsoft.com/en-us/windows/windows-11-specifications)).
    That is a setup and compatibility check, and 23H2 already runs here on
    1.3 (observed).
  - Store and WinGet apps: I found none gated on a WDDM version; apps gate on
    D3D feature levels or on D3D12.
  - Chromium: no WDDM-version gate found (section 8).
  - `dxdiag` and Task Manager would show "WDDM 3.1". That is cosmetic.
  - **Nothing practical found.**

### 9.5 Which version number to target

**Target WDDM 2.x, starting at 2.0, and stop at the highest 2.x whose
mandatory items are honest on this hardware. Expect that to be 2.6:** 2.1's
UMD callbacks and 2.6's `CollectDiagnosticInfo` are cheap. 2.5's possible
GPU-PV expectation is the thing to watch in the ladder. Report 3.0 or 3.1
only if dxgkrnl accepts it with no further code, and say in the README that
it adds nothing. Do not report a version whose mandatory DDIs would be stubs
returning success: dxgkrnl and apps may then use them.

## 10. Phased plan (approved 2026-11-21)

Each step is small, ends with a board measurement, and leaves the 1.3 driver
working. The rig (ETW plus `dxgk-decode.py`, `m5test`, the registry
counters, the UMD `perf:` and slow logs) is what exists today. The 2.x code
lives behind a build flag (`MW_WDDM2`) until P5. Estimates are person-weeks.

| Step | What | Done when, measured on the board | Estimate |
|---|---|---|---|
| P0 | **Host design work, no board.** The `DXGK_PTE` to Mali translation, the private L0 page, the AS-slot LRU and the software paging interpreter, in `tests/malikm-host` against the simulated G52, which already walks real page tables | CI host tests: two processes map the same VA to different pages; a stolen slot is re-bound; an unmap with a chain in flight is ordered after the chain | 2 to 3 |
| P1 | **Probe ladder, physical mode.** Builds of today's driver reporting `DXGKDDI_WDDMv2` with interface `WDDM2_0`, no `VirtualAddressingSupported`; then 2.1, 2.6, 2.7, 3.0, 3.1 | per build: StartDevice status and event 494 text; dxdiag "Driver Model"; whether `maliumd.dll` loads in `dwm.exe` (the existing `Get-Process` check); `d3dprobe` output; the Task Manager GPU page; whether the desktop composites (photo) | 1 to 2 |
| P2 | **GpuMmu in the KMD, one test client.** Caps, QAI 13/14/15/11, Create/DestroyProcess, SetRootPageTable, `UPDATE_PAGE_TABLE`/`FLUSH_TLB`, AS slots, `SubmitCommandVirtual` with chain heads in private data. AS0 stays reserved for physical-mode contexts, so DWM and the UMD keep working unchanged on the same node | an `m5test virt` mode (D3DKMT: CreateContextVirtual, MapGpuVirtualAddress with VidMm picking the VA, SubmitCommand) runs WRITE_VALUE to a VidMm-chosen VA: target value right, 0 MMU faults; MMU-1 answered by StartDevice; counters for AS binds, steals and TLB flushes | 5 to 8 |
| P3 | **Isolation, residency and fences.** MakeResident plus paging-fence wait; monitored fences signalled from the DPC | two `m5test virt` processes at once both pass; process B jumping to A's VA faults and only B's device is removed; 10 processes in a loop force AS steals with 0 failures; fence-to-wake latency (median, max, n stated) | 2 to 3 |
| P4 | **The OpenGL ICD on virtual mode.** The shim backend on D3DKMT virtual calls; no BO lists | GpuTest FurMark at 594 MHz, 60 s, n=3, against the PERF.md 1.3 figure (285); job failures 0; `m5test mem` shows GL BOs outside the 1 GB segment | 3 to 4 |
| P5 | **The D3D11 UMD on `D3DWDDM2_0` (after option c).** Runtime virtual callbacks in the shim; trim and evict | DWM on the Mali in virtual mode: presents per 5 s dragging a window, n=3, against PERF.md's scanned-out row (135 to 268); 0 failed jobs over a 30 min session; Edge starts with GPU compositing (`edge://gpu`); Task Manager per-process GPU | 3 to 6 |
| P6 | **Primaries in the aperture, scan-out.** `AccessedPhysically` primaries, `scanout.c` unchanged in substance | `m5test scan 1` with the 2.x driver: flips with none copied over 10 s, as PERF.md stage 2 | 1 to 2 |
| P7 | **Version number and soak.** The P1 ladder repeated on the real 2.x driver; `CollectDiagnosticInfo`; per-engine TDR cap in the full build | final WDDM version accepted; 10 cold boots and 10 reboots with the desktop up (n stated, since a 25% intermittent makes n=2 look like proof); a forced TDR recovers; sleep and resume once | 2 to 3 |

Total for (b): about 19 to 31 pw including P0 and P1, or 16 to 26 pw for
P2 to P7. That is the section 1 figure.

Order relative to (c):

- P0 and P1 now, in parallel with the D3D11.1 work.
- P2 to P4 can run before the D3D11.1 UMD exists, because the GL ICD is a
  real client.
- P5 waits for the D3D11.1 UMD. Option (c) needs no WDDM change and should
  not wait for any of this.

As carried out: P1 (the probe) runs first, because it needs no new design
and its answers shape P2; P0's host tests come with P2's code. After each
phase there is a CI build and a board test, and no phase starts more than
one ahead of the last one tested on the board.

## 11. Interaction with work in flight

- **`present-fast` (the present path and UMD copies, off `next`).** This plan
  touches the same files only at P5 (shim present path) and P6 (primaries,
  `scanout.c`). P0 to P4 add new code paths (`memory.c` GpuMmu half,
  `command.c` `SubmitCommandVirtual`, a new shim backend) and change no
  present code. Rebase onto `next` after `present-fast` merges, before P5.
- **VDPU383 decoder prototype (outside maliwddm).** Independent. Its
  surfaces will need to be shareable with the GPU:
  - under 1.3, as allocations in the aperture segment;
  - under 2.x, as `AccessedPhysically` allocations if the decoder's IOMMU is
    fed from the MDL, like the VOP2.

  Design the decoder's buffer hand-off around "the KMD gets an MDL per
  allocation", which holds in both models.
- **D3D11.1 UMD with the video DDI.** Needed for Edge video on any WDDM
  version (WDDM 1.2+). It is also the base of the `D3DWDDM2_0` UMD in P5.
  Build its DDI tables so the `D3DWDDM2_0DDI` additions slot in, and keep its
  memory calls behind the shim so only the backend changes for 2.x.

## 12. Progress log

### P1, the probe: built, not yet run on a board

What it is (`drivers/maliwddm/version.c`, `wddm2.h`, a few lines in
`adapter.c`, `memory.c`, `command.c`):

- `HKLM\SYSTEM\CurrentControlSet\Services\maliwddm\Parameters\WddmProbe`
  (DWORD) selects the reported version: `0x2000` (2.0), `0x2100` (2.1) up to
  `0x2900`, `0x3000`, `0x3100`. Without it the driver is WDDM 1.3 exactly as
  before. The same DRIVERCAPS, the same DDI table, and no new QAI is asked of
  a 1.3 driver.
- With it, `DXGK_DRIVERCAPS::WDDMVersion` and the
  `DXGKDDI_INTERFACE_VERSION` passed to `DxgkInitialize` both change. The
  memory model stays WDDM 1.x physical mode: one aperture segment,
  allocation and patch lists, `VirtualAddressingSupported` 0.
- New answers that only a 2.x driver is asked for:
  - `QUERYSEGMENT4` (11): the same aperture segment;
  - `PHYSICALADAPTERCAPS` (15): one node, which is also the paging node, no
    GpuMmu yet;
  - `DxgkDdiCollectDiagnosticInfo`, registered only at 2.6 and above, where
    it is mandatory.
- Every QAI dxgkrnl asks is still recorded as `QAI_<n>` with its status, so
  a QAI a 2.x driver is expected to answer shows up as `0xc00000bb` against
  its number.
- One-shot fallback. DriverEntry writes `WddmProbeArmed`. The 300th Render
  call (DWM drawing) clears it and writes `WddmProbeConfirmed`. A boot that
  finds the probe still armed deletes `WddmProbe`, writes
  `WddmProbeFellBack` and starts as 1.3. `WddmProbeKeep = 1` turns the
  arming off.
- `tools/windows/wddm-probe.cmd` (in the release package): `set <v>`, `keep
  <v>`, `check` and `off`. `set` also arms a boot-time DxgKrnl ETW
  AutoLogger to `C:\mali\probe\boot.etl`, because StartDevice's event 494
  happens before anyone can log on.

The board test is in section 13.3.

### P1, first board run (2026-11-22, TaishanPi 3M, 23H2, CI run 283)

- **1.3 baseline:** fine. dxdiag WDDM 1.3, FL 10_0, DDI 11.2, UMD in
  `dwm.exe`. **On 1.3 `typeperf` already lists `GPU Engine` instances**
  (`pid_*_luid_0x9F39_phys_0_eng_0..8_engtype_3D`, 9 engines per process,
  all 0%). So "the counters exist" is not the test for the Task Manager
  page; which LUID they name and whether 3D use moves is.
- **0x2000, hot (set plus `pnputil /restart-device`):** black screen and a
  hang. Recorded before it: `QAI_11` (size 0x28) and `QAI_15` (size 0x14),
  both status 0, and `Fail_QueryInterface 0xc00000bb`. That path is not a
  valid test (the loaded image is kept); n=1.
- **0x2000, full reboot:** hangs at the boot spinner; power cycle needed.
  n=1.
- **Fallback:** worked both times (`WddmProbeFellBack = 0x2000`, next boot
  1.3 with a desktop). n=2.
- **Lost:** everything from the hung boot. `boot.etl` was 0 bytes, the
  fallback boot rewrote the Device Parameters trace, and a file written just
  before the power cut vanished.

What is known: dxgkrnl accepted DRIVERCAPS at 2.0 and asked for
`QUERYSEGMENT4` and `PHYSICALADAPTERCAPS`, which this driver answered. It
then stopped somewhere later. Suspects, none tested:

- `DXGK_OPERATION_NOTIFY_RESIDENCY` (15), a 2.0 paging operation the driver
  refused with `STATUS_NOT_SUPPORTED`;
- a QAI or interface a 2.0 driver must answer;
- a wait on a fence or interrupt that a 2.0 VidMm expects in a different
  form.

### P1, second build: breadcrumbs that survive a hang

`drivers/maliwddm/probe.c`:

- Every DDI wrapper, plus StartDevice, GetNodeMetadata,
  QueryDependentEngineGroup, the ISR and the DPC, counts its enters, exits,
  failures and the sequence and time of its last enter. This is in memory,
  interlocked, at any IRQL.
- In probe mode everything the trace writes is mirrored, flushed, to
  `Device Parameters\maliwddm\Probe<version>`. The 1.3 fallback boot never
  writes that key.
- A watchdog thread, started in AddDevice so that a hang inside StartDevice
  is caught, writes the same key every second for the first 5 minutes,
  then every 10 s or on any change. Each write is flushed, and contains:
  - `Wd_Seconds`;
  - `Wd_Last` (the last DDI entered, with its sequence number);
  - `Wd_Stuck` ("still in <DDI> after N ms" for anything in progress over
    2 s) and `Wd_WorstStuck`;
  - `Cnt_<ddi>`, `Seq_<ddi>` and `Fails_<ddi>`/`LastFail_<ddi>`;
  - the adapter's render, submit, fence, IRQ, vsync and flip counters;
  - `Pg_<op>` and `PgRefused_<op>` for every paging operation VidMm built.
- `ProbeUart = 1` also prints one watchdog line a second on the debug UART,
  RK3576 UART0, 1500000 8n1. It needs neither the registry nor the file
  system, in case the SYSTEM hive is not yet being written to disk when the
  adapter starts.
- `NOTIFY_RESIDENCY` is now answered with success, the first suspect. It
  is a switch: `ProbeNotifyResidencyOk = 0` refuses it as before.
- QueryInterface records the GUID (`QI_RefusedData1`) and version of an
  interface it refuses. StartDevice records the adapter LUID
  (`AdapterLuidLow`/`High`), to map the `GPU Engine` counters to this
  adapter.

`wddm-probe.cmd`:

- `set <v> [uart]` and `watch` (the watchdog on 1.3, a baseline).
- The boot-time ETW capture keeps one file per boot (`FileMax 8`) and
  flushes every second (`FlushTimer 1`), so the fallback boot no longer
  overwrites the hung boot's file.
- `check` prints every `Probe*` key and counts the GPU counters per LUID
  with PowerShell, which fixes section 6's count.

### What Panorama taught us (2026-11-22)

`github.com/awesomefroggy29/Panorama` holds two WinMali drivers for Valhall
Malis:

- `claude-g615`, by Jamie: a Mali-G615 on a MediaTek MT6897 tablet. It
  reached a GPU-composited desktop at WDDM 2.0 with GpuMmu, and Windows
  Terminal rendered on the Mali with no WARP.
- `codex-g610`, by mr4g: a Mali-G610 on RK3588.

Both authors gave permission on Discord on 2026-11-22 to use their work
in this GPL-2.0 driver with credit; the records are kept with the
project. The repository's base, `WoA-OpenMali/rk3588-GPU`, is not covered
and nothing was taken from it. Nothing was copied: what follows are
interface facts, read from their source and notes, and re-implemented here
(`drivers/maliwddm/wddm2x.c` credits both).

What their notes and code state about dxgkrnl, each found by them on
hardware, mostly by disassembling `dxgkrnl.sys` 10.0.26100:

1. **The paging node must be a node of its own, a copy engine.**
   `PHYSICALADAPTERCAPS.PagingNodeIndex` naming the 3D node "bails at
   StartDevice on Win11 ARM64". Naming a node beyond `NumExecutionNodes`
   makes a phantom node and a 0xD1 in the scheduler later. Both drivers
   declare two nodes, 3D and COPY, with the copy node's paging buffers
   done on the CPU. **Our 2.0 probe declared one node and paging node 0:
   this is the prime suspect for the hang.**
2. **`BuildPagingBuffer` may return only `STATUS_SUCCESS` or
   `STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER`.** Anything else is bugcheck
   0x10E. Our 1.3 path refuses unknown operations with
   `STATUS_NOT_SUPPORTED`, and a 2.x VidMm sends operations a 1.3 driver
   never sees. Their driver also appends a small marker when given a DMA
   buffer.
3. **A GpuMmu driver's paging buffers arrive through
   `SubmitCommandVirtual`**, which must not be a stub (0x119).
4. **`PageTableAlignmentInBytes` must not be 0**, or dxgmms2 divides by
   zero (0x7E).
5. **`SchedulingCaps.MultiEngineAware` is checked for every WDDMv2
   driver.** Ours sets it. Their GpuMmu driver also sets PreemptionAware,
   CancelCommandAware, LowIrqlPreemptCommand and No64BitAtomics, which
   they found mandatory at 3.2. Not known to be required at 2.0.
6. **The version number matters for a display:** at `WDDMVersion >=
   0x2300` dxgkrnl tears down a lost VidPN source instead of pausing it;
   0x2200 hung at LogonUI; 0x2000 is the one that composited. **So the
   target is 2.0**, not 2.6 as section 9.5 guessed. The 2.6 and 3.x rungs
   of the ladder stay possible but are not the goal.
7. **Their GpuMmu description is four 9-bit levels and 48 bits**, 4 KB
   tables in their segment, CPU-virtual updates, and VidMm's level 0 as the
   leaf. That is exactly the shape of Bifrost's legacy LPAE walk, which
   always takes four levels (`io-pgtable-arm.c:1232`). There is no need
   for the private level-0 page section 4.3 proposed. The G52 translates
   only 35 bits, so `InternalGpuVirtualAddressRangeStart/End` keep VidMm's
   own addresses below 2^35, and the UMD bounds its own.
8. **The oracle for an adapter-start failure is `DxgKrnl-Admin` event
   549**, which names the status and the reason. `wddm-probe check` now
   dumps that log.

Their UMD is Mesa d3d10umd too, so a d3d10umd-based UMD does run on a
WDDM 2.0 GpuMmu adapter. Their memory work goes through private escapes
(VM bind) as well as VidMm. Section 13's contract for our UMD stays as
written.

### P1, third build: two shapes (option c)

The breadcrumb build (CI run 287) has not run. The next build keeps all of
it and corrects the caps, with `Wddm2Shape` choosing between two shapes
on successive boots:

- **Shape 0, physical:** WDDM 2.0 in physical mode, as before, plus:
  - two nodes, node 1 a copy node that is the paging node
    (`PagingNodeIndex 1`, `MemoryManagementCaps.PagingNode 1`), whose
    submissions complete at once;
  - `SupportPerEngineTDR` and `CancelCommandAware` set (the DDIs exist);
  - `BuildPagingBuffer` never fails at 2.x, and puts an 8-byte marker
    into a paging DMA buffer (`PagingMarker 0` turns that off);
  - per-node fences, engine reset and node metadata.
- **Shape 1, GpuMmu:** shape 0, plus:
  - `VirtualAddressingSupported` and `GpuMmuSupported`;
  - `GPUMMUCAPS` (4 levels, 48 bits, CPU virtual, read-only and no-execute
    supported) and `PAGETABLELEVELDESC` (9 bits, 4 KB, alignment 4 KB,
    segment `GpuMmuPtSegment`, default 1);
  - `CreateProcess`, `DestroyProcess`, `SetRootPageTable`,
    `GetRootPageTableSize` and `SubmitCommandVirtual`;
  - `UPDATE_PAGE_TABLE` written in the Mali format.

  The Mali still runs only physical-mode work on AS0: the tables are kept
  but not yet used (P2 binds them to address spaces).
- **Both:** the pre-StartDevice QAIs WDDM 3.x asks (device caps, physical
  memory caps, IOMMU caps, reserved ranges) are answered. They only matter
  at 3.x.
- `ProbeNotifyResidencyOk` is gone. A refusal there is now a bugcheck, not
  a test.

Order: shape 0 first, because it is the smaller change and the paging-node
fact predicts it. Then shape 1, whatever shape 0 does: if shape 0 starts,
shape 1 says whether GpuMmu also starts (P2's precondition). If it does
not, shape 1 is the other half of the comparison.

## 13. What the KMD expects of the UMD, phase by phase

This is the contract between `wddm2` (KMD) and the UMD branch. Where a
structure is named but not yet in `include/maliwddm_abi.h`, it is added by
the phase that first needs it, at the end of that header under a "WDDM 2.x"
heading, so the two branches do not edit the same lines.

### 13.1 P1 (probe) to P3: nothing changes for the UMD

- Same D3D10 DDI and the same physical-mode callbacks as today:
  - `pfnAllocateCb` with `MW_ALLOCATION_INFO` private data, and the VA from
    `MW_ESCAPE_ALLOC_INFO`;
  - `pfnLockCb`;
  - `pfnRenderCb` with `MW_COMMAND`s and the allocation list;
  - `pfnPresentCb`.
- The UMD must not depend on the reported WDDM version. On a 2.x adapter
  the runtime may pass a larger `D3DDDI_DEVICECALLBACKS`. Only the fields
  used today are used until P5.
- From P2 the KMD has both a physical path (today's, on Mali AS0) and a
  virtual path (AS1 to AS7). The node takes both kinds of context, which the
  docs allow. Physical-mode clients (today's UMD, the GL ICD) keep working
  unchanged. Only `m5test virt` uses the virtual path in P2 and P3.

### 13.2 From P4 (GL ICD) and P5 (D3D UMD): the virtual-mode contract

1. **Context.** `D3DKMTCreateContextVirtual` (GL) or
   `pfnCreateContextVirtualCb` (D3D), node 0, engine affinity 1. Private data:
   `MW_VCONTEXT_CREATE { Magic, AbiVersion }`, added in P2. The KMD answers
   with the DMA buffer private data size.
2. **Allocations.** As today: `pfnAllocateCb` or
   `D3DKMTCreateAllocation`, with `MW_ALLOCATION_INFO` (size, `MW_ALLOC_*`
   flags). In virtual mode the KMD reserves no VA and `MW_ESCAPE_ALLOC_INFO`
   returns `STATUS_NOT_SUPPORTED`.
3. **GPU VAs.** `pfnMapGpuVirtualAddressCb` or `D3DKMTMapGpuVirtualAddress`:
   - Keep VAs in `[0x2000000, 0x100000000)` (32 MB to 4 GB), Panfrost's
     range on Linux, until Panfrost is shown to cope with higher addresses.
     The hardware limit is 2^35.
   - Executable allocations (no `MW_ALLOC_NOEXEC`) must not cross a 16 MB
     boundary, because the shader PC's high bits are fixed: Linux panfrost's
     `panfrost_drm_mm_color_adjust`, and `MK_EXEC_WINDOW` here. Reserve a
     16 MB-aligned window with `pfnReserveGpuVirtualAddressCb` and map
     executable allocations inside it at addresses the UMD picks.
   - Non-executable ones may let VidMm pick (`BaseAddress` 0, the min and
     max above).
   - Every map returns a paging fence value. Wait for it, on the GPU with
     `pfnWaitForSynchronizationObjectFromGpuCb` on the context, or on the
     CPU, before the first submission that uses the range.
4. **Residency.** `pfnMakeResidentCb` right after creating an allocation,
   the Panfrost model, and wait on its paging fence. Evict only on the
   runtime's trim (`pfnTrimResidencySet`) or under budget. A submission that
   touches a non-resident or unmapped VA faults, and only that context is
   lost from P3 on.
5. **Submission.** `pfnSubmitCommandCb` or `D3DKMTSubmitCommand`:
   - `Commands` = the GPU VA of a small per-context command allocation (4 KB,
     mapped like any other). The KMD never reads it.
   - The work goes in the private data: `MW_VSUBMIT { Magic, Count,
     MW_COMMAND[Count] }`, the same `MW_COMMAND` as today (`Type =
     MW_CMD_JOB`, `Slot` 0 fragment or 1 vertex/tiler/compute, `Jc` = the
     first job header's GPU VA).
   - There are no allocation or patch lists and no "every live BO" list.
6. **Fences.** Monitored fences:
   - create with `pfnCreateSynchronizationObject2Cb` (type monitored fence);
   - after each submit, `pfnSignalSynchronizationObjectFromGpuCb` on the
     context with the next value;
   - CPU waits with `pfnWaitForSynchronizationObjectFromCpuCb`.

   The KMD writes the value when the submission's chains are done (P3).
   Per-BO busy tracking in the UMD replaces today's "wait for the device to
   go idle" (PERF.md, `wait-timeout`).
7. **CPU access.** `pfnLock2Cb` or `D3DKMTLock2`, persistent maps allowed.
   VidMm adds no implicit synchronisation in this model, so the UMD waits on
   its own fences before reading GPU output.
8. **Primaries and shared surfaces (P5, P6).** Created as today, with the
   runtime's resource handle and `MW_ALLOC_PRIMARY`. The KMD puts them in
   the aperture segment (`AccessedPhysically`) for scan-out. Each process
   that opens a shared allocation maps it into its own VA space; there is no
   global VA any more.
9. **Unchanged:** `MW_ESCAPE_GPU_INFO`, `MW_ESCAPE_STATS`, the other
   escapes, node 0.

### 13.3 P1 board test

(As first written. The second build adds the steps in 13.4.)

Install the `wddm-mali-g52-driver-arm64` package of the CI run for the
`wddm2` commit as usual (`INSTALL.txt`) and reboot once on 1.3. First, the
baseline: run `wddm-probe check` with no probe set, and keep that output.
Expect `WddmVersion 0x1300`, `DdiInterface 0x4002`, dxdiag "WDDM 1.3", and
no GPU engine counters.

Then, for each version in order 2000, 2100, 2600, 3000, 3100 (bisect
2200 to 2500 if 2600 fails where 2100 passed):

1. `wddm-probe set <v>`, reboot.
2. Watch the boot: desktop, or black, or a bugcheck. If black: RDP, or
   power off and on. The next boot falls back to 1.3 by itself.
3. `wddm-probe check`. The output answers:
   - **Accepted?** `WddmProbeConfirmed = <v>` (not `WddmProbeFellBack`),
     device status "Started" (no problem code 43), and `Started 1`,
     `WddmVersion <v>` in the trace.
   - **What dxgkrnl said:** `boot-<time>.etl`, decoded on Linux with
     `tools/etw/dxgk-decode.py`. Event 494's status and any "Driver
     reports ... but" text. Also the `QAI_<n>` lines with `0xc00000bb`.
   - **The driver model DirectX sees:** dxdiag "Driver Model: WDDM <v>",
     feature levels, DDI version.
   - **The current UMD loaded:** section 5 lists `maliumd.dll` in `dwm.exe`.
   - **The Task Manager data:** section 6 shows `GPU Engine` instances with
     `engtype_3D`, and a non-zero 3D utilisation while a window is dragged
     during the 3 samples.
4. By eye, with a photo:
   - Task Manager, Performance: a GPU entry named "Rockchip RK3576 Mali-G52
     (WDDM)" with a 3D graph that moves when a window is dragged.
   - Details: dwm.exe with GPU use.
   - The Start menu and File Explorer.
   - Edge opens and scrolls a page; `edge://gpu` shows D3D11 on the Mali
     (save the page).
5. Send `C:\mali\probe\` back.

`wddm-probe off` and a reboot return the board to 1.3.

What would stop the ladder: a refusal at 2.0. The event 494 text then says
which cap or DDI dxgkrnl wanted, and that is the next build. A pass at 2.0
with no Task Manager GPU page would mean the page needs virtual addressing,
which P2 brings. Either result is useful.

### 13.4 P1 board test, second build (breadcrumbs)

1. Install the new package and do a full reboot. Then run `wddm-probe watch
   uart`, reboot, run `wddm-probe check`, and keep the output. This is a 1.3
   baseline of the new instruments: key `Probe1300`, with `Wd_Stuck none` on
   a healthy boot, and the UART lines if a serial logger is attached.
2. Run `wddm-probe off`, then `wddm-probe set 2000 uart`, then do a **full
   reboot**, not `pnputil /restart-device`. Keep the serial logger running
   from before the reboot.
3. If it hangs, wait at least 60 s at the spinner, so the watchdog has
   written a minute of crumbs, then power off and on. The next boot falls
   back to 1.3.
4. On the fallback boot (or the 2.0 desktop, if it comes up), run `wddm-probe
   check` and send `C:\mali\probe\` and the serial log. In the output,
   section 2b is the `Probe2000` key from the 2.0 boot:
   - `Wd_Stuck` and `Wd_WorstStuck`: where it hung;
   - `Wd_Last`: the last DDI called;
   - `Wd_Seconds`: how long the watchdog stayed alive;
   - `PgRefused_<op>`: operations refused;
   - `QI_RefusedData1`;
   - and the `Cnt_` list, which shows how far the boot got.
5. If `Probe2000` is empty or missing, the hive was not being written yet.
   The UART lines are then the record, and that is itself a finding.
6. Then, only if 2000 reached a desktop: in Task Manager, compare the GPU
   entry and the per-LUID counter list in section 6 against the 1.3
   baseline.

`ProbeNotifyResidencyOk` stays at its default (1) for this run. If 2.0
comes up, one more run with it at 0 shows whether that was the fix:
`reg add HKLM\SYSTEM\CurrentControlSet\Services\maliwddm\Parameters /v
ProbeNotifyResidencyOk /t REG_DWORD /d 0 /f`, then `wddm-probe set 2000
uart` and a reboot.

### 13.5 P1 board test, third build (two shapes)

Before each step, do `wddm-probe off` and a reboot if the board is not on
1.3. Keep the serial logger running. Wait at least 60 s at a hung spinner
before you power-cycle.

1. **Shape 0:** `wddm-probe set 2000 phys uart`, full reboot. Then `wddm-probe
   check` on whatever boot comes next: the 2.0 desktop, or the 1.3
   fallback.
2. **Shape 1:** `wddm-probe off`, reboot, `wddm-probe set 2000 gpummu uart`,
   full reboot, `wddm-probe check`.
3. Send `C:\mali\probe\` and the serial log. Per shape, `check` shows:
   - **accepted?** `WddmProbeConfirmed` against `WddmProbeFellBack`, and
     the device status;
   - **why not:** section 2c, the DxgKrnl Admin log with event 549's
     reason, and section 2b, `Probe2000` with `Wd_Stuck`, `Wd_Last`, the
     `Cnt_` list and `PgRefused_*`;
   - **shape 1 only:** `Wd_Processes`, `Wd_RootSets` and `Wd_PteWrites`
     say whether VidMm worked the GpuMmu side. `Wd_VirtSubmits3d` says
     whether any 3D work came in virtual mode, which this build completes
     without running. Expect a broken picture if it did; that is the
     finding.
4. If a shape reaches the desktop:
   - Task Manager, Performance: is there a GPU entry? Take a photo.
   - Section 6, the GPU counters by LUID, against the 1.3 baseline.
   - Edge: `edge://gpu`.

## 14. Sources, and what I could not verify

**Read from source:**

- This repo at `next` `2bb2b52`: `drivers/maliwddm/{adapter,memory,command,scanout}.c`,
  `maliwddm.h`, `drivers/common/maligpu.{c,h}`, `win32-compat/malikm_drm.c`,
  docs.
- Linux linux-next-v8 (`bf01e0353815`): `drivers/gpu/drm/panfrost/panfrost_mmu.c`,
  `panfrost_drv.c`, `panfrost_features.h`, `panfrost_regs.h`;
  `drivers/iommu/io-pgtable-arm.c`; `arch/arm64/boot/dts/rockchip/rk3576.dtsi`.
- WDK 10.0.26100.0 `d3dkmddi.h`, `d3dukmdt.h`: `DXGK_GPUMMUCAPS`,
  `DXGK_PAGE_TABLE_LEVEL_DESC`, `DXGK_PTE`, `DXGK_VIDMMCAPS`,
  `DXGK_VIDSCHCAPS`, `DXGK_PHYSICALADAPTERFLAGS`, the version constants.
- `microsoft/graphics-driver-samples` `compute-only-sample/coskmd/CosKmdAdapter.cpp`
  and `CosKmdGlobal.cpp`, via `gh`.
- Chromium `ui/gl/direct_composition_support.cc` (main, via googlesource).

**Pages read:**

- [GpuMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)
- [IoMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-model)
- [GPU virtual memory in WDDM 2.0](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)
- [Driver residency in WDDM 2.0](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/driver-residency-in-wddm-2-0)
- [DXGK_GPUMMUCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_gpummucaps)
- [DXGK_PAGE_TABLE_LEVEL_DESC](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_page_table_level_desc)
- [DXGK_PHYSICALADAPTERCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_physicaladaptercaps) (section 15)
- [DXGK_QUERYSEGMENTOUT4](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_querysegmentout4) (section 15)
- [DXGK_QUERYADAPTERINFOTYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype): 11 = QUERYSEGMENT4, 15 = PHYSICALADAPTERCAPS (section 15)
- [DXGK_SEGMENTFLAGS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags) (section 15)
- [WDDM 2.1 features](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-2-1-features)
- [Features added in prior WDDM 2.x versions](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-prior-wddm-2-x-versions)
- [What's new for Windows 11 display drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-windows-11-display-and-graphics-drivers)
- [IOMMU DMA remapping](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-dma-remapping)
- [GPU paravirtualization](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-paravirtualization)
- [User-mode work submission](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/user-mode-work-submission)
- [Hardware-accelerated GPU scheduling (DirectX blog)](https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/)
- [GPUs in the Task Manager (DirectX blog)](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)
- [Windows Sandbox architecture](https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-architecture)
  (WDDM 2.5 for memory sharing)
- [DirectX heart Linux (DirectX blog)](https://devblogs.microsoft.com/directx/directx-heart-linux/) (WSL GPU, WDDM 2.9)
- [Dawn platform support](https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7032/docs/support.md)
  and [Dawn 4622ac3](https://dawn.googlesource.com/dawn/+/4622ac3f1e21dc089f61dcea20edc6218a0da3a5)
- [Windows 11 specifications](https://www.microsoft.com/en-us/windows/windows-11-specifications)

**Not verified:**

- Whether dxgkrnl accepts a 2.x driver in physical mode only (P1).
- Whether the runtime runs a D3D10-DDI UMD on a 2.x adapter (P1 for
  physical mode; section 6.1 argues no for virtual mode).
- That `pfnTrimResidencySet` lives in the `D3DWDDM2_0DDI` D3D11 table:
  recalled. `d3d10umddi.h` was not on disk for this study.
- VidMm's acceptance of a 5-bit root in a 4 KB table (MMU-1, P2).
- The full dxgkrnl validation list for any version, in particular 2.5's
  relation to GPU-PV and anything for 3.0 and 3.1.
- Hyper-V on ARM64 requiring GICv3: recalled.
- The Bifrost AArch64 4K MMU mode on RK3576 (plan C): never run on this SoC.
- Whether Chromium's D3D11 video decoder adds a feature-level floor of its
  own: the source moved and was not read. It belongs to the D3D11.1 video
  work.
- All effort figures are estimates.

## 15. The `wddm2x` build: which 2.x first, and what was wrong

Branch `wddm2x`, 2026-11-22: the seven `wddm2` commits rebased onto
`hpd-edge` (`c445902`: d3d11, HDMI hot-plug, the Mesa 0068 Edge fix), and
one commit on top. Nothing in it has run on a board.

### 15.1 Decision: WDDM 2.0, physical mode, a CPU copy node for paging

The first target is `WddmProbe 0x2000` with `Wddm2Shape 0`: WDDM 2.0, every
engine in physical mode (allocation and patch lists as on 1.3), no GpuMmu
and no IoMmu, and two nodes, 3D and a COPY node that is the paging node and
whose paging buffers are done on the CPU while they are built. GpuMmu
(`Wddm2Shape 1`) comes after that shape has booted.

Why, from the evidence:

- **Where the first probe stopped.** CI run 283 at 0x2000: dxgkrnl took
  DRIVERCAPS, asked `QAI_11` (QUERYSEGMENT4, size 0x28) and `QAI_15`
  (PHYSICALADAPTERCAPS, size 0x14), both answered with status 0, and the
  boot hung right after (2/2). Status 0 was not the problem; what was in
  them was (15.2). Both answers are asked of every 2.x driver whatever its
  memory model, so they have to be right before either shape can be
  judged.
- **GpuMmu was never asked about.** dxgkrnl did not ask `QAI_13`
  (GPUMMUCAPS) or `QAI_14` (PAGETABLELEVELDESC): it asks those only of a
  driver whose DRIVERCAPS claims GpuMmu. The hang is therefore not a
  missing GpuMmu answer, and adding GpuMmu would add page tables,
  CreateProcess, SetRootPageTable and SubmitCommandVirtual to a boot that
  already failed without them.
- **Microsoft documents physical mode under WDDM 2.0.** "Individual engines
  of a GPU can operate in either physical or virtual mode. In physical
  mode, the scheduling model remains the same as it is with WDDM v1.x"
  ([GPU virtual memory in WDDM 2.0](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)),
  and a driver must "opt-in" to GpuMmu or IoMmu through DXGK_VIDMMCAPS.
  Whether 22631 accepts a 2.0 driver that opts into neither is not
  documented (section 14, "not verified"). If it does not, dxgkrnl fails
  the adapter start with event 549 and a reason, which is a clean answer
  at the cost of one boot, not a hang.
- **Our UMD has no virtual-mode client yet** (13.2). GpuMmu would gain
  nothing on this board until P2, while physical mode keeps today's UMD
  and present path unchanged.
- **The version is 2.0, not higher.** Jamie found that dxgkrnl tears a
  lost VidPN source down at `WDDMVersion >= 0x2300` and that 0x2200 hung at
  LogonUI; 2.0 is what composited for him.

### 15.2 What the answers got wrong, and the fixes

| QAI / DDI | First probe (run 283) | Run 288 (`wddm2`) | `wddm2x` |
|---|---|---|---|
| 15 `NumExecutionNodes`, `PagingNodeIndex` | 1, 0 (the 3D node) | 2, 1 (COPY) | 2, 1 (COPY) |
| 15 `DxgkPhysicalAdapterHandle` | our adapter context | our adapter context | dxgkrnl's `DeviceHandle` from StartDevice |
| 11 count pass | decided by `pSegmentDescriptor == NULL` | same | decided by `NbSegment == 0` |
| 11 input | not checked | not checked | `DXGK_QUERYSEGMENTIN4.PhysicalAdapterIndex` 0 |
| 11 segment flags | aperture, CPU visible | same | same, plus the local budget group and application target, 0x88005 (`Wddm2Budget`, `Wddm2LocalBudget`, default 1; build 345 had the non-local group, 16.11) |
| PreemptCommand, node 1 | n/a | handled as node 0 | DMA_PREEMPTED on node 1 at once |
| BuildPagingBuffer, no room for the marker | n/a | operation done, then INSUFFICIENT_DMA_BUFFER (done twice) | INSUFFICIENT_DMA_BUFFER before anything is done |

The handle is the finding of this build. Microsoft's reference for
[DXGK_PHYSICALADAPTERCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_physicaladaptercaps)
says `DxgkPhysicalAdapterHandle` is the handle "passed to the kernel mode
driver as `DXGKRNL_INTERFACE::DeviceHandle` in DxgkDdiStartDevice", i.e.
dxgkrnl's own, and Jamie's driver returns exactly that
(`adapter->DxgkHandle`). Both earlier builds returned our `MW_ADAPTER`
pointer, which dxgkrnl would then use as one of its own objects. That
alone could hang or crash anywhere after QAI 15. If 15 is ever asked before
StartDevice, this build answers `STATUS_DEVICE_NOT_READY` rather than make
a handle up; the journal shows the order.

[DXGK_QUERYSEGMENTOUT4](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_querysegmentout4):
"First time, NbSegment will be set to 0. The driver should return
STATUS_SUCCESS and set NbSegment ... without accessing any other member".
The structure dxgkrnl 22631 passed is the 26100 header's (0x28 bytes:
`SegmentDescriptorStride` is a `SIZE_T` at the end), and 0x14 for 15 ends
exactly at `Flags`, so `VPRPagingNode` is not written there. Both sizes are
`C_ASSERT`s now, so a WDK that disagrees fails the build.

Checked and unchanged: `MultiEngineAware` (checked for every WDDMv2
driver), `SupportPerEngineTDR`, `CancelCommandAware`, GetNodeMetadata's
ordinal with the physical adapter index in the top bits, and the 3.x
pre-start QAIs (not asked at 2.0).

### 15.3 The one-shot fallback, made robust

- The arming (`WddmProbeArmed`) is flushed and read back before a probe is
  used. If it does not read back, the boot is 1.3 and
  `WddmProbeArmFailed` says why: an unarmed 2.x boot that hangs would hang
  every boot after it.
- The confirm moved from the 300th Render call to the watchdog thread, at
  PASSIVE_LEVEL: 300 renders and 120 s up and no DDI stuck. Before, a 300th
  render at raised IRQL meant no confirm at all, and 300 renders can all
  happen at LogonUI.
- Every 2.x boot re-arms, a confirmed one too, so a later hang costs only
  itself. `WddmProbeFellBackBoot` names the journal of the boot that fell.
- The decisions are `drivers/maliwddm/wddm2plan.h`, plain C.
  `tests/wddm2-host` (CI, Linux, ASan/UBSan) runs them over every pattern
  of hung and good boots: no two hung 2.x boots in a row, 1.3 after a
  fallback until the probe is set again, no probe without arming.

### 15.4 The boot journal

`Services\maliwddm\Probe\Boot<n % 8>` (n = `Parameters\ProbeBoots`), from
DriverEntry on, only while a probe is in effect:

- `E0001`, `E0002`, ...: `<ms since DriverEntry> <event>`.
- An event is one of:
  - `> <DDI> #<call> seq <n>`, written and flushed (`ZwFlushKey`) before
    the DDI runs;
  - `< <DDI> <status>` after it returns;
  - `QAI <type> asked: in <n> bytes, out <n> bytes`, then the answer, field
    by field for 1, 11, 13, 14 and 15. For 15 that includes whether the
    handle is dxgkrnl's;
  - `StartDevice: dxgkrnl DeviceHandle ...`;
  - the watchdog's "still in <DDI> after N ms";
  - `confirmed`.
- `Last` is the newest event, and `Count` the number of events.
  `Probe\LastBootKey` names the newest boot's subkey.

So the last `> ` line on disk is the DDI a hang is in. Limits:

- only at PASSIVE_LEVEL, where the registry can be written. SubmitCommand
  at DISPATCH_LEVEL and the ISR are in the watchdog's counters instead;
- the first 64 calls of each DDI, plus every failure;
- at most `ProbeJournalMax` (default 1500) events per boot.

The 1.3 fallback boot never writes the journal. The watchdog key
(`Device Parameters\maliwddm\Probe2000`) and the UART (`ProbeUart 1`) are
still there. If the hive is not yet written to disk when the adapter
starts, the UART is the only record.

### 15.5 Host checker

`make -C tests/wddm2-host run`, a CI step:

- the adapter rules for 1.3 and 2.x in both shapes and both budget
  settings:
  - the paging node is a COPY node, not node 0, and below
    NumExecutionNodes;
  - the VPR paging node is a real node;
  - one GpuMmu bit everywhere;
  - `PageTableAlignmentInBytes` is not 0, and VA bits = levels x index
    bits + 12;
  - the internal VA is below the G52's 35 bits;
  - the handle is dxgkrnl's;
  - one budget group;
  - 1.3 is unchanged;
- the QUERYSEGMENT4 pass table;
- the fallback over all 256 eight-boot patterns;
- a source lint that the driver takes each of these from `wddm2plan.h` and
  that `(HANDLE)` of our context never comes back.

A mutation (paging node 0) fails 16 checks.

The rules are Microsoft's and those that Jamie (`claude-g615`) and mr4g
(`codex-g610`) found on hardware in
[Panorama](https://github.com/awesomefroggy29/Panorama). Both gave
permission on 2026-11-22 to use their work in this GPL-2.0 driver with
credit; the records are kept with the project. As before, nothing was
copied: the code here re-implements interface facts, and code their
repository inherits from `WoA-OpenMali/rk3588-GPU` was not used.

## 16. `wddm2x` board test (one shot), reading a hang, back-out

Use the `wddm-mali-g52-driver-arm64` package of the CI run for the
`wddm2x` head (section 15). Never hot-install it, and never `pnputil
/restart-device` a display driver: both wedge dwm. Each step is a clean
restart.

### 16.1 Bind the package at 1.3 (no probe)

1. Copy the package to `C:\mali\pkg-wddm2x\` and stage it, do not install
   it: `pnputil /add-driver C:\mali\pkg-wddm2x\maliwddm.inf`.
2. As SYSTEM (task `malireinst`), set `ConfigFlags |= 0x20`
   (CONFIGFLAG_REINSTALL) on
   `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0`. Poll until the
   value has 0x20 in it; the task takes seconds on this board.
3. Clean restart. Expect the usual desktop, with HDMI hot-plug and Edge as
   on `hpd-edge`. `wddm-probe check` (from the package) shows
   `WddmVersion 0x1300`, `WddmProbeResult 0` and no `Probe` journal key:
   without `WddmProbe` the driver is 1.3 as before. Keep this output as the
   baseline.

### 16.2 The probe: WDDM 2.0, physical shape

1. From an administrator prompt: `wddm-probe set 2000 phys uart`. If a
   serial logger is on UART0 (1500000 8n1), start it now.
2. Clean restart.
3. If it hangs at the spinner, wait **at least 3 minutes**: the watchdog
   and the journal keep writing, and the confirm needs 120 s anyway. Then
   power off and on. The next boot is 1.3 by itself
   (`WddmProbeFellBack 0x2000`).
4. On whichever boot comes up (the 2.0 desktop, or the 1.3 fallback):
   `wddm-probe check`, and send `C:\mali\probe\` and the serial log.

What the output answers:

- **Accepted?** `WddmProbeConfirmed 0x2000` against `WddmProbeFellBack`,
  the device status, and the DxgKrnl Admin log (event 549 names an
  adapter-start failure and its reason).
- **Where it stopped:** section 2b2, the journal. `Last` of the 2.0 boot's
  `BootN`, and in `journal-<time>.txt` the tail of its `E` lines. Look for:
  - the `QAI 15 ... handle ... (dxgkrnl's)` line;
  - the last `> <DDI>` line without a matching `<`;
  - any `watchdog ... still in` line.
- **If it came up:** dxdiag "Driver Model", `maliumd.dll` in `dwm.exe`, the
  GPU counters by LUID against the baseline, and a photo of Task
  Manager's Performance tab.

Then, only if shape 0 came up and was confirmed: `wddm-probe off`, restart,
`wddm-probe set 2000 gpummu uart`, restart, `check`. That is the next test,
not part of this one.

### 16.3 If Windows does not come up at all: reading the journal from Linux

The fallback should make this unnecessary. If it does not (two hung boots
in a row), the journal is in the SYSTEM hive on the eMMC's Windows
partition. There are two ways to read it.

- **From TSPI-RESCUE** (the second Windows on the eMMC):

  ```
  reg load HKLM\OFF C:\Windows\System32\config\SYSTEM
  reg query HKLM\OFF\ControlSet001\Services\maliwddm\Probe /s
  ```

  Use the drive letter the main Windows has there. `reg load` replays the
  hive's transaction logs itself.

- **From Armbian** (SD or USB, through the UEFI boot menu):

  ```
  lsblk -f                              # the main Windows' NTFS partition
  sudo mount -t ntfs3 -o ro,force /dev/mmcblk0pN /mnt/win
  mkdir ~/hive && cp /mnt/win/Windows/System32/config/SYSTEM{,.LOG1,.LOG2} ~/hive/
  sudo umount /mnt/win
  python3 -m venv ~/rv && ~/rv/bin/pip install regipy
  ~/rv/bin/regipy-process-transaction-logs ~/hive/SYSTEM -p ~/hive/SYSTEM.LOG1 \
      -s ~/hive/SYSTEM.LOG2 -o ~/hive/SYSTEM.clean
  hivexregedit --export ~/hive/SYSTEM.clean '\ControlSet001\Services\maliwddm\Probe'
  ```

  - **The transaction-log replay is required.** `ZwFlushKey` makes a write
    durable in `SYSTEM.LOG1`/`LOG2` first, and the main file can lag behind
    it. Without the replay, the newest lines are exactly the ones that are
    missing.
  - Mount read-only. A volume left dirty by the power cut needs `force`
    with ntfs3. Copy nothing back.
  - `hivexregedit` is in the `libwin-hivex-perl` package; `regipy-dump`
    works as well.
  - `Select\Current` in the same hive says whether it is `ControlSet001`.

To clear the probe from there without Windows, use TSPI-RESCUE:
`reg add HKLM\OFF\ControlSet001\Services\maliwddm\Parameters /v WddmProbe
/t REG_DWORD /d 0 /f`, then `reg unload HKLM\OFF`. Do not write the hive
from Linux.

### 16.4 Back-out

- **To 1.3 with this package:** `wddm-probe off`, then a clean restart. The
  fallback does the same by itself after a failed boot.
- **To the previous package:**
  1. `pnputil /enum-drivers` and find the `oem<N>.inf` of the `hpd-edge`
     package;
  2. stage it again if it is gone;
  3. `pnputil /delete-driver oem<M>.inf` for the `wddm2x` package
     (without `/uninstall`);
  4. `ConfigFlags |= 0x20` as in 16.1, then a clean restart.

  The higher-ranked package wins, so delete the `wddm2x` one first.
- **Leftovers:** `wddm-probe off` removes the boot-time ETW capture. The
  journal key `Services\maliwddm\Probe` is harmless and can be deleted with
  `reg delete ... /f` once read.

### 16.5 First run of 16.2 (build 310, `fbc785a`), and the next one shot

TaishanPi 3M, 23H2, HDMI unplugged, `HdmiHpd 1`, 2026-11-22, n=1.

- **16.1 (1.3):** fine. `WddmVersion 0x1300`, dxdiag WDDM 1.3.
- **16.2 (`set 2000 phys uart`):** not a hang. The boot bugchecked with
  0x139 KERNEL_SECURITY_CHECK_FAILURE, P1 3 (LIST_ENTRY corruption). No
  dump: this eMMC stack cannot write one yet.
- **Fallback:** worked. `WddmProbeFellBack 0x2000`, `WddmProbeFellBackBoot
  1`, next boot 1.3.
- **UART:** no lines on the firmware console capture.
- **The journal** (`Probe\Boot1`, 48 events) got further than any earlier
  build:
  - StartDevice 0, dxgkrnl's handle `...B8030`, interface 0x5023;
  - QAI 1 at 2.0 (two nodes, paging node 1, no VA);
  - QAI 15 with dxgkrnl's handle;
  - GetNodeMetadata for both nodes;
  - QAI 11's count and fill passes (stride 104, flags 0x108005);
  - QAI 10 (in 4, out 8), then QAI 16 (out 4).

  The last line is QAI 16's return. No `> CreateDevice` follows: the
  failure is inside dxgkrnl, between the end of the QAI sequence and the
  system device.

Every byte the driver wrote into dxgkrnl's memory before that point was
checked against the size dxgkrnl passed. One answer was wrong:

- **QAI 10 = HISTORYBUFFERPRECISION.** Microsoft's reference sizes its
  output as `sizeof(DXGKARG_HISTORYBUFFERPRECISION) *
  NbAsymetricProcessingNodes`: 8 bytes for our two nodes, as asked. The
  driver, written when there was one node, filled only node 0. Node 1's
  precision was whatever the buffer held. 0 would mean "call
  DxgkDdiFormatHistoryBuffer", which this driver does not register, and
  1 to 31 is invalid. Jamie's driver also fills only the first entry, so
  this is a suspect, not a proven cause. It is the only answer of the
  sequence that was incomplete.
- **QAI 16 = DISPLAY_DRIVERCAPS_EXTENSION** (one UINT,
  `VirtualModeSupport`): zeroed within its 4 bytes. Correct, unchanged.
- **GetNodeMetadata:** the WDK 26100 layout is the same at every interface
  version from 1.3 on (a `Reserved` UINT stands where 2.2's `Flags` go).
  The driver's 76-byte write is within the structure.

Build `a135ed6` answers QAI 10 for every node, with 64 bits as node 0
always had. It also adds these journal lines:

- QAI 10 and 16 field by field;
- CreateDevice and CreateContext with their flags and node;
- the first 64 BuildPagingBuffer operations;
- the callbacks made at PASSIVE_LEVEL (AcquirePostDisplayOwnership,
  IndicateChildStatus);
- a ring of what happens above PASSIVE_LEVEL, copied in by the watchdog
  once a second: the ISR, the DPC, and the DMA-completed, preempted and
  vsync notifications. The first 32 of each kind are kept.

**Next one shot** (one change against build 310: QAI 10):

1. Bind the new package at 1.3 as in 16.1: stage, `ConfigFlags |= 0x20`
   as SYSTEM, clean restart. Check that `WddmVersion 0x1300` comes up as
   before.
2. `wddm-probe set 2000 phys uart`, then a clean restart. Same conditions
   as before: HDMI unplugged, `HdmiHpd 1`.
3. On whatever comes up next, run `wddm-probe check`. Send:
   - the journal of the new boot (`LastBootKey`);
   - the `Device Parameters\maliwddm\Probe2000` key;
   - a photo of any bugcheck.

What the result says:

- **The journal passes QAI 16 and shows `> CreateDevice`:** QAI 10 was it.
  The next lines say how far the boot got.
- **The same 0x139 after the same QAI 16 line:** QAI 10 was not it. The
  next one shot is `wddm-probe set 2000 phys uart nobudget`, which drops
  the only other field that differs from the 1.3 segment, the budget flags
  (0x108005 becomes 0x5).
- **Anything else:** the new journal lines are the starting point.

### 16.6 Runs 2 and 3 of 16.2 (build 318), what dxgkrnl did, and the next one shot

TaishanPi 3M, 23H2 (22631.2428), DSI panel, HDMI unplugged, `HdmiHpd 1`,
package DriverVer 22.17.7.389 (CI run 45076819707, build 318), 2026-11-23.

| | 1.3 | `set 2000 phys uart` | `set 2000 phys uart nobudget` |
|---|---|---|---|
| boot | fine, `WddmVersion 0x1300` | bugcheck 0x139, P1 3 | bugcheck 0x139, P1 3 |
| P2 / P3 | | `FFFFF803BA3C8600` / `...84B8` | `FFFFF801C5E88600` / `...84B8` |
| journal | | `Boot2`, 49 events | `Boot3`, 49 events, the same |
| last line | | `< QueryAdapterInfo` (QAI 16) at 7363 ms | the same at 7444 ms |
| `Wd_Seconds` | | 8 | 8 |
| fallback | | 1.3 on the next boot | 1.3 on the next boot |

So the QAI 10 fix (`a135ed6`) and the budget flags changed nothing: the
journal is the same line for line, QAI 10 now answered for both nodes.

**Timing.** The watchdog starts at AddDevice (18 ms) and writes
`Wd_Seconds` every second, flushed. Its last value, 8, was written at about
8.0 s, after QAI 16 returned (7.36 s and 7.44 s); the write at about 9 s
never reached the disk. So the system ran for 0.6 to 1.7 s after QAI 16
with no DDI entered, no ISR, DPC or vsync in the ring, and then
bugchecked. P2 (the trap frame) and P3 (the exception record) of 0x139 lie
in the `FFFFF80x` range, at the same page offset (`0x600`) on both boots:
not an ordinary thread stack. Processor 0's boot-time DPC stack is mapped
there, which fits a timer or DPC, not the adapter-start thread. That part
is inference; no stack was seen.

#### What dxgkrnl does after QAI 16 (read from its code)

The board's dxgkrnl is 10.0.22621.2428 (the same binary serves 22631).
`tools/re/dxgk-annotate.py` fetched the amd64 build of that version and
its public symbols (`--winbindex dxgkrnl.sys 10.0.22621.2428`; the ARM64
code is the same source) and disassembled it. In
`DXGADAPTER::Initialize`:

1. `ADAPTER_RENDER::CreateRenderCore`: the QAI 15, GetNodeMetadata,
   QAI 11 and QAI 10 of the journal.
2. `ADAPTER_DISPLAY::CreateDisplayCore` -> `ADAPTER_DISPLAY::Initialize`:
   QAI 16 (`Type 0x10`), then sources, monitor manager, VidPN manager;
   no DDI.
3. Then, for a dxgmms2 adapter with a render core:

   ```
   cmp r13d, 0x5008            ; DRIVER_INITIALIZATION_DATA.Version
   jb  ok
   cmp [adapter+0x388], 0      ; pfnCalibrateGpuClock
   je  fail
   cmp [adapter+0x3e8], 0      ; pfnSetStablePowerState
   jne ok
   fail: "Driver is compiled against DXGKDDI_INTERFACE_VERSION_WDDM2_0_M2_2_1
          or greater, but does not fill in the pfnCalibrateGpuClock or
          pfnSetStablePowerState DDI."   -> STATUS_INVALID_PARAMETER
   ```

   We claim 0x5023 and registered neither. 1.3 (0x4002) is below the
   test, which is why 1.3 never met it.
4. `DpiFdoStartAdapter` sees `DxgkAddAdapter` fail and unwinds: when it
   owns the POST display it calls `DxgkDdiStopDeviceAndReleasePostDisplayOwnership`
   (and `DxgkDdiStopDevice` only if that fails), and the failed start
   leads PnP to `DxgkDdiRemoveDevice`.

Our `StopDeviceAndReleasePostDisplayOwnership` stopped only the scan-out,
and `RemoveDevice` then freed `MW_ADAPTER`. The HPD thread (a 200 ms
`KeWaitForSingleObject` on an event inside it) and the copy thread (an
event inside it) were still running. A wait that times out unlinks its
wait block from the event's list in freed pool: a `LIST_ENTRY` check,
0x139 P1 3, from timer expiry on a DPC stack. Neither DDI was journaled,
which is why nothing showed after QAI 16.

#### Candidates, ranked

1. **pfnCalibrateGpuClock and pfnSetStablePowerState missing at interface
   0x5023.** Proven as the refusal: the condition, the message and its
   place right after QAI 16 are in dxgkrnl's code, and nothing of ours
   runs between QAI 16 and it. Jamie's driver registers both.
2. **Use after free in our teardown after a refused start.** Most likely
   cause of the 0x139: timing, the trap frame's address range, and the
   code. Not seen on a stack.
3. Checked and cleared for this point:
   - QAI 10 and 16: answered within the sizes asked; changing QAI 10 did
     not move anything.
   - The segment (stride 104, flags `0x108005` or `0x5`): `nobudget` gave
     the same journal.
   - DRIVERCAPS, PHYSICALADAPTERCAPS and GetNodeMetadata: read before
     the refusal, within their sizes, and the start went past them.
   - SupportPerEngineTDR's DDIs (ResetEngine, QueryEngineStatus,
     QueryDependentEngineGroup): checked in `ADAPTER_RENDER`, which passed.
   - CreateProcess, SetRootPageTable, GetRootPageTableSize,
     SubmitCommandVirtual: GpuMmu only, not reached in physical mode.
   - SignalMonitoredFence is a callback, not a DDI. CollectDbgInfo is
     registered.

   Still unknown after the check: dxgmms2's paging process and system
   devices (`VidMmInitializePagingProcess` -> `VidSchCreateSystemDevices`),
   where the first `CreateDevice` of a 2.0 adapter comes from.

#### What changed (`0e06f76`)

- `DxgkDdiCalibrateGpuClock` (GPU time modelled as the performance
  counter: no GPU timestamps are written) and `DxgkDdiSetStablePowerState`
  (nothing to pin) at every 2.x interface. `Wddm2PowerDdis 0`
  (`wddm-probe set 2000 ... nopowerddi`) leaves them out. 1.3's table is
  unchanged.
- `StopDeviceAndReleasePostDisplayOwnership` does all of StopDevice's work
  (`MwAdapterStop`, once per start). `RemoveDevice` without a stop first
  stops the copy and HPD threads and the backlight timer.
- Journaled now: `RemoveDevice`, `StopDeviceAndReleasePostDisplayOwnership`,
  `CalibrateGpuClock`, `SetStablePowerState`, and RemoveDevice's
  "already stopped" or not.
- `tests/wddm2-host`: the 0x5008 rule, and that the driver registers from
  `MwPlan`.

Package: `wddm-mali-g52-driver-arm64` of CI run 45085983678 (build 325),
DriverVer 23.57.31.401. Later commits on `wddm2x` that touch only docs do
not change the driver; use this run's package for 16.6.

#### Next one shot: two boots, one variable each

Bind the new package at 1.3 first (16.1: stage, `ConfigFlags |= 0x20` as
SYSTEM, clean restart, `WddmVersion 0x1300`). No serial logger and no
kernel debugger are needed. The user is needed physically only if a boot
hangs (power cycle); a bugcheck stops at the blue screen (`AutoReboot 0`)
and needs a power cycle too.

**A. The diagnosis.** `wddm-probe set 2000 phys uart nopowerddi`, clean
restart. Against build 318, the only change is the teardown.

- Expected: no bugcheck. Windows comes up, the adapter with a problem code
  and the screen on Basic Display (or left as it was). Then
  `wddm-probe check`. The journal ends: `< QueryAdapterInfo` (QAI 16),
  `> StopDeviceAndReleasePostDisplayOwnership` (or `> StopDevice` if
  dxgkrnl does not own the POST display), its `<` line, then
  `> RemoveDevice` and `RemoveDevice: already stopped`.
- That confirms both the refusal point and that the 0x139 was ours.
- A 0x139 again means the teardown is not the cause: use the kernel
  debugger (below) on the same setting.

**B. The fix.** `wddm-probe off`, restart (1.3), then
`wddm-probe set 2000 phys uart`, clean restart. Against A, the only change
is the two DDIs.

- Expected: the journal passes QAI 16 with no stop or remove after it.
  Whatever comes next is new ground: `CalibrateGpuClock` calls, then
  dxgmms2's paging process, the first `> CreateDevice` (the system device),
  `CreateContext` on node 1 and the first `BuildPagingBuffer`.
- If the desktop comes up: the 16.2 checks (dxdiag Driver Model,
  `maliumd.dll` in dwm, the GPU counters), and the watchdog confirms after
  120 s.
- If it stops: the last journal lines, and with this build a refusal ends
  in a clean stop rather than a bugcheck.

Send `C:\mali\probe\` after each, plus photos of anything blue.

**Already on the board, no reboot needed:** both crashed boots left a
DxgKrnl ETW file, `C:\mali\probe\etl-20261123-122639\boot.etl.001` and
`C:\mali\probe\etl-20261123-123740\boot.etl.001` (flushed every second).
Copy them off; `tools/etw/dxgk-decode.py` decodes them with an
`events.json` made from dxgkrnl 22621.2428 (`make-events-json.py`). Any
event dxgkrnl wrote about the refused start, before the bugcheck, is in
there.

#### If a stack is ever needed: the kernel debugger over UART0

`scripts/kd-listen.py` in edk2-rk3576 is a minimal KDCOM peer. Since
`99496f6` it answers a `Fatal System Error` by reading the register context
and 48 KB of stack and saving them to `kd-dump-<time>.bin`, with every
module's base in its log. It did that for the CM5-IO WHEA bugcheck on
2026-10-03. On the TaishanPi:

1. On the host, `/dev/ttyACM0` (the Raspberry Pi Debug Probe on UART0) must
   have one reader. Start
   `scripts/kd-resilient.sh <raw-file> <log-file>` (1500000 baud, finds the
   probe by USB id), then stop the console logger, so that the reader
   count never drops to zero.
2. On the board, as administrator:
   `bcdedit /dbgsettings serial debugport:1 baudrate:1500000`,
   `bcdedit /debug on`. UART0 is the DBG2 port (`0x2AD40000`). The rate is
   UEFI's whatever the BCD says: SPCR's BaudRate is 0.
3. Set the probe **without `uart`**. The driver's watchdog writes to the
   same UART0 and would corrupt KDCOM packets.
4. Restart with the listener already running. With `debug on` and nobody
   answering, Windows waits on every packet and the boot crawls.
5. Afterwards: `bcdedit /debug off`. Copy `dxgkrnl.sys`, `dxgmms2.sys` and
   `ntoskrnl.exe` from `C:\Windows\System32` (and `drivers\`). Then
   `tools/re/dxgk-annotate.py <dir> --local <file>` names the return
   addresses in the dump: subtract each module's base, then look the
   offset up in `syms.txt`.

KD changes boot timing, and on CM5-IO it starved USB enumeration. Use it
only if run A disagrees with the prediction.

### 16.7 Runs A and B of 16.6 (build 325), the 0x113, and the next one shot

TaishanPi 3M, same setup, package DriverVer 23.57.31.401 (build 325),
2026-11-23. Files: `board-session-prep/wddm2x-23.57.31.401/`.

- **A, `set 2000 phys uart nopowerddi`: as predicted.** There was no
  bugcheck. Basic Display came up and the adapter showed Error, code 43.
  The journal after QAI 16: `> StopDeviceAndReleasePostDisplayOwnership`,
  `<` 0, `> RemoveDevice`, `RemoveDevice: already stopped`, `<` 0. This
  confirms both halves of 16.6: the refusal point, and that the 0x139 was
  our teardown.
- **`wddm-probe off`:** 1.3 back, fine.
- **B, `set 2000 phys uart`, power DDIs on:** the start went past the
  check. The journal (`Boot5`), after QAI 16:
  - `CreateDevice` twice (system device), then `CreateContext` on node 1;
  - `BuildPagingBuffer` op 5 (MAP_APERTURE_SEGMENT, done on the CPU, no
    DMA buffer, as at 1.3) twice;
  - QueryInterface, QueryDeviceDescriptor, RecommendMonitorModes, and
    three IsSupportedVidPn calls, all 0;
  - last, `> EnumVidPnCofuncModality #1` with no exit.

  Then bugcheck 0x113 VIDEO_DXGKRNL_FATAL_ERROR (0x1C, 0xF, 0x1, 0).

  `CalibrateGpuClock` was called twice (`Cnt_CalibrateGpuClock 2`, seq 31
  and 32). It is not in the journal because dxgkrnl calls it at
  DISPATCH_LEVEL, where the journal does not write.

**0x113 subcode 0x1C.** dxgkrnl 10.0.22621.2428 raises it at one place
(`tools/re`, `WdLogSingleEntry5(0, 0x113, 0x1C, ...)`): in
`DMMVIDPNPRESENTPATH::SetRotationSupport`, which is what our
`pfnUpdatePathSupportInfo` call in EnumVidPnCofuncModality reaches. The
check applies only when two things hold: the driver supports
path-independent rotation (interface 0x4003 or later), and test signing is
on. It then requires the RotationSupport Offset bits:

| path | rule | P3 if broken |
|---|---|---|
| the primary clone path (a path alone is one) | Offset0 set | 1 |
| the same | no Offset90, 180 or 270 | 2 |
| any other path | at least one Offset bit | 0 |

P2 is the bits we reported: 0xF, which is Identity plus Rotate90, 180
and 270, with no offset. The interface at 1.3 is 0x4002, below the rule,
so it never fired there.

**Build 330 (`4d1400d`):**

- At 2.x every path we update in EnumVidPnCofuncModality (display.c and
  hpd.c) also carries Offset0, so 0xF becomes 0x1F. This driver never
  rotates one clone against another, and Offset0 satisfies all three
  rows. 1.3's paths are unchanged.
- `Wddm2RotOffset 0` (`wddm-probe ... norotoffset`) reproduces build 325.
- `tests/wddm2-host` checks the rule on the bits (build 325's 0xF gives
  P3 1, 0x1F passes) and that both files apply it.
- Package: `wddm-mali-g52-driver-arm64` of CI run 45088503229 (build 330),
  DriverVer 0.23.50.775.

**Next one shot (C).** Against B the only change is Offset0.

1. Bind build 330 at 1.3 as in 16.1, and check `WddmVersion 0x1300`.
2. `wddm-probe set 2000 phys uart`, then a clean restart. HDMI unplugged,
   `HdmiHpd 1`, as before.
3. On whatever comes up, run `wddm-probe check`. Send `C:\mali\probe\` and
   a photo of anything blue.

What the result says:

- **The journal passes `< EnumVidPnCofuncModality`:** Offset0 was it. The
  next lines are the first mode set at 2.0: CommitVidPn,
  SetVidPnSourceAddress, then the first Present or Render. If the
  desktop comes up, run the 16.2 checks, and after 120 s the watchdog
  confirms.
- **The same 0x113 0x1C:** the change did not reach the path. Look for a
  `Fail_` value in the device key.
- **Another bugcheck:** photograph its four parameters. A new 0x113
  subcode can be looked up the same way: in `tools/re` output for
  dxgkrnl 22621.2428, search `dis.txt` for `mov edx, 0x113` followed by
  `r8d` set to the subcode.

The user is needed physically only to power cycle after a hang or a blue
screen.

### 16.8 Run C (build 330): WDDM 2.0 is up, the panel is black, and the next one shot

TaishanPi 3M, same setup, package DriverVer 0.23.50.775 (build 330),
2026-11-23. Files: `board-session-prep/wddm2x-330-stepC/`.

**No bugcheck. WDDM 2.0 is up.**

- The device status is OK and `WddmVersion` is 0x2000.
- dwm.exe and LogonUI are running, and there was no fallback.
- The journal (856+ events) has CommitVidPn, SetVidPnSourceAddress,
  Present, and devices and contexts on node 0, all succeeding.

The DSI panel showed backlight only:

- **UMD.** `m5test` printed `DRM_IOCTL_PANFROST_CREATE_BO failed
  (err=12)` and then "Panfrost could not create a screen ... using
  softpipe". It also printed `Escape(ALLOC_INFO): 0xc0000008`, which is
  STATUS_INVALID_HANDLE, not a memory error. `Fail_Escape 0xc0000008`
  agrees. Every UMD, DWM's included, ran on softpipe.
- **KMD.** 74 presents, all skipped ("0 blts, 0 fills, 74 skipped").
  4 flips, 3 of them scanned out by the VOP2. No jobs.

**One cause for both.** The escape and OpenAllocation found our
allocation through `DxgkCbGetHandleData`. In dxgkrnl 22621.2428,
`DxgGetHandleDataCB` on a dxgmms2 adapter tests a global that a WDDMv2
adapter sets. If it is set, the call logs "WDDM2 driver calls WDDM1.x
DDI!" and returns NULL. So at 2.0:

- every OpenAllocation handed back a NULL `hDeviceSpecificAllocation`;
- every Present saw no source and no destination (`DoPresent`: skipped);
- `MwEscapeAllocInfo` answered STATUS_INVALID_HANDLE.

Jamie's driver measured the same: 0 of 542 opens resolved. The WDDM 2.0
replacement is `DxgkCbAcquireHandleData` with `DxgkCbReleaseHandleData`.
Microsoft's WDDM 2.6 compute-only sample uses it in
`CosKmDevice::OpenAllocation`.

**Build 333 (`2cf66d7`):**

- At 2.x, OpenAllocation and the ALLOC_INFO escape go through
  `MwAllocFromHandle`. It calls AcquireHandleData and releases at once,
  which is safe because the allocation lives until DestroyAllocation.
  1.3 keeps GetHandleData.
- `Wddm2AcquireHandle 0` (`wddm-probe ... gethandledata`) goes back to
  GetHandleData.
- The first 8 unresolved opens are journaled
  (`OpenAllocation: handle ... not resolved`).
- Package: `wddm-mali-g52-driver-arm64` of CI run 45089830454 (build 333),
  DriverVer 0.37.30.73.

**Next one shot (D).** Against C the only change is how a handle is
resolved.

1. Bind build 333 at 1.3 as in 16.1, and check `WddmVersion 0x1300`.
2. `wddm-probe set 2000 phys uart`, then a clean restart. HDMI unplugged,
   `HdmiHpd 1`.
3. Run `wddm-probe check` and `m5test`. Send `C:\mali\probe\`, the m5test
   output, and photos of the panel and of Task Manager's Performance tab.

What the result says:

- **The desktop on the panel, m5test without the softpipe line, no
  `Fail_Escape`, presents counted as blts and fills, not skipped:** the
  handle was it. Then run the 16.2 checks: dxdiag Driver Model,
  `maliumd.dll` in dwm, the GPU counters by LUID. The watchdog confirms
  after 120 s.
- **Still black, with `OpenAllocation: handle ... not resolved` in the
  journal:** AcquireHandleData does not resolve these handles either. The
  journal lines give the handles. The next step would be Jamie's
  private-data id, which is a UMD change.
- **The UMD now on the Mali but GPU work failing (jobs failed, MMU
  faults):** a new problem, in physical mode at 2.0, for the next round.

### 16.9 Run D (build 333): the handles resolve, the GPU never runs, and a probe without a reboot

TaishanPi 3M, 2026-11-24. The DSI ribbon is broken, so this run used HDMI
only (2560x1440, `HdmiHpd 1`). Package build 333, restamped as DriverVer
2.29.41.819. Files: `board-session-prep/wddm2x-333-stepD/`.

- **What AcquireHandleData fixed:**
  - no softpipe and no CREATE_BO errors;
  - no `Fail_Escape`;
  - 140 present blts and none skipped;
  - 97 flips, 50 of them scanned out by the VOP2;
  - UMD reports from LogonUI and dwm.
- **Still black.** HDMI has a signal and HDMI audio plays. `renders 0`,
  `jobs done 0`, and no `Cnt_Render` in the whole boot: the driver's
  DxgkDdiRender was never called. m5test's own render failed in
  D3DKMTRender with 0xc0000001. The UMDs used CPU paths only: upload,
  mapW, cpubuf and direct present. Every pfnRenderCb, DWM's included,
  most likely failed the same way, so nothing was ever drawn by the GPU.
- **"A primary shown" (scan-out state bit 4):** clear (0x1b) in the stats
  taken at m5test's start, set (0x1f) in those taken at its failure, with
  flips scanned out in between. It reflects the moment the stats were
  read, not a 2.0 fault. Scan-out works, so it is not the cause of the
  black screen; the frames scanned out were never drawn.

**Where 0xc0000001 comes from (dxgkrnl 22621.2428, `tools/re`):**

- `DxgkRender` validates the context and the allocation list, then calls
  `DXGCONTEXT::Render`.
- Inside it, the only place that returns 0xc0000001 directly is "Hardware
  context render is not provided with hardware queue array". That needs
  the context flag HwQueueSupported, which neither m5test nor the UMD
  sets.
- Everything else between there and `DxgkDdiRender` is dxgmms2 called
  through its interface tables:
  - `VidMmAcquireDmaBuffer` (VidMm +0x1e8);
  - `VidMmReferenceDmaBuffer` with the allocation list (+0x238);
  - Begin CPU access, and the length and patch-list getters.

  Those statuses cannot be traced statically with confidence. Two more
  guesses would cost two boots, so measure instead.

**Instrument (`d1b6ff6`, CI run 45314806057, all green): `m5test
renderprobe`.** No reboot, no driver change: the driver stays build 333.
The new `m5test.exe` is in that run's `m3test-arm64` artifact. It runs six
D3DKMTRender calls with no GPU work, each changing one thing:

| | variant | says |
|---|---|---|
| A | node 0, one allocation, locked before (as M1) | the baseline |
| B | node 0, no allocation list | the allocation list (VidMmReferenceDmaBuffer) |
| C | one allocation, not locked before | the lock |
| D | one allocation, NullRendering | the DMA-buffer path |
| E | no allocations, one command the driver refuses | 0xc000000d means DxgkDdiRender was reached |
| F | node 1, the copy node | the node |

Each line prints the status and how many renders the driver accepted.

**Next step on the board (E), no reboot.** Boot with the probe again:
`wddm-probe set 2000 phys uart`, clean restart, HDMI as in D. Then, from
an administrator prompt:

```
m5test renderprobe > C:\mali\probe\renderprobe.txt
wddm-probe check
```

Send `C:\mali\probe\`. The boot's DxgKrnl ETW file (`boot.etl.*` of this
boot) is useful too.

What the six lines say:

- **B succeeds, A fails:** VidMm refuses our allocations at 2.0, most
  likely because they are in an aperture segment. The next change goes
  into CreateAllocation's segment answer.
- **All fail with 0xc0000001, E included:** the DMA-buffer side (acquire
  or CPU access) fails before any list is looked at. The next change goes
  into CreateContext's DMA-buffer description (DmaBufferSegmentSet 0,
  i.e. system memory).
- **F succeeds, A to E fail:** node 0 is set up differently from the copy
  node. Compare their CreateContext answers.
- **E returns 0xc000000d:** DxgkDdiRender is reachable, and the failure
  is in our Render's input at 2.0.

### 16.10 Run E (renderprobe): residency, the 2.x rule the UMD never kept

TaishanPi 3M, 2026-11-24, HDMI only, WDDM 2.0 up (logon sound, black
screen with signal). `m5test renderprobe` gave:

| | variant | Render | the driver accepted |
|---|---|---|---|
| A | one allocation, locked before | 0 | 0 |
| B | no allocation list | 0 | 0 |
| C | one allocation, not locked | 0 | 0 |
| D | one allocation, NullRendering | 0 | 0 |
| E | no allocations, one command the driver refuses | 0xc000000d | 0 |
| F | node 1 | 0 | 0 |

How to read it:

- A to D and F have no command (length 0). dxgkrnl completes those without
  calling DxgkDdiRender, so "accepted 0" says nothing about them.
- E's 0xc000000d is the driver's own refusal of the bad command:
  DxgkDdiRender is reachable.
- What differed in run D's failing render: a command and an allocation in
  the list together.

**dxgmms2 22621.2428, `VIDMM_GLOBAL::ReferenceDmaBuffer`.** It runs only
for a DMA buffer with a command. For each allocation in the list:

- it returns 0xc0000001 with the triage text "Failed to reference DMA
  buffer: Allocation is not requested to be resident" (it tests the
  allocation's residency-request count, `+0x98`);
- unless the allocation is pinned, or the adapter is in IoMmu mode.

WDDM 1.x paged in whatever the allocation list named. WDDM 2.x requires
user mode to call MakeResident (with a paging queue) on the rendering
device first. Neither our UMD nor m5test ever did, so at 2.0 every render
with a buffer in it was refused before the driver. DWM and LogonUI drew
nothing with the GPU, and the black screen follows from that.

**Build (`5c2164d` + `b84e2d9`):**

- KMD: escape `MW_ESCAPE_WDDM` returns the WDDM version and
  `MW_WDDM_NEEDS_RESIDENCY` at 2.x. A 1.3 KMD refuses the code, so the UMD
  stays as it was.
- m5test:
  - the M1 job is made resident first;
  - renderprobe gains G (one allocation, a command the driver refuses)
    and H (the same, made resident first). G should give 0xc0000001 and
    H 0xc000000d.
- UMD (`malikm_drm.c`, so a new `maliumd.dll`):
  - when the KMD asks for it, every buffer the shim creates or opens is
    made resident on the current device as it is made, through the
    runtime's `pfnCreatePagingQueueCb`/`pfnMakeResidentCb`, or the D3DKMT
    calls when not bound to a runtime device;
  - one paging queue per device, and the paging fence is waited for on
    the CPU;
  - nothing is evicted;
  - the log lines are `residency: ...`.
- Package: `wddm-mali-g52-driver-arm64` of CI run 45325052451 (`b925555`,
  Mesa run 45325052531), DriverVer 4.15.16.474; m5test in that run's
  `m3test-arm64` artifact.

**Next one shot (F), in two halves on one 2.0 boot.**

1. Install the new package at 1.3 (16.1), and check `WddmVersion 0x1300`.
   The desktop must be as before: at 1.3 the UMD makes nothing resident.
2. `mkdir C:\mali\log` (the UMD log; remove `C:\mali\log\off` if it is
   there), `wddm-probe set 2000 phys uart`, then a clean restart, HDMI
   only.
3. **First half, the rule:** `m5test renderprobe` gives G 0xc0000001 and
   H 0xc000000d. Then plain `m5test` should say `MakeResident: 0x00000000`
   and `PASS: the Mali ran a job submitted through WDDM`.
4. **Second half, the fix:** the screen. LogonUI and the desktop drawn on
   HDMI, `renders` above 0, `jobs done` climbing. Run `wddm-probe check`,
   then `echo > C:\mali\log\off`, and send `C:\mali\probe\` with
   `C:\mali\log\dwm.exe-*.txt` and `LogonUI.exe-*.txt`. Their
   `residency:` lines say whether the escape saw 2.x, whether a paging
   queue was made, and any MakeResident that failed.

The two halves are separate variables. A passing (3) with a still-black
(4) means the rule is right and the UMD's use of it is not.

### 16.11 Run F (build 345): GPU work at 2.0, then a divide by zero in dxgmms2

TaishanPi 3M, 2026-11-24, HDMI only, package DriverVer 4.15.16.474 (CI
run 45325052451, build 345). Files: `board-session-prep/wddm2x-stepF/`
(`dxgmms2.sys` and its public PDB, `dis.txt`, `publics.txt`,
`journal-boot1.txt`, `serial-tail.txt`, the bugcheck photo).

**Further than any 2.0 boot before it.** With MakeResident in the UMD the
GPU ran: 47 submissions, every fence completed. CreateAllocation,
OpenAllocation, Escape, BuildPagingBuffer (operation 5, map aperture
segment) and Patch all returned 0. Then:

- bugcheck 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED in dxgmms2.sys;
- P1 0xC0000094, integer divide by zero;
- P2 0xFFFFF8040B6C62C0.

**Where (dxgmms2 10.0.22621.2428).** P2 is RVA 0xE62C0, a `brk #0xf004`
(the ARM64 divide-by-zero trap) at
`VIDMM_COMMIT_TELEMETRY::LogMemoryState(_GUID*)+0x160`:

```
1c00e6274:  ldr   w8, [x7, #0x1b68]     ; physical adapter count
1c00e6290:  mov   w2, #0x0              ; D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL
1c00e6294:  mov   w1, w20               ; adapter index
1c00e629c:  bl    GetSegmentGroupState
1c00e62ac:  ldr   x12, [x15, #0x78]     ; usage
1c00e62b0:  ldr   x6,  [x15, #0x10]     ; budget
1c00e62b8:  mul   x8, x12, x25          ; usage * 100
1c00e62bc:  cbnz  x6, 1c00e62c4
1c00e62c0:  brk   #0xf004               ; budget 0
1c00e62c4:  udiv  x9, x8, x6
```

- Nothing before it tests the budget.
- A second division follows, of another group state, behind a test of a
  per-adapter flag that picks the string "Discrete" over "Integrated".
  On an integrated adapter, as ours, the local group is the only one
  divided.
- dxgmms2's budget configuration has an `L_` set (local only, e.g.
  `L_LocalMemoryBudgetDWMTarget`) next to the `LNL_` one (local and
  non-local). An integrated adapter is expected to have its memory in the
  local group.
- LogMemoryState is called from four events: LogLongPagingPacket,
  LogFullDefragPass, LogUnderBudgetButCantCommitEvent and
  LogNoCandidatesToSuspendFound. Which one fired is not on the photo. Any
  of them divides by the local budget.

**Why it was 0.** At 2.x our one segment is an aperture with
`NonLocalBudgetGroup` (flags 0x108005, journal `segment ... nonlocal 1`).
No segment was in the local group, so the local budget was 0. Earlier 2.0
boots never did enough paging to reach the telemetry.

**Build 349 (`902297c`):**

- At 2.x the segment has `LocalBudgetGroup` instead (flags 0x88005:
  aperture, CPU visible, application target, local budget group). On a
  UMA GPU all of its memory is the system's.
- 1.3 is unchanged. QUERYSEGMENT3 (`MwQuerySegment`) is not touched and
  the 1.3 plan has no budget flag (0x5). The host test checks both, and
  the lint checks that the 1.3 function has no budget flag.
- `Wddm2LocalBudget 0` (`wddm-probe ... nonlocalbudget`) gives run F's
  segment back. `nobudget` still drops both groups.
- The plan journal line now prints `local %u (flags 0x...)`. The
  QUERYSEGMENT4 fill line prints the flags dxgkrnl was given.
- Package: `wddm-mali-g52-driver-arm64` of CI run 45331666347 (build 349,
  Mesa run 45331666052), DriverVer 5.12.54.228.

Not chosen: a second, memory segment with `PopulatedFromSystemMemory` in
the local group, which is what Intel and AMD integrated drivers do with
their firmware carve-out. It changes segment ids, needs backing memory
and the paging operations of a memory segment. It is the next step if
VidMm refuses an aperture in the local group.

**Next one shot (G).** Against F the only change is the budget group.

1. Install the package at 1.3 (16.1) and check `WddmVersion 0x1300` and
   the desktop as before.
2. `mkdir C:\mali\log`, `wddm-probe set 2000 phys uart`, clean restart,
   HDMI only.
3. Run `m5test renderprobe` and `m5test` (as F), then `wddm-probe check`,
   `echo > C:\mali\log\off`, and send `C:\mali\probe\` with the dwm and
   LogonUI logs.

What the result says:

- **The journal has `local 1 (flags 0x88005)` and the QUERYSEGMENT4 fill
  pass `flags 0x00088005`:** the build is the one meant.
- **No 0x7E, LogonUI or the desktop on HDMI, `renders` climbing:** the
  local budget was it. Then Task Manager should show the GPU's memory as
  dedicated, about 1 GB, not shared. Run the 16.2 checks.
- **0x7E again at the same RVA:** the local group is still empty, so
  VidMm did not take the flag on an aperture. The next build is the
  memory segment above.
- **The adapter does not start (event 549, or a fallback with the journal
  ending at QAI 11):** VidMm refuses an aperture in the local group. Same
  next step.
- **Another bugcheck after more paging:** read the new journal. The event
  that called LogMemoryState (long paging packet, defrag, commit) is
  worth knowing then, since the paging may be slow or failing; a kernel
  debugger stack would name it.
- **To A/B on one install:** `wddm-probe set 2000 phys uart
  nonlocalbudget` must give run F's 0x7E.
