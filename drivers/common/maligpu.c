// SPDX-License-Identifier: GPL-2.0
/*
 * The Mali-G52 itself: see maligpu.h. Moved out of drivers/malikm/gpu.c
 * unchanged in behaviour, so the WDDM driver can share it.
 */
#include "maligpu.h"

/* Poll (reg & mask) == want, 10 us apart, for up to Us microseconds. */
static BOOLEAN Poll(MK_GPU *Gpu, ULONG Off, ULONG Mask, ULONG Want, ULONG Us)
{
    for (ULONG i = 0; i <= Us / 10; i++) {
        if ((MK_RD(Gpu, Off) & Mask) == Want)
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

static void ReadFeatures(MK_GPU *Gpu)
{
    GPU_FEATURES *f = &Gpu->F;
    ULONG id = MK_RD(Gpu, GPU_ID);

    f->Id = id >> 16;
    f->Revision = id & 0xffff;
    f->L2Features = MK_RD(Gpu, GPU_L2_FEATURES);
    f->CoreFeatures = MK_RD(Gpu, GPU_CORE_FEATURES);
    f->TilerFeatures = MK_RD(Gpu, GPU_TILER_FEATURES);
    f->MemFeatures = MK_RD(Gpu, GPU_MEM_FEATURES);
    f->MmuFeatures = MK_RD(Gpu, GPU_MMU_FEATURES);
    f->ThreadFeatures = MK_RD(Gpu, GPU_THREAD_FEATURES);
    f->MaxThreads = MK_RD(Gpu, GPU_THREAD_MAX_THREADS);
    f->MaxWorkgroupSize = MK_RD(Gpu, GPU_THREAD_MAX_WORKGROUP_SIZE);
    f->MaxBarrierSize = MK_RD(Gpu, GPU_THREAD_MAX_BARRIER_SIZE);
    f->CoherencyFeatures = MK_RD(Gpu, GPU_COHERENCY_FEATURES);   /* G52 has COHERENCY_REG */
    f->AfbcFeatures = MK_RD(Gpu, GPU_AFBC_FEATURES);
    for (ULONG i = 0; i < 4; i++)
        f->TextureFeatures[i] = MK_RD(Gpu, GPU_TEXTURE_FEATURES(i));
    f->AsPresent = MK_RD(Gpu, GPU_AS_PRESENT);
    f->JsPresent = MK_RD(Gpu, GPU_JS_PRESENT);
    for (ULONG i = 0; i < 16; i++)
        if (f->JsPresent & (1u << i))
            f->JsFeatures[i] = MK_RD(Gpu, GPU_JS_FEATURES(i));
    f->ShaderPresent = MK_RD(Gpu, GPU_SHADER_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_SHADER_PRESENT_HI) << 32);
    f->TilerPresent = MK_RD(Gpu, GPU_TILER_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_TILER_PRESENT_HI) << 32);
    f->L2Present = MK_RD(Gpu, GPU_L2_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_L2_PRESENT_HI) << 32);
    f->StackPresent = MK_RD(Gpu, GPU_STACK_PRESENT_LO) | ((ULONG64)MK_RD(Gpu, GPU_STACK_PRESENT_HI) << 32);
    f->ThreadTlsAlloc = MK_RD(Gpu, GPU_THREAD_TLS_ALLOC);
    f->NrCoreGroups = 0;
    for (ULONG64 m = f->L2Present; m != 0; m &= m - 1)
        f->NrCoreGroups++;
}

