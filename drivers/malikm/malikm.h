/* SPDX-License-Identifier: GPL-2.0 */
/*
 * malikm: the Mali-G52 kernel driver for milestone M3. It gives Mesa's
 * Panfrost the Linux Panfrost uAPI (version 1.1) through \\.\MaliG52, so
 * Panfrost renders on the GPU from inside the D3D10 software-driver DLL.
 *
 * One GPU address space (AS0) for every client, one job on the GPU at a
 * time. Both are simplifications that cost speed, not correctness: jobs run
 * in submission order, so every dependency between them is already met.
 */
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "malikm_ioctl.h"

/* ntifs.h, which cannot be included next to ntddk.h. */
NTKERNELAPI BOOLEAN PsIsThreadTerminating(PETHREAD Thread);

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

/* Mali LPAE page table entries (see docs/M3-ABI.md). */
#define MK_PTE_TABLE            0x3ULL
#define MK_PTE_PAGE             0x2C5ULL    /* block | ATTRINDX 1 | read | write | outer shareable */
#define MK_PTE_XN               (1ULL << 54)

/* ---- Linux uAPI structs ---- */
typedef struct { mk_u64 jc, in_syncs; mk_u32 in_sync_count, out_sync; mk_u64 bo_handles;
                 mk_u32 bo_handle_count, requirements, jm_ctx_handle, pad; } MK_SUBMIT;
typedef struct { mk_u32 handle, pad; mk_s64 timeout_ns; } MK_WAIT_BO;
typedef struct { mk_u32 size, flags, handle, pad; mk_u64 offset; } MK_CREATE_BO;
typedef struct { mk_u32 handle, flags; mk_u64 offset; } MK_MMAP_BO;
typedef struct { mk_u32 param, pad; mk_u64 value; } MK_GET_PARAM;
typedef struct { mk_u32 handle, pad; mk_u64 offset; } MK_GET_BO_OFFSET;
typedef struct { mk_u32 handle, madv, retained; } MK_MADVISE;
typedef struct { mk_u32 handle, pad; } MK_GEM_CLOSE;
typedef struct { mk_u32 handle, flags; } MK_SYNCOBJ_CREATE;
typedef struct { mk_u32 handle, pad; } MK_SYNCOBJ_DESTROY;
typedef struct { mk_u64 handles; mk_s64 timeout_nsec; mk_u32 count_handles, flags, first_signaled, pad;
                 mk_u64 deadline_nsec; } MK_SYNCOBJ_WAIT;
typedef struct { mk_u64 handles; mk_u32 count_handles, pad; } MK_SYNCOBJ_ARRAY;

#define PANFROST_JD_REQ_FS      1
#define PANFROST_BO_NOEXEC      1
#define PANFROST_BO_HEAP        2
#define DRM_SYNCOBJ_CREATE_SIGNALED 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT 2

/* ---- objects ---- */
typedef struct _MK_BO {
    LIST_ENTRY Link;            /* the device's zombie list, once closed */
    PMDL Mdl;
    SIZE_T Size;
    ULONG64 GpuVa;
    ULONG Flags;                /* PANFROST_BO_* */
    ULONG64 LastSeq;            /* last job that listed this BO */
    LONG Maps;                  /* live user mappings */
    BOOLEAN Closed;             /* handle gone; freed once idle and unmapped */
} MK_BO;

typedef struct _MK_SYNCOBJ {
    BOOLEAN HasFence;           /* DRM: a syncobj may hold no fence at all */
    ULONG64 Seq;                /* the fence: signalled once CompletedSeq >= Seq */
} MK_SYNCOBJ;

typedef struct _MK_MAPPING {
    LIST_ENTRY Link;
    PVOID Address;
    MK_BO *Bo;
} MK_MAPPING;

typedef struct _MK_JOB {
    LIST_ENTRY Link;
    ULONG64 Seq;
    ULONG64 Jc;
    ULONG Requirements;
} MK_JOB;

