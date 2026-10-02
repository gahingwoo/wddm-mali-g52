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
#include "../common/maligpu.h"

/* ntifs.h, which cannot be included next to ntddk.h. */
NTKERNELAPI BOOLEAN PsIsThreadTerminating(PETHREAD Thread);

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

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE Device;
    MK_GPU Gpu;                 /* registers, page tables, interrupt status */

    /* Serialises every object table and the page tables. */
    FAST_MUTEX Lock;

    /* Jobs. */
    KSPIN_LOCK QueueLock;
    LIST_ENTRY Queue;
    KEVENT QueueEvent;          /* synchronization: work queued */
    KEVENT JobIrqEvent;         /* synchronization: a job or MMU interrupt */
    KEVENT ProgressEvent;       /* notification: CompletedSeq moved */
    volatile LONG64 SubmittedSeq;
    volatile LONG64 CompletedSeq;
    PKTHREAD Worker;
    BOOLEAN StopWorker;
    LIST_ENTRY Zombies;         /* closed BOs a queued job may still use */

    /* Counters, mirrored to the registry for diagnosis. */
    ULONG JobsDone, JobsFailed, JobsTimedOut;
    ULONG LastJsStatus;

    WDFINTERRUPT Interrupts[3];
    ULONG InterruptCount;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* gpu.c: the worker and BO mapping on top of maligpu */
NTSTATUS MkMmuMap(PDEVICE_CONTEXT Dev, MK_BO *Bo);
void MkMmuUnmap(PDEVICE_CONTEXT Dev, MK_BO *Bo);
NTSTATUS MkWorkerStart(PDEVICE_CONTEXT Dev);
void MkWorkerStop(PDEVICE_CONTEXT Dev);
void MkQueueJob(PDEVICE_CONTEXT Dev, MK_JOB *Job);
void MkLog(PDEVICE_CONTEXT Dev, PCWSTR Name, ULONG Value);
void MkLogHook(void *Owner, PCWSTR Name, ULONG Value);   /* MK_GPU.Log */

/* bo.c */
void MkBoRelease(PDEVICE_CONTEXT Dev, MK_BO *Bo);   /* caller holds Lock */
void MkReapZombies(PDEVICE_CONTEXT Dev);             /* caller holds Lock */

/* uapi.c */
mk_s32 MkDrmIoctl(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, ULONG Nr, PUCHAR Arg, SIZE_T ArgSize);
mk_s32 MkMap(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, MALIKM_MAP *Map);
mk_s32 MkUnmap(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File, PVOID Address);
void MkFileCleanup(PDEVICE_CONTEXT Dev, PFILE_CONTEXT File);
