// SPDX-License-Identifier: GPL-2.0
/*
 * What a WDDM 2.x driver has to say that a 1.3 one does not (docs/WDDM2.md,
 * P1). Nothing here runs unless WddmProbe is in effect (version.c); a 1.3
 * driver is byte for byte what it was.
 *
 * Two shapes, chosen by Services\maliwddm\Parameters\Wddm2Shape:
 *
 *   0  physical mode, corrected. Two nodes: 0 is the Mali job manager, 1 is
 *      a copy node that is the paging node. Its paging buffers are done on
 *      the CPU while they are built (memory.c), so a submission on it
 *      completes at once. No GPU virtual addressing.
 *   1  the same, and GpuMmu: VirtualAddressingSupported, the page-table
 *      description, CreateProcess, SetRootPageTable, SubmitCommandVirtual
 *      and the page-table paging operations. The Mali still runs only
 *      physical-mode work on AS0; per-process address spaces come in P2.
 *      This shape asks whether dxgkrnl starts this adapter as GpuMmu.
 *
 * The interface facts this follows (not code) were learned from the two
 * WinMali drivers in github.com/awesomefroggy29/Panorama: Jamie's
 * claude-g615 (Mali-G615, a GPU-composited desktop at WDDM 2.0 with GpuMmu)
 * and mr4g's codex-g610 (Mali-G610). Both authors gave permission on
 * 2026-11-22 to use their work under GPL-2.0 with credit; the records are
 * kept by the project. In short:
 *   - the paging node must be a real node of its own, a copy engine: with
 *     PagingNodeIndex naming the 3D node dxgkrnl gives up at StartDevice on
 *     Windows 11 ARM64, and naming a node that does not exist makes a
 *     phantom node that crashes the scheduler later;
 *   - BuildPagingBuffer may return only STATUS_SUCCESS or
 *     STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER (anything else is bugcheck
 *     0x10E), and when it is given a DMA buffer something should be put in
 *     it;
 *   - a GpuMmu driver's paging buffers arrive through SubmitCommandVirtual,
 *     which must not be a stub (0x119);
 *   - PageTableAlignmentInBytes must not be 0 (a divide by zero in dxgmms2);
 *   - GetNodeMetadata's ordinal carries the physical adapter in its top 16
 *     bits, and its GpuMmuSupported must agree with the adapter's caps;
 *   - a display driver reporting WDDM 2.3 or later has a lost VidPN source
 *     torn down instead of paused, and 2.2 hung at LogonUI there: 2.0 is the
 *     version that worked for a display.
 */
#include "maliwddm.h"
#include "wddm2.h"

#define FITS(Size, Type, Field) (FIELD_OFFSET(Type, Field) + RTL_FIELD_SIZE(Type, Field) <= (Size))

/* ---- the copy node ---- */

static volatile LONG CopyFence;     /* last fence completed on node 1 */
static volatile LONG CopySubmits;
static volatile LONG CopyPreempts;  /* preemption requests on node 1 */

/* Every node count, paging node and GpuMmu bit below comes from MwPlan
 * (wddm2plan.h, set in DriverEntry), whose rules tests/wddm2-host checks. */
ULONG MwNodeCount(void)
{
    return MwPlan.Nodes != 0 ? MwPlan.Nodes : 1;
}

BOOLEAN MwGpuMmu(void)
{
    return MwWddmProbe != 0 && MwPlan.GpuMmu != 0;
}

typedef struct {
    MW_ADAPTER *A;
    ULONG Fence;
} MW_COPY_DONE;

