/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The Mali-G52 itself, shared by malikm (KMDF, Panfrost uAPI) and the WDDM
 * driver: power-up and reset, the GPU's page tables and address space, the
 * job slots and the interrupt status. No WDF, no scheduling, no objects:
 * the drivers decide who maps what and when jobs run.
 *
 * The sequences follow Linux Panfrost (panfrost_gpu.c, panfrost_mmu.c,
 * panfrost_job.c) for the G52; M1 proved the page table format and the job
 * registers on the board, M3 and M4 the rest.
 */
#pragma once
#include <ntddk.h>

/* ---- registers (names and offsets as in Linux panfrost_regs.h) ---- */
#define PMU_PA                  0x27380000ULL
#define PMU_ACK0                0x120
#define PMU_STATUS0             0x230

#define GPU_ID                  0x000
#define GPU_L2_FEATURES         0x004
#define GPU_CORE_FEATURES       0x008
#define GPU_TILER_FEATURES      0x00C
#define GPU_MEM_FEATURES        0x010
#define GPU_MMU_FEATURES        0x014
#define GPU_AS_PRESENT          0x018
#define GPU_JS_PRESENT          0x01C
#define GPU_INT_RAWSTAT         0x020
#define GPU_INT_CLEAR           0x024
#define GPU_INT_MASK            0x028
#define GPU_INT_STAT            0x02C
#define GPU_CMD                 0x030
#define GPU_STATUS              0x034
#define GPU_FAULT_STATUS        0x03C
#define GPU_FAULT_ADDRESS_LO    0x040
#define GPU_AFBC_FEATURES       0x04C
#define GPU_THREAD_MAX_THREADS  0x0A0
#define GPU_THREAD_MAX_WORKGROUP_SIZE 0x0A4
#define GPU_THREAD_MAX_BARRIER_SIZE 0x0A8
#define GPU_THREAD_FEATURES     0x0AC
#define GPU_TEXTURE_FEATURES(n) (0x0B0 + (n) * 4)
#define GPU_JS_FEATURES(n)      (0x0C0 + (n) * 4)
#define GPU_SHADER_PRESENT_LO   0x100
#define GPU_SHADER_PRESENT_HI   0x104
#define GPU_TILER_PRESENT_LO    0x110
#define GPU_TILER_PRESENT_HI    0x114
#define GPU_L2_PRESENT_LO       0x120
#define GPU_L2_PRESENT_HI       0x124
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160
#define SHADER_PWRON_LO         0x180
#define TILER_PWRON_LO          0x190
#define L2_PWRON_LO             0x1A0
#define GPU_COHERENCY_FEATURES  0x300
#define GPU_THREAD_TLS_ALLOC    0x310
#define GPU_STACK_PRESENT_LO    0xE00
#define GPU_STACK_PRESENT_HI    0xE04
#define GPU_JM_CONFIG           0xF00
#define GPU_TILER_CONFIG        0xF08

#define GPU_CMD_SOFT_RESET      0x01
#define GPU_CMD_HARD_RESET      0x02
#define GPU_IRQ_FAULT           (1u << 0)
#define GPU_IRQ_MULTIPLE_FAULT  (1u << 7)
#define GPU_IRQ_RESET_COMPLETED (1u << 8)

#define JM_IDVS_GROUP_SIZE_SHIFT 16
#define JM_DEFAULT_IDVS_GROUP_SIZE 0xF

#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define JOB_INT_MASK            0x1008
#define JOB_INT_STAT            0x100C
#define JS_BASE                 0x1800
#define JS_SLOT(n)              (JS_BASE + (n) * 0x80)
#define JS_HEAD_LO              0x00
#define JS_HEAD_HI              0x04
#define JS_COMMAND              0x20
#define JS_STATUS               0x24
#define JS_HEAD_NEXT_LO         0x40
#define JS_HEAD_NEXT_HI         0x44
#define JS_AFFINITY_NEXT_LO     0x50
#define JS_AFFINITY_NEXT_HI     0x54
#define JS_CONFIG_NEXT          0x58
#define JS_COMMAND_NEXT         0x60
#define JS_COMMAND_START        0x01
#define JS_COMMAND_HARD_STOP    0x03

#define JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3u << 8)
#define JS_CONFIG_JOB_CHAIN_FLAG (1u << 11)
#define JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE (3u << 12)
#define JS_CONFIG_THREAD_PRI(n) ((n) << 16)

#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define MMU_INT_MASK            0x2008
#define MMU_INT_STAT            0x200C
#define AS0                     0x2400
#define AS_TRANSTAB_LO          0x00
#define AS_TRANSTAB_HI          0x04
#define AS_MEMATTR_LO           0x08
#define AS_MEMATTR_HI           0x0C
#define AS_LOCKADDR_LO          0x10
#define AS_LOCKADDR_HI          0x14
#define AS_COMMAND              0x18
#define AS_FAULTSTATUS          0x1C
#define AS_FAULTADDRESS_LO      0x20
#define AS_FAULTADDRESS_HI      0x24
#define AS_STATUS               0x28
#define AS_TRANSCFG_LO          0x30
#define AS_TRANSCFG_HI          0x34
#define AS_COMMAND_UPDATE       0x01
#define AS_COMMAND_LOCK         0x02
#define AS_COMMAND_FLUSH_PT     0x04
#define AS_STATUS_ACTIVE        0x01

