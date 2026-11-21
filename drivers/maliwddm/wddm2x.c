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

ULONG MwNodeCount(void)
{
    return MwWddmProbe != 0 ? 2 : 1;
}

BOOLEAN MwGpuMmu(void)
{
    return MwWddmProbe != 0 && MwWddm2Shape == MW_SHAPE_GPUMMU;
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
    c->A->Dxgk.DxgkCbNotifyInterrupt(c->A->Dxgk.DeviceHandle, &n);
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

/* ---- DRIVERCAPS: what a 2.x driver adds ---- */

void Mw2xDriverCaps(MW_ADAPTER *A, DXGK_DRIVERCAPS *Caps, UINT Size)
{
    UNREFERENCED_PARAMETER(A);
    if (MwWddmProbe == 0)
        return;
    /* Two nodes, node 1 the paging node (above). */
    if (FITS(Size, DXGK_DRIVERCAPS, GpuEngineTopology))
        Caps->GpuEngineTopology.NbAsymetricProcessingNodes = MwNodeCount();
    if (FITS(Size, DXGK_DRIVERCAPS, MemoryManagementCaps))
        Caps->MemoryManagementCaps.PagingNode = MW_COPY_NODE;
    /* Implemented in command.c (and for node 1 here); the WDDM 1.2 table
     * calls it mandatory, and both WinMali drivers set it. */
    if (FITS(Size, DXGK_DRIVERCAPS, SupportPerEngineTDR))
        Caps->SupportPerEngineTDR = TRUE;
    if (FITS(Size, DXGK_DRIVERCAPS, SchedulingCaps))
        Caps->SchedulingCaps.CancelCommandAware = 1;    /* MwCancelCommand: nothing queued in hardware to cancel */
    if (MwWddm2Shape != MW_SHAPE_GPUMMU)
        return;
    if (FITS(Size, DXGK_DRIVERCAPS, MemoryManagementCaps)) {
        Caps->MemoryManagementCaps.VirtualAddressingSupported = 1;
        Caps->MemoryManagementCaps.GpuMmuSupported = 1;
    }
    /* VidMm's own GPU VAs (paging process, fences) above Panfrost's range
     * (32 MB to 4 GB) and below the G52's 35-bit limit. */
    if (FITS(Size, DXGK_DRIVERCAPS, InternalGpuVirtualAddressRangeStart))
        Caps->InternalGpuVirtualAddressRangeStart = MW_GPUVA_INTERNAL_START;
    if (FITS(Size, DXGK_DRIVERCAPS, InternalGpuVirtualAddressRangeEnd))
        Caps->InternalGpuVirtualAddressRangeEnd = MW_GPUVA_INTERNAL_END;
}

/* ---- QAI ---- */

NTSTATUS MwQueryPhysicalAdapterCaps(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_PHYSICALADAPTERCAPS *c = (DXGK_PHYSICALADAPTERCAPS *)Info->pOutputData;
    UINT size = Info->OutputDataSize;

    if (c == NULL || size < RTL_SIZEOF_THROUGH_FIELD(DXGK_PHYSICALADAPTERCAPS, Flags))
        return STATUS_BUFFER_TOO_SMALL;
    if (Info->pInputData != NULL && Info->InputDataSize >= sizeof(DXGK_QUERYPHYSICALADAPTERCAPSIN) &&
        ((const DXGK_QUERYPHYSICALADAPTERCAPSIN *)Info->pInputData)->PhysicalAdapterIndex != 0)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(c, size);
    c->NumExecutionNodes = (WORD)MwNodeCount();
    c->PagingNodeIndex = MW_COPY_NODE;
    c->DxgkPhysicalAdapterHandle = (HANDLE)A;
    c->Flags.GpuMmuSupported = MwGpuMmu() ? 1 : 0;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_1)
    /* No video protected region; name a real node, not a phantom one. */
    if (FITS(size, DXGK_PHYSICALADAPTERCAPS, VPRPagingNode))
        c->VPRPagingNode = MW_COPY_NODE;
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
    c->VirtualAddressBitCount = MW_GPUMMU_LEVELS * MW_GPUMMU_INDEX_BITS + 12;
    c->PageTableLevelCount = MW_GPUMMU_LEVELS;
    return STATUS_SUCCESS;
}

NTSTATUS MwQueryPageTableLevelDesc(const DXGKARG_QUERYADAPTERINFO *Info)
{
    const DXGK_QUERYPAGETABLELEVELDESCIN *in = (const DXGK_QUERYPAGETABLELEVELDESCIN *)Info->pInputData;
    DXGK_PAGE_TABLE_LEVEL_DESC *d = (DXGK_PAGE_TABLE_LEVEL_DESC *)Info->pOutputData;

    if (!MwGpuMmu())
        return STATUS_NOT_SUPPORTED;
    if (in == NULL || Info->InputDataSize < sizeof(*in) || in->PhysicalAdapterIndex != 0 ||
        in->LevelIndex >= MW_GPUMMU_LEVELS)
        return STATUS_INVALID_PARAMETER;
    if (d == NULL || Info->OutputDataSize < sizeof(*d))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(d, Info->OutputDataSize);
    d->PageTableIndexBitCount = MW_GPUMMU_INDEX_BITS;
    d->PageTableSegmentId = MwGpuMmuPtSegment;
    d->PagingProcessPageTableSegmentId = MwGpuMmuPtSegment;
    d->PageTableSizeInBytes = PAGE_SIZE;
    d->PageTableAlignmentInBytes = PAGE_SIZE;   /* never 0: dxgmms2 divides by it */
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

/* For the watchdog. */
void Mw2xCounters(ULONG *Out, ULONG Count)
{
    ULONG v[8];
    v[0] = (ULONG)CopySubmits;
    v[1] = (ULONG)CopyFence;
    v[2] = (ULONG)Processes;
    v[3] = (ULONG)RootSets;
    v[4] = (ULONG)VirtSubmits;
    v[5] = (ULONG)VirtSubmits3d;
    v[6] = (ULONG)PtEntries;
    v[7] = (ULONG)PtRefusedMode;
    for (ULONG i = 0; i < Count && i < 8; i++)
        Out[i] = v[i];
}