static BOOLEAN SyncCopyDone(PVOID Context)
{
    MW_COPY_DONE *c = (MW_COPY_DONE *)Context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    n.DmaCompleted.SubmissionFenceId = c->Fence;
    n.DmaCompleted.NodeOrdinal = MW_COPY_NODE;
    n.DmaCompleted.EngineOrdinal = 0;
    MwRing(MW_RING_DMA_DONE, MW_COPY_NODE, c->Fence);
    c->A->Dxgk.DxgkCbNotifyInterrupt(c->A->Dxgk.DeviceHandle, &n);
    c->A->LastNotifiedFence = c->Fence;
    c->A->LastNotifiedNode = MW_COPY_NODE;
    InterlockedExchange(&CopyFence, (LONG)c->Fence);
    c->A->Dxgk.DxgkCbQueueDpc(c->A->Dxgk.DeviceHandle);
    return TRUE;
}

/* A submission on the copy node: its work (paging) was done on the CPU when
 * it was built, so it is complete now. */
NTSTATUS MwCopyNodeSubmit(MW_ADAPTER *A, ULONG Fence)
{
    MW_COPY_DONE c;
    BOOLEAN ret = FALSE;

    c.A = A;
    c.Fence = Fence;
    InterlockedIncrement(&CopySubmits);
    return A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, SyncCopyDone, &c, 0, &ret);
}

ULONG MwCopyNodeFence(void)
{
    return (ULONG)CopyFence;
}

/* A preemption request on the copy node. Nothing is ever in flight there
 * (a submission completes inside SubmitCommand), so it is answered at once:
 * DMA_PREEMPTED with the last completed fence. Left unanswered, VidSch
 * waits on node 1 forever; answered as if for node 0 (the 3D path), the
 * wrong node's state moves. */
typedef struct {
    MW_ADAPTER *A;
    ULONG PreemptionFence;
} MW_COPY_PREEMPT;

static BOOLEAN SyncCopyPreempt(PVOID Context)
{
    MW_COPY_PREEMPT *p = (MW_COPY_PREEMPT *)Context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    n.DmaPreempted.PreemptionFenceId = p->PreemptionFence;
    n.DmaPreempted.LastCompletedFenceId = (UINT)CopyFence;
    n.DmaPreempted.NodeOrdinal = MW_COPY_NODE;
    n.DmaPreempted.EngineOrdinal = 0;
    MwRing(MW_RING_PREEMPTED, MW_COPY_NODE, p->PreemptionFence);
    p->A->Dxgk.DxgkCbNotifyInterrupt(p->A->Dxgk.DeviceHandle, &n);
    p->A->Dxgk.DxgkCbQueueDpc(p->A->Dxgk.DeviceHandle);
    return TRUE;
}

NTSTATUS MwCopyNodePreempt(MW_ADAPTER *A, ULONG PreemptionFence)
{
    MW_COPY_PREEMPT p;
    BOOLEAN ret = FALSE;

    p.A = A;
    p.PreemptionFence = PreemptionFence;
    InterlockedIncrement(&CopyPreempts);
    return A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, SyncCopyPreempt, &p, 0, &ret);
}

/* ---- DRIVERCAPS: what a 2.x driver adds ---- */