/* ---- the GPU virtual address space, as Linux Panfrost lays it out ---- */
#define MK_VA_START             0x2000000ULL            /* 32 MB */
#define MK_VA_END               0x100000000ULL          /* 4 GB */
#define MK_VA_PAGES             ((ULONG)((MK_VA_END - MK_VA_START) >> PAGE_SHIFT))
#define MK_EXEC_WINDOW          0x1000000ULL            /* the PC is 24 bits */

/* Mali LPAE page table entries. */
#define MK_PTE_TABLE            0x3ULL
#define MK_PTE_PAGE             0x2C5ULL    /* block | ATTRINDX 1 | read | write | outer shareable */
#define MK_PTE_XN               (1ULL << 54)

typedef struct _GPU_FEATURES {
    ULONG Id, Revision;
    ULONG64 ShaderPresent, TilerPresent, L2Present, StackPresent;
    ULONG AsPresent, JsPresent;
    ULONG L2Features, CoreFeatures, TilerFeatures, MemFeatures, MmuFeatures;
    ULONG ThreadFeatures, MaxThreads, MaxWorkgroupSize, MaxBarrierSize;
    ULONG CoherencyFeatures, AfbcFeatures, ThreadTlsAlloc, NrCoreGroups;
    ULONG TextureFeatures[4];
    ULONG JsFeatures[16];
} GPU_FEATURES;

/* Diagnostics go wherever the owning driver keeps them (the registry). */
typedef void (*MK_LOG_FN)(void *Owner, PCWSTR Name, ULONG Value);

typedef struct _MK_GPU {
    volatile ULONG *Regs;
    SIZE_T RegsLength;
    BOOLEAN Ready;              /* powered and AS0 live: registers may be touched */
    GPU_FEATURES F;

    /* AS0's page tables: L0, one L1, up to four L2 (one per GB), L3 on demand.
     * Callers serialise map and unmap. */
    PULONG64 L0, L1, L2[4];
    PULONG64 L3[4][512];
    ULONG64 L0Pa;
    RTL_BITMAP VaMap;
    PULONG VaBits;

    /* What the ISR collected, for whoever waits on jobs. */
    volatile LONG JobIrqStatus; /* JOB_INT_STAT bits */
    volatile LONG MmuIrqStatus;
    ULONG Irqs, Resets, LastMmuFault;
    ULONG64 LastFaultAddress;

    MK_LOG_FN Log;
    void *LogOwner;
} MK_GPU;

#define MK_RD(g, o)     READ_REGISTER_ULONG((PULONG)((PUCHAR)(g)->Regs + (o)))
#define MK_WR(g, o, v)  WRITE_REGISTER_ULONG((PULONG)((PUCHAR)(g)->Regs + (o)), (v))
#define MK_LOG(g, n, v) do { if ((g)->Log) (g)->Log((g)->LogOwner, (n), (v)); } while (0)

/* Complete every store this CPU has made, to memory the GPU reads through a
 * write-combined (Normal non-cacheable) mapping, before the GPU is told. */
#define MkStoreBarrier() __dsb(_ARM64_BARRIER_SY)

/* Power-up from cold; Bringup again after a fault (reset, power, AS0, masks). */
NTSTATUS MkGpuInit(MK_GPU *Gpu);
BOOLEAN MkGpuBringup(MK_GPU *Gpu);
void MkGpuStop(MK_GPU *Gpu);

/* Page tables and addresses. MkVaAlloc keeps executable ranges inside one
 * 16 MB window (the shader program counter is 24 bits). */
NTSTATUS MkMmuInit(MK_GPU *Gpu);
void MkMmuFree(MK_GPU *Gpu);
BOOLEAN MkVaAlloc(MK_GPU *Gpu, ULONG Pages, BOOLEAN Exec, ULONG64 *Va);
void MkVaFree(MK_GPU *Gpu, ULONG64 Va, ULONG Pages);
NTSTATUS MkMmuMapPages(MK_GPU *Gpu, ULONG64 Va, const PFN_NUMBER *Pfn, ULONG Pages, BOOLEAN NoExec);
void MkMmuUnmapPages(MK_GPU *Gpu, ULONG64 Va, ULONG Pages);

/* Jobs: start a chain on a slot without waiting; collect its done (bit
 * slot) and error (bit 16 + slot) status, from the ISR or the raw register. */
void MkJobStart(MK_GPU *Gpu, ULONG Slot, ULONG64 Jc, BOOLEAN ChainFlag);
ULONG MkJobTakeStatus(MK_GPU *Gpu, ULONG Slot);
ULONG MkJobSlotStatus(MK_GPU *Gpu, ULONG Slot);
void MkJobHardStop(MK_GPU *Gpu, ULONG Slot);

/* One handler for all three lines; claims whatever is pending. */
BOOLEAN MkIsr(MK_GPU *Gpu);