/* Per open handle (per WDFFILEOBJECT). Handles are index + 1. */
typedef struct _FILE_CONTEXT {
    MK_BO **Bos;
    ULONG BoCap;
    MK_SYNCOBJ **Syncobjs;
    ULONG SyncobjCap;
    LIST_ENTRY Mappings;
} FILE_CONTEXT, *PFILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILE_CONTEXT, GetFileContext)

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

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE Device;
    volatile ULONG *Gpu;
    SIZE_T GpuLength;
    BOOLEAN Ready;              /* GPU powered and AS0 live */
    GPU_FEATURES F;

    /* Serialises every object table and the page tables. */
    FAST_MUTEX Lock;

    /* Page tables: L0, one L1, up to four L2 (one per GB), L3 on demand. */
    PULONG64 L0, L1, L2[4];
    PULONG64 L3[4][512];
    ULONG64 L0Pa;
    RTL_BITMAP VaMap;
    PULONG VaBits;

    /* Jobs. */
    KSPIN_LOCK QueueLock;
    LIST_ENTRY Queue;
    KEVENT QueueEvent;          /* synchronization: work queued */
    KEVENT JobIrqEvent;         /* synchronization: a job or MMU interrupt */
    KEVENT ProgressEvent;       /* notification: CompletedSeq moved */
    volatile LONG64 SubmittedSeq;
    volatile LONG64 CompletedSeq;
    volatile LONG JobIrqStatus; /* JOB_INT_STAT bits the ISR collected */
    volatile LONG MmuIrqStatus;
    PKTHREAD Worker;
    BOOLEAN StopWorker;
    LIST_ENTRY Zombies;         /* closed BOs a queued job may still use */

    /* Counters, mirrored to the registry for diagnosis. */
    ULONG JobsDone, JobsFailed, JobsTimedOut, Irqs, Resets;
    ULONG LastJsStatus, LastMmuFault;
    ULONG64 LastFaultAddress;

    WDFINTERRUPT Interrupts[3];
    ULONG InterruptCount;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* Complete every store this CPU has made, to memory the GPU reads through a
 * write-combined (Normal non-cacheable) mapping, before the GPU is told. */
#define MkStoreBarrier() __dsb(_ARM64_BARRIER_SY)

#define RD(d, o)        READ_REGISTER_ULONG((PULONG)((PUCHAR)(d)->Gpu + (o)))
#define WR(d, o, v)     WRITE_REGISTER_ULONG((PULONG)((PUCHAR)(d)->Gpu + (o)), (v))

/* gpu.c */
NTSTATUS MkGpuInit(PDEVICE_CONTEXT Dev);
void MkGpuStop(PDEVICE_CONTEXT Dev);
NTSTATUS MkMmuInit(PDEVICE_CONTEXT Dev);
void MkMmuFree(PDEVICE_CONTEXT Dev);
NTSTATUS MkMmuMap(PDEVICE_CONTEXT Dev, MK_BO *Bo);
void MkMmuUnmap(PDEVICE_CONTEXT Dev, MK_BO *Bo);
NTSTATUS MkWorkerStart(PDEVICE_CONTEXT Dev);
void MkWorkerStop(PDEVICE_CONTEXT Dev);
void MkQueueJob(PDEVICE_CONTEXT Dev, MK_JOB *Job);
BOOLEAN MkIsr(PDEVICE_CONTEXT Dev);
void MkLog(PDEVICE_CONTEXT Dev, PCWSTR Name, ULONG Value);

/* bo.c */
void MkBoRelease(PDEVICE_CONTEXT Dev, MK_BO *Bo);   /* caller holds Lock */
void MkReapZombies(PDEVICE_CONTEXT Dev);             /* caller holds Lock */

/* uapi.c */
mk_s32 MkDrmIoctl(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, ULONG Nr, PUCHAR Arg, SIZE_T ArgSize);
mk_s32 MkMap(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, MALIKM_MAP *Map);
mk_s32 MkUnmap(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, PVOID Address);
void MkFileCleanup(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File);