void Mw2xDriverCaps(MW_ADAPTER *A, DXGK_DRIVERCAPS *Caps, UINT Size)
{
    const MW2_PLAN *p = &MwPlan;

    UNREFERENCED_PARAMETER(A);
    if (MwWddmProbe == 0)
        return;
    /* Two nodes, node 1 the paging node (above). */
    if (FITS(Size, DXGK_DRIVERCAPS, GpuEngineTopology))
        Caps->GpuEngineTopology.NbAsymetricProcessingNodes = p->Nodes;
    if (FITS(Size, DXGK_DRIVERCAPS, MemoryManagementCaps))
        Caps->MemoryManagementCaps.PagingNode = p->PagingNode;
    /* Implemented in command.c (and for node 1 here); the WDDM 1.2 table
     * calls it mandatory, and both WinMali drivers set it. */
    if (FITS(Size, DXGK_DRIVERCAPS, SupportPerEngineTDR))
        Caps->SupportPerEngineTDR = p->PerEngineTdr ? TRUE : FALSE;
    if (FITS(Size, DXGK_DRIVERCAPS, SchedulingCaps)) {
        Caps->SchedulingCaps.MultiEngineAware = p->MultiEngineAware;
        Caps->SchedulingCaps.CancelCommandAware = p->CancelCommandAware; /* MwCancelCommand: nothing queued in hardware to cancel */
    }
    if (FITS(Size, DXGK_DRIVERCAPS, MemoryManagementCaps)) {
        Caps->MemoryManagementCaps.VirtualAddressingSupported = p->VirtualAddressing;
        Caps->MemoryManagementCaps.GpuMmuSupported = p->GpuMmu;
    }
    if (!p->GpuMmu)
        return;
    /* VidMm's own GPU VAs (paging process, fences) above Panfrost's range
     * (32 MB to 4 GB) and below the G52's 35-bit limit. */
    if (FITS(Size, DXGK_DRIVERCAPS, InternalGpuVirtualAddressRangeStart))
        Caps->InternalGpuVirtualAddressRangeStart = p->InternalVaStart;
    if (FITS(Size, DXGK_DRIVERCAPS, InternalGpuVirtualAddressRangeEnd))
        Caps->InternalGpuVirtualAddressRangeEnd = p->InternalVaEnd;
}

/* ---- QAI ---- */

/* DXGKQAITYPE_PHYSICALADAPTERCAPS (15). The first 2.0 probe (CI run 283)
 * answered this with status 0 and two wrong values, and hung at the boot
 * spinner right after:
 *   - NumExecutionNodes 1 and PagingNodeIndex 0, the 3D node. Jamie found
 *     on hardware that dxgkrnl gives up on that (WDDM2.md, section 12);
 *   - DxgkPhysicalAdapterHandle = our own adapter context. Microsoft's
 *     reference says it is the handle "passed to the kernel mode driver as
 *     DXGKRNL_INTERFACE::DeviceHandle in DxgkDdiStartDevice", that is,
 *     dxgkrnl's own handle, which dxgkrnl then uses as its own. Jamie's
 *     driver returns that one.
 * dxgkrnl 22631 asks with OutputDataSize 0x14, which ends at Flags: the
 * fields after it are written only if they fit. */
C_ASSERT(RTL_SIZEOF_THROUGH_FIELD(DXGK_PHYSICALADAPTERCAPS, Flags) == 0x14);

NTSTATUS MwQueryPhysicalAdapterCaps(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_PHYSICALADAPTERCAPS *c = (DXGK_PHYSICALADAPTERCAPS *)Info->pOutputData;
    UINT size = Info->OutputDataSize;
    const MW2_PLAN *p = &MwPlan;

    if (c == NULL || size < RTL_SIZEOF_THROUGH_FIELD(DXGK_PHYSICALADAPTERCAPS, Flags))
        return STATUS_BUFFER_TOO_SMALL;
    if (Info->pInputData != NULL && Info->InputDataSize >= sizeof(DXGK_QUERYPHYSICALADAPTERCAPSIN) &&
        ((const DXGK_QUERYPHYSICALADAPTERCAPSIN *)Info->pInputData)->PhysicalAdapterIndex != 0)
        return STATUS_INVALID_PARAMETER;
    /* dxgkrnl asks after StartDevice; without its handle there is no right
     * answer, and a made-up one is what the first probe gave. */
    if (p->HandleIsDxgkrnls && A->Dxgk.DeviceHandle == NULL)
        return STATUS_DEVICE_NOT_READY;
    RtlZeroMemory(c, size);
    c->NumExecutionNodes = (WORD)p->Nodes;
    c->PagingNodeIndex = (WORD)p->PagingNode;
    c->DxgkPhysicalAdapterHandle = A->Dxgk.DeviceHandle;
    c->Flags.GpuMmuSupported = p->GpuMmu;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_1)
    /* No video protected region; name a real node, not a phantom one. */
    if (FITS(size, DXGK_PHYSICALADAPTERCAPS, VPRPagingNode))
        c->VPRPagingNode = p->VprPagingNode;
