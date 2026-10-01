// SPDX-License-Identifier: GPL-2.0
/*
 * The hardware side of malikm: power-up, the GPU's page tables, the job
 * worker and the interrupt handler. The sequences follow Linux Panfrost
 * (panfrost_gpu.c, panfrost_mmu.c, panfrost_job.c) for the G52; M1 proved
 * the page table format and the job registers on this board.
 */
#include "malikm.h"

void MkLog(PDEVICE_CONTEXT Dev, PCWSTR Name, ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING name;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "malikm: %ws = 0x%08x\n", Name, Value);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Dev->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&name, Name);
        (void)WdfRegistryAssignULong(key, &name, Value);
        WdfRegistryClose(key);
    }
}

/* Poll (reg & mask) == want, 10 us apart, for up to Us microseconds. */
static BOOLEAN Poll(PDEVICE_CONTEXT Dev, ULONG Off, ULONG Mask, ULONG Want, ULONG Us)
{
    for (ULONG i = 0; i <= Us / 10; i++) {
        if ((RD(Dev, Off) & Mask) == Want)
            return TRUE;
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

/* ---- power-up ---- */

enum { STEP_PMU = 1, STEP_ID, STEP_RESET, STEP_POWER, STEP_MMU, STEP_READY };

/* The firmware powers PD_GPU and sets its clock. A GPU access while the
 * domain is off or its bus is idle is an SError, so look before touching. */
static BOOLEAN PmuSaysGpuIsOn(void)
{
    PHYSICAL_ADDRESS pa;
    volatile ULONG *pmu;
    ULONG status, ack;

    pa.QuadPart = PMU_PA;
    pmu = (volatile ULONG *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    if (pmu == NULL)
        return FALSE;
    status = READ_REGISTER_ULONG((PULONG)((PUCHAR)pmu + PMU_STATUS0));
    ack = READ_REGISTER_ULONG((PULONG)((PUCHAR)pmu + PMU_ACK0));
    MmUnmapIoSpace((PVOID)pmu, 0x1000);
    return ((status >> 25) & 1) == 0 && (ack & 1) == 0;
}

static void ReadFeatures(PDEVICE_CONTEXT Dev)
{
    GPU_FEATURES *f = &Dev->F;
    ULONG id = RD(Dev, GPU_ID);

    f->Id = id >> 16;
    f->Revision = id & 0xffff;
    f->L2Features = RD(Dev, GPU_L2_FEATURES);
    f->CoreFeatures = RD(Dev, GPU_CORE_FEATURES);
    f->TilerFeatures = RD(Dev, GPU_TILER_FEATURES);
    f->MemFeatures = RD(Dev, GPU_MEM_FEATURES);
    f->MmuFeatures = RD(Dev, GPU_MMU_FEATURES);
    f->ThreadFeatures = RD(Dev, GPU_THREAD_FEATURES);
    f->MaxThreads = RD(Dev, GPU_THREAD_MAX_THREADS);
    f->MaxWorkgroupSize = RD(Dev, GPU_THREAD_MAX_WORKGROUP_SIZE);
    f->MaxBarrierSize = RD(Dev, GPU_THREAD_MAX_BARRIER_SIZE);
    f->CoherencyFeatures = RD(Dev, GPU_COHERENCY_FEATURES);   /* G52 has COHERENCY_REG */
    f->AfbcFeatures = RD(Dev, GPU_AFBC_FEATURES);
    for (ULONG i = 0; i < 4; i++)
        f->TextureFeatures[i] = RD(Dev, GPU_TEXTURE_FEATURES(i));
    f->AsPresent = RD(Dev, GPU_AS_PRESENT);
    f->JsPresent = RD(Dev, GPU_JS_PRESENT);
    for (ULONG i = 0; i < 16; i++)
        if (f->JsPresent & (1u << i))
            f->JsFeatures[i] = RD(Dev, GPU_JS_FEATURES(i));
    f->ShaderPresent = RD(Dev, GPU_SHADER_PRESENT_LO) | ((ULONG64)RD(Dev, GPU_SHADER_PRESENT_HI) << 32);
    f->TilerPresent = RD(Dev, GPU_TILER_PRESENT_LO) | ((ULONG64)RD(Dev, GPU_TILER_PRESENT_HI) << 32);
    f->L2Present = RD(Dev, GPU_L2_PRESENT_LO) | ((ULONG64)RD(Dev, GPU_L2_PRESENT_HI) << 32);
    f->StackPresent = RD(Dev, GPU_STACK_PRESENT_LO) | ((ULONG64)RD(Dev, GPU_STACK_PRESENT_HI) << 32);
    f->ThreadTlsAlloc = RD(Dev, GPU_THREAD_TLS_ALLOC);
    f->NrCoreGroups = 0;
    for (ULONG64 m = f->L2Present; m != 0; m &= m - 1)
        f->NrCoreGroups++;
}

static BOOLEAN SoftReset(PDEVICE_CONTEXT Dev)
{
    WR(Dev, GPU_INT_MASK, 0);
    WR(Dev, GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
    WR(Dev, GPU_CMD, GPU_CMD_SOFT_RESET);
    if (!Poll(Dev, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 10000)) {
        WR(Dev, GPU_CMD, GPU_CMD_HARD_RESET);
        if (!Poll(Dev, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 10000))
            return FALSE;
    }
    WR(Dev, GPU_INT_CLEAR, 0xffffffff);
    WR(Dev, GPU_INT_MASK, GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT);
    return TRUE;
}

static BOOLEAN PowerOn(PDEVICE_CONTEXT Dev)
{
    GPU_FEATURES *f = &Dev->F;

    /* panfrost_gpu_init_quirks for a G52: no shader quirks, TILER_CONFIG
     * written back as read, the default IDVS group size. */
    WR(Dev, GPU_TILER_CONFIG, RD(Dev, GPU_TILER_CONFIG));
    WR(Dev, GPU_JM_CONFIG, JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT);

    WR(Dev, L2_PWRON_LO, (ULONG)f->L2Present);
    if (!Poll(Dev, L2_READY_LO, 0xffffffff, (ULONG)f->L2Present, 20000))
        return FALSE;
    WR(Dev, SHADER_PWRON_LO, (ULONG)f->ShaderPresent);
    if (!Poll(Dev, SHADER_READY_LO, 0xffffffff, (ULONG)f->ShaderPresent, 20000))
        return FALSE;
    WR(Dev, TILER_PWRON_LO, (ULONG)f->TilerPresent);
    return Poll(Dev, TILER_READY_LO, 0xffffffff, (ULONG)f->TilerPresent, 1000);
}

static BOOLEAN AsWaitReady(PDEVICE_CONTEXT Dev)
{
    return Poll(Dev, AS0 + AS_STATUS, AS_STATUS_ACTIVE, 0, 10000);
}

static void AsEnable(PDEVICE_CONTEXT Dev)
{
    WR(Dev, MMU_INT_CLEAR, 0xffffffff);
    WR(Dev, MMU_INT_MASK, 0xffffffff);
    /* Mali LPAE: table, read-inner. Memory attributes as Linux programs them
     * for this format; ATTRINDX 1 is write-allocate. */
    WR(Dev, AS0 + AS_TRANSTAB_LO, (ULONG)(Dev->L0Pa | 0x7));
    WR(Dev, AS0 + AS_TRANSTAB_HI, (ULONG)(Dev->L0Pa >> 32));
    WR(Dev, AS0 + AS_MEMATTR_LO, 0x00888d88);
    WR(Dev, AS0 + AS_MEMATTR_HI, 0);
    WR(Dev, AS0 + AS_TRANSCFG_LO, 0);
    WR(Dev, AS0 + AS_TRANSCFG_HI, 0);
    (void)AsWaitReady(Dev);
    WR(Dev, AS0 + AS_COMMAND, AS_COMMAND_UPDATE);
    (void)AsWaitReady(Dev);
}

/* Reset, power, address space, interrupt masks: from cold and after a fault. */
static BOOLEAN Bringup(PDEVICE_CONTEXT Dev)
{
    if (!SoftReset(Dev))
        return FALSE;
    if (!PowerOn(Dev))
        return FALSE;
    AsEnable(Dev);
    WR(Dev, JOB_INT_CLEAR, 0xffffffff);
    WR(Dev, JOB_INT_MASK, 0x00070007);   /* done and error, slots 0-2 */
    return TRUE;
}

NTSTATUS MkGpuInit(PDEVICE_CONTEXT Dev)
{
    MkLog(Dev, L"Step", STEP_PMU);
    if (!PmuSaysGpuIsOn())
        return STATUS_DEVICE_NOT_READY;

    MkLog(Dev, L"Step", STEP_ID);
    MkLog(Dev, L"GpuId", RD(Dev, GPU_ID));
    if ((RD(Dev, GPU_ID) >> 16) != 0x7402)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    MkLog(Dev, L"Step", STEP_RESET);
    if (!SoftReset(Dev))
        return STATUS_DEVICE_NOT_READY;
    ReadFeatures(Dev);
    MkLog(Dev, L"ShaderPresent", (ULONG)Dev->F.ShaderPresent);
    MkLog(Dev, L"MmuFeatures", Dev->F.MmuFeatures);

    MkLog(Dev, L"Step", STEP_POWER);
    if (!PowerOn(Dev))
        return STATUS_DEVICE_NOT_READY;

    MkLog(Dev, L"Step", STEP_MMU);
    AsEnable(Dev);
    WR(Dev, JOB_INT_CLEAR, 0xffffffff);
    WR(Dev, JOB_INT_MASK, 0x00070007);

    Dev->Ready = TRUE;
    MkLog(Dev, L"Step", STEP_READY);
    return STATUS_SUCCESS;
}

void MkGpuStop(PDEVICE_CONTEXT Dev)
{
    if (!Dev->Ready)
        return;
    WR(Dev, JOB_INT_MASK, 0);
    WR(Dev, MMU_INT_MASK, 0);
    WR(Dev, GPU_INT_MASK, 0);
    Dev->Ready = FALSE;
}

/* ---- page tables ---- */

static PULONG64 AllocTable(PULONG64 Pa)
{
    PHYSICAL_ADDRESS low, high, skip;
    PVOID p;

    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFFFULL;        /* the GPU sees 40 bits */
    skip.QuadPart = 0;
    /* Uncached, so the GPU's walker sees every write without maintenance. */
    p = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high, skip, MmNonCached);
    if (p == NULL)
        return NULL;
    RtlZeroMemory(p, PAGE_SIZE);
    *Pa = (ULONG64)MmGetPhysicalAddress(p).QuadPart;
    return (PULONG64)p;
}

static void FreeTable(PULONG64 T)
{
    if (T != NULL)
        MmFreeContiguousMemorySpecifyCache(T, PAGE_SIZE, MmNonCached);
}

NTSTATUS MkMmuInit(PDEVICE_CONTEXT Dev)
{
    ULONG64 l1pa;
    SIZE_T bytes = ((SIZE_T)MK_VA_PAGES + 31) / 32 * 4;

    Dev->L0 = AllocTable(&Dev->L0Pa);
    Dev->L1 = AllocTable(&l1pa);
    Dev->VaBits = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, 'kmlM');
    if (Dev->L0 == NULL || Dev->L1 == NULL || Dev->VaBits == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Dev->L0[0] = l1pa | MK_PTE_TABLE;
    RtlInitializeBitMap(&Dev->VaMap, Dev->VaBits, MK_VA_PAGES);
    RtlClearAllBits(&Dev->VaMap);
    return STATUS_SUCCESS;
}

void MkMmuFree(PDEVICE_CONTEXT Dev)
{
    for (ULONG g = 0; g < 4; g++) {
        for (ULONG i = 0; i < 512; i++) {
            FreeTable(Dev->L3[g][i]);
            Dev->L3[g][i] = NULL;
        }
        FreeTable(Dev->L2[g]);
        Dev->L2[g] = NULL;
    }
    FreeTable(Dev->L1);
    FreeTable(Dev->L0);
    Dev->L1 = Dev->L0 = NULL;
    if (Dev->VaBits != NULL)
        ExFreePoolWithTag(Dev->VaBits, 'kmlM');
    Dev->VaBits = NULL;
}

/* The L3 table covering Va, created on first use. */
static PULONG64 L3For(PDEVICE_CONTEXT Dev, ULONG64 Va)
{
    ULONG g = (ULONG)(Va >> 30) & 3, m = (ULONG)(Va >> 21) & 511;
    ULONG64 pa;

    if (Dev->L2[g] == NULL) {
        Dev->L2[g] = AllocTable(&pa);
        if (Dev->L2[g] == NULL)
            return NULL;
        Dev->L1[g] = pa | MK_PTE_TABLE;
    }
    if (Dev->L3[g][m] == NULL) {
        Dev->L3[g][m] = AllocTable(&pa);
        if (Dev->L3[g][m] == NULL)
            return NULL;
        Dev->L2[g][m] = pa | MK_PTE_TABLE;
    }
    return Dev->L3[g][m];
}

static ULONG Fls64(ULONG64 X)
{
    ULONG idx;
    return _BitScanReverse64(&idx, X) ? idx + 1 : 0;
}

/* panfrost_mmu.c: lock the region, then flush the page-table caches. */
static void FlushRange(PDEVICE_CONTEXT Dev, ULONG64 Va, ULONG64 Size)
{
    ULONG width;
    ULONG64 region;

    if (!Dev->Ready)
        return;
    width = Fls64(Va ^ (Va + Size - 1));
    if (width < 15)
        width = 15;                         /* AS_LOCK_REGION_MIN_SIZE: 32 KB */
    width -= 1;
    region = (Va & ~((1ULL << width) - 1)) | width;

    (void)AsWaitReady(Dev);
    WR(Dev, AS0 + AS_LOCKADDR_LO, (ULONG)region);
    WR(Dev, AS0 + AS_LOCKADDR_HI, (ULONG)(region >> 32));
    WR(Dev, AS0 + AS_COMMAND, AS_COMMAND_LOCK);
    (void)AsWaitReady(Dev);
    WR(Dev, AS0 + AS_COMMAND, AS_COMMAND_FLUSH_PT);
    (void)AsWaitReady(Dev);
}

/* Find GPU addresses for Pages pages. Executable buffers must not cross a
 * 16 MB boundary: the shader program counter is 24 bits. */
static BOOLEAN AllocVa(PDEVICE_CONTEXT Dev, ULONG Pages, BOOLEAN Exec, ULONG64 *Va)
{
    ULONG hint = 0, idx;

    if (Exec && (ULONG64)Pages * PAGE_SIZE > MK_EXEC_WINDOW)
        return FALSE;
    for (ULONG tries = 0; tries < 512; tries++) {
        idx = RtlFindClearBits(&Dev->VaMap, Pages, hint);
        if (idx == 0xFFFFFFFF)
            return FALSE;
        ULONG64 start = MK_VA_START + ((ULONG64)idx << PAGE_SHIFT);
        ULONG64 last = start + ((ULONG64)Pages << PAGE_SHIFT) - 1;
        if (!Exec || (start / MK_EXEC_WINDOW) == (last / MK_EXEC_WINDOW)) {
            RtlSetBits(&Dev->VaMap, idx, Pages);
            *Va = start;
            return TRUE;
        }
        hint = (ULONG)(((last & ~(MK_EXEC_WINDOW - 1)) - MK_VA_START) >> PAGE_SHIFT);
    }
    return FALSE;
}

NTSTATUS MkMmuMap(PDEVICE_CONTEXT Dev, MK_BO *Bo)
{
    ULONG pages = (ULONG)(Bo->Size >> PAGE_SHIFT);
    PPFN_NUMBER pfn = MmGetMdlPfnArray(Bo->Mdl);
    ULONG64 attr = MK_PTE_PAGE | ((Bo->Flags & PANFROST_BO_NOEXEC) ? MK_PTE_XN : 0);

    if (!AllocVa(Dev, pages, (Bo->Flags & PANFROST_BO_NOEXEC) == 0, &Bo->GpuVa))
        return STATUS_INSUFFICIENT_RESOURCES;

    for (ULONG i = 0; i < pages; i++) {
        ULONG64 va = Bo->GpuVa + ((ULONG64)i << PAGE_SHIFT);
        PULONG64 l3 = L3For(Dev, va);
        if (l3 == NULL) {
            /* Undo what was written; the tables themselves stay. */
            for (ULONG j = 0; j < i; j++) {
                ULONG64 v = Bo->GpuVa + ((ULONG64)j << PAGE_SHIFT);
                L3For(Dev, v)[(v >> 12) & 511] = 0;
            }
            RtlClearBits(&Dev->VaMap, (ULONG)((Bo->GpuVa - MK_VA_START) >> PAGE_SHIFT), pages);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        l3[(va >> 12) & 511] = ((ULONG64)pfn[i] << PAGE_SHIFT) | attr;
    }
    KeMemoryBarrier();
    FlushRange(Dev, Bo->GpuVa, Bo->Size);
    return STATUS_SUCCESS;
}

void MkMmuUnmap(PDEVICE_CONTEXT Dev, MK_BO *Bo)
{
    ULONG pages = (ULONG)(Bo->Size >> PAGE_SHIFT);

    for (ULONG i = 0; i < pages; i++) {
        ULONG64 va = Bo->GpuVa + ((ULONG64)i << PAGE_SHIFT);
        PULONG64 l3 = Dev->L3[(va >> 30) & 3][(va >> 21) & 511];
        if (l3 != NULL)
            l3[(va >> 12) & 511] = 0;
    }
    KeMemoryBarrier();
    FlushRange(Dev, Bo->GpuVa, Bo->Size);
    RtlClearBits(&Dev->VaMap, (ULONG)((Bo->GpuVa - MK_VA_START) >> PAGE_SHIFT), pages);
}

/* ---- interrupts ---- */

/* One handler for all three lines (job, MMU, GPU): it claims whatever is
 * pending, so a wrong line order in the DSDT costs nothing. */
BOOLEAN MkIsr(PDEVICE_CONTEXT Dev)
{
    ULONG j, m, g;

    if (!Dev->Ready)
        return FALSE;
    j = RD(Dev, JOB_INT_STAT);
    m = RD(Dev, MMU_INT_STAT);
    g = RD(Dev, GPU_INT_STAT);
    if ((j | m | g) == 0)
        return FALSE;
    if (j != 0) {
        WR(Dev, JOB_INT_CLEAR, j);
        InterlockedOr(&Dev->JobIrqStatus, (LONG)j);
    }
    if (m != 0) {
        Dev->LastMmuFault = RD(Dev, AS0 + AS_FAULTSTATUS);
        Dev->LastFaultAddress = RD(Dev, AS0 + AS_FAULTADDRESS_LO) |
                                ((ULONG64)RD(Dev, AS0 + AS_FAULTADDRESS_HI) << 32);
        WR(Dev, MMU_INT_CLEAR, m);
        InterlockedOr(&Dev->MmuIrqStatus, (LONG)m);
    }
    if (g != 0)
        WR(Dev, GPU_INT_CLEAR, g);
    Dev->Irqs++;
    return TRUE;
}

/* ---- jobs ---- */

#define JOB_TIMEOUT_MS  2000

static void RunJob(PDEVICE_CONTEXT Dev, MK_JOB *Job)
{
    ULONG slot = (Job->Requirements & PANFROST_JD_REQ_FS) ? 0 : 1;
    ULONG done = 1u << slot, err = 1u << (16 + slot);
    ULONG cfg, bits;
    ULONG64 start = KeQueryInterruptTime();
    LARGE_INTEGER tick;

    InterlockedExchange(&Dev->JobIrqStatus, 0);
    InterlockedExchange(&Dev->MmuIrqStatus, 0);
    KeClearEvent(&Dev->JobIrqEvent);

    cfg = 0 /* AS0 */ | JS_CONFIG_THREAD_PRI(8) |
          JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE |
          ((Job->Seq & 1) ? JS_CONFIG_JOB_CHAIN_FLAG : 0);
    WR(Dev, JS_SLOT(slot) + JS_HEAD_NEXT_LO, (ULONG)Job->Jc);
    WR(Dev, JS_SLOT(slot) + JS_HEAD_NEXT_HI, (ULONG)(Job->Jc >> 32));
    WR(Dev, JS_SLOT(slot) + JS_AFFINITY_NEXT_LO, (ULONG)Dev->F.ShaderPresent);
    WR(Dev, JS_SLOT(slot) + JS_AFFINITY_NEXT_HI, (ULONG)(Dev->F.ShaderPresent >> 32));
    WR(Dev, JS_SLOT(slot) + JS_CONFIG_NEXT, cfg);
    WR(Dev, JS_SLOT(slot) + JS_COMMAND_NEXT, JS_COMMAND_START);

    /* The interrupt wakes us; the raw status is also read every tick, so a
     * misrouted interrupt only costs latency. */
    tick.QuadPart = -10000;                 /* 1 ms, relative */
    for (;;) {
        (void)KeWaitForSingleObject(&Dev->JobIrqEvent, Executive, KernelMode, FALSE, &tick);
        bits = (ULONG)InterlockedExchange(&Dev->JobIrqStatus, 0);
        ULONG raw = RD(Dev, JOB_INT_RAWSTAT) & (done | err);
        if (raw != 0)
            WR(Dev, JOB_INT_CLEAR, raw);
        bits |= raw;

        if (bits & err) {
            Dev->LastJsStatus = RD(Dev, JS_SLOT(slot) + JS_STATUS);
            Dev->JobsFailed++;
            break;
        }
        if (bits & done) {
            Dev->JobsDone++;
            return;
        }
        if (Dev->MmuIrqStatus != 0) {
            Dev->JobsFailed++;
            break;
        }
        if ((KeQueryInterruptTime() - start) / 10000 > JOB_TIMEOUT_MS) {
            Dev->LastJsStatus = RD(Dev, JS_SLOT(slot) + JS_STATUS);
            WR(Dev, JS_SLOT(slot) + JS_COMMAND, JS_COMMAND_HARD_STOP);
            Dev->JobsTimedOut++;
            break;
        }
    }

    /* Something went wrong: record it and start the GPU over. */
    MkLog(Dev, L"JobsFailed", Dev->JobsFailed);
    MkLog(Dev, L"JobsTimedOut", Dev->JobsTimedOut);
    MkLog(Dev, L"LastJsStatus", Dev->LastJsStatus);
    MkLog(Dev, L"LastMmuFault", Dev->LastMmuFault);
    MkLog(Dev, L"LastFaultAddressLo", (ULONG)Dev->LastFaultAddress);
    MkLog(Dev, L"LastFaultAddressHi", (ULONG)(Dev->LastFaultAddress >> 32));
    MkLog(Dev, L"LastFailedJcLo", (ULONG)Job->Jc);
    Dev->Resets++;
    if (!Bringup(Dev))
        MkLog(Dev, L"ResetFailed", Dev->Resets);
    MkLog(Dev, L"Resets", Dev->Resets);
}

static void Complete(PDEVICE_CONTEXT Dev, MK_JOB *Job)
{
    InterlockedExchange64(&Dev->CompletedSeq, (LONG64)Job->Seq);
    KeSetEvent(&Dev->ProgressEvent, 0, FALSE);
    ExAcquireFastMutex(&Dev->Lock);
    MkReapZombies(Dev);
    ExReleaseFastMutex(&Dev->Lock);
    ExFreePoolWithTag(Job, 'jmlM');
}

static MK_JOB *Pop(PDEVICE_CONTEXT Dev)
{
    KIRQL irql;
    PLIST_ENTRY e = NULL;

    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    if (!IsListEmpty(&Dev->Queue))
        e = RemoveHeadList(&Dev->Queue);
    KeReleaseSpinLock(&Dev->QueueLock, irql);
    return e ? CONTAINING_RECORD(e, MK_JOB, Link) : NULL;
}

static VOID Worker(PVOID Context)
{
    PDEVICE_CONTEXT dev = (PDEVICE_CONTEXT)Context;
    MK_JOB *job;

    for (;;) {
        (void)KeWaitForSingleObject(&dev->QueueEvent, Executive, KernelMode, FALSE, NULL);
        while ((job = Pop(dev)) != NULL) {
            if (!dev->StopWorker) {
                KeClearEvent(&dev->ProgressEvent);
                RunJob(dev, job);
                if (dev->JobsDone == 1 || (dev->JobsDone & 255) == 0)
                    MkLog(dev, L"JobsDone", dev->JobsDone);
            }
            Complete(dev, job);
        }
        if (dev->StopWorker)
            break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS MkWorkerStart(PDEVICE_CONTEXT Dev)
{
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;

    Dev->StopWorker = FALSE;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, Worker, Dev);
    if (!NT_SUCCESS(st))
        return st;
    st = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                   (PVOID *)&Dev->Worker, NULL);
    ZwClose(h);
    return st;
}

void MkWorkerStop(PDEVICE_CONTEXT Dev)
{
    if (Dev->Worker == NULL)
        return;
    Dev->StopWorker = TRUE;
    KeSetEvent(&Dev->QueueEvent, 0, FALSE);
    (void)KeWaitForSingleObject(Dev->Worker, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(Dev->Worker);
    Dev->Worker = NULL;
    MkLog(Dev, L"JobsDone", Dev->JobsDone);
    MkLog(Dev, L"Irqs", Dev->Irqs);
}

void MkQueueJob(PDEVICE_CONTEXT Dev, MK_JOB *Job)
{
    KIRQL irql;

    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    InsertTailList(&Dev->Queue, &Job->Link);
    KeReleaseSpinLock(&Dev->QueueLock, irql);
    KeSetEvent(&Dev->QueueEvent, 0, FALSE);
}