static BOOLEAN SoftReset(MK_GPU *Gpu)
{
    MK_WR(Gpu, GPU_INT_MASK, 0);
    MK_WR(Gpu, GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
    MK_WR(Gpu, GPU_CMD, GPU_CMD_SOFT_RESET);
    if (!Poll(Gpu, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 10000)) {
        MK_WR(Gpu, GPU_CMD, GPU_CMD_HARD_RESET);
        if (!Poll(Gpu, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 10000))
            return FALSE;
    }
    MK_WR(Gpu, GPU_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, GPU_INT_MASK, GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT);
    return TRUE;
}

static BOOLEAN GpuPowerOn(MK_GPU *Gpu)
{
    GPU_FEATURES *f = &Gpu->F;

    /* panfrost_gpu_init_quirks for a G52: no shader quirks, TILER_CONFIG
     * written back as read, the default IDVS group size. */
    MK_WR(Gpu, GPU_TILER_CONFIG, MK_RD(Gpu, GPU_TILER_CONFIG));
    MK_WR(Gpu, GPU_JM_CONFIG, JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT);

    MK_WR(Gpu, L2_PWRON_LO, (ULONG)f->L2Present);
    if (!Poll(Gpu, L2_READY_LO, 0xffffffff, (ULONG)f->L2Present, 20000))
        return FALSE;
    MK_WR(Gpu, SHADER_PWRON_LO, (ULONG)f->ShaderPresent);
    if (!Poll(Gpu, SHADER_READY_LO, 0xffffffff, (ULONG)f->ShaderPresent, 20000))
        return FALSE;
    MK_WR(Gpu, TILER_PWRON_LO, (ULONG)f->TilerPresent);
    return Poll(Gpu, TILER_READY_LO, 0xffffffff, (ULONG)f->TilerPresent, 1000);
}

static BOOLEAN AsWaitReady(MK_GPU *Gpu)
{
    return Poll(Gpu, AS0 + AS_STATUS, AS_STATUS_ACTIVE, 0, 10000);
}

static void AsEnable(MK_GPU *Gpu)
{
    MK_WR(Gpu, MMU_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, MMU_INT_MASK, 0xffffffff);
    /* Mali LPAE: table, read-inner. Memory attributes as Linux programs them
     * for this format; ATTRINDX 1 is write-allocate. */
    MK_WR(Gpu, AS0 + AS_TRANSTAB_LO, (ULONG)(Gpu->L0Pa | 0x7));
    MK_WR(Gpu, AS0 + AS_TRANSTAB_HI, (ULONG)(Gpu->L0Pa >> 32));
    MK_WR(Gpu, AS0 + AS_MEMATTR_LO, 0x00888d88);
    MK_WR(Gpu, AS0 + AS_MEMATTR_HI, 0);
    MK_WR(Gpu, AS0 + AS_TRANSCFG_LO, 0);
    MK_WR(Gpu, AS0 + AS_TRANSCFG_HI, 0);
    (void)AsWaitReady(Gpu);
    MK_WR(Gpu, AS0 + AS_COMMAND, AS_COMMAND_UPDATE);
    (void)AsWaitReady(Gpu);
}

BOOLEAN MkGpuBringup(MK_GPU *Gpu)
{
    if (!SoftReset(Gpu))
        return FALSE;
    if (!GpuPowerOn(Gpu))
        return FALSE;
    AsEnable(Gpu);
    MK_WR(Gpu, JOB_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, JOB_INT_MASK, 0x00070007);   /* done and error, slots 0-2 */
    return TRUE;
}

NTSTATUS MkGpuInit(MK_GPU *Gpu)
{
    MK_LOG(Gpu, L"Step", STEP_PMU);
    if (!PmuSaysGpuIsOn())
        return STATUS_DEVICE_NOT_READY;

    MK_LOG(Gpu, L"Step", STEP_ID);
    MK_LOG(Gpu, L"GpuId", MK_RD(Gpu, GPU_ID));
    if ((MK_RD(Gpu, GPU_ID) >> 16) != 0x7402)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    MK_LOG(Gpu, L"Step", STEP_RESET);
    if (!SoftReset(Gpu))
        return STATUS_DEVICE_NOT_READY;
    ReadFeatures(Gpu);
    MK_LOG(Gpu, L"ShaderPresent", (ULONG)Gpu->F.ShaderPresent);
    MK_LOG(Gpu, L"MmuFeatures", Gpu->F.MmuFeatures);

    MK_LOG(Gpu, L"Step", STEP_POWER);
    if (!GpuPowerOn(Gpu))
        return STATUS_DEVICE_NOT_READY;

    MK_LOG(Gpu, L"Step", STEP_MMU);
    AsEnable(Gpu);
    MK_WR(Gpu, JOB_INT_CLEAR, 0xffffffff);
    MK_WR(Gpu, JOB_INT_MASK, 0x00070007);

    Gpu->Ready = TRUE;
    MK_LOG(Gpu, L"Step", STEP_READY);
    return STATUS_SUCCESS;
}

void MkGpuStop(MK_GPU *Gpu)
{
    if (!Gpu->Ready)
        return;
    MK_WR(Gpu, JOB_INT_MASK, 0);
    MK_WR(Gpu, MMU_INT_MASK, 0);
    MK_WR(Gpu, GPU_INT_MASK, 0);
    Gpu->Ready = FALSE;
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

NTSTATUS MkMmuInit(MK_GPU *Gpu)
{
    ULONG64 l1pa;
    SIZE_T bytes = ((SIZE_T)MK_VA_PAGES + 31) / 32 * 4;

    Gpu->L0 = AllocTable(&Gpu->L0Pa);
    Gpu->L1 = AllocTable(&l1pa);
    Gpu->VaBits = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, 'kmlM');
    if (Gpu->L0 == NULL || Gpu->L1 == NULL || Gpu->VaBits == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Gpu->L0[0] = l1pa | MK_PTE_TABLE;
    RtlInitializeBitMap(&Gpu->VaMap, Gpu->VaBits, MK_VA_PAGES);
    RtlClearAllBits(&Gpu->VaMap);
    return STATUS_SUCCESS;
}

void MkMmuFree(MK_GPU *Gpu)
{
    for (ULONG g = 0; g < 4; g++) {
        for (ULONG i = 0; i < 512; i++) {
            FreeTable(Gpu->L3[g][i]);
            Gpu->L3[g][i] = NULL;
        }
        FreeTable(Gpu->L2[g]);
        Gpu->L2[g] = NULL;
    }
    FreeTable(Gpu->L1);
    FreeTable(Gpu->L0);
    Gpu->L1 = Gpu->L0 = NULL;
    if (Gpu->VaBits != NULL)
        ExFreePoolWithTag(Gpu->VaBits, 'kmlM');
    Gpu->VaBits = NULL;
}

/* The L3 table covering Va, created on first use. */
static PULONG64 L3For(MK_GPU *Gpu, ULONG64 Va)
{
    ULONG g = (ULONG)(Va >> 30) & 3, m = (ULONG)(Va >> 21) & 511;
    ULONG64 pa;

    if (Gpu->L2[g] == NULL) {
        Gpu->L2[g] = AllocTable(&pa);
        if (Gpu->L2[g] == NULL)
            return NULL;
        Gpu->L1[g] = pa | MK_PTE_TABLE;
    }
    if (Gpu->L3[g][m] == NULL) {
        Gpu->L3[g][m] = AllocTable(&pa);
        if (Gpu->L3[g][m] == NULL)
            return NULL;
        Gpu->L2[g][m] = pa | MK_PTE_TABLE;
    }
    return Gpu->L3[g][m];
}

static ULONG Fls64(ULONG64 X)
{
    ULONG idx;
    return _BitScanReverse64(&idx, X) ? idx + 1 : 0;
}

/* panfrost_mmu.c: lock the region, then flush the page-table caches. */
static void FlushRange(MK_GPU *Gpu, ULONG64 Va, ULONG64 Size)
{
    ULONG width;
    ULONG64 region;

    if (!Gpu->Ready)
        return;
    width = Fls64(Va ^ (Va + Size - 1));
    if (width < 15)
        width = 15;                         /* AS_LOCK_REGION_MIN_SIZE: 32 KB */
    width -= 1;
    region = (Va & ~((1ULL << width) - 1)) | width;

    (void)AsWaitReady(Gpu);
    MK_WR(Gpu, AS0 + AS_LOCKADDR_LO, (ULONG)region);
    MK_WR(Gpu, AS0 + AS_LOCKADDR_HI, (ULONG)(region >> 32));
    MK_WR(Gpu, AS0 + AS_COMMAND, AS_COMMAND_LOCK);
    (void)AsWaitReady(Gpu);
    MK_WR(Gpu, AS0 + AS_COMMAND, AS_COMMAND_FLUSH_PT);
    (void)AsWaitReady(Gpu);
}

BOOLEAN MkVaAlloc(MK_GPU *Gpu, ULONG Pages, BOOLEAN Exec, ULONG64 *Va)
{
    ULONG hint = 0, idx;

    if (Exec && (ULONG64)Pages * PAGE_SIZE > MK_EXEC_WINDOW)
        return FALSE;
    for (ULONG tries = 0; tries < 512; tries++) {
        idx = RtlFindClearBits(&Gpu->VaMap, Pages, hint);
        if (idx == 0xFFFFFFFF)
            return FALSE;
        ULONG64 start = MK_VA_START + ((ULONG64)idx << PAGE_SHIFT);
        ULONG64 last = start + ((ULONG64)Pages << PAGE_SHIFT) - 1;
        if (!Exec || (start / MK_EXEC_WINDOW) == (last / MK_EXEC_WINDOW)) {
            RtlSetBits(&Gpu->VaMap, idx, Pages);
            *Va = start;
            return TRUE;
        }
        hint = (ULONG)(((last & ~(MK_EXEC_WINDOW - 1)) - MK_VA_START) >> PAGE_SHIFT);
    }
    return FALSE;
}

void MkVaFree(MK_GPU *Gpu, ULONG64 Va, ULONG Pages)
{
    RtlClearBits(&Gpu->VaMap, (ULONG)((Va - MK_VA_START) >> PAGE_SHIFT), Pages);
}

NTSTATUS MkMmuMapPages(MK_GPU *Gpu, ULONG64 Va, const PFN_NUMBER *Pfn, ULONG Pages, BOOLEAN NoExec)
{
    ULONG64 attr = MK_PTE_PAGE | (NoExec ? MK_PTE_XN : 0);

    for (ULONG i = 0; i < Pages; i++) {
        ULONG64 va = Va + ((ULONG64)i << PAGE_SHIFT);
        PULONG64 l3 = L3For(Gpu, va);
        if (l3 == NULL) {
            /* Undo what was written; the tables themselves stay. */
            for (ULONG j = 0; j < i; j++) {
                ULONG64 v = Va + ((ULONG64)j << PAGE_SHIFT);
                L3For(Gpu, v)[(v >> 12) & 511] = 0;
            }
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        l3[(va >> 12) & 511] = ((ULONG64)Pfn[i] << PAGE_SHIFT) | attr;
    }
    MkStoreBarrier();
    FlushRange(Gpu, Va, (ULONG64)Pages << PAGE_SHIFT);
    return STATUS_SUCCESS;
}

void MkMmuUnmapPages(MK_GPU *Gpu, ULONG64 Va, ULONG Pages)
{
    for (ULONG i = 0; i < Pages; i++) {
        ULONG64 va = Va + ((ULONG64)i << PAGE_SHIFT);
        PULONG64 l3 = Gpu->L3[(va >> 30) & 3][(va >> 21) & 511];
        if (l3 != NULL)
            l3[(va >> 12) & 511] = 0;
    }
    MkStoreBarrier();
    FlushRange(Gpu, Va, (ULONG64)Pages << PAGE_SHIFT);
}

/* ---- jobs ---- */

void MkJobStart(MK_GPU *Gpu, ULONG Slot, ULONG64 Jc, BOOLEAN ChainFlag)
{
    ULONG cfg = 0 /* AS0 */ | JS_CONFIG_THREAD_PRI(8) |
                JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE |
                (ChainFlag ? JS_CONFIG_JOB_CHAIN_FLAG : 0);

    MK_WR(Gpu, JS_SLOT(Slot) + JS_HEAD_NEXT_LO, (ULONG)Jc);
    MK_WR(Gpu, JS_SLOT(Slot) + JS_HEAD_NEXT_HI, (ULONG)(Jc >> 32));
    MK_WR(Gpu, JS_SLOT(Slot) + JS_AFFINITY_NEXT_LO, (ULONG)Gpu->F.ShaderPresent);
    MK_WR(Gpu, JS_SLOT(Slot) + JS_AFFINITY_NEXT_HI, (ULONG)(Gpu->F.ShaderPresent >> 32));
    MK_WR(Gpu, JS_SLOT(Slot) + JS_CONFIG_NEXT, cfg);
    MkStoreBarrier();
    MK_WR(Gpu, JS_SLOT(Slot) + JS_COMMAND_NEXT, JS_COMMAND_START);
}

/* What the ISR saw plus what the raw register says now, so a misrouted
 * interrupt only costs latency. The raw bits are acknowledged. */
ULONG MkJobTakeStatus(MK_GPU *Gpu, ULONG Slot)
{
    ULONG mine = (1u << Slot) | (1u << (16 + Slot));
    ULONG bits = (ULONG)InterlockedAnd(&Gpu->JobIrqStatus, ~(LONG)mine) & mine;
    ULONG raw = MK_RD(Gpu, JOB_INT_RAWSTAT) & mine;

    if (raw != 0)
        MK_WR(Gpu, JOB_INT_CLEAR, raw);
    return bits | raw;
}

ULONG MkJobSlotStatus(MK_GPU *Gpu, ULONG Slot)
{
    return MK_RD(Gpu, JS_SLOT(Slot) + JS_STATUS);
}

void MkJobHardStop(MK_GPU *Gpu, ULONG Slot)
{
    MK_WR(Gpu, JS_SLOT(Slot) + JS_COMMAND, JS_COMMAND_HARD_STOP);
}

/* ---- interrupts ---- */

/* One handler for all three lines (job, MMU, GPU): it claims whatever is
 * pending, so a wrong line order in the DSDT costs nothing. */
BOOLEAN MkIsr(MK_GPU *Gpu)
{
    ULONG j, m, g;

    if (!Gpu->Ready)
        return FALSE;
    j = MK_RD(Gpu, JOB_INT_STAT);
    m = MK_RD(Gpu, MMU_INT_STAT);
    g = MK_RD(Gpu, GPU_INT_STAT);
    if ((j | m | g) == 0)
        return FALSE;
    if (j != 0) {
        MK_WR(Gpu, JOB_INT_CLEAR, j);
        InterlockedOr(&Gpu->JobIrqStatus, (LONG)j);
    }
    if (m != 0) {
        Gpu->LastMmuFault = MK_RD(Gpu, AS0 + AS_FAULTSTATUS);
        Gpu->LastFaultAddress = MK_RD(Gpu, AS0 + AS_FAULTADDRESS_LO) |
                                ((ULONG64)MK_RD(Gpu, AS0 + AS_FAULTADDRESS_HI) << 32);
        MK_WR(Gpu, MMU_INT_CLEAR, m);
        InterlockedOr(&Gpu->MmuIrqStatus, (LONG)m);
    }
    if (g != 0)
        MK_WR(Gpu, GPU_INT_CLEAR, g);
    Gpu->Irqs++;
    return TRUE;
}