#endif
    return STATUS_SUCCESS;
}

/* The G52's page tables, described to VidMm: the legacy Mali LPAE format
 * this driver already uses (maligpu.c, TRANSCFG 0) always walks four levels
 * of 9 bits over 4 KB tables, 48 bits in all. VidMm's levels are the same
 * four. The hardware translates only 35 of those bits (MMU_FEATURES 0x23),
 * so VidMm's own addresses are kept below 2^35 by the internal range in
 * DRIVERCAPS, and the UMD's by its own bounds (docs/WDDM2.md 13.2). */
NTSTATUS MwQueryGpuMmuCaps(const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_GPUMMUCAPS *c = (DXGK_GPUMMUCAPS *)Info->pOutputData;
    UINT size = Info->OutputDataSize;

    if (!MwGpuMmu())
        return STATUS_NOT_SUPPORTED;
    if (c == NULL || size < RTL_SIZEOF_THROUGH_FIELD(DXGK_GPUMMUCAPS, PageTableLevelCount))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(c, size);
    c->ReadOnlyMemorySupported = 1;
    c->NoExecuteMemorySupported = 1;
    c->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    c->VirtualAddressBitCount = MwPlan.VaBits;
    c->PageTableLevelCount = MwPlan.PtLevels;
    return STATUS_SUCCESS;
}

NTSTATUS MwQueryPageTableLevelDesc(const DXGKARG_QUERYADAPTERINFO *Info)
{
    const DXGK_QUERYPAGETABLELEVELDESCIN *in = (const DXGK_QUERYPAGETABLELEVELDESCIN *)Info->pInputData;
    DXGK_PAGE_TABLE_LEVEL_DESC *d = (DXGK_PAGE_TABLE_LEVEL_DESC *)Info->pOutputData;

    if (!MwGpuMmu())
        return STATUS_NOT_SUPPORTED;
    if (in == NULL || Info->InputDataSize < sizeof(*in) || in->PhysicalAdapterIndex != 0 ||
        in->LevelIndex >= MwPlan.PtLevels)
        return STATUS_INVALID_PARAMETER;
    if (d == NULL || Info->OutputDataSize < sizeof(*d))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(d, Info->OutputDataSize);
    d->PageTableIndexBitCount = MwPlan.PtIndexBits;
    d->PageTableSegmentId = MwGpuMmuPtSegment;
    d->PagingProcessPageTableSegmentId = MwGpuMmuPtSegment;
    d->PageTableSizeInBytes = MwPlan.PtSize;
    d->PageTableAlignmentInBytes = MwPlan.PtAlign;  /* never 0: dxgmms2 divides by it */
    return STATUS_SUCCESS;
}

/* WDDM 3.x asks these before StartDevice; a 1.3 or 2.0 driver is not asked.
 * Honest answers: 40 physical address bits, no IOMMU in front of the GPU,
 * no reserved ranges, and the version echoed. */
