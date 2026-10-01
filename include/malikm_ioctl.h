/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The interface between the Mali kernel driver (drivers/malikm) and the
 * user-mode libdrm shim (win32-compat/xf86drm.h). Shared by both.
 *
 * The driver implements Linux Panfrost's uAPI, version 1.1, so Mesa's own
 * Linux backend (panfrost_kmod.c) runs unchanged. Every DRM ioctl travels as
 * IOCTL_MALIKM_DRM: a header naming the DRM ioctl number, then the Linux
 * struct as is. Pointer fields that name user arrays (a submit's BO and
 * syncobj lists, a syncobj wait's handles) are rewritten by the shim into
 * byte offsets from the start of the Linux struct, and the arrays are appended
 * to the same buffer, so the driver never dereferences a user pointer.
 *
 * Timeouts: DRM takes absolute CLOCK_MONOTONIC deadlines. The shim converts
 * them to relative nanoseconds against the clock Mesa used, so the driver
 * only ever sees relative ones; MALIKM_TIMEOUT_INFINITE means wait forever.
 *
 * Errors come back in the header as negative Linux errno values.
 *
 * Mapping a buffer into the caller (mmap on Linux) is its own ioctl, because
 * it has to run in the calling process.
 */
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

typedef unsigned __int32 mk_u32;
typedef signed __int32   mk_s32;
typedef unsigned __int64 mk_u64;
typedef signed __int64   mk_s64;

#define MALIKM_DEVICE_NAME      L"\\Device\\MaliG52"
#define MALIKM_DOS_NAME         L"\\DosDevices\\MaliG52"
#define MALIKM_USER_PATH        L"\\\\.\\MaliG52"

#define FILE_DEVICE_MALIKM      0x8A52
#define IOCTL_MALIKM_DRM        CTL_CODE(FILE_DEVICE_MALIKM, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALIKM_MAP        CTL_CODE(FILE_DEVICE_MALIKM, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALIKM_UNMAP      CTL_CODE(FILE_DEVICE_MALIKM, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALIKM_VERSION    CTL_CODE(FILE_DEVICE_MALIKM, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* The uAPI version reported to Mesa. 1.1 is the oldest it accepts and keeps
 * it off the later extensions (timestamps, JM contexts, BO sync and labels). */
#define MALIKM_DRM_MAJOR        1
#define MALIKM_DRM_MINOR        1

/* DRM ioctl numbers (the nr byte of the Linux request code). */
#define MALIKM_NR_GEM_CLOSE             0x09
#define MALIKM_NR_SYNCOBJ_CREATE        0xBF
#define MALIKM_NR_SYNCOBJ_DESTROY       0xC0
#define MALIKM_NR_SYNCOBJ_WAIT          0xC3
#define MALIKM_NR_SYNCOBJ_RESET         0xC4
#define MALIKM_NR_SYNCOBJ_SIGNAL        0xC5
#define MALIKM_NR_PANFROST_SUBMIT       0x40
#define MALIKM_NR_PANFROST_WAIT_BO      0x41
#define MALIKM_NR_PANFROST_CREATE_BO    0x42
#define MALIKM_NR_PANFROST_MMAP_BO      0x43
#define MALIKM_NR_PANFROST_GET_PARAM    0x44
#define MALIKM_NR_PANFROST_GET_BO_OFFSET 0x45
#define MALIKM_NR_PANFROST_MADVISE      0x48
/* Not DRM: a sync file has no Windows equivalent, so the shim keeps one as
 * the fence's sequence number and these move it in and out of a syncobj. */
#define MALIKM_NR_SYNCOBJ_EXPORT_SEQ    0xF0
#define MALIKM_NR_SYNCOBJ_IMPORT_SEQ    0xF1

typedef struct _MALIKM_DRM_HEADER {
    mk_u32 Nr;          /* DRM ioctl number, see above */
    mk_u32 Size;        /* size of the Linux struct that follows */
    mk_s32 Result;      /* out: 0 or a negative Linux errno */
    mk_u32 Reserved;
} MALIKM_DRM_HEADER;

typedef struct _MALIKM_MAP {
    mk_u64 Offset;      /* in: the fake offset MMAP_BO returned */
    mk_u64 Size;        /* in */
    mk_u64 Address;     /* out: user address in the calling process */
} MALIKM_MAP;

typedef struct _MALIKM_UNMAP {
    mk_u64 Address;
} MALIKM_UNMAP;

typedef struct _MALIKM_VERSION {
    mk_s32 Major, Minor, Patch;
    char   Name[16];    /* "panfrost" */
} MALIKM_VERSION;

/* MMAP_BO's fake offset: the BO handle in the high half. */
#define MALIKM_MMAP_OFFSET(handle)      ((mk_u64)(handle) << 32)
#define MALIKM_MMAP_HANDLE(offset)      ((mk_u32)((offset) >> 32))

#define MALIKM_TIMEOUT_INFINITE         ((mk_s64)0x7FFFFFFFFFFFFFFFLL)

/* EXPORT_SEQ / IMPORT_SEQ. Importing MALIKM_SEQ_LATEST means "everything
 * submitted so far", the safe reading of a sync file the shim cannot name. */
typedef struct _MALIKM_SYNCOBJ_SEQ {
    mk_u32 Handle;
    mk_u32 Pad;
    mk_u64 Seq;
} MALIKM_SYNCOBJ_SEQ;
#define MALIKM_SEQ_LATEST               (~(mk_u64)0)

/* Linux errno values the driver returns. */
#define MK_EPERM        1
#define MK_ENOENT       2
#define MK_EINTR        4
#define MK_EIO          5
#define MK_ENOMEM       12
#define MK_EFAULT       14
#define MK_EBUSY        16
#define MK_ENODEV       19
#define MK_EINVAL       22
#define MK_ENOSPC       28
#define MK_ETIME        62
#define MK_ENOSYS       38
#define MK_ETIMEDOUT    110
