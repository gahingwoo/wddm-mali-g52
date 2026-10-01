// SPDX-License-Identifier: GPL-2.0
/*
 * The Linux Panfrost uAPI, as Mesa's panfrost_kmod.c and pan_jm.c use it:
 * buffer objects, submission, waits and DRM syncobjs. Semantics follow
 * drivers/gpu/drm/panfrost/panfrost_drv.c and drm_syncobj.c; where this
 * driver is simpler (one queue, one address space), the comment says so.
 *
 * Every function here runs at PASSIVE_LEVEL in the caller's thread.
 */
#include "malikm.h"

#define POOL_TAG_BO     'bmlM'
#define POOL_TAG_SYNC   'smlM'
#define POOL_TAG_MAP    'mmlM'
#define POOL_TAG_TABLE  'tmlM'
#define POOL_TAG_JOB    'jmlM'

/* ---- handle tables ---- */

/* Store P in the first free slot of *Table, growing it as needed.
 * Returns the handle (index + 1), or 0. */
static ULONG TableAdd(PVOID **Table, ULONG *Cap, PVOID P)
{
    for (ULONG i = 0; i < *Cap; i++) {
        if ((*Table)[i] == NULL) {
            (*Table)[i] = P;
            return i + 1;
        }
    }
    ULONG cap = *Cap ? *Cap * 2 : 64;
    PVOID *t = (PVOID *)ExAllocatePool2(POOL_FLAG_NON_PAGED, cap * sizeof(PVOID), POOL_TAG_TABLE);
    if (t == NULL)
        return 0;
    if (*Table != NULL) {
        RtlCopyMemory(t, *Table, *Cap * sizeof(PVOID));
        ExFreePoolWithTag(*Table, POOL_TAG_TABLE);
    }
    ULONG h = *Cap + 1;
    t[*Cap] = P;
    *Table = t;
    *Cap = cap;
    return h;
}

static MK_BO *BoGet(PFILE_CONTEXT F, mk_u32 H)
{
    return (H != 0 && H <= F->BoCap) ? F->Bos[H - 1] : NULL;
}

static MK_SYNCOBJ *SyncGet(PFILE_CONTEXT F, mk_u32 H)
{
    return (H != 0 && H <= F->SyncobjCap) ? F->Syncobjs[H - 1] : NULL;
}

/* ---- buffer objects ---- */

static void BoFree(PDEVICE_CONTEXT Dev, MK_BO *Bo)
{
    MkMmuUnmap(Dev, Bo);
    MmFreePagesFromMdl(Bo->Mdl);
    ExFreePool(Bo->Mdl);
    ExFreePoolWithTag(Bo, POOL_TAG_BO);
}

static BOOLEAN BoIdle(PDEVICE_CONTEXT Dev, MK_BO *Bo)
{
    return Bo->Maps == 0 && Bo->LastSeq <= (ULONG64)Dev->CompletedSeq;
}

void MkBoRelease(PDEVICE_CONTEXT Dev, MK_BO *Bo)
{
    Bo->Closed = TRUE;
    InsertTailList(&Dev->Zombies, &Bo->Link);
    MkReapZombies(Dev);
}

void MkReapZombies(PDEVICE_CONTEXT Dev)
{
    PLIST_ENTRY e = Dev->Zombies.Flink;

    while (e != &Dev->Zombies) {
        MK_BO *bo = CONTAINING_RECORD(e, MK_BO, Link);
        e = e->Flink;
        if (BoIdle(Dev, bo)) {
            RemoveEntryList(&bo->Link);
            BoFree(Dev, bo);
        }
    }
}