NTSTATUS MwQuery3xCaps(const DXGKARG_QUERYADAPTERINFO *Info)
{
    PVOID out = Info->pOutputData;
    UINT size = Info->OutputDataSize;

    if (out == NULL || size == 0)
        return STATUS_BUFFER_TOO_SMALL;
    switch (Info->Type) {
    case DXGKQAITYPE_WDDMDEVICECAPS:
        RtlZeroMemory(out, size);
        if (size < sizeof(DXGK_WDDMDEVICECAPS))
            return STATUS_BUFFER_TOO_SMALL;
        ((DXGK_WDDMDEVICECAPS *)out)->WDDMVersion = (DXGK_WDDMVERSION)MwWddmVersion;
        return STATUS_SUCCESS;
    case DXGKQAITYPE_PHYSICAL_MEMORY_CAPS:
        RtlZeroMemory(out, size);
        if (size < sizeof(DXGK_PHYSICAL_MEMORY_CAPS))
            return STATUS_BUFFER_TOO_SMALL;
        ((DXGK_PHYSICAL_MEMORY_CAPS *)out)->HighestVisibleAddress.QuadPart = (1LL << 40) - 1;
        return STATUS_SUCCESS;
    case DXGKQAITYPE_IOMMU_CAPS:
        RtlZeroMemory(out, size);
        return STATUS_SUCCESS;
    case DXGKQAITYPE_HARDWARERESERVEDRANGES:
    case DXGKQAITYPE_HARDWARERESERVEDRANGES2:
        if (size < sizeof(DXGK_HARDWARERESERVEDRANGES))
            return STATUS_BUFFER_TOO_SMALL;
        ((DXGK_HARDWARERESERVEDRANGES *)out)->NumRanges = 0;   /* the range array is dxgkrnl's */
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* ---- the GpuMmu DDIs (shape 1) ----
 * Recorded, not yet used by the Mali (P2 binds roots to address spaces). */

static volatile LONG Processes, RootSets, VirtSubmits, VirtSubmits3d;

typedef struct {
    HANDLE hDxgkProcess;
    ULONG Flags;
} MW_PROCESS;

NTSTATUS APIENTRY MwCreateProcess(IN_CONST_HANDLE hAdapter, DXGKARG_CREATEPROCESS *Arg)
{
    MW_PROCESS *p = (MW_PROCESS *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*p), MW_TAG);

    UNREFERENCED_PARAMETER(hAdapter);
    if (p == NULL)
        return STATUS_NO_MEMORY;
    p->hDxgkProcess = Arg->hDxgkProcess;
    p->Flags = Arg->Flags.Value;
    Arg->hKmdProcess = p;
    InterlockedIncrement(&Processes);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyProcess(IN_CONST_HANDLE hAdapter, IN_CONST_HANDLE hKmdProcess)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (hKmdProcess != NULL)
        ExFreePoolWithTag(hKmdProcess, MW_TAG);
    InterlockedDecrement(&Processes);
    return STATUS_SUCCESS;
}

VOID APIENTRY MwSetRootPageTable(IN_CONST_HANDLE hAdapter, const DXGKARG_SETROOTPAGETABLE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    InterlockedIncrement(&RootSets);
}

SIZE_T APIENTRY MwGetRootPageTableSize(IN_CONST_HANDLE hAdapter, DXGKARG_GETROOTPAGETABLESIZE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    /* Only asked of a 2-level description; ours is fixed at 512 entries. */
    Arg->NumberOfPte = 1u << MW_GPUMMU_INDEX_BITS;
    return PAGE_SIZE;
}

/* Paging buffers of a GpuMmu driver come here (on node 1): done when built,
 * so complete. Anything on node 0 goes through the ordinary queue as a
 * submission with no jobs, so fences complete in order: no UMD submits in
 * virtual mode yet, and the count says if one did. */
NTSTATUS APIENTRY MwSubmitCommandVirtual(IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMANDVIRTUAL *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    DXGKARG_SUBMITCOMMAND s;

    InterlockedIncrement(&VirtSubmits);
    if (Arg->NodeOrdinal == MW_COPY_NODE)
        return MwCopyNodeSubmit(a, Arg->SubmissionFenceId);
    InterlockedIncrement(&VirtSubmits3d);
    RtlZeroMemory(&s, sizeof(s));
    s.hContext = Arg->hContext;
    s.SubmissionFenceId = Arg->SubmissionFenceId;
    s.Flags = Arg->Flags;
    s.NodeOrdinal = Arg->NodeOrdinal;
    s.EngineOrdinal = Arg->EngineOrdinal;
    return MwSubmitCommand(hAdapter, &s);
}

/* ---- the two DDIs every 2.x interface must fill in ----
 * dxgkrnl refuses the adapter start without them at any interface from
 * 0x5008 on (wddm2plan.h, Mw2NeedsPowerDdis). The first two 2.0 probes on
 * the TaishanPi (2026-11-23) were refused there, after QAI 16. */

/* History-buffer timestamps against the CPU clock, at DISPATCH_LEVEL and
 * as often as every 30 ms. This driver writes no GPU timestamps into
 * history buffers, so GPU time is modelled as the performance counter
 * itself: the same counter read once for both, an exact correlation. */
static volatile LONG Calibrations, StablePowerOn;

NTSTATUS APIENTRY MwCalibrateGpuClock(IN_CONST_HANDLE hAdapter, IN UINT32 NodeOrdinal, IN UINT32 EngineOrdinal,
                                      DXGKARG_CALIBRATEGPUCLOCK *Arg)
{
    LARGE_INTEGER hz, now;

    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(EngineOrdinal);
    if (Arg == NULL || NodeOrdinal >= MwNodeCount())
        return STATUS_INVALID_PARAMETER;
    now = KeQueryPerformanceCounter(&hz);
    Arg->GpuFrequency = (UINT64)hz.QuadPart;
    Arg->GpuClockCounter = (UINT64)now.QuadPart;
    Arg->CpuClockCounter = (UINT64)now.QuadPart;
    InterlockedIncrement(&Calibrations);
    return STATUS_SUCCESS;
}

/* "Stable power" for profiling: the clock is fixed already (GpuClk, set
 * once at StartDevice; no DVFS here), so there is nothing to pin. */
VOID APIENTRY MwSetStablePowerState(IN_CONST_HANDLE hAdapter, const DXGKARG_SETSTABLEPOWERSTATE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (Arg != NULL)
        InterlockedExchange(&StablePowerOn, Arg->Enabled ? 1 : 0);
}

/* ---- a path's rotation, past path-independent rotation ----
 * From display.c and hpd.c, after Identity and Rotate90..270 are set.
 * Offset0 on every path at 2.x (wddm2plan.h, Mw2RotationSupportFault):
 * without it dxgkrnl bugchecks 0x113/0x1C on a test-signed system. */
void Mw2xRotationSupport(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT *R)
{
    if (MwPlan.RotationOffset0 && Mw2PathIndependentRotation(MwDdiInterface))
        R->Offset0 = 1;
}

/* ---- paging operations a 2.x VidMm asks for ---- */

/* The legacy Mali LPAE encoding (maligpu.h): leaf pages pa | 0x2C5 (block
 * type, ATTRINDX 1, read, write, outer shareable), XN at bit 54; tables
 * pa | 3. ReadOnly clears the write bit 0x80. */
static ULONG64 MaliPte(const DXGK_PTE *P, BOOLEAN Leaf)
{
    ULONG64 pa = P->PageAddress & 0x000000FFFFFFF000ULL;

    if (!P->Valid)
        return 0;
    if (!Leaf)
        return pa | MK_PTE_TABLE;
    pa |= MK_PTE_PAGE;
    if (P->ReadOnly)
        pa &= ~0x80ULL;
    if (P->NoExecute)
        pa |= MK_PTE_XN;
    return pa;
}

static volatile LONG PtUpdates, PtEntries, PtRefusedMode;

static void UpdatePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *U)
{
    ULONG64 *pt;
    /* VidMm's level 0 is the leaf (as both WinMali drivers found). */
    BOOLEAN leaf = U->PageTableLevel == 0;

    InterlockedIncrement(&PtUpdates);
    if (U->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL || U->PageTableAddress.CpuVirtual == NULL ||
        U->pPageTableEntries == NULL || U->Flags.Use64KBPages) {
        InterlockedIncrement(&PtRefusedMode);
        return;
    }
    pt = (ULONG64 *)U->PageTableAddress.CpuVirtual + U->StartIndex;
    for (UINT i = 0; i < U->NumPageTableEntries; i++)
        pt[i] = MaliPte(&U->pPageTableEntries[U->Flags.Repeat ? 0 : i], leaf);
    InterlockedAdd(&PtEntries, (LONG)U->NumPageTableEntries);
    KeMemoryBarrier();
}

