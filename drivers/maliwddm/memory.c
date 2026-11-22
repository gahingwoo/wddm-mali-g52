// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's memory model (docs/M5.2-PLAN.md): one aperture segment, as
 * viogpu3d. VidMm owns residency; the driver owns the Mali's address space.
 * An allocation gets its GPU address at creation and keeps it; when VidMm
 * makes it resident (MAP_APERTURE_SEGMENT) its pages are mapped there, and
 * unmapped again on eviction. The aperture offsets VidMm assigns are not
 * used for anything: the Mali MMU is the aperture.
 */
#include "maliwddm.h"
#include "wddm2.h"

/* ---- the segment ---- */

NTSTATUS MwQuerySegment(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_QUERYSEGMENTOUT3 *out = (DXGK_QUERYSEGMENTOUT3 *)Info->pOutputData;
    UNREFERENCED_PARAMETER(A);

    if (Info->OutputDataSize < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;
    if (out->pSegmentDescriptor == NULL) {
        out->NbSegment = 1;                 /* the first call asks how many */
        return STATUS_SUCCESS;
    }
    DXGK_SEGMENTDESCRIPTOR3 *d = &out->pSegmentDescriptor[0];
    RtlZeroMemory(d, sizeof(*d));
    d->BaseAddress.QuadPart = MK_VA_START;  /* nominal: nothing uses aperture offsets */
    d->Size = MW_SEGMENT_SIZE;
    d->CommitLimit = MW_SEGMENT_SIZE;
    d->Flags.Aperture = TRUE;
    /* DXGK_SEGMENTFLAGS: CpuVisible "has no meaning for an aperture segment",
     * except for a primary whose UMD has not set UseAlternateVA, which is
     * ours. Without it dxgkrnl refuses the shared primary ("All segment
     * specified in supported write segment set for a shared primary must
     * be CPU visible", installed Windows 2026-10-17), even with
     * SectionBackedPrimary set. */
    d->Flags.CpuVisible = TRUE;
    /* The Mali is not coherent with the CPU caches, so VidMm maps CPU views
     * of these allocations uncached, as malikm's write-combined BOs. */
    d->Flags.CacheCoherent = FALSE;
    /* As viogpu3d. Paging buffers stay empty: map and unmap are done on
     * the CPU while the paging buffer is built. */
    out->PagingBufferSegmentId = MW_SEGMENT_ID;
    out->PagingBufferSize = 10 * PAGE_SIZE;
    out->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

/* The WDDM 2.x form of the same answer (DXGKQAITYPE_QUERYSEGMENT4, 11),
 * asked of a driver that reports 2.0 or later (docs/WDDM2.md). The segment
 * is the one above, field for field, plus the budget flags MwPlan gives
 * (wddm2plan.h, default: non-local budget, application target).
 *
 * Microsoft's reference for DXGK_QUERYSEGMENTOUT4: dxgkrnl calls twice.
 * "First time, NbSegment will be set to 0. The driver should return
 * STATUS_SUCCESS and set NbSegment to the number of GPU memory segments
 * ... without accessing any other member of the structure." The second
 * call has the reported count, and pSegmentDescriptor is an array of
 * SegmentDescriptorStride-byte elements. The first probe (CI run 283) went
 * by the pointer instead of the count; which pass is which is now
 * Mw2SegmentPass's, checked by tests/wddm2-host.
 *
 * dxgkrnl 22631 asked with OutputDataSize 0x28 on the board: the 26100
 * header's size, checked here at build time. */
C_ASSERT(sizeof(DXGK_QUERYSEGMENTOUT4) == 0x28);

NTSTATUS MwQuerySegment4(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_QUERYSEGMENTOUT4 *out = (DXGK_QUERYSEGMENTOUT4 *)Info->pOutputData;
    DXGK_SEGMENTDESCRIPTOR4 *d;
    const MW2_PLAN *p = &MwPlan;
    SIZE_T stride;
    UNREFERENCED_PARAMETER(A);

    if (out == NULL || Info->OutputDataSize < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;
    if (Info->pInputData != NULL && Info->InputDataSize >= sizeof(DXGK_QUERYSEGMENTIN4) &&
        ((const DXGK_QUERYSEGMENTIN4 *)Info->pInputData)->PhysicalAdapterIndex != 0)
        return STATUS_INVALID_PARAMETER;
    stride = out->SegmentDescriptorStride != 0 ? out->SegmentDescriptorStride : sizeof(*d);
    switch (Mw2SegmentPass(out->NbSegment, out->pSegmentDescriptor != NULL, stride,
                           RTL_SIZEOF_THROUGH_FIELD(DXGK_SEGMENTDESCRIPTOR4, CommitLimit), p->NbSegment)) {
    case MW2_SEG_COUNT:
        out->NbSegment = p->NbSegment;
        return STATUS_SUCCESS;
    case MW2_SEG_TOO_SMALL:
        return STATUS_BUFFER_TOO_SMALL;
    default:
        break;
    }
    RtlZeroMemory(out->pSegmentDescriptor, stride * p->NbSegment);
    d = (DXGK_SEGMENTDESCRIPTOR4 *)out->pSegmentDescriptor;
    d->BaseAddress.QuadPart = MK_VA_START;
    d->Size = MW_SEGMENT_SIZE;
    d->CommitLimit = MW_SEGMENT_SIZE;
    d->Flags.Aperture = p->SegAperture;
    d->Flags.CpuVisible = p->SegCpuVisible;         /* the shared primary, as above */
    d->Flags.CacheCoherent = p->SegCacheCoherent;
    d->Flags.ApplicationTarget = p->SegApplicationTarget;
    d->Flags.NonLocalBudgetGroup = p->SegNonLocalBudget;
    d->Flags.LocalBudgetGroup = p->SegLocalBudget;
    out->NbSegment = p->NbSegment;
    out->PagingBufferSegmentId = p->PagingBufferSegmentId;
    out->PagingBufferSize = 10 * PAGE_SIZE;
    out->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

/* ---- accounting: bytes allocated and resident, by process ----
 *
 * The process is the one current in DxgkDdiCreateAllocation. For an
 * allocation a UMD asks for (pfnAllocateCb, D3DKMTCreateAllocation) that is
 * the UMD's process: dxgkrnl calls the DDI on the calling thread. For one
 * dxgkrnl makes on its own behalf it may be another process, or System.
 * The entry is kept in the allocation, so a destroy made in another
 * process's context (a process's teardown, a worker thread) gives the
 * bytes back to the right one.
 *
 * Interlocked operations only, no lock: two threads that claim an entry for
 * a new process at the same moment get different entries, and an entry is
 * reused only once it holds nothing. A reuse racing with a create of the
 * process that is leaving can count that one allocation against the new
 * process; it is a counter, not a resource. */

static void MemMax(volatile LONG64 *Max, LONG64 Value)
{
    LONG64 old = *Max;
    while (Value > old) {
        LONG64 seen = InterlockedCompareExchange64(Max, Value, old);
        if (seen == old)
            break;
        old = seen;
    }
}

static ULONG MemSlotFor(MW_ADAPTER *A, LONG64 Pid)
{
    ULONG i;

    for (i = 0; i < MW_MEM_PROCS; i++)
        if (A->MemProc[i].Pid == Pid)
            return i + 1;
    for (i = 0; i < MW_MEM_PROCS; i++)
        if (A->MemProc[i].Pid == 0 && InterlockedCompareExchange64(&A->MemProc[i].Pid, Pid, 0) == 0)
            return i + 1;
    /* Full: take an entry that holds nothing (a process that has gone). */
    for (i = 0; i < MW_MEM_PROCS; i++) {
        MW_MEM_SLOT *e = &A->MemProc[i];
        LONG64 old = e->Pid;
        if (e->Count == 0 && e->Bytes == 0 && old != Pid &&
            InterlockedCompareExchange64(&e->Pid, Pid, old) == old) {
            InterlockedExchange64(&e->PeakBytes, 0);
            InterlockedExchange(&e->Creates, 0);
            return i + 1;
        }
    }
    return 0;
}

static void MemCreated(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    LONG64 pid = (LONG64)(ULONG_PTR)PsGetCurrentProcessId();
    LONG64 size = (LONG64)Alloc->Size;

    MemMax(&A->MemPeakBytes, InterlockedAdd64(&A->MemBytes, size));
    InterlockedIncrement(&A->MemCount);
    InterlockedIncrement(&A->MemCreates);
    Alloc->MemSlot = MemSlotFor(A, pid);
    if (Alloc->MemSlot == 0) {
        InterlockedIncrement(&A->MemNoSlot);
        return;
    }
    MW_MEM_SLOT *e = &A->MemProc[Alloc->MemSlot - 1];
    MemMax(&e->PeakBytes, InterlockedAdd64(&e->Bytes, size));
    InterlockedIncrement(&e->Count);
    InterlockedIncrement(&e->Creates);
}

/* Resident: VidMm has mapped it (MAP_APERTURE_SEGMENT) and it is at its GPU
 * address; counted once, whatever the number of map calls. */
static void MemResident(MW_ADAPTER *A, MW_ALLOCATION *Alloc, BOOLEAN Resident)
{
    LONG64 size = (LONG64)Alloc->Size;

    if (Alloc->MemResident == Resident)
        return;
    Alloc->MemResident = Resident;
    if (!Resident)
        size = -size;
    InterlockedAdd64(&A->MemResidentBytes, size);
    InterlockedAdd(&A->MemResidentCount, Resident ? 1 : -1);
    if (Alloc->MemSlot != 0) {
        MW_MEM_SLOT *e = &A->MemProc[Alloc->MemSlot - 1];
        InterlockedAdd64(&e->ResidentBytes, size);
        InterlockedAdd(&e->ResidentCount, Resident ? 1 : -1);
    }
}

static void MemDestroyed(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    LONG64 size = (LONG64)Alloc->Size;

    MemResident(A, Alloc, FALSE);
    InterlockedAdd64(&A->MemBytes, -size);
    InterlockedDecrement(&A->MemCount);
    InterlockedIncrement(&A->MemDestroys);
    if (Alloc->MemSlot != 0) {
        MW_MEM_SLOT *e = &A->MemProc[Alloc->MemSlot - 1];
        InterlockedAdd64(&e->Bytes, -size);
        InterlockedDecrement(&e->Count);
    }
    Alloc->MemSlot = 0;
}

void MwEscapeMemory(MW_ADAPTER *A, MW_ESCAPE_MEMORY_DATA *D)
{
    ULONG n = 0;

    D->Bytes = (mw_u64)A->MemBytes;
    D->PeakBytes = (mw_u64)A->MemPeakBytes;
    D->ResidentBytes = (mw_u64)A->MemResidentBytes;
    D->SegmentSize = MW_SEGMENT_SIZE;
    D->Count = (mw_u32)A->MemCount;
    D->ResidentCount = (mw_u32)A->MemResidentCount;
    D->Creates = (mw_u32)A->MemCreates;
    D->Destroys = (mw_u32)A->MemDestroys;
    D->CreateFails = (mw_u32)A->MemCreateFails;
    D->NoSlot = (mw_u32)A->MemNoSlot;
    for (ULONG i = 0; i < MW_MEM_PROCS; i++) {
        const MW_MEM_SLOT *e = &A->MemProc[i];
        MW_MEM_PROC *o;
        if (e->Pid == 0)
            continue;
        o = &D->Proc[n++];
        o->Pid = (mw_u32)e->Pid;
        o->Count = (mw_u32)e->Count;
        o->Creates = (mw_u32)e->Creates;
        o->ResidentCount = (mw_u32)e->ResidentCount;
        o->Bytes = (mw_u64)e->Bytes;
        o->ResidentBytes = (mw_u64)e->ResidentBytes;
        o->PeakBytes = (mw_u64)e->PeakBytes;
    }
    D->Procs = n;
}

/* ---- allocations ---- */

static NTSTATUS NewAllocation(MW_ADAPTER *A, const MW_ALLOCATION_INFO *In, DXGK_ALLOCATIONINFO *Out)
{
    MW_ALLOCATION *alloc;
    ULONG pages;

    if (In->Size == 0 || In->Size > MW_SEGMENT_SIZE)
        return STATUS_INVALID_PARAMETER;
    pages = (ULONG)((In->Size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    alloc = (MW_ALLOCATION *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*alloc), MW_TAG);
    if (alloc == NULL)
        return STATUS_NO_MEMORY;
    alloc->Pages = pages;
    alloc->Size = (SIZE_T)pages << PAGE_SHIFT;
    alloc->Flags = In->Flags;
    alloc->Width = In->Width;
    alloc->Height = In->Height;
    alloc->Pitch = In->Pitch;
    alloc->Format = (D3DDDIFORMAT)In->Format;

    /* The GPU address, for the allocation's whole life. */
    ExAcquireFastMutex(&A->MmuLock);
    BOOLEAN ok = A->GpuUp && MkVaAlloc(&A->Gpu, pages, !(In->Flags & MW_ALLOC_NOEXEC), &alloc->GpuVa);
    ExReleaseFastMutex(&A->MmuLock);
    if (!ok && A->GpuUp) {
        ExFreePoolWithTag(alloc, MW_TAG);
        InterlockedIncrement(&A->MemCreateFails);
        return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    }

    MemCreated(A, alloc);
    A->Allocations++;
    if (A->Hdmi.Switch && (alloc->Flags & MW_ALLOC_PRIMARY)) {
        InterlockedIncrement(&A->Hdmi.PrimCreates);
        A->Hdmi.LastPrimWH = (LONG)(alloc->Width << 16 | (alloc->Height & 0xFFFF));
        A->Hdmi.LastPrimPitch = (LONG)alloc->Pitch;
    }
    A->LastAllocVa = alloc->GpuVa;
    Out->hAllocation = alloc;
    Out->Size = alloc->Size;
    Out->Alignment = 0;
    Out->PitchAlignedSize = 0;
    Out->HintedBank.Value = 0;
    Out->PreferredSegment.Value = 0;
    Out->PreferredSegment.SegmentId0 = MW_SEGMENT_ID;
    Out->SupportedReadSegmentSet = 1u << (MW_SEGMENT_ID - 1);
    Out->SupportedWriteSegmentSet = 1u << (MW_SEGMENT_ID - 1);
    Out->EvictionSegmentSet = 0;
    Out->MaximumRenamingListLength = 0;
    Out->pAllocationUsageHint = NULL;
    Out->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    Out->Flags.Value = 0;
    Out->Flags.CpuVisible = TRUE;
    return STATUS_SUCCESS;
}

static void FreeAllocation(MW_ADAPTER *A, MW_ALLOCATION *Alloc)
{
    MemDestroyed(A, Alloc);
    MwCopyForget(A, Alloc, TRUE);
    ExAcquireFastMutex(&A->MmuLock);
    MwScanUnmap(A, Alloc);
    if (Alloc->Mapped && A->GpuUp)
        MkMmuUnmapPages(&A->Gpu, Alloc->GpuVa, Alloc->Pages);
    if (Alloc->GpuVa != 0 && A->GpuUp)
        MkVaFree(&A->Gpu, Alloc->GpuVa, Alloc->Pages);
    ExReleaseFastMutex(&A->MmuLock);
    ExFreePoolWithTag(Alloc, MW_TAG);
}

NTSTATUS APIENTRY MwCreateAllocation(IN_CONST_HANDLE hAdapter, DXGKARG_CREATEALLOCATION *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    for (UINT i = 0; i < Arg->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *info = &Arg->pAllocationInfo[i];
        const MW_ALLOCATION_INFO *in = (const MW_ALLOCATION_INFO *)info->pPrivateDriverData;
        NTSTATUS st;

        if (in == NULL || info->PrivateDriverDataSize < sizeof(*in) || in->Version != MW_ABI_VERSION)
            st = STATUS_INVALID_PARAMETER;
        else
            st = NewAllocation(a, in, info);
        if (!NT_SUCCESS(st)) {
            for (UINT j = 0; j < i; j++)
                FreeAllocation(a, (MW_ALLOCATION *)Arg->pAllocationInfo[j].hAllocation);
            return st;
        }
    }
    /* A resource needs a handle of its own; nothing is kept in it. */
    if (Arg->Flags.Resource && Arg->hResource == NULL)
        Arg->hResource = (HANDLE)a;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyAllocation(IN_CONST_HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    for (UINT i = 0; i < Arg->NumAllocations; i++)
        if (Arg->pAllocationList[i] != NULL)
            FreeAllocation(a, (MW_ALLOCATION *)Arg->pAllocationList[i]);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDescribeAllocation(IN_CONST_HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *Arg)
{
    MW_ALLOCATION *alloc = (MW_ALLOCATION *)Arg->hAllocation;
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    Arg->Width = alloc->Width;
    Arg->Height = alloc->Height;
    Arg->Format = alloc->Format;
    Arg->MultisampleMethod.NumSamples = 0;
    Arg->MultisampleMethod.NumQualityLevels = 0;
    Arg->RefreshRate.Numerator = 60;
    Arg->RefreshRate.Denominator = 1;
    /* HdmiHpd 3: a primary has the refresh rate of the mode it is shown in
     * (the committed target's VSyncFreq), not a nominal 60: 2560x1440 at
     * 241.70 MHz is 59.998 Hz. The firmware's own mode on the panel is
     * exactly 60 (display.c TimingMadeUp), which is why 60/1 never
     * mattered there. */
    if (a != NULL && a->Hdmi.Switch && (alloc->Flags & MW_ALLOC_PRIMARY) && a->Hdmi.RefDen != 0) {
        Arg->RefreshRate.Numerator = a->Hdmi.RefNum;
        Arg->RefreshRate.Denominator = a->Hdmi.RefDen;
    } else if (a != NULL && !a->Hdmi.Switch && (alloc->Flags & MW_ALLOC_PRIMARY) &&
               a->TimingTotalW != 0 && a->TimingTotalH != 0 &&
               (ULONG64)a->TimingPixelRate != 60ull * a->TimingTotalW * a->TimingTotalH) {
        /* Every other level, the same: the one target mode display.c
         * reports (FillSignal's VSyncFreq), when it is not 60 Hz exactly.
         * The made-up timing (the panel, a boot without EDID) is 60 by
         * construction and keeps 60/1 as before; an HDMI boot with the
         * firmware's EDID in the DSDT is at the EDID's rate (59.998 Hz
         * at 2560x1440 on the AOC), where 60/1 kept DWM from flipping
         * (board, 2026-11-22, the level 3 run that fixed it). */
        Arg->RefreshRate.Numerator = a->TimingPixelRate;
        Arg->RefreshRate.Denominator = a->TimingTotalW * a->TimingTotalH;
    }
    if (a != NULL && a->Hdmi.Switch) {
        InterlockedIncrement(&a->Hdmi.Describes);
        a->Hdmi.LastDescWH = (LONG)(alloc->Width << 16 | (alloc->Height & 0xFFFF) | ((alloc->Flags & MW_ALLOC_PRIMARY) ? 0x8000u : 0));
        a->Hdmi.LastDescRef = (LONG)(Arg->RefreshRate.Denominator ? Arg->RefreshRate.Numerator * 100ull / Arg->RefreshRate.Denominator : 0);
    }
    Arg->PrivateDriverFormatAttribute = 0;
    return STATUS_SUCCESS;
}

/* The runtime's own surfaces: describe them in our private data format.
 * Called twice: first with no buffers, for the sizes. */
NTSTATUS APIENTRY MwGetStandardAllocationDriverData(IN_CONST_HANDLE hAdapter,
                                                    DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *Arg)
{
    MW_ALLOCATION_INFO *out = (MW_ALLOCATION_INFO *)Arg->pAllocationPrivateDriverData;
    UINT w = 0, h = 0;
    D3DDDIFORMAT f = D3DDDIFMT_A8R8G8B8;
    UINT *pitch = NULL;
    ULONG flags = MW_ALLOC_NOEXEC;
    UNREFERENCED_PARAMETER(hAdapter);

    switch (Arg->StandardAllocationType) {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        if (((MW_ADAPTER *)hAdapter)->Hdmi.Switch) {
            ((MW_ADAPTER *)hAdapter)->Hdmi.SharedPrimRefNum = Arg->pCreateSharedPrimarySurfaceData->RefreshRate.Numerator;
            ((MW_ADAPTER *)hAdapter)->Hdmi.SharedPrimRefDen = Arg->pCreateSharedPrimarySurfaceData->RefreshRate.Denominator;
        }
        w = Arg->pCreateSharedPrimarySurfaceData->Width;
        h = Arg->pCreateSharedPrimarySurfaceData->Height;
        f = Arg->pCreateSharedPrimarySurfaceData->Format;
        flags |= MW_ALLOC_PRIMARY;
        break;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        w = Arg->pCreateShadowSurfaceData->Width;
        h = Arg->pCreateShadowSurfaceData->Height;
        f = Arg->pCreateShadowSurfaceData->Format;
        pitch = &Arg->pCreateShadowSurfaceData->Pitch;
        break;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        w = Arg->pCreateStagingSurfaceData->Width;
        h = Arg->pCreateStagingSurfaceData->Height;
        pitch = &Arg->pCreateStagingSurfaceData->Pitch;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    if (out == NULL) {
        Arg->AllocationPrivateDriverDataSize = sizeof(MW_ALLOCATION_INFO);
        Arg->ResourcePrivateDriverDataSize = 0;
        return STATUS_SUCCESS;
    }
    if (Arg->AllocationPrivateDriverDataSize < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(out, sizeof(*out));
    out->Version = MW_ABI_VERSION;
    out->Flags = flags;
    out->Width = w;
    out->Height = h;
    out->Pitch = w * 4;
    out->Format = (mw_u32)f;
    out->Size = (mw_u64)out->Pitch * h;
    if (pitch != NULL)
        *pitch = out->Pitch;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwOpenAllocation(IN_CONST_HANDLE hDevice, const DXGKARG_OPENALLOCATION *Arg)
{
    MW_DEVICE *dev = (MW_DEVICE *)hDevice;
    DXGKRNL_INTERFACE *dx = &dev->Adapter->Dxgk;

    for (UINT i = 0; i < Arg->NumAllocations; i++) {
        DXGKARGCB_GETHANDLEDATA get = {0};
        get.hObject = Arg->pOpenAllocation[i].hAllocation;
        get.Type = DXGK_HANDLE_ALLOCATION;
        Arg->pOpenAllocation[i].hDeviceSpecificAllocation = dx->DxgkCbGetHandleData(&get);
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCloseAllocation(IN_CONST_HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *Arg)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

/* ---- residency ---- */

/* Map and unmap happen here, on the CPU, as viogpu3d does: the paging
 * buffer itself stays empty. Everything else VidMm may ask (transfers,
 * fills) is not supported, as in viogpu3d; with one aperture segment it
 * has not been needed there. */
static NTSTATUS BuildPaging1x(MW_ADAPTER *a, DXGKARG_BUILDPAGINGBUFFER *Arg)
{
    NTSTATUS st = STATUS_SUCCESS;

    if ((ULONG)Arg->Operation < 16)
        a->PagingOps[Arg->Operation]++;
    switch (Arg->Operation) {
    case DXGK_OPERATION_MAP_APERTURE_SEGMENT: {
        MW_ALLOCATION *alloc = (MW_ALLOCATION *)Arg->MapApertureSegment.hAllocation;
        if (alloc == NULL || !a->GpuUp)
            return STATUS_SUCCESS;
        ULONG pages = (ULONG)Arg->MapApertureSegment.NumberOfPages;
        if (pages > alloc->Pages)
            pages = alloc->Pages;
        PPFN_NUMBER pfn = MmGetMdlPfnArray(Arg->MapApertureSegment.pMdl) + Arg->MapApertureSegment.MdlOffset;
        ExAcquireFastMutex(&a->MmuLock);
        st = MkMmuMapPages(&a->Gpu, alloc->GpuVa, pfn, pages, (alloc->Flags & MW_ALLOC_NOEXEC) != 0);
        alloc->Mapped = NT_SUCCESS(st);
        MemResident(a, alloc, alloc->Mapped);
        alloc->Mdl = Arg->MapApertureSegment.pMdl;
        alloc->MdlPage = (ULONG)Arg->MapApertureSegment.MdlOffset;
        if (alloc->Mapped)
            MwScanMap(a, alloc, pfn, pages);
        ExReleaseFastMutex(&a->MmuLock);
        a->Maps++;
        if (!NT_SUCCESS(st))
            a->MapFails++;
        a->LastMapVa = alloc->GpuVa;
        a->LastMapPages = pages;
        a->LastMapStatus = st;
        return st;
    }
    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT: {
        MW_ALLOCATION *alloc = (MW_ALLOCATION *)Arg->UnmapApertureSegment.hAllocation;
        if (alloc == NULL || !a->GpuUp)
            return STATUS_SUCCESS;
        a->Unmaps++;
        ExAcquireFastMutex(&a->MmuLock);
        MwScanUnmap(a, alloc);
        if (alloc->Mapped)
            MkMmuUnmapPages(&a->Gpu, alloc->GpuVa, alloc->Pages);
        alloc->Mapped = FALSE;
        MemResident(a, alloc, FALSE);
        ExReleaseFastMutex(&a->MmuLock);
        MwCopyForget(a, alloc, FALSE);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS APIENTRY MwBuildPagingBuffer(IN_CONST_HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    NTSTATUS st;
    static volatile LONG journaled;

    if (MwBcOn() && InterlockedIncrement(&journaled) <= 64)
        MwBc("BuildPagingBuffer: operation %u, DMA buffer %p size %u, private size %u",
             (ULONG)Arg->Operation, Arg->pDmaBuffer, Arg->DmaSize, Arg->DmaBufferPrivateDataSize);

    /* WDDM 1.3: as it always was. */
    if (MwWddmProbe == 0) {
        st = BuildPaging1x(a, Arg);
        MwProbePagingOp((ULONG)Arg->Operation, !NT_SUCCESS(st));
        return st;
    }
    /* WDDM 2.x (wddm2x.c). dxgmms2 accepts only STATUS_SUCCESS or
     * STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER from this DDI; anything else
     * is bugcheck 0x10E. Map and unmap are done as before; the operations a
     * 1.3 driver never sees are taken; a failure is counted (PgRefused_<op>
     * in the probe's key) and reported as success. */
    /* No room for the marker: ask for a new buffer before doing anything,
     * so that the operation is not done twice. */
    if (MwPagingMarker != 0 && Arg->pDmaBuffer != NULL && Arg->DmaSize != 0 && Arg->DmaSize < 8)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if (Arg->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
        Arg->Operation == DXGK_OPERATION_UNMAP_APERTURE_SEGMENT)
        st = BuildPaging1x(a, Arg);
    else
        st = Mw2xPagingOp(a, Arg) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    MwProbePagingOp((ULONG)Arg->Operation, !NT_SUCCESS(st));
    return Mw2xPagingFill(Arg);
}

/* ---- the UMD's question: where is this allocation on the GPU? ---- */

NTSTATUS MwEscapeAllocInfo(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_ALLOC_INFO_DATA *D)
{
    DXGKARGCB_GETHANDLEDATA get = {0};
    MW_ALLOCATION *alloc;

    if (Esc->hDevice == NULL)
        return STATUS_INVALID_PARAMETER;
    get.hObject = (D3DKMT_HANDLE)D->hAllocation;
    get.Type = DXGK_HANDLE_ALLOCATION;
    alloc = (MW_ALLOCATION *)A->Dxgk.DxgkCbGetHandleData(&get);
    if (alloc == NULL)
        return STATUS_INVALID_HANDLE;
    D->GpuVa = alloc->GpuVa;
    D->Size = alloc->Size;
    return STATUS_SUCCESS;
}