static mk_s32 CreateBo(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_CREATE_BO *A)
{
    PHYSICAL_ADDRESS low, high, skip;
    SIZE_T size;
    MK_BO *bo;

    if (A->size == 0 || (A->flags & ~(ULONG)(PANFROST_BO_NOEXEC | PANFROST_BO_HEAP)) != 0)
        return -MK_EINVAL;
    /* Heap BOs grow on fault under Linux; here they are allocated whole.
     * Linux insists they are not executable. */
    if ((A->flags & PANFROST_BO_HEAP) && !(A->flags & PANFROST_BO_NOEXEC))
        return -MK_EINVAL;
    size = ROUND_TO_PAGES((SIZE_T)A->size);
    if (A->flags & PANFROST_BO_HEAP)
        size = (size + 0x1FFFFF) & ~(SIZE_T)0x1FFFFF;

    bo = (MK_BO *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*bo), POOL_TAG_BO);
    if (bo == NULL)
        return -MK_ENOMEM;
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFFFULL;
    skip.QuadPart = 0;
    /* Write-combined: the GPU is not coherent with the CPU caches, and the
     * user mapping has to use the same attributes. Pages come back zeroed. */
    bo->Mdl = MmAllocatePagesForMdlEx(low, high, skip, size, MmWriteCombined, MM_ALLOCATE_FULLY_REQUIRED);
    if (bo->Mdl == NULL) {
        ExFreePoolWithTag(bo, POOL_TAG_BO);
        return -MK_ENOMEM;
    }
    if (MmGetMdlByteCount(bo->Mdl) != size) {
        MmFreePagesFromMdl(bo->Mdl);
        ExFreePool(bo->Mdl);
        ExFreePoolWithTag(bo, POOL_TAG_BO);
        return -MK_ENOMEM;
    }
    bo->Size = size;
    bo->Flags = A->flags;

    ExAcquireFastMutex(&Dev->Lock);
    NTSTATUS st = MkMmuMap(Dev, bo);
    ULONG h = NT_SUCCESS(st) ? TableAdd((PVOID **)&F->Bos, &F->BoCap, bo) : 0;
    if (NT_SUCCESS(st) && h == 0)
        MkMmuUnmap(Dev, bo);
    ExReleaseFastMutex(&Dev->Lock);

    if (h == 0) {
        MmFreePagesFromMdl(bo->Mdl);
        ExFreePool(bo->Mdl);
        ExFreePoolWithTag(bo, POOL_TAG_BO);
        return -MK_ENOMEM;
    }
    A->handle = h;
    A->offset = bo->GpuVa;
    return 0;
}

static mk_s32 GemClose(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_GEM_CLOSE *A)
{
    mk_s32 r = -MK_EINVAL;

    ExAcquireFastMutex(&Dev->Lock);
    MK_BO *bo = BoGet(F, A->handle);
    if (bo != NULL) {
        F->Bos[A->handle - 1] = NULL;
        MkBoRelease(Dev, bo);
        r = 0;
    }
    ExReleaseFastMutex(&Dev->Lock);
    return r;
}

/* ---- waiting ---- */

/* Wait until job Seq has completed. TimeoutNs is relative. */
static mk_s32 WaitSeq(PDEVICE_CONTEXT Dev, ULONG64 Seq, mk_s64 TimeoutNs, mk_s32 TimeoutErr)
{
    ULONG64 start = KeQueryInterruptTime();

    for (;;) {
        if ((ULONG64)Dev->CompletedSeq >= Seq)
            return 0;
        LONG64 slice = 10 * 1000 * 1000;   /* 10 ms in ns */
        if (TimeoutNs != MALIKM_TIMEOUT_INFINITE) {
            LONG64 left = TimeoutNs - (LONG64)(KeQueryInterruptTime() - start) * 100;
            if (left <= 0)
                return TimeoutErr;
            if (left < slice)
                slice = left;
        }
        if (PsIsThreadTerminating(PsGetCurrentThread()))
            return -MK_EINTR;
        LARGE_INTEGER t;
        t.QuadPart = -(slice / 100 > 0 ? slice / 100 : 1);
        (void)KeWaitForSingleObject(&Dev->ProgressEvent, Executive, KernelMode, FALSE, &t);
    }
}