/* From MwBuildPagingBuffer when WddmProbe is in effect: everything the
 * 1.3 path does not know. Returns TRUE if handled. Never a failure status:
 * dxgmms2 bugchecks on one (0x10E). */
BOOLEAN Mw2xPagingOp(MW_ADAPTER *A, DXGKARG_BUILDPAGINGBUFFER *Arg)
{
    UNREFERENCED_PARAMETER(A);
    switch (Arg->Operation) {
    case DXGK_OPERATION_UPDATE_PAGE_TABLE:
        UpdatePageTable(&Arg->UpdatePageTable);
        return TRUE;
    case DXGK_OPERATION_FLUSH_TLB:
        /* No process address space is bound to a Mali AS yet (P2). */
        return TRUE;
    default:
        /* NOTIFY_RESIDENCY, COPY_PAGE_TABLE_ENTRIES,
         * UPDATE_CONTEXT_ALLOCATION, the WDDMv1 transfer and fill, the
         * virtual transfer and fill: nothing to do with one aperture
         * segment and no virtual GPU work. Counted by probe.c. */
        return TRUE;
    }
}

/* A paging DMA buffer handed over is given something in it: an 8-byte zero
 * marker that SubmitCommand ignores (paging submissions carry no jobs). */
NTSTATUS Mw2xPagingFill(DXGKARG_BUILDPAGINGBUFFER *Arg)
{
    if (Arg->pDmaBuffer == NULL || Arg->DmaSize == 0 || MwPagingMarker == 0)
        return STATUS_SUCCESS;
    if (Arg->DmaSize < 8)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    RtlZeroMemory(Arg->pDmaBuffer, 8);
    Arg->pDmaBuffer = (PUCHAR)Arg->pDmaBuffer + 8;
    Arg->DmaSize -= 8;
    return STATUS_SUCCESS;
}

