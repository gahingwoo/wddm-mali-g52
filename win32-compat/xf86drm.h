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

/* util/libdrm.h defines its own no-op stand-ins for these when HAVE_LIBDRM
 * is not set. A file that includes it after this header gets ours. */
#ifndef LIBDRM_H
#define LIBDRM_H
#define WIN32_COMPAT_DRM_TYPES

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

/* The part of libdrm's drmDevice that Mesa reads (panvk, the Vulkan WSI).
 * drmGetDevice2 describes the one Mali this shim drives. */
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3

#define DRM_BUS_PCI       0
#define DRM_BUS_USB       1
#define DRM_BUS_PLATFORM  2
#define DRM_BUS_HOST1X    3

#define DRM_PLATFORM_DEVICE_NAME_LEN 512

typedef struct _drmPlatformBusInfo {
   char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
   char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmDevice {
   char **nodes;           /* DRM_NODE_MAX entries */
   int available_nodes;    /* 1 << DRM_NODE_* */
   int bustype;
   union {
      drmPlatformBusInfoPtr platform;
      void *other;
   } businfo;
   union {
      drmPlatformDeviceInfoPtr platform;
      void *other;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

#define DRM_DEVICE_GET_PCI_REVISION (1 << 0)

drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevice(drmDevicePtr *device);
void drmFreeDevices(drmDevicePtr devices[], int count);
#endif /* LIBDRM_H */

/* Not libdrm: opens \\.\MaliG52 and returns the fd Panfrost should use. */
int malikm_open(void);

int drmIoctl(int fd, unsigned long request, void *arg);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/* Not libdrm: give each syncobj of this process a state of its own (see
 * "syncobjs that keep their state" in malikm_drm.c). The Vulkan driver turns
 * it on; the Direct3D and OpenGL drivers leave it off. */
void malikm_syncobj_state(int on);
/* Not libdrm: the LUID of the maliwddm adapter the shim opened, 8 bytes.
 * 0 if it has none (MALIKM_FAKE, or malikm). */
int malikm_adapter_luid(void *luid);

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
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
                             uint32_t handle_count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
                     uint32_t flags);

#ifdef __cplusplus
}
#endif
