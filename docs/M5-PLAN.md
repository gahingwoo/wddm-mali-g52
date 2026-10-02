# M5: a WDDM driver, and DWM on the Mali

Written 2026-10-02, before M3 has run on the board. It turns "WDDM 2.0
driver, hardware UMD, DWM composites on the Mali" into steps, and records
what Microsoft's documentation settles and what it leaves open. Sources at
the end.

## Why M3's shape does not carry over as is

M3 works because d3d10umd is a *software* driver: an application loads it by
path, and it talks to our kernel driver privately. DWM does neither. It opens
the display adapter, whose kernel driver (KMD) is a WDDM miniport, and loads
the user-mode driver (UMD) that adapter's INF names. That UMD must get memory,
GPU addresses and submissions from the Direct3D runtime's callbacks
(`pfnAllocateCb`, `pfnMapGpuVirtualAddressCb`, `pfnSubmitCommandCb` and the
rest), never from a private device. And the KMD must also drive the display:
the earlier analysis (docs/PLAN.md) found that a render-only adapter next to
BasicDisplay leaves DWM on WARP.

So M5 is two new components, and most of M3 is reused inside them:

| | M3 | M5 |
|---|---|---|
| Kernel | `malikm`, KMDF, private IOCTLs | WDDM 2.0 miniport (`DxgkInitialize`), display and render |
| GPU memory | our page tables, one AS | VidMm's page tables (GpuMmu), one AS per process |
| User | d3d10umd + Panfrost + libdrm shim | same Mesa, with a WDDM `pan_kmod` backend on the runtime callbacks |
| Display | none (GDI copy) | VOP2 scan-out, flip by window address |

Reused unchanged or nearly: the GPU bring-up, job-slot and interrupt code
from `gpu.c`, the Mali page-table entry format, Panfrost and its Windows port
(16 Mesa patches, the MSVC-ABI fixes, the Present path for testing), and the
host-test approach (tests/malikm-host) for whatever logic can run without
the WDK.

## What the documentation settles

**GPU virtual memory: the GpuMmu model fits the Mali.** In GpuMmu the GPU has
its own MMU with per-process page tables. VidMm allocates and owns the page
tables; their hardware format is the driver's, behind DDIs. VidMm assigns no
addresses itself: the UMD maps allocations at addresses it chooses (or within
bounds it gives). The alternative, IoMmu, shares the CPU's page tables through
a system IOMMU with PASIDs, which the Mali's own MMU is not.

The page-table description maps onto Mali LPAE directly:

| `DXGK_GPUMMUCAPS` / `DXGK_PAGE_TABLE_LEVEL_DESC` | Mali LPAE |
|---|---|
| `PageTableLevelCount` 2..max; all levels fixed-size when > 2 | 4 levels, 512 entries each, 4 KB tables |
| `PageTableSegmentId` 0 = system memory, tables ≤ 4 KB there | tables in system memory, 4 KB |
| `PageTableUpdateMode` = `DXGK_PAGETABLEUPDATE_CPU_VIRTUAL` | the driver writes PTEs with the CPU, as malikm does |
| `ReadOnlyMemorySupported`, `NoExecuteMemorySupported` | read/write permission bits, XN (bit 54) |
| `PageTableUpdateRequireAddressSpaceIdle` | optional: the AS LOCK + FLUSH_PT sequence works on a live AS |
| `DXGK_PTE` (Valid, ReadOnly, NoExecute, Segment, PageAddress) | translated into `pa | 0x2C5 (| XN)`, tables `pa | 3` |

**Feature-level requirements are certification rules.** Microsoft's table for
WDDM 1.2+ says D3D10-class hardware must ship the D3D9, D3D10 and D3D11.1 UMD
DDIs. That is what a driver needs to be certified, not necessarily what DWM
needs to run. CI already shows the D3D11 runtime creating a device at feature
level 10_0 on d3d10umd, which implements only the D3D10 and D3D10.1 DDIs.

## Open questions, in the order they can kill the plan