static mk_s32 WaitBo(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_WAIT_BO *A)
{
    ULONG64 seq;

    if (A->pad != 0)
        return -MK_EINVAL;
    ExAcquireFastMutex(&Dev->Lock);
    MK_BO *bo = BoGet(F, A->handle);
    seq = bo ? bo->LastSeq : 0;
    ExReleaseFastMutex(&Dev->Lock);
    if (bo == NULL)
        return -MK_ENOENT;
    /* Linux: -EBUSY for a poll, -ETIMEDOUT for a real timeout. */
    return WaitSeq(Dev, seq, A->timeout_ns, A->timeout_ns == 0 ? -MK_EBUSY : -MK_ETIMEDOUT);
}

/* ---- submission ---- */

static mk_s32 Submit(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_SUBMIT *A, SIZE_T ArgSize)
{
    PUCHAR base = (PUCHAR)A;
    mk_u32 *bos, *ins;
    MK_JOB *job;
    mk_s32 r = 0;

    if (A->jc == 0 || A->jm_ctx_handle != 0)
        return -MK_EINVAL;
    /* The shim put the arrays after the struct and their offsets in the
     * pointer fields; check they lie inside what was sent. */
    if (A->in_sync_count > 0x10000 || A->bo_handle_count > 0x100000)
        return -MK_EINVAL;
    if (A->in_sync_count && (A->in_syncs > ArgSize || ArgSize - A->in_syncs < A->in_sync_count * 4ULL))
        return -MK_EFAULT;
    if (A->bo_handle_count && (A->bo_handles > ArgSize || ArgSize - A->bo_handles < A->bo_handle_count * 4ULL))
        return -MK_EFAULT;
    ins = (mk_u32 *)(base + A->in_syncs);
    bos = (mk_u32 *)(base + A->bo_handles);

    if (!Dev->Ready || Dev->StopWorker)
        return -MK_ENODEV;
    job = (MK_JOB *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*job), POOL_TAG_JOB);
    if (job == NULL)
        return -MK_ENOMEM;
    job->Jc = A->jc;
    job->Requirements = A->requirements;

    ExAcquireFastMutex(&Dev->Lock);
    /* In-fences: every earlier job runs first on the single queue, so they
     * only need to exist. Linux fails a syncobj with no fence the same way. */
    for (ULONG i = 0; i < A->in_sync_count && r == 0; i++) {
        MK_SYNCOBJ *s = SyncGet(F, ins[i]);
        if (s == NULL || !s->HasFence)
            r = -MK_EINVAL;
    }
    for (ULONG i = 0; i < A->bo_handle_count && r == 0; i++)
        if (BoGet(F, bos[i]) == NULL)
            r = -MK_ENOENT;
    MK_SYNCOBJ *out = NULL;
    if (r == 0 && A->out_sync != 0 && (out = SyncGet(F, A->out_sync)) == NULL)
        r = -MK_ENOENT;
    if (r == 0) {
        job->Seq = (ULONG64)InterlockedIncrement64(&Dev->SubmittedSeq);
        for (ULONG i = 0; i < A->bo_handle_count; i++)
            BoGet(F, bos[i])->LastSeq = job->Seq;
        if (out != NULL) {
            out->HasFence = TRUE;
            out->Seq = job->Seq;
        }
        MkQueueJob(Dev, job);   /* under Lock, so the queue stays in Seq order */
    }
    ExReleaseFastMutex(&Dev->Lock);
    if (r != 0)
        ExFreePoolWithTag(job, POOL_TAG_JOB);
    return r;
}

/* ---- syncobjs ---- */

