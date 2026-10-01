/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The subset of libdrm Panfrost calls, for Windows. Prototypes match libdrm's
 * xf86drm.h. Every call fails with ENOSYS until M3, where they become
 * DeviceIoControl calls into the Mali KMDF driver with Linux Panfrost
 * semantics.
 */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>

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

#define DRM_WIN32_NOSYS(ret) do { errno = ENOSYS; return (ret); } while (0)

static inline int drmIoctl(int fd, unsigned long request, void *arg)
{ (void)fd; (void)request; (void)arg; DRM_WIN32_NOSYS(-1); }

static inline drmVersionPtr drmGetVersion(int fd)
{ (void)fd; DRM_WIN32_NOSYS((drmVersionPtr)0); }

static inline void drmFreeVersion(drmVersionPtr v) { (void)v; }

static inline int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{ (void)fd; (void)handle; (void)flags; (void)prime_fd; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{ (void)fd; (void)prime_fd; (void)handle; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmCloseBufferHandle(int fd, uint32_t handle)
{ (void)fd; (void)handle; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{ (void)fd; (void)flags; (void)handle; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjDestroy(int fd, uint32_t handle)
{ (void)fd; (void)handle; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{ (void)fd; (void)handle; (void)obj_fd; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{ (void)fd; (void)obj_fd; (void)handle; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                                 int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{ (void)fd; (void)handles; (void)num_handles; (void)timeout_nsec; (void)flags; (void)first_signaled; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{ (void)fd; (void)handles; (void)handle_count; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                                         int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{ (void)fd; (void)handles; (void)points; (void)num_handles; (void)timeout_nsec; (void)flags; (void)first_signaled; DRM_WIN32_NOSYS(-ENOSYS); }

static inline int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                                     uint32_t src_handle, uint64_t src_point, uint32_t flags)
{ (void)fd; (void)dst_handle; (void)dst_point; (void)src_handle; (void)src_point; (void)flags; DRM_WIN32_NOSYS(-ENOSYS); }