1. **Does DWM accept a UMD with only the D3D10/10.1 DDI?** If DWM insists on
   the D3D11 DDI (or D3D9Ex) from a hardware adapter, d3d10umd needs a D3D11
   DDI table before any desktop appears, a large job on an unmaintained
   frontend. The test is cheap once the M5.2 driver loads: run a D3D11
   hardware-adapter program, then check which adapter DWM picks.
2. **The VA width.** The G52 reports 35 VA bits (`MMU_FEATURES` 0x2823), but
   Mali LPAE always walks four levels. Plan: describe three 9-bit levels to
   VidMm (39 bits, a 4 KB root) and keep a private level-0 page per address
   space whose entry 0 points at VidMm's root. `DxgkDdiSetRootPageTable` then
   rewrites one entry, and the UMD maps below 2^35 (it chooses the
   addresses). Whether VidMm itself ever places mappings above 2^35 (paging
   process, system context) needs checking against the DDK headers and
   behaviour.
3. **Scan-out memory.** VOP2 takes 32-bit physical addresses and, without its
   own IOMMU programmed, contiguous ones. Plan: the firmware reserves a
   contiguous block below 4 GB, and the KMD reports it as a CPU-visible
   memory segment; primaries are allocated there. RK3576's VOP2 does have an
   IOMMU (Linux uses it), the alternative if a carve-out is a problem.
4. **The paging engine.** VidMm submits paging buffers (fill, transfer,
   UpdatePageTable) to an engine. The Mali has no DMA engine of the kind; a
   software paging node that runs these on the CPU at submit time and
   completes them at once is the plan. To check: the rules for a node that is
   not hardware.

## Milestones

| | Milestone | Done when |
|---|---|---|
| M5.1 | Display-only driver (KMDOD) for VOP2 | it replaces BasicDisplay, inherits the firmware's mode, and presents by writing the framebuffer; the display half of the KMD exists and is tested alone |
| M5.2 | WDDM 2.0 KMD: M5.1 + one 3D node (GpuMmu, CPU-updated page tables, `DxgkDdiSubmitCommandVirtual` onto the job slots, DMA-completed interrupts) + software paging node | the adapter starts with Panfrost's UMD loaded; question 1 answered |
| M5.3 | UMD: Panfrost's WDDM `pan_kmod` backend (allocations through `pfnAllocateCb`, VAs through `pfnMapGpuVirtualAddressCb`, submits through `pfnSubmitCommandCb`, monitored fences) | the M4 triangle on `D3D_DRIVER_TYPE_HARDWARE` |
| M5.4 | Flip: primaries in the scan-out segment, `DxgkDdiSetVidPnSourceAddress` writes the VOP2 window address | a full-screen D3D11 program flips without tearing |
| M5.5 | DWM on the Mali | the desktop composites on the GPU, measured (GPU job counters vs WARP's CPU time) |

**M5.1 done on the board (2026-10-02):** `drivers/malidod` drives the HDMI output under Windows ([result](results/m5.1-board-2026-10-02.md)). It takes the firmware framebuffer, offers that one mode and presents by copying dirty rectangles, as Basic Display does; it does not touch VOP2 yet.

M5.1 needs only the board's display and is worth doing early: it fails or
succeeds independently of the GPU, and its code is the display half of M5.2.

## Testing without the board

- CI builds the KMD with the WDK (as for m1probe and malikm) and runs
  InfVerif on the INF.
- The page-table translation (DXGK_PTE to Mali PTE, the private level-0 page)
  gets a host test on the simulated G52, as malikm has.
- The WDDM `pan_kmod` backend can be exercised against a fake runtime in the
  same way MALIKM_FAKE exercises the shim today.

## Sources

- [GpuMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)
- [IoMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-model)
- [DXGK_GPUMMUCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_gpummucaps)
- [DXGK_PAGE_TABLE_LEVEL_DESC](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_page_table_level_desc)
- [DXGK_PTE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_dxgk_pte)
- [DXGKDDI_SETROOTPAGETABLE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setrootpagetable)
- [Direct3D software requirements (WDDM 1.2+)](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/software-requirements)