static mk_s32 SyncCreate(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_SYNCOBJ_CREATE *A)
{
    MK_SYNCOBJ *s;

    if (A->flags & ~(ULONG)DRM_SYNCOBJ_CREATE_SIGNALED)
        return -MK_EINVAL;
    s = (MK_SYNCOBJ *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*s), POOL_TAG_SYNC);
    if (s == NULL)
        return -MK_ENOMEM;
    s->HasFence = (A->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
    s->Seq = 0;
    ExAcquireFastMutex(&Dev->Lock);
    A->handle = TableAdd((PVOID **)&F->Syncobjs, &F->SyncobjCap, s);
    ExReleaseFastMutex(&Dev->Lock);
    if (A->handle == 0) {
        ExFreePoolWithTag(s, POOL_TAG_SYNC);
        return -MK_ENOMEM;
    }
    return 0;
}

static mk_s32 SyncDestroy(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_SYNCOBJ_DESTROY *A)
{
    MK_SYNCOBJ *s;

    ExAcquireFastMutex(&Dev->Lock);
    s = SyncGet(F, A->handle);
    if (s != NULL)
        F->Syncobjs[A->handle - 1] = NULL;
    ExReleaseFastMutex(&Dev->Lock);
    if (s == NULL)
        return -MK_EINVAL;
    ExFreePoolWithTag(s, POOL_TAG_SYNC);
    return 0;
}

/* RESET drops the fence; SIGNAL installs an already signalled one. */
static mk_s32 SyncArray(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_SYNCOBJ_ARRAY *A, SIZE_T ArgSize,
                        BOOLEAN Signal)
{
    mk_u32 *h;
    mk_s32 r = 0;

    if (A->count_handles == 0 || A->handles > ArgSize || ArgSize - A->handles < A->count_handles * 4ULL)
        return -MK_EINVAL;
    h = (mk_u32 *)((PUCHAR)A + A->handles);
    ExAcquireFastMutex(&Dev->Lock);
    for (ULONG i = 0; i < A->count_handles; i++)
        if (SyncGet(F, h[i]) == NULL)
            r = -MK_ENOENT;
    for (ULONG i = 0; i < A->count_handles && r == 0; i++) {
        MK_SYNCOBJ *s = SyncGet(F, h[i]);
        s->HasFence = Signal;
        s->Seq = 0;
    }
    ExReleaseFastMutex(&Dev->Lock);
    return r;
}

