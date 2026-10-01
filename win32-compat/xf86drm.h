/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The subset of libdrm Panfrost calls, for Windows. Prototypes match libdrm's
 * xf86drm.h; the implementations (malikm_drm.c) turn them into DeviceIoControl
 * calls on the Mali kernel driver (drivers/malikm), which keeps Linux
 * Panfrost's semantics. This header stays free of windows.h on purpose: half
 * of Panfrost includes it.
 */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>

/* libdrm's xf86drm.h brings in drm.h (and with it drm_mode.h); so does this. */
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0x80000
#endif
#define DRM_CLOEXEC O_CLOEXEC
#define DRM_RDWR    O_RDWR

typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

/* Not libdrm: opens \\.\MaliG52 and returns the fd Panfrost should use. */
int malikm_open(void);

int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmCloseBufferHandle(int fd, uint32_t handle);

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point, uint32_t flags);

#ifdef __cplusplus
}
#endif