/* For the boot journal (probe.c): what a QAI was answered, field by field
 * for the ones a 2.x dxgkrnl decides by. From MwQueryAdapterInfo. */
void Mw2xBcQai(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info, NTSTATUS St)
{
    ULONG t = (ULONG)Info->Type;
    UINT size = Info->OutputDataSize;

    if (!MwBcOn())
        return;
    if (!NT_SUCCESS(St) || Info->pOutputData == NULL) {
        MwBc("QAI %u answered 0x%08x", t, (ULONG)St);
        return;
    }
    switch (Info->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        const DXGK_DRIVERCAPS *c = (const DXGK_DRIVERCAPS *)Info->pOutputData;
        MwBc("QAI 1 DRIVERCAPS answered 0: WDDMVersion 0x%x, nodes %u, PagingNode %u, VA %u, GpuMmu %u, "
             "Scheduling 0x%08x, PerEngineTDR %u, SectionBackedPrimary %u",
             (ULONG)c->WDDMVersion, c->GpuEngineTopology.NbAsymetricProcessingNodes,
             c->MemoryManagementCaps.PagingNode, c->MemoryManagementCaps.VirtualAddressingSupported,
             c->MemoryManagementCaps.GpuMmuSupported, c->SchedulingCaps.Value,
             FITS(size, DXGK_DRIVERCAPS, SupportPerEngineTDR) ? (ULONG)c->SupportPerEngineTDR : 0xFFu,
             c->MemoryManagementCaps.SectionBackedPrimary);
        return;
    }
    case DXGKQAITYPE_QUERYSEGMENT4: {
        const DXGK_QUERYSEGMENTOUT4 *o = (const DXGK_QUERYSEGMENTOUT4 *)Info->pOutputData;
        const DXGK_SEGMENTDESCRIPTOR4 *d = (const DXGK_SEGMENTDESCRIPTOR4 *)o->pSegmentDescriptor;
        if (d == NULL) {
            MwBc("QAI 11 QUERYSEGMENT4 answered 0 (count pass): NbSegment %u", o->NbSegment);
            return;
        }
        MwBc("QAI 11 QUERYSEGMENT4 answered 0 (fill pass): NbSegment %u, stride %u, base 0x%I64x, "
             "size 0x%I64x, commit 0x%I64x, flags 0x%08x, paging buffer segment %u size %u private %u",
             o->NbSegment, (ULONG)o->SegmentDescriptorStride, d->BaseAddress.QuadPart, (ULONG64)d->Size,
             (ULONG64)d->CommitLimit, d->Flags.Value, o->PagingBufferSegmentId, o->PagingBufferSize,
             o->PagingBufferPrivateDataSize);
        return;
    }
    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        const DXGK_PHYSICALADAPTERCAPS *c = (const DXGK_PHYSICALADAPTERCAPS *)Info->pOutputData;
        MwBc("QAI 15 PHYSICALADAPTERCAPS answered 0: nodes %u, PagingNodeIndex %u, handle %p (%hs), "
             "flags 0x%08x, out %u bytes",
             c->NumExecutionNodes, c->PagingNodeIndex, c->DxgkPhysicalAdapterHandle,
             c->DxgkPhysicalAdapterHandle == A->Dxgk.DeviceHandle ? "dxgkrnl's" : "NOT dxgkrnl's",
             c->Flags.Value, size);
        return;
    }
    case DXGKQAITYPE_GPUMMUCAPS: {
        const DXGK_GPUMMUCAPS *c = (const DXGK_GPUMMUCAPS *)Info->pOutputData;
        MwBc("QAI 13 GPUMMUCAPS answered 0: VA bits %u, levels %u, update mode %u",
             c->VirtualAddressBitCount, c->PageTableLevelCount, (ULONG)c->PageTableUpdateMode);
        return;
    }
    case DXGKQAITYPE_PAGETABLELEVELDESC: {
        const DXGK_PAGE_TABLE_LEVEL_DESC *d = (const DXGK_PAGE_TABLE_LEVEL_DESC *)Info->pOutputData;
        MwBc("QAI 14 PAGETABLELEVELDESC answered 0: index bits %u, segment %u, size %u, align %u",
             d->PageTableIndexBitCount, d->PageTableSegmentId, (ULONG)d->PageTableSizeInBytes,
             (ULONG)d->PageTableAlignmentInBytes);
        return;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        const DXGKARG_HISTORYBUFFERPRECISION *h = (const DXGKARG_HISTORYBUFFERPRECISION *)Info->pOutputData;
        ULONG n = Mw2PerNodeEntries(size, sizeof(*h));
        MwBc("QAI 10 HISTORYBUFFERPRECISION answered 0: %u entries (nodes %u), bits %u, %u",
             n, MwPlan.Nodes, h[0].PrecisionBits, n > 1 ? h[1].PrecisionBits : 0xFFFFFFFFu);
        return;
    }
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        MwBc("QAI 16 DISPLAY_DRIVERCAPS_EXTENSION answered 0: 0x%08x (%u bytes)",
             size >= sizeof(ULONG) ? *(const ULONG *)Info->pOutputData : 0u, size);
        return;
    default:
        MwBc("QAI %u answered 0", t);
        return;
    }
}

/* For the watchdog. */
void Mw2xCounters(ULONG *Out, ULONG Count)
{
    ULONG v[9];
    v[0] = (ULONG)CopySubmits;
    v[1] = (ULONG)CopyFence;
    v[2] = (ULONG)Processes;
    v[3] = (ULONG)RootSets;
    v[4] = (ULONG)VirtSubmits;
    v[5] = (ULONG)VirtSubmits3d;
    v[6] = (ULONG)PtEntries;
    v[7] = (ULONG)PtRefusedMode;
    v[8] = (ULONG)CopyPreempts;
    for (ULONG i = 0; i < Count && i < 9; i++)
        Out[i] = v[i];
}