static mk_s32 SyncWait(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_SYNCOBJ_WAIT *A, SIZE_T ArgSize)
{
    mk_u32 *h;
    ULONG64 start = KeQueryInterruptTime();
    BOOLEAN all = (A->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
    BOOLEAN forSubmit = (A->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0;

    if (A->count_handles == 0 || A->handles > ArgSize || ArgSize - A->handles < A->count_handles * 4ULL)
        return -MK_EINVAL;
    h = (mk_u32 *)((PUCHAR)A + A->handles);

    /* Re-read the syncobjs each round: with WAIT_FOR_SUBMIT a fence may be
     * installed by another thread while this one waits. */
    for (;;) {
        ULONG64 target = 0, done = (ULONG64)Dev->CompletedSeq;
        BOOLEAN missing = FALSE, any = FALSE;
        mk_s32 r = 0;

        ExAcquireFastMutex(&Dev->Lock);
        for (ULONG i = 0; i < A->count_handles; i++) {
            MK_SYNCOBJ *s = SyncGet(F, h[i]);
            if (s == NULL) {
                r = -MK_ENOENT;
                break;
            }
            if (!s->HasFence) {
                if (!forSubmit) {
                    r = -MK_EINVAL;
                    break;
                }
                missing = TRUE;
                continue;
            }
            if (s->Seq <= done) {
                if (!any)
                    A->first_signaled = i;
                any = TRUE;
            }
            if (s->Seq > target)
                target = s->Seq;
        }
        ExReleaseFastMutex(&Dev->Lock);
        if (r != 0)
            return r;
        if (all ? (!missing && target <= done) : any)
            return 0;

        LONG64 left = MALIKM_TIMEOUT_INFINITE;
        if (A->timeout_nsec != MALIKM_TIMEOUT_INFINITE) {
            left = A->timeout_nsec - (LONG64)(KeQueryInterruptTime() - start) * 100;
            if (left <= 0)
                return -MK_ETIME;
        }
        /* One job's worth of progress, or a 10 ms slice, then look again. */
        r = WaitSeq(Dev, done + 1, left < 10000000 ? left : 10000000, 0);
        if (r == -MK_EINTR)
            return r;
    }
}

static mk_s32 SyncSeq(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MALIKM_SYNCOBJ_SEQ *A, BOOLEAN Import)
{
    mk_s32 r = 0;

    ExAcquireFastMutex(&Dev->Lock);
    MK_SYNCOBJ *s = SyncGet(F, A->Handle);
    if (s == NULL) {
        r = -MK_ENOENT;
    } else if (Import) {
        s->HasFence = TRUE;
        s->Seq = A->Seq == MALIKM_SEQ_LATEST ? (ULONG64)Dev->SubmittedSeq : A->Seq;
    } else if (!s->HasFence) {
        r = -MK_EINVAL;
    } else {
        A->Seq = s->Seq;
    }
    ExReleaseFastMutex(&Dev->Lock);
    return r;
}

/* ---- the rest of Panfrost ---- */

static mk_s32 GetParam(PDEVICE_CONTEXT Dev, MK_GET_PARAM *A)
{
    GPU_FEATURES *f = &Dev->F;
    mk_u32 p = A->param;

    if (A->pad != 0)
        return -MK_EINVAL;
    switch (p) {
    case 0:  A->value = f->Id; break;
    case 1:  A->value = f->Revision; break;
    case 2:  A->value = f->ShaderPresent; break;
    case 3:  A->value = f->TilerPresent; break;
    case 4:  A->value = f->L2Present; break;
    case 5:  A->value = f->StackPresent; break;
    case 6:  A->value = f->AsPresent; break;
    case 7:  A->value = f->JsPresent; break;
    case 8:  A->value = f->L2Features; break;
    case 9:  A->value = f->CoreFeatures; break;
    case 10: A->value = f->TilerFeatures; break;
    case 11: A->value = f->MemFeatures; break;
    case 12: A->value = f->MmuFeatures; break;
    case 13: A->value = f->ThreadFeatures; break;
    case 14: A->value = f->MaxThreads; break;
    case 15: A->value = f->MaxWorkgroupSize; break;
    case 16: A->value = f->MaxBarrierSize; break;
    case 17: A->value = f->CoherencyFeatures; break;
    case 18: case 19: case 20: case 21:
        A->value = f->TextureFeatures[p - 18]; break;
    case 38: A->value = f->NrCoreGroups; break;
    case 39: A->value = f->ThreadTlsAlloc; break;
    case 40: A->value = f->AfbcFeatures; break;
    case 44: A->value = 31; break;       /* SELECTED_COHERENCY: none */
    default:
        if (p >= 22 && p <= 37) {
            A->value = f->JsFeatures[p - 22];
            break;
        }
        /* Timestamps (1.3) and JM priorities (1.5) are past version 1.1. */
        return -MK_EINVAL;
    }
    return 0;
}

static mk_s32 MmapBo(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_MMAP_BO *A)
{
    mk_s32 r = 0;

    if (A->flags != 0)
        return -MK_EINVAL;
    ExAcquireFastMutex(&Dev->Lock);
    MK_BO *bo = BoGet(F, A->handle);
    if (bo == NULL)
        r = -MK_ENOENT;
    else if (bo->Flags & PANFROST_BO_HEAP)
        r = -MK_EINVAL;                   /* as Linux: heaps are GPU-only */
    else
        A->offset = MALIKM_MMAP_OFFSET(A->handle);
    ExReleaseFastMutex(&Dev->Lock);
    return r;
}

static mk_s32 GetBoOffset(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_GET_BO_OFFSET *A)
{
    ExAcquireFastMutex(&Dev->Lock);
    MK_BO *bo = BoGet(F, A->handle);
    if (bo != NULL)
        A->offset = bo->GpuVa;
    ExReleaseFastMutex(&Dev->Lock);
    return bo ? 0 : -MK_ENOENT;
}

static mk_s32 Madvise(PDEVICE_CONTEXT Dev, PFILE_CONTEXT F, MK_MADVISE *A)
{
    ExAcquireFastMutex(&Dev->Lock);
    MK_BO *bo = BoGet(F, A->handle);
    ExReleaseFastMutex(&Dev->Lock);
    if (bo == NULL)
        return -MK_ENOENT;
    A->retained = 1;
    return 0;
}

#define NEED(T) do { if (ArgSize < sizeof(T)) return -MK_EINVAL; } while (0)

mk_s32 MkDrmIoctl(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, ULONG Nr, PUCHAR Arg, SIZE_T ArgSize)
{
    switch (Nr) {
    case MALIKM_NR_PANFROST_GET_PARAM:
        NEED(MK_GET_PARAM);
        return GetParam(Dev, (MK_GET_PARAM *)Arg);
    case MALIKM_NR_PANFROST_CREATE_BO:
        NEED(MK_CREATE_BO);
        return CreateBo(Dev, File, (MK_CREATE_BO *)Arg);
    case MALIKM_NR_PANFROST_MMAP_BO:
        NEED(MK_MMAP_BO);
        return MmapBo(Dev, File, (MK_MMAP_BO *)Arg);
    case MALIKM_NR_PANFROST_GET_BO_OFFSET:
        NEED(MK_GET_BO_OFFSET);
        return GetBoOffset(Dev, File, (MK_GET_BO_OFFSET *)Arg);
    case MALIKM_NR_PANFROST_WAIT_BO:
        NEED(MK_WAIT_BO);
        return WaitBo(Dev, File, (MK_WAIT_BO *)Arg);
    case MALIKM_NR_PANFROST_MADVISE:
        /* Nothing is ever purged, so every BO is retained. */
        NEED(MK_MADVISE);
        return Madvise(Dev, File, (MK_MADVISE *)Arg);
    case MALIKM_NR_PANFROST_SUBMIT:
        /* Kernels before JM contexts had a 40-byte struct; accept both. */
        if (ArgSize < 40)
            return -MK_EINVAL;
        return Submit(Dev, File, (MK_SUBMIT *)Arg, ArgSize);
    case MALIKM_NR_GEM_CLOSE:
        NEED(MK_GEM_CLOSE);
        return GemClose(Dev, File, (MK_GEM_CLOSE *)Arg);
    case MALIKM_NR_SYNCOBJ_CREATE:
        NEED(MK_SYNCOBJ_CREATE);
        return SyncCreate(Dev, File, (MK_SYNCOBJ_CREATE *)Arg);
    case MALIKM_NR_SYNCOBJ_DESTROY:
        NEED(MK_SYNCOBJ_DESTROY);
        return SyncDestroy(Dev, File, (MK_SYNCOBJ_DESTROY *)Arg);
    case MALIKM_NR_SYNCOBJ_WAIT:
        NEED(MK_SYNCOBJ_WAIT);
        return SyncWait(Dev, File, (MK_SYNCOBJ_WAIT *)Arg, ArgSize);
    case MALIKM_NR_SYNCOBJ_RESET:
    case MALIKM_NR_SYNCOBJ_SIGNAL:
        NEED(MK_SYNCOBJ_ARRAY);
        return SyncArray(Dev, File, (MK_SYNCOBJ_ARRAY *)Arg, ArgSize, Nr == MALIKM_NR_SYNCOBJ_SIGNAL);
    case MALIKM_NR_SYNCOBJ_EXPORT_SEQ:
    case MALIKM_NR_SYNCOBJ_IMPORT_SEQ:
        NEED(MALIKM_SYNCOBJ_SEQ);
        return SyncSeq(Dev, File, (MALIKM_SYNCOBJ_SEQ *)Arg, Nr == MALIKM_NR_SYNCOBJ_IMPORT_SEQ);
    default:
        return -MK_ENOSYS;
    }
}

/* ---- user mappings: these run in the calling process ---- */

mk_s32 MkMap(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, MALIKM_MAP *Map)
{
    MK_MAPPING *m;
    PVOID va = NULL;
    mk_s32 r = 0;

    if ((Map->Offset & (PAGE_SIZE - 1)) != 0)
        return -MK_EINVAL;
    m = (MK_MAPPING *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*m), POOL_TAG_MAP);
    if (m == NULL)
        return -MK_ENOMEM;

    ExAcquireFastMutex(&Dev->Lock);
    MK_BO *bo = BoGet(File, MALIKM_MMAP_HANDLE(Map->Offset));
    if (bo == NULL || (bo->Flags & PANFROST_BO_HEAP)) {
        r = -MK_EINVAL;
    } else if (Map->Size == 0 || Map->Size > bo->Size) {
        r = -MK_EINVAL;
    } else {
        __try {
            va = MmMapLockedPagesSpecifyCache(bo->Mdl, UserMode, MmWriteCombined, NULL, FALSE,
                                              NormalPagePriority | MdlMappingNoExecute);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            va = NULL;
        }
        if (va == NULL) {
            r = -MK_ENOMEM;
        } else {
            bo->Maps++;
            m->Address = va;
            m->Bo = bo;
            InsertTailList(&File->Mappings, &m->Link);
            Map->Address = (mk_u64)(ULONG_PTR)va;
        }
    }
    ExReleaseFastMutex(&Dev->Lock);
    if (r != 0)
        ExFreePoolWithTag(m, POOL_TAG_MAP);
    return r;
}

static void Unmap(PDEVICE_CONTEXT Dev, MK_MAPPING *M)
{
    MmUnmapLockedPages(M->Address, M->Bo->Mdl);
    M->Bo->Maps--;
    if (M->Bo->Closed)
        MkReapZombies(Dev);
    RemoveEntryList(&M->Link);
    ExFreePoolWithTag(M, POOL_TAG_MAP);
}

mk_s32 MkUnmap(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, PVOID Address)
{
    mk_s32 r = -MK_EINVAL;

    ExAcquireFastMutex(&Dev->Lock);
    for (PLIST_ENTRY e = File->Mappings.Flink; e != &File->Mappings; e = e->Flink) {
        MK_MAPPING *m = CONTAINING_RECORD(e, MK_MAPPING, Link);
        if (m->Address == Address) {
            Unmap(Dev, m);
            r = 0;
            break;
        }
    }
    ExReleaseFastMutex(&Dev->Lock);
    return r;
}

/* The last handle to the file is gone. This runs in the closing process, so
 * its mappings can still be undone. BOs a queued job uses wait as zombies. */
void MkFileCleanup(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File)
{
    ExAcquireFastMutex(&Dev->Lock);
    while (!IsListEmpty(&File->Mappings))
        Unmap(Dev, CONTAINING_RECORD(File->Mappings.Flink, MK_MAPPING, Link));
    for (ULONG i = 0; i < File->BoCap; i++) {
        if (File->Bos[i] != NULL)
            MkBoRelease(Dev, File->Bos[i]);
        File->Bos[i] = NULL;
    }
    for (ULONG i = 0; i < File->SyncobjCap; i++) {
        if (File->Syncobjs[i] != NULL)
            ExFreePoolWithTag(File->Syncobjs[i], POOL_TAG_SYNC);
        File->Syncobjs[i] = NULL;
    }
    if (File->Bos != NULL)
        ExFreePoolWithTag(File->Bos, POOL_TAG_TABLE);
    if (File->Syncobjs != NULL)
        ExFreePoolWithTag(File->Syncobjs, POOL_TAG_TABLE);
    File->Bos = NULL;
    File->Syncobjs = NULL;
    File->BoCap = File->SyncobjCap = 0;
    ExReleaseFastMutex(&Dev->Lock);
}
