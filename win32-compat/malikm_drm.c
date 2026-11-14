/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libdrm and mmap for Panfrost on Windows: each call becomes a DeviceIoControl
 * on \\.\MaliG52 (drivers/malikm), whose ABI is include/malikm_ioctl.h.
 *
 * The fd Panfrost holds is a CRT fd wrapping the device handle
 * (_open_osfhandle in d3d10_gdi.c), so _get_osfhandle gets it back.
 *
 * What does not carry over from Linux:
 *  - Pointers to user arrays inside DRM structs are sent inline instead.
 *  - Absolute CLOCK_MONOTONIC deadlines become relative, measured on the
 *    clock Mesa took them from (os_time_get_nano).
 *  - A sync file is a sequence number kept here against a placeholder fd.
 */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d9types.h>   /* d3dumddi.h uses the D3D9 types */
#include <d3dumddi.h>
#include <dxgiddi.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_time.h"
#include "util/mali_slow.h"
#include "drm-uapi/drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "malikm_ioctl.h"
#include "maliwddm_abi.h"
#include "xf86drm.h"
#include "sys/mman.h"

/* The maliwddm backend, below. */
static int wddm_on(void);
static int wddm_init(void);
static void wddm_ioctl(int fd, DWORD code, void *buf, DWORD len);

/* Linux errno, as the driver reports it, to the CRT's. Most agree. */
static int
crt_errno(int e)
{
   switch (e) {
   case MK_ETIMEDOUT: return ETIMEDOUT;
   case MK_ETIME:     return ETIME;
   case MK_ENOSYS:    return ENOSYS;
   default:           return e;
   }
}

/* The device handle behind Mesa's fd. The UCRT will not wrap a handle whose
 * GetFileType is FILE_TYPE_UNKNOWN, which a KMDF device without a device type
 * is, so malikm_open() hands Mesa a placeholder fd (NUL) and keeps the real
 * handle here. Mesa _dup()s the fd it is given; a duplicate is not in the
 * table, and falls back to the handle this process opened. */
#define MAX_DEV_FDS 64
static struct { int fd; HANDLE h; } dev_fds[MAX_DEV_FDS];
static HANDLE dev_handle = INVALID_HANDLE_VALUE;
static SRWLOCK dev_lock = SRWLOCK_INIT;

static int trace_on(void);

/* d3d10umd's log (Debug.cpp): the debugger and, when on, the file log. */
void DebugPrintf(const char *format, ...);

/* An fd for Mesa to hold, duplicate and close where Linux has a real one:
 * the device, a dma-buf, a sync file. NUL, or where a process may not open
 * NUL, the read end of a pipe: the shell's XAML hosts are AppContainer
 * processes and got "errno 13, error 5" for NUL, which cost them the Mali. */
static int
placeholder_fd(void)
{
   int fd = _open("NUL", _O_RDWR | _O_NOINHERIT);

   if (fd < 0) {
      int e = errno, p[2];
      if (_pipe(p, 16, _O_BINARY | _O_NOINHERIT) == 0) {
         _close(p[1]);
         fd = p[0];
      } else {
         DebugPrintf("placeholder fd: NUL does not open (errno %d) and no pipe either"
                     " (errno %d, error %lu)\n", e, errno, (unsigned long)GetLastError());
      }
   }
   return fd;
}

int
malikm_open(void)
{
   HANDLE h = CreateFileW(MALIKM_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
   if (h == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      /* No malikm: the Mali may be driven by maliwddm, the WDDM driver,
       * reached through the D3DKMT system calls instead. */
      if (wddm_init() == 0) {
         int nul = placeholder_fd();
         if (trace_on())
            fprintf(stderr, "malikm: no \\\\.\\MaliG52; using the maliwddm adapter\n");
         /* The fd is a placeholder for Mesa to hold and duplicate. A
          * process that may not open NUL would lose the Mali here. */
         if (nul < 0)
            DebugPrintf("malikm_open: maliwddm is bound, but there is no placeholder fd\n");
         return nul;
      }
      DebugPrintf("malikm_open: no \\\\.\\MaliG52 (error %lu) and no maliwddm adapter (on %d)\n",
                  (unsigned long)err, wddm_on());
      fprintf(stderr, "malikm: cannot open \\\\.\\MaliG52 (error %lu), and no maliwddm adapter\n",
              (unsigned long)err);
      errno = err == ERROR_FILE_NOT_FOUND ? ENOENT : ENODEV;
      return -1;
   }
   int fd = placeholder_fd();
   if (fd < 0) {
      CloseHandle(h);
      return -1;
   }
   AcquireSRWLockExclusive(&dev_lock);
   for (int i = 0; i < MAX_DEV_FDS; i++) {
      if (dev_fds[i].h == NULL) {
         dev_fds[i].fd = fd;
         dev_fds[i].h = h;
         break;
      }
   }
   if (dev_handle == INVALID_HANDLE_VALUE)
      dev_handle = h;
   ReleaseSRWLockExclusive(&dev_lock);
   if (trace_on())
      fprintf(stderr, "malikm: opened \\\\.\\MaliG52 as fd %d\n", fd);
   return fd;
}

static HANDLE
fd_handle(int fd)
{
   HANDLE h = INVALID_HANDLE_VALUE;
   AcquireSRWLockShared(&dev_lock);
   for (int i = 0; i < MAX_DEV_FDS && h == INVALID_HANDLE_VALUE; i++)
      if (dev_fds[i].h != NULL && dev_fds[i].fd == fd)
         h = dev_fds[i].h;
   if (h == INVALID_HANDLE_VALUE)
      h = dev_handle;
   ReleaseSRWLockShared(&dev_lock);
   return h;
}

/* Send Buf (a MALIKM_DRM_HEADER and what follows) and return the driver's
 * result, 0 or -1 with errno set. */
static int fake_on(void);
static void fake_ioctl(DWORD code, void *buf, DWORD len);

/* MALIKM_TRACE=1: log every call to the real driver, with the DRM number,
 * the driver's result and any Win32 error. */
static int
trace_on(void)
{
   static int t = -1;
   if (t < 0) {
      const char *e = getenv("MALIKM_TRACE");
      t = e && *e && *e != '0';
   }
   return t;
}

static int
call(int fd, DWORD code, void *buf, DWORD len)
{
   HANDLE h = fd_handle(fd);
   DWORD got = 0;

   if (fake_on() || wddm_on()) {
      if (fake_on())
         fake_ioctl(code, buf, len);
      else
         wddm_ioctl(fd, code, buf, len);
      int r = ((MALIKM_DRM_HEADER *)buf)->Result;
      if (r < 0) {
         errno = crt_errno(-r);
         return -1;
      }
      return 0;
   }
   if (h == INVALID_HANDLE_VALUE) {
      if (trace_on())
         fprintf(stderr, "malikm: fd %d has no handle\n", fd);
      errno = EBADF;
      return -1;
   }
   if (!DeviceIoControl(h, code, buf, len, buf, len, &got, NULL)) {
      DWORD err = GetLastError();
      if (trace_on())
         fprintf(stderr, "malikm: ioctl 0x%lx nr 0x%x len %lu: DeviceIoControl failed, error %lu\n",
                 (unsigned long)code, ((MALIKM_DRM_HEADER *)buf)->Nr, (unsigned long)len,
                 (unsigned long)err);
      errno = err == ERROR_INVALID_FUNCTION ? ENOTTY : EIO;
      return -1;
   }
   int r = ((MALIKM_DRM_HEADER *)buf)->Result;
   if (trace_on())
      fprintf(stderr, "malikm: ioctl 0x%lx nr 0x%x len %lu -> %d (got %lu)\n", (unsigned long)code,
              ((MALIKM_DRM_HEADER *)buf)->Nr, (unsigned long)len, r, (unsigned long)got);
   if (r < 0) {
      errno = crt_errno(-r);
      return -1;
   }
   return 0;
}

/* ---- where a frame's time goes ----
 *
 * One line in the UMD's file log every few seconds while a process presents:
 * for each stage, the time spent in it per present and how often it ran.
 * The stages are the places a present can wait or copy; see MALIKM_PERF_* in
 * the frontend (d3d10_gdi.c has the same numbers). Sums are per process, not
 * per device: the process that presents full frames is the one of interest.
 */
#define PERF_STAGES 10
static const char *const perf_names[PERF_STAGES] = {
   "flush", "readback-map", "readback-copy", "primary-copy", "present-cb",
   "rotate", "submit", "gpu-wait", "idle-in-present", "bo-create",
};
static struct {
   volatile LONG64 ticks;
   volatile LONG calls;
} perf[PERF_STAGES];
static volatile LONG perf_presents, perf_reporting;
static volatile LONG64 perf_since;

unsigned long long
malikm_perf_now(void)
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (unsigned long long)t.QuadPart;
}

/* The time since Since (a malikm_perf_now) went into this stage. */
void
malikm_perf_add(unsigned stage, unsigned long long since)
{
   if (stage >= PERF_STAGES)
      return;
   InterlockedAdd64(&perf[stage].ticks, (LONG64)(malikm_perf_now() - since));
   InterlockedIncrement(&perf[stage].calls);
}

/* A present is over. Every PERF_REPORT_MS, say what the presents since the
 * last report cost. */
#define PERF_REPORT_MS 5000
void
malikm_perf_frame(void)
{
   LARGE_INTEGER hz;
   LONG64 now = (LONG64)malikm_perf_now(), since = perf_since;
   char line[640];
   int off;

   InterlockedIncrement(&perf_presents);
   QueryPerformanceFrequency(&hz);
   if (since == 0) {
      InterlockedCompareExchange64(&perf_since, now, 0);
      return;
   }
   if ((now - since) * 1000 / hz.QuadPart < PERF_REPORT_MS ||
       InterlockedExchange(&perf_reporting, 1) != 0)
      return;
   LONG n = InterlockedExchange(&perf_presents, 0);
   InterlockedExchange64(&perf_since, now);
   if (n < 1)
      n = 1;
   off = snprintf(line, sizeof line, "perf: %ld presents in %lld ms; us per present (calls):",
                  (long)n, (long long)((now - since) * 1000 / hz.QuadPart));
   for (int i = 0; i < PERF_STAGES && off < (int)sizeof line - 48; i++) {
      LONG64 t = InterlockedExchange64(&perf[i].ticks, 0);
      LONG c = InterlockedExchange(&perf[i].calls, 0);
      off += snprintf(line + off, sizeof line - off, " %s %lld (%ld)", perf_names[i],
                      (long long)(t * 1000000 / hz.QuadPart / n), (long)c);
   }
   DebugPrintf("%s\n", line);
   InterlockedExchange(&perf_reporting, 0);
}

static int64_t
relative_timeout(int64_t abs_ns)
{
   if (abs_ns == INT64_MAX)
      return MALIKM_TIMEOUT_INFINITE;
   int64_t now = os_time_get_nano();
   return abs_ns > now ? abs_ns - now : 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
   unsigned nr = request & 0xff;
   unsigned size = (request >> 16) & 0x1fff;
   size_t extra = 0;

   switch (nr) {
   case MALIKM_NR_PANFROST_SUBMIT: {
      const struct drm_panfrost_submit *s = arg;
      extra = ((size_t)s->in_sync_count + s->bo_handle_count) * 4;
      break;
   }
   case MALIKM_NR_SYNCOBJ_WAIT:
      extra = (size_t)((const struct drm_syncobj_wait *)arg)->count_handles * 4;
      break;
   case MALIKM_NR_SYNCOBJ_RESET:
   case MALIKM_NR_SYNCOBJ_SIGNAL:
      extra = (size_t)((const struct drm_syncobj_array *)arg)->count_handles * 4;
      break;
   }

   size_t len = sizeof(MALIKM_DRM_HEADER) + size + extra;
   uint8_t *buf = calloc(1, len);
   if (!buf) {
      errno = ENOMEM;
      return -1;
   }
   MALIKM_DRM_HEADER *hdr = (MALIKM_DRM_HEADER *)buf;
   uint8_t *body = buf + sizeof(*hdr);
   hdr->Nr = nr;
   hdr->Size = size;
   memcpy(body, arg, size);

   /* Inline the arrays; their "pointers" become offsets from body. */
   switch (nr) {
   case MALIKM_NR_PANFROST_SUBMIT: {
      const struct drm_panfrost_submit *s = arg;
      struct drm_panfrost_submit *d = (void *)body;
      size_t in = (size_t)s->in_sync_count * 4;
      d->in_syncs = size;
      d->bo_handles = size + in;
      if (in)
         memcpy(body + size, (void *)(uintptr_t)s->in_syncs, in);
      if (s->bo_handle_count)
         memcpy(body + size + in, (void *)(uintptr_t)s->bo_handles, (size_t)s->bo_handle_count * 4);
      break;
   }
   case MALIKM_NR_PANFROST_WAIT_BO: {
      struct drm_panfrost_wait_bo *d = (void *)body;
      d->timeout_ns = relative_timeout(d->timeout_ns);
      break;
   }
   case MALIKM_NR_SYNCOBJ_WAIT: {
      const struct drm_syncobj_wait *s = arg;
      struct drm_syncobj_wait *d = (void *)body;
      d->handles = size;
      d->timeout_nsec = relative_timeout(s->timeout_nsec);
      memcpy(body + size, (void *)(uintptr_t)s->handles, extra);
      break;
   }
   case MALIKM_NR_SYNCOBJ_RESET:
   case MALIKM_NR_SYNCOBJ_SIGNAL: {
      const struct drm_syncobj_array *s = arg;
      ((struct drm_syncobj_array *)body)->handles = size;
      memcpy(body + size, (void *)(uintptr_t)s->handles, extra);
      break;
   }
   }

   int ret = call(fd, IOCTL_MALIKM_DRM, buf, (DWORD)len);
   if (ret == 0 && (request & IOC_OUT)) {
      /* Copy the results back, keeping the caller's own pointer and
       * timeout fields as they were. */
      switch (nr) {
      case MALIKM_NR_PANFROST_SUBMIT:
      case MALIKM_NR_PANFROST_WAIT_BO:
         break;   /* nothing comes back */
      case MALIKM_NR_SYNCOBJ_WAIT:
         ((struct drm_syncobj_wait *)arg)->first_signaled =
            ((struct drm_syncobj_wait *)body)->first_signaled;
         break;
      case MALIKM_NR_SYNCOBJ_RESET:
      case MALIKM_NR_SYNCOBJ_SIGNAL:
         break;
      default:
         memcpy(arg, body, size);
         break;
      }
   }
   free(buf);
   return ret;
}

drmVersionPtr
drmGetVersion(int fd)
{
   MALIKM_VERSION v = {0};
   HANDLE h = fd_handle(fd);
   DWORD got = 0;

   if (fake_on())
      fake_ioctl(IOCTL_MALIKM_VERSION, &v, sizeof(v));
   else if (wddm_on())
      wddm_ioctl(fd, IOCTL_MALIKM_VERSION, &v, sizeof(v));
   else if (h == INVALID_HANDLE_VALUE ||
            !DeviceIoControl(h, IOCTL_MALIKM_VERSION, &v, sizeof(v), &v, sizeof(v), &got, NULL) ||
            got < sizeof(v)) {
      if (trace_on())
         fprintf(stderr, "malikm: VERSION failed (fd %d, error %lu)\n", fd, (unsigned long)GetLastError());
      errno = ENODEV;
      return NULL;
   }
   if (trace_on())
      fprintf(stderr, "malikm: VERSION %.16s %d.%d\n", v.Name, v.Major, v.Minor);
   drmVersionPtr r = calloc(1, sizeof(*r));
   if (!r)
      return NULL;
   v.Name[sizeof(v.Name) - 1] = 0;
   r->version_major = v.Major;
   r->version_minor = v.Minor;
   r->version_patchlevel = v.Patch;
   r->name = _strdup(v.Name);
   r->name_len = (int)strlen(v.Name);
   r->date = _strdup("");
   r->desc = _strdup("wddm-mali-g52 malikm");
   r->desc_len = (int)strlen(r->desc ? r->desc : "");
   return r;
}

void
drmFreeVersion(drmVersionPtr v)
{
   if (!v)
      return;
   free(v->name);
   free(v->date);
   free(v->desc);
   free(v);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   struct drm_gem_close c = {.handle = handle};
   return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* Set by the maliwddm backend below: 1 if the BO is the allocation of a
 * runtime resource (a shared resource, created or opened). */
static int wddm_bo_is_resource(int fd, uint32_t handle);

/* Panfrost exports a BO to mark it as shared: from then on it neither goes
 * back to the BO cache nor is swapped for a fresh one when it is mapped for
 * writing. On maliwddm a shared resource travels through the runtime, not
 * through an fd, so the export of such a BO only has to succeed: the fd is
 * a placeholder the caller closes. Everything else is still unsupported. */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)flags;
   if (wddm_on() && prime_fd && wddm_bo_is_resource(fd, handle)) {
      int p = placeholder_fd();
      if (p >= 0) {
         *prime_fd = p;
         return 0;
      }
   }
   errno = ENOSYS;
   return -ENOSYS;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd; (void)prime_fd; (void)handle;
   errno = ENOSYS;
   return -ENOSYS;
}

/* ---- syncobjs: return conventions as libdrm's ---- */

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   struct drm_syncobj_create c = {.flags = flags};
   int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &c);
   if (ret)
      return ret;
   *handle = c.handle;
   return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   struct drm_syncobj_destroy d = {.handle = handle};
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
               int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   struct drm_syncobj_wait w = {
      .handles = (uintptr_t)handles,
      .timeout_nsec = timeout_nsec,
      .count_handles = num_handles,
      .flags = flags,
   };
   if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) < 0)
      return -errno;
   if (first_signaled)
      *first_signaled = w.first_signaled;
   return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array a = {.handles = (uintptr_t)handles, .count_handles = handle_count};
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &a);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array a = {.handles = (uintptr_t)handles, .count_handles = handle_count};
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &a);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
   (void)fd; (void)handle; (void)obj_fd;
   errno = ENOSYS;
   return -1;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   (void)fd; (void)obj_fd; (void)handle;
   errno = ENOSYS;
   return -1;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                       int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   (void)fd; (void)handles; (void)points; (void)num_handles;
   (void)timeout_nsec; (void)flags; (void)first_signaled;
   errno = ENOSYS;
   return -ENOSYS;
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                   uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   (void)fd; (void)dst_handle; (void)dst_point; (void)src_handle; (void)src_point; (void)flags;
   errno = ENOSYS;
   return -1;
}

/* ---- sync files ----
 * Panfrost snapshots a syncobj's fence by exporting it to a sync file and
 * importing that into a new syncobj. Here the snapshot is the fence's
 * sequence number, filed under a placeholder fd that close() can take. An fd
 * this table does not know (a dup, say) imports as "everything so far". */

#define SYNC_FDS 4096
static uint64_t sync_seq[SYNC_FDS];        /* seq + 1; 0 = unknown */
static SRWLOCK sync_lock = SRWLOCK_INIT;

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   struct {
      MALIKM_DRM_HEADER h;
      MALIKM_SYNCOBJ_SEQ s;
   } m = {{MALIKM_NR_SYNCOBJ_EXPORT_SEQ, sizeof(MALIKM_SYNCOBJ_SEQ)}, {handle}};

   if (call(fd, IOCTL_MALIKM_DRM, &m, sizeof(m)))
      return -1;
   int sfd = placeholder_fd();
   if (sfd < 0)
      return -1;
   AcquireSRWLockExclusive(&sync_lock);
   if (sfd < SYNC_FDS)
      sync_seq[sfd] = m.s.Seq + 1;
   ReleaseSRWLockExclusive(&sync_lock);
   *sync_file_fd = sfd;
   return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   struct {
      MALIKM_DRM_HEADER h;
      MALIKM_SYNCOBJ_SEQ s;
   } m = {{MALIKM_NR_SYNCOBJ_IMPORT_SEQ, sizeof(MALIKM_SYNCOBJ_SEQ)}, {handle, 0, MALIKM_SEQ_LATEST}};

   AcquireSRWLockShared(&sync_lock);
   if (sync_file_fd >= 0 && sync_file_fd < SYNC_FDS && sync_seq[sync_file_fd] != 0)
      m.s.Seq = sync_seq[sync_file_fd] - 1;
   ReleaseSRWLockShared(&sync_lock);
   return call(fd, IOCTL_MALIKM_DRM, &m, sizeof(m));
}

/* ---- mmap ----
 * munmap has no fd, so remember which device each mapping came from. */

struct mapping {
   void *addr;
   int fd;
};
static struct mapping *maps;
static size_t nmaps, capmaps;
static SRWLOCK map_lock = SRWLOCK_INIT;

void *
mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
   struct {
      MALIKM_DRM_HEADER h;
      MALIKM_MAP m;
   } m = {{0}, {(uint64_t)(uint32_t)off, len, 0}};
   (void)addr; (void)prot; (void)flags;

   if (call(fd, IOCTL_MALIKM_MAP, &m, sizeof(m))) {
      if (trace_on())
         fprintf(stderr, "malikm: mmap fd %d offset 0x%llx len %zu failed\n", fd,
                 (unsigned long long)(uint32_t)off, len);
      return MAP_FAILED;
   }
   void *va = (void *)(uintptr_t)m.m.Address;

   AcquireSRWLockExclusive(&map_lock);
   if (nmaps == capmaps) {
      size_t cap = capmaps ? capmaps * 2 : 256;
      struct mapping *n = realloc(maps, cap * sizeof(*n));
      if (n) {
         maps = n;
         capmaps = cap;
      }
   }
   if (nmaps < capmaps)
      maps[nmaps++] = (struct mapping){va, fd};
   ReleaseSRWLockExclusive(&map_lock);
   return va;
}

int
munmap(void *addr, size_t len)
{
   int fd = -1;
   (void)len;

   AcquireSRWLockExclusive(&map_lock);
   for (size_t i = 0; i < nmaps; i++) {
      if (maps[i].addr == addr) {
         fd = maps[i].fd;
         maps[i] = maps[--nmaps];
         break;
      }
   }
   ReleaseSRWLockExclusive(&map_lock);
   if (fd < 0) {
      errno = EINVAL;
      return -1;
   }

   struct {
      MALIKM_DRM_HEADER h;
      MALIKM_UNMAP u;
   } m = {{0}, {(uint64_t)(uintptr_t)addr}};
   return call(fd, IOCTL_MALIKM_UNMAP, &m, sizeof(m));
}


/* ---- maliwddm ----
 * The Panfrost uAPI over the WDDM driver (drivers/maliwddm), through the
 * D3DKMT system calls in win32u (WinPE's gdi32 forwards its D3DKMT exports
 * to dxcore, which WinPE lacks). One D3DKMT device and context per process.
 *
 *   CREATE_BO      CreateAllocation (MW_ALLOCATION_INFO); its fixed GPU
 *                  address from MW_ESCAPE_ALLOC_INFO
 *   mmap           a Lock held until GEM_CLOSE
 *   SUBMIT         one MW_CMD_JOB through Render, the BOs as the allocation
 *                  list (the KMD patches each, which makes VidMm map them)
 *   WAIT_BO, syncobj waits
 *                  WaitForIdle: the KMD runs one queue in order, so
 *                  everything submitted before the wait is done after it
 *   GET_PARAM      MW_ESCAPE_GPU_INFO
 */

typedef NTSTATUS (APIENTRY *KMT_FN)(void *);
static struct {
   KMT_FN CreateDevice, DestroyDevice, CreateContext, CreateAllocation, DestroyAllocation,
          Lock, Unlock, Render, Escape, EnumAdapters2, WaitForIdle;
} kmt;

struct wbo {
   D3DKMT_HANDLE h;
   uint64_t size, va;
   void *cpu;
   uint64_t listed;     /* submit that last put it in the allocation list */
   /* The allocation of a runtime resource (a shared resource, created here
    * or opened from another process): the runtime resource and the runtime
    * device it belongs to. Such an allocation is given back through the
    * resource (malikm_shared_destroy), never by handle at GEM_CLOSE. */
   void *rt_resource;
   HANDLE rt_device;
   /* Given back already, while Panfrost still holds the BO: kept out of
    * every allocation list until its GEM_CLOSE. */
   int released;
   /* A primary Panfrost renders into (malikm_primary_begin_create). Listed
    * only in the submissions that name it: a job that does not draw to a
    * primary has no business holding a reference on one the display is
    * flipping to or from. */
   int primary;
};
static struct wbo *wbo;
static uint32_t wbo_cap;
static D3DKMT_HANDLE wadapter, wdevice, wcontext;
static void *wcmd;
static UINT wcmd_size;
static D3DDDI_ALLOCATIONLIST *walloc;
static UINT walloc_size;
static D3DDDI_PATCHLOCATIONLIST *wpatch;
static UINT wpatch_size;
static MW_ESCAPE_GPU_INFO_DATA wgpu;
static uint64_t wseq;
static int wddm = -1;
static SRWLOCK wddm_lock = SRWLOCK_INIT;

/* wddm_lock, timed: a long wait here is another thread's wait for the GPU
 * or the runtime, made inside the lock. */
static void
wlock(const char *who)
{
   uint64_t slow_t0 = mali_slow_now();
   AcquireSRWLockExclusive(&wddm_lock);
   MALI_SLOW(slow_t0, "shim-lock", "waited for it in %s", who);
}

/* Inside a hardware UMD (maliumd.dll, d3d10umd), the runtime's device and
 * its kernel callbacks replace the D3DKMT calls: the buffers then are the
 * runtime device's allocations, which Present needs. malikm_bind_d3dddi()
 * switches over before the device's screen is created. */
static struct {
   int on;
   HANDLE adapter, device, context;
   D3DDDI_DEVICECALLBACKS cb;
   const DXGI_DDI_BASE_CALLBACKS *dxgi;
   /* The surface DXGI presents from: X8R8G8B8, linear, its size told to the
    * KMD at creation, kept locked. */
   D3DKMT_HANDLE present;
   void *present_cpu;
   unsigned present_w, present_h;
   HANDLE last_event;          /* signalled when the last submission is done */
   /* The newest submission has no event (the signal callback failed), so
    * last_event is older than it and waiting on it alone would return
    * early. wddm_idle() polls the KMD's fences instead while this is set. */
   int last_unsignalled;
} ddi;

/* Allocations whose BO is closed but which a job submitted earlier may
 * still use. Linux's panfrost keeps a BO alive until its jobs are done,
 * and Mesa's BO cache relies on that: it closes buffers without waiting.
 * Destroying the allocation at GEM_CLOSE let VidMm unmap pages under a
 * running job (PE runs 18 on: two job faults per present.exe, a read
 * translation fault, 0x4002c3, every time at the same VA). They are
 * destroyed after the next wait for idle instead. */
static D3DKMT_HANDLE *wdead;
static uint32_t wdead_n, wdead_cap;
#define WDEAD_FLUSH 64

/*
 * One of these per runtime device.
 *
 * Everything above (the BO table, the command buffer, the runtime's device
 * and callbacks, the deferred destroys) was one set per process, rebound by
 * each CreateDevice. dwm.exe and LogonUI.exe have one device and never
 * noticed. explorer.exe makes a device per window and more (five in 17 s),
 * shares surfaces between them and destroys them: BOs allocated on one
 * device were then rendered through another's context, and its log filled
 * with "panfrost_batch_submit failed: 5".
 *
 * The code below still works on that one set of variables. Each entry point
 * takes wddm_lock and then makes the device the call is for the current
 * one: the variables are saved into the device that was current and loaded
 * from the one that becomes current. Device 0 is the process's own (the
 * D3DKMT path, with no runtime).
 *
 * Which device a DRM call is for is told by its fd. Each device's Panfrost
 * screen holds a duplicate of the placeholder fd, made while the screen is
 * created; so an fd first seen on a thread that is inside CreateDevice,
 * between malikm_bind_d3dddi and malikm_bind_done, belongs to that device.
 */
struct wstate {
   struct wbo *wbo;
   uint32_t wbo_cap;
   D3DKMT_HANDLE wdevice, wcontext;
   void *wcmd;
   UINT wcmd_size;
   D3DDDI_ALLOCATIONLIST *walloc;
   UINT walloc_size;
   D3DDDI_PATCHLOCATIONLIST *wpatch;
   UINT wpatch_size;
   uint64_t wseq;
   D3DKMT_HANDLE *wdead;
   uint32_t wdead_n, wdead_cap;
   /* a copy of ddi */
   int on;
   HANDLE adapter, device, context;
   D3DDDI_DEVICECALLBACKS cb;
   const DXGI_DDI_BASE_CALLBACKS *dxgi;
   D3DKMT_HANDLE present;
   void *present_cpu;
   unsigned present_w, present_h;
   HANDLE last_event;
   int last_unsignalled;
};

#define WDEV_MAX 64
#define WDEV_FDS 8
static struct wdev {
   int used;
   HANDLE rt_device;            /* the runtime's device handle; NULL for 0 */
   int fds[WDEV_FDS];
   int nfds;
   struct wstate st;
} wdevs[WDEV_MAX] = {{1}};
static struct wdev *wactive = &wdevs[0];
static __declspec(thread) struct wdev *wbinding;

static void
wsave(struct wstate *s)
{
   s->wbo = wbo; s->wbo_cap = wbo_cap;
   s->wdevice = wdevice; s->wcontext = wcontext;
   s->wcmd = wcmd; s->wcmd_size = wcmd_size;
   s->walloc = walloc; s->walloc_size = walloc_size;
   s->wpatch = wpatch; s->wpatch_size = wpatch_size;
   s->wseq = wseq;
   s->wdead = wdead; s->wdead_n = wdead_n; s->wdead_cap = wdead_cap;
   s->on = ddi.on; s->adapter = ddi.adapter; s->device = ddi.device; s->context = ddi.context;
   s->cb = ddi.cb; s->dxgi = ddi.dxgi;
   s->present = ddi.present; s->present_cpu = ddi.present_cpu;
   s->present_w = ddi.present_w; s->present_h = ddi.present_h;
   s->last_event = ddi.last_event; s->last_unsignalled = ddi.last_unsignalled;
}

static void
wload(const struct wstate *s)
{
   wbo = s->wbo; wbo_cap = s->wbo_cap;
   wdevice = s->wdevice; wcontext = s->wcontext;
   wcmd = s->wcmd; wcmd_size = s->wcmd_size;
   walloc = s->walloc; walloc_size = s->walloc_size;
   wpatch = s->wpatch; wpatch_size = s->wpatch_size;
   wseq = s->wseq;
   wdead = s->wdead; wdead_n = s->wdead_n; wdead_cap = s->wdead_cap;
   ddi.on = s->on; ddi.adapter = s->adapter; ddi.device = s->device; ddi.context = s->context;
   ddi.cb = s->cb; ddi.dxgi = s->dxgi;
   ddi.present = s->present; ddi.present_cpu = s->present_cpu;
   ddi.present_w = s->present_w; ddi.present_h = s->present_h;
   ddi.last_event = s->last_event; ddi.last_unsignalled = s->last_unsignalled;
}

/* Under wddm_lock. */
static void
wselect(struct wdev *d)
{
   if (d == wactive)
      return;
   wsave(&wactive->st);
   wload(&d->st);
   wactive = d;
}

/* The device a DRM call on this fd is for. Under wddm_lock. */
static void
wselect_fd(int fd)
{
   struct wdev *d = wbinding, *found = NULL;

   for (int i = 0; i < WDEV_MAX && !found; i++)
      for (int k = 0; wdevs[i].used && k < wdevs[i].nfds; k++)
         if (wdevs[i].fds[k] == fd)
            found = &wdevs[i];
   if (d && d->used && found != d) {
      /* Inside this device's CreateDevice: the fd is its screen's. A device
       * that had the number before has closed it. */
      if (found) {
         for (int k = 0; k < found->nfds; k++)
            if (found->fds[k] == fd)
               found->fds[k--] = found->fds[--found->nfds];
      }
      if (d->nfds < WDEV_FDS)
         d->fds[d->nfds++] = fd;
      found = d;
   }
   if (!found) {
      static volatile LONG said = 0;
      found = &wdevs[0];
      if (ddi.on || wactive != found) {
         if (InterlockedIncrement(&said) <= 20)
            DebugPrintf("shim: fd %d belongs to no runtime device\n", fd);
      }
   }
   wselect(found);
}

/* The device with this runtime handle; 0 if there is none. Under wddm_lock. */
static int
wselect_rt(void *hRTDevice)
{
   for (int i = 1; i < WDEV_MAX; i++) {
      if (wdevs[i].used && wdevs[i].rt_device == (HANDLE)hRTDevice) {
         wselect(&wdevs[i]);
         return 1;
      }
   }
   return 0;
}

static KMT_FN
kmt_resolve(const char *name)
{
   char nt[96];
   HMODULE w = LoadLibraryA("win32u.dll"), g = LoadLibraryA("gdi32.dll");
   FARPROC f = NULL;

   snprintf(nt, sizeof(nt), "NtGdiDdDDI%s", name);
   if (w)
      f = GetProcAddress(w, nt);
   if (!f && g) {
      snprintf(nt, sizeof(nt), "D3DKMT%s", name);
      f = GetProcAddress(g, nt);
   }
   return (KMT_FN)(void *)f;
}

static NTSTATUS
kmt_escape(D3DKMT_HANDLE device, void *data, UINT size)
{
   D3DKMT_ESCAPE e = {0};
   e.hAdapter = wadapter;
   e.hDevice = device;
   e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
   e.pPrivateDriverData = data;
   e.PrivateDriverDataSize = size;
   return kmt.Escape(&e);
}

static int
wddm_on(void)
{
   return wddm > 0;
}

/* Find the adapter that answers MW_ESCAPE_GPU_INFO, open a device and a
 * context on it. 0 on success. */
static int
wddm_init(void)
{
   if (wddm >= 0)
      return wddm > 0 ? 0 : -1;
   wddm = 0;
   const char *off = getenv("MALIKM_WDDM");
   if (off && *off == '0')
      return -1;

#define R(n) kmt.n = kmt_resolve(#n)
   R(CreateDevice); R(DestroyDevice); R(CreateContext); R(CreateAllocation); R(DestroyAllocation);
   R(Lock); R(Unlock); R(Render); R(Escape); R(EnumAdapters2); R(WaitForIdle);
#undef R
   if (!kmt.CreateDevice || !kmt.CreateContext || !kmt.CreateAllocation || !kmt.DestroyAllocation ||
       !kmt.Lock || !kmt.Unlock || !kmt.Render || !kmt.Escape || !kmt.EnumAdapters2 || !kmt.WaitForIdle)
      return -1;

   D3DKMT_ADAPTERINFO info[16];
   D3DKMT_ENUMADAPTERS2 en = {0};
   en.NumAdapters = 16;
   en.pAdapters = info;
   if (kmt.EnumAdapters2(&en) < 0)
      return -1;
   for (ULONG i = 0; i < en.NumAdapters && !wadapter; i++) {
      MW_ESCAPE_GPU_INFO_DATA g = {0};
      g.Code = MW_ESCAPE_GPU_INFO;
      wadapter = info[i].hAdapter;
      if (kmt_escape(0, &g, sizeof(g)) >= 0 && g.Count != 0 && g.Valid != 0)
         wgpu = g;
      else
         wadapter = 0;
   }
   if (!wadapter)
      return -1;

   D3DKMT_CREATEDEVICE dev = {0};
   dev.hAdapter = wadapter;
   if (kmt.CreateDevice(&dev) < 0)
      return -1;
   wdevice = dev.hDevice;
   D3DKMT_CREATECONTEXT ctx = {0};
   ctx.hDevice = wdevice;
   ctx.NodeOrdinal = 0;
   ctx.EngineAffinity = 1;
   ctx.ClientHint = D3DKMT_CLIENTHINT_DX10;
   if (kmt.CreateContext(&ctx) < 0)
      return -1;
   wcontext = ctx.hContext;
   wcmd = ctx.pCommandBuffer;
   wcmd_size = ctx.CommandBufferSize;
   walloc = ctx.pAllocationList;
   walloc_size = ctx.AllocationListSize;
   wpatch = ctx.pPatchLocationList;
   wpatch_size = ctx.PatchLocationListSize;
   wddm = 1;
   if (trace_on())
      fprintf(stderr, "malikm-wddm: adapter 0x%x device 0x%x context 0x%x, %u allocation slots\n",
              wadapter, wdevice, wcontext, walloc_size);
   return 0;
}

/* ---- the kernel calls, by D3DKMT or by the runtime's callbacks ---- */

static NTSTATUS
w_escape(void *data, UINT size)
{
   if (ddi.on) {
      D3DDDICB_ESCAPE e = {0};
      e.hDevice = ddi.device;
      e.pPrivateDriverData = data;
      e.PrivateDriverDataSize = size;
      return ddi.cb.pfnEscapeCb(ddi.adapter, &e) >= 0 ? 0 : (NTSTATUS)0xC0000001;
   }
   return kmt_escape(wdevice, data, size);
}

static NTSTATUS
w_create(D3DDDI_ALLOCATIONINFO *ai)
{
   if (ddi.on) {
      D3DDDICB_ALLOCATE a = {0};
      a.NumAllocations = 1;
      a.pAllocationInfo = ai;
      return ddi.cb.pfnAllocateCb(ddi.device, &a) >= 0 ? 0 : (NTSTATUS)0xC0000017;
   }
   D3DKMT_CREATEALLOCATION ca = {0};
   ca.hDevice = wdevice;
   ca.NumAllocations = 1;
   ca.pAllocationInfo = ai;
   return kmt.CreateAllocation(&ca);
}

static void
w_destroy(D3DKMT_HANDLE h)
{
   if (ddi.on) {
      D3DDDICB_DEALLOCATE d = {0};
      d.NumAllocations = 1;
      d.HandleList = &h;
      ddi.cb.pfnDeallocateCb(ddi.device, &d);
      return;
   }
   D3DKMT_DESTROYALLOCATION da = {0};
   da.hDevice = wdevice;
   da.phAllocationList = &h;
   da.AllocationCount = 1;
   kmt.DestroyAllocation(&da);
}

static void *
w_lock(D3DKMT_HANDLE h)
{
   if (ddi.on) {
      D3DDDICB_LOCK l = {0};
      l.hAllocation = h;
      l.Flags.IgnoreSync = 1;
      return ddi.cb.pfnLockCb(ddi.device, &l) >= 0 ? l.pData : NULL;
   }
   D3DKMT_LOCK l = {0};
   l.hDevice = wdevice;
   l.hAllocation = h;
   l.Flags.IgnoreSync = 1;
   return kmt.Lock(&l) >= 0 ? l.pData : NULL;
}

static void
w_unlock(D3DKMT_HANDLE h)
{
   if (ddi.on) {
      D3DDDICB_UNLOCK u = {0};
      u.NumAllocations = 1;
      u.phAllocations = &h;
      ddi.cb.pfnUnlockCb(ddi.device, &u);
      return;
   }
   D3DKMT_UNLOCK u = {0};
   u.hDevice = wdevice;
   u.NumAllocations = 1;
   u.phAllocations = &h;
   kmt.Unlock(&u);
}

/* Render what is in the command buffer and allocation list; on return the
 * buffers are the new ones the runtime handed back. */
static NTSTATUS
w_render_inner(UINT commandLength, UINT allocations)
{
   if (ddi.on) {
      D3DDDICB_RENDER r = {0};
      r.hContext = ddi.context;
      r.CommandLength = commandLength;
      r.NumAllocations = allocations;
      if (ddi.cb.pfnRenderCb(ddi.device, &r) < 0)
         return (NTSTATUS)0xC0000001;
      wcmd = r.pNewCommandBuffer;
      wcmd_size = r.NewCommandBufferSize;
      walloc = r.pNewAllocationList;
      walloc_size = r.NewAllocationListSize;
      wpatch = r.pNewPatchLocationList;
      wpatch_size = r.NewPatchLocationListSize;

      /* A CPU event the scheduler sets when the context gets here. */
      HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
      if (ev) {
         D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 sig = {0};
         sig.hContext = ddi.context;
         sig.Flags.EnqueueCpuEvent = 1;
         sig.CpuEventHandle = ev;
         if (ddi.cb.pfnSignalSynchronizationObject2Cb(ddi.device, &sig) >= 0) {
            if (ddi.last_event)
               CloseHandle(ddi.last_event);
            ddi.last_event = ev;
            ddi.last_unsignalled = 0;
            return 0;
         }
         CloseHandle(ev);
      }
      if (!ddi.last_unsignalled || trace_on())
         fprintf(stderr, "malikm-wddm: no completion event for this submission;"
                 " waits poll the KMD until it catches up\n");
      ddi.last_unsignalled = 1;
      return 0;
   }
   D3DKMT_RENDER r = {0};
   r.hContext = wcontext;
   r.CommandLength = commandLength;
   r.AllocationCount = allocations;
   NTSTATUS st = kmt.Render(&r);
   if (st < 0)
      return st;
   wcmd = r.pNewCommandBuffer;
   wcmd_size = r.NewCommandBufferSize;
   walloc = r.pNewAllocationList;
   walloc_size = r.NewAllocationListSize;
   wpatch = r.pNewPatchLocationList;
   wpatch_size = r.NewPatchLocationListSize;
   return 0;
}

static NTSTATUS
w_render(UINT commandLength, UINT allocations)
{
   unsigned long long t0 = malikm_perf_now();
   NTSTATUS st = w_render_inner(commandLength, allocations);
   malikm_perf_add(6 /* submit */, t0);
   return st;
}

/* Everything the KMD was given has completed: its fences agree and nothing
 * is queued or running. For when there is no event to wait on. */
static int
wddm_poll_idle(void)
{
   for (int ms = 0; ms < 10000; ms++) {
      MW_ESCAPE_STATS_DATA st = {0};
      st.Code = MW_ESCAPE_STATS;
      if (w_escape(&st, sizeof(st)) < 0)
         return -MK_EIO;
      if (st.LastCompletedFence == st.LastSubmittedFence && !st.Queued && !st.Running)
         return 0;
      Sleep(1);
   }
   return -MK_ETIMEDOUT;
}

static void
wdead_flush(void)
{
   for (uint32_t i = 0; i < wdead_n; i++)
      w_destroy(wdead[i]);
   if (wdead_n && trace_on())
      fprintf(stderr, "malikm-wddm: destroyed %u closed allocations after idle\n", wdead_n);
   wdead_n = 0;
}

/* Everything submitted so far is done (one in-order queue in the KMD).
 * Why is for the slow-operation log: which caller waited. The wait is made
 * with wddm_lock held, so every other thread of the process that calls into
 * the shim waits as long (shim-lock in the log). */
static int
wddm_idle(const char *why)
{
   int ret = 0;
   unsigned long long t0 = malikm_perf_now();
   uint64_t slow_t0 = mali_slow_now();
   uint32_t slow_dead = wdead_n;

   if (ddi.on) {
      if (ddi.last_event && WaitForSingleObject(ddi.last_event, 10000) != WAIT_OBJECT_0)
         ret = -MK_ETIMEDOUT;
      else if (ddi.last_unsignalled)
         ret = wddm_poll_idle();
   } else {
      D3DKMT_WAITFORIDLE w = {0};
      w.hDevice = wdevice;
      ret = kmt.WaitForIdle(&w) >= 0 ? 0 : -MK_EIO;
   }
   malikm_perf_add(7 /* gpu-wait */, t0);
   MALI_SLOW(slow_t0, "gpu-idle", "for %s, %s, %s", why,
             ddi.on ? (ddi.last_unsignalled ? "polled the KMD" : "on the last event")
                    : "WaitForIdle", ret ? "timed out" : "idle");
   if (ret == 0) {
      uint64_t dead_t0 = mali_slow_now();
      wdead_flush();
      MALI_SLOW(dead_t0, "destroy-closed", "%u allocations", slow_dead);
   }
   if (ret == 0 && trace_on()) {
      /* A job that failed since the last wait, with the BO trace above it
       * that says which buffer its fault address belongs to. */
      static uint32_t failed_seen;
      MW_ESCAPE_STATS_DATA st = {0};
      st.Code = MW_ESCAPE_STATS;
      if (w_escape(&st, sizeof(st)) >= 0 && st.JobsFailed != failed_seen) {
         fprintf(stderr, "malikm-wddm: %u job(s) failed by now: js 0x%x fault 0x%x @0x%llx\n",
                 st.JobsFailed, st.LastJsStatus, st.LastFaultStatus,
                 (unsigned long long)st.LastFaultAddress);
         failed_seen = st.JobsFailed;
      }
   }
   return ret;
}

/*
 * WAIT_BO and SYNCOBJ_WAIT carry a timeout, relative by now. Panfrost asks
 * with a timeout of 0 whether a buffer is busy: panfrost_bo_cache_fetch
 * before reusing a cached BO, and panfrost_ptr_map before deciding to give
 * a WRITE_DISCARD map a fresh buffer instead of waiting. Both expect an
 * answer at once. wddm_idle ignores the timeout and waits until the GPU
 * has finished everything this device submitted, so each of those
 * questions can cost a whole frame of GPU time, with wddm_lock held.
 *
 * The timeout is kept: the wait gives up when it runs out and answers Busy,
 * which is what Panfrost expects of the Linux kernel. The answer is still
 * for the whole queue, not for the one BO, so it says busy more often than
 * Linux would; never ready when it is not. Busy is MK_ETIMEDOUT for WAIT_BO
 * and MK_ETIME for a syncobj, as Linux returns them.
 *
 * On the TaishanPi 3M, dwm.exe's log of slow operations had 38 waits of up
 * to 95 ms in 605 s with the timeout ignored, most of them inside a 64 KB
 * "Batch pool" BO taken from the cache, and none in 67 s with it kept. The
 * file C:\mali\wait-all (read once per process) brings the old waits back.
 */
static int
wait_keeps_timeout(void)
{
   static int on = -1;
   if (on < 0)
      on = GetFileAttributesA("C:\\mali\\wait-all") == INVALID_FILE_ATTRIBUTES;
   return on;
}

static int
wddm_idle_within(int64_t timeout_ns, int busy, const char *why)
{
   uint64_t slow_t0 = mali_slow_now();
   int ret;

   if (!wait_keeps_timeout() || !ddi.on || timeout_ns == MALIKM_TIMEOUT_INFINITE ||
       timeout_ns >= 10000000000ll) {
      ret = wddm_idle(why);
   } else if (!ddi.last_event && !ddi.last_unsignalled) {
      ret = wddm_idle(why);            /* nothing submitted: returns at once */
   } else if (ddi.last_unsignalled) {
      /* No event to wait on: one look at the KMD's fences, or the poll. */
      MW_ESCAPE_STATS_DATA st = {0};
      st.Code = MW_ESCAPE_STATS;
      if (timeout_ns > 0)
         ret = wddm_idle(why);
      else if (w_escape(&st, sizeof(st)) >= 0 &&
               st.LastCompletedFence == st.LastSubmittedFence && !st.Queued && !st.Running)
         ret = wddm_idle(why);
      else
         ret = busy;
   } else {
      DWORD ms = (DWORD)((timeout_ns + 999999) / 1000000);
      ret = WaitForSingleObject(ddi.last_event, ms) == WAIT_OBJECT_0 ? wddm_idle(why) : busy;
   }
   MALI_SLOW(slow_t0, why, "timeout %lld ns%s, %s", (long long)timeout_ns,
             wait_keeps_timeout() ? "" : " (not kept)", ret == busy ? "busy" : ret ? "error" : "ready");
   return ret;
}

/*
 * Primaries: the surfaces of a swap chain that the runtime presents itself.
 *
 * d3d10umd's CreateResource gets a pPrimaryDesc for them (dwm.exe makes
 * three at 2560x1440 for its full-screen swap chain). The runtime can only
 * present an allocation that belongs to such a resource: handing
 * pfnPresentCb an allocation of ours that it has never been told about, as
 * the code below did for every present, returns E_INVALIDARG there. So each
 * primary gets an allocation made through pfnAllocateCb with the runtime's
 * resource handle and the Primary flag.
 *
 * Panfrost still renders into its own buffer. A present copies the frame
 * into the primary's allocation on the CPU and then presents or flips that.
 */
unsigned
malikm_primary_create(void *hRTDevice, void *hRTResource, unsigned w, unsigned h,
                      unsigned vidpn_source)
{
   D3DKMT_HANDLE handle = 0;

   wlock(__func__);
   if (wselect_rt(hRTDevice) && ddi.on && ddi.cb.pfnAllocateCb) {
      MW_ALLOCATION_INFO pi = {0};
      D3DDDI_ALLOCATIONINFO ai = {0};
      D3DDDICB_ALLOCATE a = {0};
      HRESULT hr;

      pi.Version = MW_ABI_VERSION;
      pi.Flags = MW_ALLOC_NOEXEC | MW_ALLOC_PRIMARY;
      pi.Width = w;
      pi.Height = h;
      pi.Pitch = w * 4;
      pi.Format = D3DDDIFMT_A8R8G8B8;
      pi.Size = (uint64_t)w * h * 4;
      ai.pPrivateDriverData = &pi;
      ai.PrivateDriverDataSize = sizeof(pi);
      ai.VidPnSourceId = vidpn_source;
      ai.Flags.Primary = 1;
      a.hResource = hRTResource;
      a.NumAllocations = 1;
      a.pAllocationInfo = &ai;
      hr = ddi.cb.pfnAllocateCb(ddi.device, &a);
      if (hr >= 0)
         handle = ai.hAllocation;
      DebugPrintf("primary: %ux%u source %u, pfnAllocateCb 0x%08lx, allocation 0x%x\n",
                  w, h, vidpn_source, (unsigned long)hr, (unsigned)handle);
   }
   ReleaseSRWLockExclusive(&wddm_lock);
   return handle;
}

void
malikm_primary_destroy(void *hRTDevice, unsigned handle)
{
   if (!handle)
      return;
   wlock(__func__);
   if (wselect_rt(hRTDevice) && ddi.on) {
      uint64_t slow_t0 = mali_slow_now();
      w_destroy(handle);
      MALI_SLOW(slow_t0, "primary-destroy", "pfnDeallocateCb 0x%x", handle);
   }
   ReleaseSRWLockExclusive(&wddm_lock);
}

/*
 * DXGI's SetDisplayMode for a primary: make the swap chain's surface the one
 * the display shows. This is what moves a VidPN source from the desktop's
 * shared primary to the swap chain; until it has happened dxgkrnl answers
 * every flip of that chain with STATUS_GRAPHICS_PRESENT_MODE_CHANGED, and
 * DXGI calls SetDisplayMode again. d3d10umd had it as a stub: dwm.exe called
 * it 1887 times in one run, once at start and once after each failed flip.
 * Returns the HRESULT of pfnSetDisplayModeCb.
 */
long
malikm_set_display_mode(void *hRTDevice, unsigned primary_allocation)
{
   HRESULT hr = E_FAIL;

   wlock(__func__);
   if (wselect_rt(hRTDevice) && ddi.on && ddi.cb.pfnSetDisplayModeCb && primary_allocation) {
      static volatile LONG said = 0;
      D3DDDICB_SETDISPLAYMODE m = {0};

      m.hPrimaryAllocation = primary_allocation;
      hr = ddi.cb.pfnSetDisplayModeCb(ddi.device, &m);
      if (InterlockedIncrement(&said) <= 20)
         DebugPrintf("SetDisplayMode: primary 0x%x, pfnSetDisplayModeCb 0x%08lx, format attribute 0x%x\n",
                     primary_allocation, (unsigned long)hr, (unsigned)m.PrivateDriverFormatAttribute);
   } else {
      DebugPrintf("SetDisplayMode: not possible (bound %d, primary 0x%x)\n", ddi.on,
                  primary_allocation);
   }
   ReleaseSRWLockExclusive(&wddm_lock);
   return hr;
}

/* The HRESULT of the last pfnPresentCb on this thread, for d3d10umd's
 * Present to hand back to DXGI. */
static __declspec(thread) long present_result;

long
malikm_present_result(void)
{
   return present_result;
}

/* What the present in progress on this thread is for: set by d3d10umd's
 * Present around the frame read-back that ends in malikm_present_d3dddi. */
static __declspec(thread) struct {
   void *device;                /* the runtime device that presents */
   D3DKMT_HANDLE src, dst;
   int set;
} present_target;

void
malikm_present_target(void *hRTDevice, unsigned src_allocation, unsigned dst_allocation, int set)
{
   if (set)
      present_result = 0;
   present_target.device = hRTDevice;
   present_target.src = src_allocation;
   present_target.dst = dst_allocation;
   present_target.set = set;
}

/* Present an allocation the runtime knows as the presented resource's, as
 * it is: a shared surface, which Panfrost has rendered into directly. The
 * HRESULT of pfnPresentCb. */
long
malikm_present_allocation(void *hRTDevice, unsigned src_allocation, unsigned dst_allocation,
                          void *dxgi_context)
{
   static volatile LONG said = 0;
   HRESULT hr = E_FAIL;

   wlock(__func__);
   if (wselect_rt(hRTDevice) && ddi.on && ddi.dxgi && ddi.dxgi->pfnPresentCb && src_allocation) {
      DXGIDDICB_PRESENT p = {0};
      p.hSrcAllocation = src_allocation;
      p.hDstAllocation = dst_allocation;
      p.pDXGIContext = dxgi_context;
      p.hContext = ddi.context;
      unsigned long long t0 = malikm_perf_now();
      uint64_t slow_t0 = mali_slow_now();
      hr = ddi.dxgi->pfnPresentCb(ddi.device, &p);
      malikm_perf_add(4 /* present-cb */, t0);
      MALI_SLOW(slow_t0, "present-cb", "pfnPresentCb 0x%x -> 0x%x, 0x%08lx", src_allocation,
                dst_allocation, (unsigned long)hr);
   }
   if (InterlockedIncrement(&said) <= 40 || hr < 0)
      DebugPrintf("present: shared allocation 0x%x -> 0x%x, pfnPresentCb 0x%08lx\n",
                  src_allocation, dst_allocation, (unsigned long)hr);
   ReleaseSRWLockExclusive(&wddm_lock);
   return hr;
}

/* Present for d3d10umd on the hardware runtime: Bgrx is the frame, linear
 * and top-down; it is copied into a surface of the runtime device and
 * handed to DXGI, which has maliwddm blt it (on the CPU, for now) to the
 * screen. 0 if presented; -1 if not bound to the runtime (the caller falls
 * back to GDI). */
int
malikm_present_d3dddi(const void *bgrx, unsigned w, unsigned h, void *dxgi_context)
{
   int ret = -1;

   static volatile LONG said = 0;

   wlock(__func__);
   if (!present_target.set || !wselect_rt(present_target.device) || !ddi.on || !ddi.dxgi ||
       !ddi.dxgi->pfnPresentCb) {
      if (InterlockedIncrement(&said) <= 20)
         DebugPrintf("present: not through DXGI (bound %d, dxgi callbacks %p)\n",
                     ddi.on, (void *)ddi.dxgi);
      goto out;
   }
   if (present_target.set && present_target.src) {
      /* A primary: copy the frame into its own allocation and present that. */
      unsigned long long t0 = malikm_perf_now();
      void *cpu = w_lock(present_target.src);
      HRESULT hr = E_FAIL;

      if (cpu) {
         DXGIDDICB_PRESENT p = {0};
         memcpy(cpu, bgrx, (size_t)w * h * 4);
         w_unlock(present_target.src);
         malikm_perf_add(3 /* primary-copy */, t0);
         p.hSrcAllocation = present_target.src;
         p.hDstAllocation = present_target.dst;
         p.pDXGIContext = dxgi_context;
         p.hContext = ddi.context;
         t0 = malikm_perf_now();
         hr = ddi.dxgi->pfnPresentCb(ddi.device, &p);
         malikm_perf_add(4 /* present-cb */, t0);
      }
      present_result = hr;
      if (InterlockedIncrement(&said) <= 20)
         DebugPrintf("present: primary 0x%x -> 0x%x, %ux%u, lock %p, pfnPresentCb 0x%08lx\n",
                     (unsigned)present_target.src, (unsigned)present_target.dst, w, h, cpu,
                     (unsigned long)hr);
      ret = hr >= 0 ? 0 : -1;
      goto out;
   }
   if (ddi.present && (ddi.present_w != w || ddi.present_h != h)) {
      w_unlock(ddi.present);
      w_destroy(ddi.present);
      ddi.present = 0;
      ddi.present_cpu = NULL;
   }
   if (!ddi.present) {
      MW_ALLOCATION_INFO pi = {0};
      D3DDDI_ALLOCATIONINFO ai = {0};
      pi.Version = MW_ABI_VERSION;
      pi.Flags = MW_ALLOC_NOEXEC;
      pi.Width = w;
      pi.Height = h;
      pi.Pitch = w * 4;
      pi.Format = D3DDDIFMT_X8R8G8B8;
      pi.Size = (uint64_t)w * h * 4;
      ai.pPrivateDriverData = &pi;
      ai.PrivateDriverDataSize = sizeof(pi);
      if (w_create(&ai) < 0)
         goto out;
      ddi.present = ai.hAllocation;
      ddi.present_cpu = w_lock(ddi.present);
      ddi.present_w = w;
      ddi.present_h = h;
      if (!ddi.present_cpu)
         goto out;
   }
   memcpy(ddi.present_cpu, bgrx, (size_t)w * h * 4);

   DXGIDDICB_PRESENT p = {0};
   p.hSrcAllocation = ddi.present;
   p.pDXGIContext = dxgi_context;
   p.hContext = ddi.context;
   HRESULT hr = ddi.dxgi->pfnPresentCb(ddi.device, &p);
   ret = hr >= 0 ? 0 : -1;
   if (InterlockedIncrement(&said) <= 20)
      DebugPrintf("present: %ux%u, pfnPresentCb 0x%08lx, DXGI context %p\n", w, h,
                  (unsigned long)hr, dxgi_context);
   if (trace_on() && ret)
      fprintf(stderr, "malikm: pfnPresentCb failed\n");
out:
   ReleaseSRWLockExclusive(&wddm_lock);
   return ret;
}

/* The runtime's device and callbacks, from d3d10umd's CreateDevice. 0 if
 * this is maliwddm's adapter and Panfrost now goes through the callbacks. */
int
malikm_bind_d3dddi(HANDLE hRTAdapter, HANDLE hRTDevice, const void *pKTCallbacks,
                   const void *pDXGIBaseCallbacks)
{
   const D3DDDI_DEVICECALLBACKS *cb = pKTCallbacks;
   MW_ESCAPE_GPU_INFO_DATA g = {0};
   int ret = -1;

   if (!cb || !cb->pfnEscapeCb || !cb->pfnAllocateCb || !cb->pfnRenderCb || !cb->pfnCreateContextCb) {
      DebugPrintf("bind: the runtime's callbacks are incomplete (%p)\n", (void *)cb);
      return -1;
   }
   wlock(__func__);
   /* This device's own set of state: a free one, or the one a destroyed
    * device with the same handle left behind. */
   struct wdev *dev = NULL;
   for (int i = 1; i < WDEV_MAX; i++)
      if (wdevs[i].used && wdevs[i].rt_device == hRTDevice)
         dev = &wdevs[i];
   for (int i = 1; i < WDEV_MAX && !dev; i++)
      if (!wdevs[i].used)
         dev = &wdevs[i];
   wbinding = NULL;
   if (!dev) {
      ReleaseSRWLockExclusive(&wddm_lock);
      DebugPrintf("bind: runtime device %p: no free device slot\n", hRTDevice);
      return -1;
   }
   if (dev->used) {
      /* Left over: its allocations went with the runtime's device. */
      wselect(dev);
      free(wbo);
      free(wdead);
      if (ddi.last_event)
         CloseHandle(ddi.last_event);
      wselect(&wdevs[0]);
   }
   memset(dev, 0, sizeof(*dev));
   dev->used = 1;
   dev->rt_device = hRTDevice;
   wselect(dev);
   /* Ask the adapter through the callbacks: a software runtime routes them
    * to d3d10umd's own D3DKMT stubs, which fail. */
   D3DDDICB_ESCAPE e = {0};
   e.hDevice = hRTDevice;
   e.pPrivateDriverData = &g;
   e.PrivateDriverDataSize = sizeof(g);
   g.Code = MW_ESCAPE_GPU_INFO;
   HRESULT ehr = cb->pfnEscapeCb(hRTAdapter, &e), chr = E_FAIL;
   if (ehr >= 0 && g.Count != 0 && g.Valid != 0) {
      D3DDDICB_CREATECONTEXT c = {0};
      c.NodeOrdinal = 0;
      c.EngineAffinity = 1;
      chr = cb->pfnCreateContextCb(hRTDevice, &c);
      if (chr >= 0) {
         ddi.on = 1;
         ddi.adapter = hRTAdapter;
         ddi.device = hRTDevice;
         ddi.context = c.hContext;
         ddi.cb = *cb;
         ddi.dxgi = pDXGIBaseCallbacks;
         wcmd = c.pCommandBuffer;
         wcmd_size = c.CommandBufferSize;
         walloc = c.pAllocationList;
         walloc_size = c.AllocationListSize;
         wpatch = c.pPatchLocationList;
         wpatch_size = c.PatchLocationListSize;
         wgpu = g;
         wddm = 1;
         ret = 0;
      }
   }
   if (ret == 0) {
      wbinding = dev;
   } else {
      wselect(&wdevs[0]);
      memset(dev, 0, sizeof(*dev));
   }
   ReleaseSRWLockExclusive(&wddm_lock);
   /* Why a process does or does not get the Mali: without this the only
    * sign is "screen softpipe" a few lines later. */
   DebugPrintf("bind: runtime device %p: %s (GPU info escape 0x%08lx, %u params, valid 0x%llx;"
               " create context 0x%08lx)\n", hRTDevice, ret == 0 ? "bound" : "NOT bound",
               (unsigned long)ehr, (unsigned)g.Count, (unsigned long long)g.Valid,
               (unsigned long)chr);
   if (trace_on())
      fprintf(stderr, "malikm: bind to the runtime device %p: %s\n", hRTDevice,
              ret == 0 ? "yes, through its callbacks" : "no (not maliwddm, or software runtime)");
   return ret;
}

/* The device's screen exists (or will not): fds seen on this thread from
 * now on are looked up, not adopted. */
void
malikm_bind_done(void)
{
   wbinding = NULL;
}

/* DestroyDevice: the runtime's device is going, and its allocations with
 * it. Forget the device. */
void
malikm_unbind_d3dddi(void *hRTDevice)
{
   wlock(__func__);
   for (int i = 1; i < WDEV_MAX; i++) {
      struct wdev *dev = &wdevs[i];
      if (!dev->used || dev->rt_device != (HANDLE)hRTDevice)
         continue;
      wselect(dev);
      unsigned live = 0;
      for (uint32_t k = 0; k < wbo_cap; k++)
         live += wbo[k].h != 0;
      DebugPrintf("unbind: runtime device %p, %u buffer(s) still open, %u waiting to be"
                  " destroyed\n", hRTDevice, live, (unsigned)wdead_n);
      free(wbo);
      free(wdead);
      if (ddi.last_event)
         CloseHandle(ddi.last_event);
      wselect(&wdevs[0]);
      memset(dev, 0, sizeof(*dev));
   }
   if (wbinding && !wbinding->used)
      wbinding = NULL;
   ReleaseSRWLockExclusive(&wddm_lock);
}

static struct wbo *
wddm_bo(uint32_t h)
{
   return h && h <= wbo_cap && wbo[h - 1].h ? &wbo[h - 1] : NULL;
}

static int
wddm_bo_is_resource(int fd, uint32_t handle)
{
   int ret;

   wlock(__func__);
   wselect_fd(fd);
   struct wbo *bo = wddm_bo(handle);
   ret = bo && bo->rt_resource && !bo->released;
   ReleaseSRWLockExclusive(&wddm_lock);
   return ret;
}

/*
 * Shared resources (D3D10_DDI_RESOURCE_MISC_SHARED): one process renders
 * into the surface and another samples it, with no copy in between
 * (LogonUI.exe and dwm.exe: the logon screen). So the buffer Panfrost
 * renders into has to be an allocation the runtime knows as the resource's:
 * made through pfnAllocateCb with the runtime's resource handle on the
 * creating side, and on the opening side the allocation handle the runtime
 * passes to OpenResource. maliwddm gives an allocation one GPU address for
 * its whole life in the one address space all processes share, so both
 * sides see the same GPU address.
 *
 * d3d10umd brackets screen->resource_create with malikm_shared_begin_* and
 * malikm_shared_end on the calling thread; the first CREATE_BO in between
 * becomes (create) or wraps (open) that allocation. The template has
 * PIPE_BIND_SHARED, which keeps Panfrost away from its BO cache and makes
 * the layout linear on both sides.
 */
#define SHARED_CREATE 1
#define SHARED_OPEN   2
#define SHARED_PRIMARY 3        /* malikm_primary_begin_create, below */
#define SHARED_PRIV_MAX 128

static __declspec(thread) struct {
   int mode;                    /* 0, SHARED_CREATE, SHARED_OPEN */
   int done;                    /* a BO took the allocation */
   HANDLE device;               /* the runtime device of the resource */
   void *rt_resource;
   unsigned w, h;
   unsigned pitch, vidpn_source;   /* a primary's */
   unsigned char priv[SHARED_PRIV_MAX];
   unsigned priv_size;
   D3DKMT_HANDLE allocation;    /* open: in; create: out */
} shared_next;

void
malikm_shared_begin_create(void *hRTDevice, void *hRTResource, unsigned w, unsigned h,
                           const void *desc, unsigned desc_size)
{
   memset(&shared_next, 0, sizeof(shared_next));
   if (desc_size > SHARED_PRIV_MAX) {
      DebugPrintf("shared create: description of %u bytes does not fit\n", desc_size);
      return;
   }
   shared_next.mode = SHARED_CREATE;
   shared_next.device = hRTDevice;
   shared_next.rt_resource = hRTResource;
   shared_next.w = w;
   shared_next.h = h;
   memcpy(shared_next.priv, desc, desc_size);
   shared_next.priv_size = desc_size;
}

void
malikm_shared_begin_open(void *hRTDevice, void *hRTResource, unsigned hAllocation)
{
   memset(&shared_next, 0, sizeof(shared_next));
   shared_next.mode = SHARED_OPEN;
   shared_next.device = hRTDevice;
   shared_next.rt_resource = hRTResource;
   shared_next.allocation = hAllocation;
}

/*
 * A primary Panfrost renders into.
 *
 * malikm_primary_create (above) gives the runtime an allocation next to
 * Panfrost's own buffer, and every present copies the frame from one to the
 * other on the CPU. A primary can instead be made the way a shared resource
 * is: the buffer Panfrost creates between this call and malikm_shared_end is
 * the runtime's primary allocation itself, made through pfnAllocateCb with
 * the resource handle and the Primary flag. maliwddm already gives every
 * allocation, primaries included, a GPU address and maps its pages there
 * when VidMm makes it resident, so nothing changes in the kernel. A present
 * of such a primary is malikm_present_allocation: no pixel is touched.
 *
 * Pitch is the row pitch Panfrost will use (the caller checks afterwards
 * that it did); maliwddm copies the primary to the framebuffer with it.
 * The allocation goes back through malikm_shared_destroy.
 */
void
malikm_primary_begin_create(void *hRTDevice, void *hRTResource, unsigned w, unsigned h,
                            unsigned pitch, unsigned vidpn_source)
{
   memset(&shared_next, 0, sizeof(shared_next));
   shared_next.mode = SHARED_PRIMARY;
   shared_next.device = hRTDevice;
   shared_next.rt_resource = hRTResource;
   shared_next.w = w;
   shared_next.h = h;
   shared_next.pitch = pitch;
   shared_next.vidpn_source = vidpn_source;
}

/* RotateResourceIdentities gave the primary on this allocation to another
 * runtime resource: it is that resource's deallocation that returns it. */
void
malikm_primary_rebind(void *hRTDevice, unsigned allocation, void *hRTResource)
{
   wlock(__func__);
   if (wselect_rt(hRTDevice)) {
      for (uint32_t i = 0; i < wbo_cap; i++) {
         if (wbo[i].h == allocation && wbo[i].primary && !wbo[i].released)
            wbo[i].rt_resource = hRTResource;
      }
   }
   ReleaseSRWLockExclusive(&wddm_lock);
}

/* Everything this device has submitted is done. For a present that may not
 * rely on the scheduler to keep a flip behind the rendering it shows. */
int
malikm_wait_idle(void *hRTDevice)
{
   int ret = -1;

   wlock(__func__);
   if (wselect_rt(hRTDevice) && ddi.on)
      ret = wddm_idle("present-wait");
   ReleaseSRWLockExclusive(&wddm_lock);
   return ret;
}

/* The allocation the BO was made on; 0 if no BO was created in between (a
 * BO from Panfrost's cache, or not the runtime's device) or it failed. */
unsigned
malikm_shared_end(void)
{
   unsigned ret = shared_next.done ? shared_next.allocation : 0;

   if (shared_next.mode && !shared_next.done)
      DebugPrintf("shared %s: resource %p: no buffer was made on the allocation\n",
                  shared_next.mode == SHARED_CREATE ? "create" :
                  shared_next.mode == SHARED_PRIMARY ? "primary" : "open",
                  shared_next.rt_resource);
   memset(&shared_next, 0, sizeof(shared_next));
   return ret;
}

/* Give the resource's allocations back to the runtime: the creator's are
 * destroyed when the last process lets go, an opener's are closed. Called
 * after d3d10umd has flushed and dropped its reference. */
void
malikm_shared_destroy(void *hRTDevice, void *hRTResource)
{
   if (!hRTResource)
      return;
   wlock(__func__);
   if (wselect_rt(hRTDevice) && ddi.on && ddi.cb.pfnDeallocateCb) {
      D3DDDICB_DEALLOCATE d = {0};
      unsigned held = 0;
      HRESULT hr;
      int idle;

      /* Nothing submitted may still be running on it. */
      uint64_t slow_t0 = mali_slow_now();
      idle = wddm_idle("shared-destroy");
      for (uint32_t i = 0; i < wbo_cap; i++) {
         struct wbo *bo = &wbo[i];
         if (!bo->h || bo->rt_resource != hRTResource || bo->rt_device != (HANDLE)hRTDevice)
            continue;
         /* Panfrost has not closed it (a batch still refers to it). */
         if (bo->cpu)
            w_unlock(bo->h);
         bo->cpu = NULL;
         bo->released = 1;
         held++;
      }
      d.hResource = hRTResource;
      hr = ddi.cb.pfnDeallocateCb((HANDLE)hRTDevice, &d);
      MALI_SLOW(slow_t0, "shared-destroy", "resource %p, %u buffer(s) held, pfnDeallocateCb 0x%08lx",
                hRTResource, held, (unsigned long)hr);
      DebugPrintf("shared destroy: resource %p device %p, idle %d, %u buffer(s) still held,"
                  " pfnDeallocateCb 0x%08lx\n", hRTResource, hRTDevice, idle, held,
                  (unsigned long)hr);
   }
   ReleaseSRWLockExclusive(&wddm_lock);
}

/* BOs this device holds, for the slow-operation log. Under wddm_lock. */
static unsigned
wbo_live(void)
{
   unsigned n = 0;
   for (uint32_t i = 0; i < wbo_cap; i++)
      n += wbo[i].h != 0;
   return n;
}

static int
wddm_drm(uint32_t nr, uint8_t *b, uint32_t size)
{
   switch (nr) {
   case MALIKM_NR_PANFROST_GET_PARAM: {
      struct drm_panfrost_get_param *a = (void *)b;
      if (a->param >= MW_GPU_PARAMS || !(wgpu.Valid & (1ull << a->param)))
         return -MK_EINVAL;
      a->value = wgpu.Value[a->param];
      return 0;
   }
   case MALIKM_NR_PANFROST_CREATE_BO: {
      struct drm_panfrost_create_bo *a = (void *)b;
      uint64_t sz = ((uint64_t)a->size + 4095) & ~4095ull;
      if (a->flags & PANFROST_BO_HEAP)
         sz = (sz + 0x1fffff) & ~0x1fffffull;
      struct {
         MW_ALLOCATION_INFO pi;
         unsigned char desc[SHARED_PRIV_MAX];   /* a shared resource's description */
      } priv;
      D3DDDI_ALLOCATIONINFO ai = {0};
      MW_ESCAPE_ALLOC_INFO_DATA where = {0};
      int shared = ddi.on && !shared_next.done ? shared_next.mode : 0;
      unsigned long long t0 = malikm_perf_now();
      uint64_t slow_t0 = mali_slow_now();

      /* The primary's buffer holds all its rows; anything smaller made
       * first is something else. */
      if (shared == SHARED_PRIMARY && sz < (uint64_t)shared_next.pitch * shared_next.h)
         shared = 0;

      memset(&priv, 0, sizeof(priv));
      priv.pi.Version = MW_ABI_VERSION;
      priv.pi.Flags = (a->flags & PANFROST_BO_NOEXEC) ? MW_ALLOC_NOEXEC : 0;
      priv.pi.Size = sz;
      ai.pPrivateDriverData = &priv;
      ai.PrivateDriverDataSize = sizeof(priv.pi);
      where.Code = MW_ESCAPE_ALLOC_INFO;

      if (shared_next.mode && !shared)
         DebugPrintf("shared: another buffer of %llu bytes while resource %p is being made"
                     " (bound %d); it gets an allocation of its own\n",
                     (unsigned long long)sz, shared_next.rt_resource, ddi.on);

      if (shared == SHARED_OPEN) {
         /* The allocation exists: the runtime opened it for this device. */
         NTSTATUS est;
         where.hAllocation = shared_next.allocation;
         est = w_escape(&where, sizeof(where));
         DebugPrintf("shared open: resource %p device %p (bound device %p), allocation 0x%x:"
                     " escape 0x%08lx, GPU 0x%llx, %llu bytes, wanted %llu\n",
                     shared_next.rt_resource, (void *)shared_next.device, (void *)ddi.device,
                     (unsigned)shared_next.allocation, (unsigned long)est,
                     (unsigned long long)where.GpuVa, (unsigned long long)where.Size,
                     (unsigned long long)sz);
         if (est < 0 || where.GpuVa == 0 || where.Size < sz)
            return -MK_ENOMEM;
         ai.hAllocation = shared_next.allocation;
      } else if (shared == SHARED_CREATE) {
         /* The runtime's resource gets this allocation, and with it the
          * description the opening side rebuilds the resource from: as the
          * resource's private data and again behind the allocation's. */
         D3DDDICB_ALLOCATE al = {0};
         HRESULT hr;

         priv.pi.Width = shared_next.w;
         priv.pi.Height = shared_next.h;
         priv.pi.Pitch = shared_next.w * 4;
         priv.pi.Format = D3DDDIFMT_A8R8G8B8;
         memcpy(priv.desc, shared_next.priv, shared_next.priv_size);
         ai.PrivateDriverDataSize = sizeof(priv.pi) + shared_next.priv_size;
         al.pPrivateDriverData = shared_next.priv;
         al.PrivateDriverDataSize = shared_next.priv_size;
         al.hResource = shared_next.rt_resource;
         al.NumAllocations = 1;
         al.pAllocationInfo = &ai;
         hr = ddi.cb.pfnAllocateCb(shared_next.device, &al);
         DebugPrintf("shared create: resource %p device %p (bound device %p), %llu bytes:"
                     " pfnAllocateCb 0x%08lx, allocation 0x%x, kernel resource 0x%x\n",
                     shared_next.rt_resource, (void *)shared_next.device, (void *)ddi.device,
                     (unsigned long long)sz, (unsigned long)hr, (unsigned)ai.hAllocation,
                     (unsigned)al.hKMResource);
         if (hr < 0)
            return -MK_ENOMEM;
         where.hAllocation = ai.hAllocation;
         if (w_escape(&where, sizeof(where)) < 0 || where.GpuVa == 0) {
            D3DDDICB_DEALLOCATE d = {0};
            DebugPrintf("shared create: no GPU address for allocation 0x%x\n",
                        (unsigned)ai.hAllocation);
            d.hResource = shared_next.rt_resource;
            ddi.cb.pfnDeallocateCb(shared_next.device, &d);
            return -MK_ENOMEM;
         }
         DebugPrintf("shared create: allocation 0x%x at GPU 0x%llx, %llu bytes\n",
                     (unsigned)ai.hAllocation, (unsigned long long)where.GpuVa,
                     (unsigned long long)where.Size);
      } else if (shared == SHARED_PRIMARY) {
         /* The runtime's primary: what malikm_primary_create asks for, with
          * the size and pitch of the buffer Panfrost is creating. */
         D3DDDICB_ALLOCATE al = {0};
         HRESULT hr;

         priv.pi.Flags = MW_ALLOC_NOEXEC | MW_ALLOC_PRIMARY;
         priv.pi.Width = shared_next.w;
         priv.pi.Height = shared_next.h;
         priv.pi.Pitch = shared_next.pitch;
         priv.pi.Format = D3DDDIFMT_A8R8G8B8;
         ai.VidPnSourceId = shared_next.vidpn_source;
         ai.Flags.Primary = 1;
         al.hResource = shared_next.rt_resource;
         al.NumAllocations = 1;
         al.pAllocationInfo = &ai;
         hr = ddi.cb.pfnAllocateCb(shared_next.device, &al);
         if (hr >= 0) {
            where.hAllocation = ai.hAllocation;
            if (w_escape(&where, sizeof(where)) < 0 || where.GpuVa == 0 || where.Size < sz) {
               D3DDDICB_DEALLOCATE d = {0};
               d.hResource = shared_next.rt_resource;
               ddi.cb.pfnDeallocateCb(shared_next.device, &d);
               hr = E_FAIL;
            }
         }
         DebugPrintf("primary: rendered into, %ux%u pitch %u source %u, %llu bytes:"
                     " pfnAllocateCb 0x%08lx, allocation 0x%x at GPU 0x%llx\n",
                     shared_next.w, shared_next.h, shared_next.pitch, shared_next.vidpn_source,
                     (unsigned long long)sz, (unsigned long)hr, (unsigned)ai.hAllocation,
                     (unsigned long long)where.GpuVa);
         if (hr < 0)
            return -MK_ENOMEM;
      } else {
         if (w_create(&ai) < 0)
            return -MK_ENOMEM;
         where.hAllocation = ai.hAllocation;
         if (w_escape(&where, sizeof(where)) < 0 || where.GpuVa == 0) {
            w_destroy(ai.hAllocation);
            return -MK_ENOMEM;
         }
      }
      uint32_t h = 0;
      for (uint32_t i = 0; i < wbo_cap && !h; i++)
         if (!wbo[i].h)
            h = i + 1;
      if (!h) {
         uint32_t cap = wbo_cap ? wbo_cap * 2 : 256;
         struct wbo *n = realloc(wbo, cap * sizeof(*n));
         if (!n) {
            if (shared == SHARED_CREATE || shared == SHARED_PRIMARY) {
               D3DDDICB_DEALLOCATE d = {0};
               d.hResource = shared_next.rt_resource;
               ddi.cb.pfnDeallocateCb(shared_next.device, &d);
            } else if (!shared) {
               w_destroy(ai.hAllocation);
            }
            return -MK_ENOMEM;
         }
         memset(n + wbo_cap, 0, (cap - wbo_cap) * sizeof(*n));
         wbo = n;
         h = wbo_cap + 1;
         wbo_cap = cap;
      }
      wbo[h - 1] = (struct wbo){ai.hAllocation, where.Size, where.GpuVa, NULL};
      if (shared) {
         wbo[h - 1].rt_resource = shared_next.rt_resource;
         wbo[h - 1].rt_device = shared_next.device;
         wbo[h - 1].primary = shared == SHARED_PRIMARY;
         shared_next.allocation = ai.hAllocation;
         shared_next.done = 1;
      }
      a->handle = h;
      a->offset = where.GpuVa;
      malikm_perf_add(9 /* bo-create */, t0);
      MALI_SLOW(slow_t0, "bo-create", "%llu bytes, %s, %u live BOs on this device",
                (unsigned long long)sz,
                shared == SHARED_CREATE ? "shared" : shared == SHARED_OPEN ? "opened" :
                shared == SHARED_PRIMARY ? "primary" : "plain", wbo_live());
      /* Where every BO lives, so a GPU fault address can be traced back to
       * the buffer it fell in (the 0x4002c3 faults at a fixed VA). */
      if (trace_on())
         fprintf(stderr, "malikm-wddm: bo %u va 0x%llx-0x%llx flags 0x%x%s\n", h,
                 (unsigned long long)where.GpuVa,
                 (unsigned long long)(where.GpuVa + where.Size),
                 (unsigned)a->flags, (a->flags & PANFROST_BO_HEAP) ? " HEAP" : "");
      return 0;
   }
   case MALIKM_NR_PANFROST_MMAP_BO: {
      struct drm_panfrost_mmap_bo *a = (void *)b;
      if (!wddm_bo(a->handle))
         return -MK_ENOENT;
      a->offset = MALIKM_MMAP_OFFSET(a->handle);
      return 0;
   }
   case MALIKM_NR_PANFROST_GET_BO_OFFSET: {
      struct drm_panfrost_get_bo_offset *a = (void *)b;
      struct wbo *bo = wddm_bo(a->handle);
      if (!bo)
         return -MK_ENOENT;
      a->offset = bo->va;
      return 0;
   }
   case MALIKM_NR_PANFROST_MADVISE:
      ((struct drm_panfrost_madvise *)b)->retained = 1;
      return 0;
   case MALIKM_NR_PANFROST_WAIT_BO: {
      struct drm_panfrost_wait_bo *a = (void *)b;
      if (!wddm_bo(a->handle))
         return -MK_ENOENT;
      return wddm_idle_within(a->timeout_ns, -MK_ETIMEDOUT, "wait-bo");
   }
   case MALIKM_NR_PANFROST_SUBMIT: {
      struct drm_panfrost_submit *a = (void *)b;
      const uint32_t *bos = (const uint32_t *)(b + a->bo_handles);
      if (!a->jc)
         return -MK_EINVAL;
      if (a->bo_handle_count > walloc_size || sizeof(MW_COMMAND) > wcmd_size)
         return -MK_E2BIG;
      static uint64_t mark;
      uint32_t n = 0;
      mark++;
      for (uint32_t i = 0; i < a->bo_handle_count; i++) {
         struct wbo *bo = wddm_bo(bos[i]);
         if (!bo)
            return -MK_ENOENT;
         if (bo->listed == mark || bo->released)
            continue;
         bo->listed = mark;
         memset(&walloc[n], 0, sizeof(walloc[n]));
         walloc[n].hAllocation = bo->h;
         walloc[n].WriteOperation = 1;
         n++;
      }
      /* Every other live BO too. Linux's panfrost maps a BO into the GPU
       * VM when it is created, so a job may touch BOs it does not name,
       * and Mesa relies on that. VidMm only pages in (and maliwddm only
       * maps) what is in the allocation list, so such a job faulted: PE,
       * 2026-10-17, two read translation faults (0x4002c3) at 0xa131000,
       * the first byte of a 64 KiB BO, in every present.exe run. */
      uint32_t named = n;
      for (uint32_t i = 0; i < wbo_cap && n < walloc_size; i++) {
         struct wbo *bo = &wbo[i];
         if (!bo->h || bo->released || bo->primary || bo->listed == mark)
            continue;
         bo->listed = mark;
         memset(&walloc[n], 0, sizeof(walloc[n]));
         walloc[n].hAllocation = bo->h;
         walloc[n].WriteOperation = 1;
         n++;
      }
      if (trace_on() && n != named)
         fprintf(stderr, "malikm-wddm: submit names %u BOs, %u more listed for residency\n",
                 named, n - named);
      MW_COMMAND *c = wcmd;
      c->Type = MW_CMD_JOB;
      c->Slot = (a->requirements & PANFROST_JD_REQ_FS) ? 0 : 1;
      c->Jc = a->jc;
      unsigned slow_slot = c->Slot;
      uint64_t slow_t0 = mali_slow_now();
      NTSTATUS st = w_render(sizeof(*c), n);
      /* c is the old command buffer now; the runtime has handed out a new one. */
      MALI_SLOW(slow_t0, "submit", "pfnRenderCb, %s chain, %u BOs named, %u listed",
                slow_slot ? "vertex/tiler" : "fragment", named, n);
      if (st < 0) {
         if (trace_on())
            fprintf(stderr, "malikm-wddm: Render failed 0x%08lx\n", (unsigned long)st);
         return -MK_EIO;
      }
      wseq++;
      return 0;
   }
   case MALIKM_NR_GEM_CLOSE: {
      struct wbo *bo = wddm_bo(((struct drm_gem_close *)b)->handle);
      if (!bo)
         return -MK_EINVAL;
      if (trace_on())
         fprintf(stderr, "malikm-wddm: close bo %u va 0x%llx\n",
                 ((struct drm_gem_close *)b)->handle, (unsigned long long)bo->va);
      if (bo->cpu) {
         uint64_t slow_t0 = mali_slow_now();
         w_unlock(bo->h);
         MALI_SLOW(slow_t0, "bo-unmap", "pfnUnlockCb at close, %llu bytes",
                   (unsigned long long)bo->size);
      }
      if (bo->rt_resource) {
         /* A runtime resource's allocation goes back through the resource,
          * in malikm_shared_destroy. */
         memset(bo, 0, sizeof(*bo));
         return 0;
      }
      /* Not destroyed yet: see wdead. The handle number is free at once. */
      if (wdead_n == wdead_cap) {
         uint32_t cap = wdead_cap ? wdead_cap * 2 : WDEAD_FLUSH;
         D3DKMT_HANDLE *n = realloc(wdead, cap * sizeof(*n));
         if (!n) {
            wddm_idle("bo-close, no memory");
            w_destroy(bo->h);
            memset(bo, 0, sizeof(*bo));
            return 0;
         }
         wdead = n;
         wdead_cap = cap;
      }
      wdead[wdead_n++] = bo->h;
      memset(bo, 0, sizeof(*bo));
      if (wdead_n >= WDEAD_FLUSH)
         wddm_idle("64 closed BOs");
      return 0;
   }
   case MALIKM_NR_SYNCOBJ_CREATE: {
      static uint32_t next = 1;
      ((struct drm_syncobj_create *)b)->handle = next++;
      return 0;
   }
   case MALIKM_NR_SYNCOBJ_WAIT: {
      struct drm_syncobj_wait *a = (void *)b;
      a->first_signaled = 0;
      return wddm_idle_within(a->timeout_nsec, -MK_ETIME, "syncobj-wait");
   }
   case MALIKM_NR_SYNCOBJ_DESTROY:
   case MALIKM_NR_SYNCOBJ_RESET:
   case MALIKM_NR_SYNCOBJ_SIGNAL:
      return 0;
   case MALIKM_NR_SYNCOBJ_EXPORT_SEQ:
      ((MALIKM_SYNCOBJ_SEQ *)b)->Seq = wseq;
      return 0;
   case MALIKM_NR_SYNCOBJ_IMPORT_SEQ:
      return 0;
   default:
      (void)size;
      return -MK_ENOSYS;
   }
}

static void
wddm_ioctl(int fd, DWORD code, void *buf, DWORD len)
{
   MALIKM_DRM_HEADER *h = buf;
   (void)len;

   /* One lock for the whole process: a thread waiting for the GPU inside
    * it holds up every other thread's allocations, maps and submissions. */
   uint64_t slow_t0 = mali_slow_now();
   AcquireSRWLockExclusive(&wddm_lock);
   MALI_SLOW(slow_t0, "shim-lock", "waited for it, ioctl 0x%lx nr 0x%x", (unsigned long)code,
             code == IOCTL_MALIKM_DRM ? h->Nr : 0);
   wselect_fd(fd);
   switch (code) {
   case IOCTL_MALIKM_VERSION: {
      MALIKM_VERSION *v = buf;
      memset(v, 0, sizeof(*v));
      v->Major = MALIKM_DRM_MAJOR;
      v->Minor = MALIKM_DRM_MINOR;
      strcpy(v->Name, "panfrost");
      break;
   }
   case IOCTL_MALIKM_MAP: {
      /* A persistent mapping: Panfrost keeps BOs mapped for their lifetime.
       * IgnoreSync: the GPU may still use the BO; ordering is Panfrost's. */
      MALIKM_MAP *m = (MALIKM_MAP *)(h + 1);
      struct wbo *bo = wddm_bo(MALIKM_MMAP_HANDLE(m->Offset));
      h->Result = -MK_EINVAL;
      if (bo && !bo->released && m->Size <= bo->size) {
         if (!bo->cpu) {
            uint64_t lock_t0 = mali_slow_now();
            bo->cpu = w_lock(bo->h);
            MALI_SLOW(lock_t0, "bo-map", "pfnLockCb, %llu bytes, %s", (unsigned long long)bo->size,
                      bo->cpu ? "mapped" : "failed");
         }
         if (bo->cpu) {
            m->Address = (uint64_t)(uintptr_t)bo->cpu;
            h->Result = 0;
         }
      }
      break;
   }
   case IOCTL_MALIKM_UNMAP:
      h->Result = 0;   /* the Lock stays until GEM_CLOSE */
      break;
   case IOCTL_MALIKM_DRM:
      h->Result = wddm_drm(h->Nr, (uint8_t *)(h + 1), h->Size);
      break;
   default:
      h->Result = -MK_ENOSYS;
      break;
   }
   if (trace_on() && code == IOCTL_MALIKM_DRM)
      fprintf(stderr, "malikm-wddm: nr 0x%x -> %d\n", h->Nr, h->Result);
   ReleaseSRWLockExclusive(&wddm_lock);
}

/* ---- MALIKM_FAKE ----
 * The driver, emulated in this process on the same marshalled buffers, for
 * CI runners with no Mali: buffers are ordinary memory, the GET_PARAM values
 * are the RK3576's G52 as Linux Panfrost reported it, and every job
 * "completes" the moment it is submitted without anything running. It drives
 * Panfrost's whole Windows path (screen, compiler, command streams, the shim's
 * marshalling) short of the GPU. Pixels read back are whatever was there. */

struct fake_bo {
   void *cpu;
   uint64_t size, va;
   uint32_t flags;
};
static struct fake_bo *fbo;
static uint32_t fbo_cap, fsync_next = 1;
static uint64_t fva = 0x2000000, fseq, fsubmits;
static SRWLOCK fake_lock = SRWLOCK_INIT;
static int fake = -1;

static int
fake_on(void)
{
   if (fake < 0) {
      const char *e = getenv("MALIKM_FAKE");
      fake = e && *e && *e != '0';
   }
   return fake;
}

/* MALIKM_FAKE=2: also log every call. */
static int
fake_trace(void)
{
   const char *e = getenv("MALIKM_FAKE");
   return e && *e == '2';
}

static struct fake_bo *
fake_bo(uint32_t h)
{
   return h && h <= fbo_cap && fbo[h - 1].cpu ? &fbo[h - 1] : NULL;
}

static int
fake_drm(uint32_t nr, uint8_t *b, uint32_t size)
{
   switch (nr) {
   case MALIKM_NR_PANFROST_GET_PARAM: {
      struct drm_panfrost_get_param *a = (void *)b;
      static const uint64_t v[] = {
         [DRM_PANFROST_PARAM_GPU_PROD_ID] = 0x7402,
         [DRM_PANFROST_PARAM_GPU_REVISION] = 0x1000,
         [DRM_PANFROST_PARAM_SHADER_PRESENT] = 0x7,
         [DRM_PANFROST_PARAM_TILER_PRESENT] = 0x1,
         [DRM_PANFROST_PARAM_L2_PRESENT] = 0x1,
         [DRM_PANFROST_PARAM_AS_PRESENT] = 0xff,
         [DRM_PANFROST_PARAM_JS_PRESENT] = 0x7,
         [DRM_PANFROST_PARAM_L2_FEATURES] = 0x07120206,
         [DRM_PANFROST_PARAM_CORE_FEATURES] = 0x2,
         [DRM_PANFROST_PARAM_TILER_FEATURES] = 0x209,
         [DRM_PANFROST_PARAM_MEM_FEATURES] = 0x1,
         [DRM_PANFROST_PARAM_MMU_FEATURES] = 0x2823,
         [DRM_PANFROST_PARAM_NR_CORE_GROUPS] = 1,
         [DRM_PANFROST_PARAM_SELECTED_COHERENCY] = DRM_PANFROST_GPU_COHERENCY_NONE,
      };
      if (a->param > DRM_PANFROST_PARAM_AFBC_FEATURES &&
          a->param != DRM_PANFROST_PARAM_SELECTED_COHERENCY)
         return -MK_EINVAL;
      a->value = v[a->param];
      return 0;
   }
   case MALIKM_NR_PANFROST_CREATE_BO: {
      struct drm_panfrost_create_bo *a = (void *)b;
      uint64_t sz = ((uint64_t)a->size + 4095) & ~4095ull;
      if (a->flags & PANFROST_BO_HEAP)
         sz = (sz + 0x1fffff) & ~0x1fffffull;
      void *cpu = VirtualAlloc(NULL, (SIZE_T)sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (!cpu)
         return -MK_ENOMEM;
      uint32_t h = 0;
      for (uint32_t i = 0; i < fbo_cap && !h; i++)
         if (!fbo[i].cpu)
            h = i + 1;
      if (!h) {
         uint32_t cap = fbo_cap ? fbo_cap * 2 : 256;
         struct fake_bo *n = realloc(fbo, cap * sizeof(*n));
         if (!n) {
            VirtualFree(cpu, 0, MEM_RELEASE);
            return -MK_ENOMEM;
         }
         memset(n + fbo_cap, 0, (cap - fbo_cap) * sizeof(*n));
         fbo = n;
         h = fbo_cap + 1;
         fbo_cap = cap;
      }
      /* Executable buffers stay inside one 16 MB window, as in the driver. */
      if (!(a->flags & PANFROST_BO_NOEXEC) && (fva >> 24) != ((fva + sz - 1) >> 24))
         fva = (fva + 0xffffff) & ~0xffffffull;
      fbo[h - 1] = (struct fake_bo){cpu, sz, fva, a->flags};
      fva += sz;
      a->handle = h;
      a->offset = fbo[h - 1].va;
      return 0;
   }
   case MALIKM_NR_PANFROST_MMAP_BO: {
      struct drm_panfrost_mmap_bo *a = (void *)b;
      if (!fake_bo(a->handle))
         return -MK_ENOENT;
      a->offset = MALIKM_MMAP_OFFSET(a->handle);
      return 0;
   }
   case MALIKM_NR_PANFROST_GET_BO_OFFSET: {
      struct drm_panfrost_get_bo_offset *a = (void *)b;
      struct fake_bo *bo = fake_bo(a->handle);
      if (!bo)
         return -MK_ENOENT;
      a->offset = bo->va;
      return 0;
   }
   case MALIKM_NR_PANFROST_MADVISE:
      ((struct drm_panfrost_madvise *)b)->retained = 1;
      return 0;
   case MALIKM_NR_PANFROST_WAIT_BO:
      return fake_bo(((struct drm_panfrost_wait_bo *)b)->handle) ? 0 : -MK_ENOENT;
   case MALIKM_NR_PANFROST_SUBMIT: {
      struct drm_panfrost_submit *a = (void *)b;
      const uint32_t *bos = (const uint32_t *)(b + a->bo_handles);
      for (uint32_t i = 0; i < a->bo_handle_count; i++)
         if (!fake_bo(bos[i]))
            return -MK_ENOENT;
      if (!a->jc)
         return -MK_EINVAL;
      fseq++;
      if (++fsubmits <= 16 || (fsubmits & 255) == 0)
         fprintf(stderr, "malikm-fake: submit %llu jc=0x%llx reqs=%u bos=%u in=%u out=%u\n",
                 (unsigned long long)fsubmits, (unsigned long long)a->jc, a->requirements,
                 a->bo_handle_count, a->in_sync_count, a->out_sync);
      return 0;
   }
   case MALIKM_NR_GEM_CLOSE: {
      struct fake_bo *bo = fake_bo(((struct drm_gem_close *)b)->handle);
      if (!bo)
         return -MK_EINVAL;
      VirtualFree(bo->cpu, 0, MEM_RELEASE);
      bo->cpu = NULL;
      return 0;
   }
   case MALIKM_NR_SYNCOBJ_CREATE:
      ((struct drm_syncobj_create *)b)->handle = fsync_next++;
      return 0;
   case MALIKM_NR_SYNCOBJ_WAIT:
      ((struct drm_syncobj_wait *)b)->first_signaled = 0;
      return 0;
   case MALIKM_NR_SYNCOBJ_DESTROY:
   case MALIKM_NR_SYNCOBJ_RESET:
   case MALIKM_NR_SYNCOBJ_SIGNAL:
      return 0;
   case MALIKM_NR_SYNCOBJ_EXPORT_SEQ:
      ((MALIKM_SYNCOBJ_SEQ *)b)->Seq = fseq;
      return 0;
   case MALIKM_NR_SYNCOBJ_IMPORT_SEQ:
      return 0;
   default:
      (void)size;
      return -MK_ENOSYS;
   }
}

static void
fake_ioctl(DWORD code, void *buf, DWORD len)
{
   MALIKM_DRM_HEADER *h = buf;
   (void)len;

   AcquireSRWLockExclusive(&fake_lock);
   if (fake_trace())
      fprintf(stderr, "malikm-fake: ioctl 0x%lx nr 0x%x size %u\n", (unsigned long)code,
              code == IOCTL_MALIKM_DRM ? h->Nr : 0, code == IOCTL_MALIKM_DRM ? h->Size : 0);
   switch (code) {
   case IOCTL_MALIKM_VERSION: {
      MALIKM_VERSION *v = buf;
      memset(v, 0, sizeof(*v));
      v->Major = MALIKM_DRM_MAJOR;
      v->Minor = MALIKM_DRM_MINOR;
      strcpy(v->Name, "panfrost");
      break;
   }
   case IOCTL_MALIKM_MAP: {
      MALIKM_MAP *m = (MALIKM_MAP *)(h + 1);
      struct fake_bo *bo = fake_bo(MALIKM_MMAP_HANDLE(m->Offset));
      h->Result = bo && m->Size <= bo->size ? 0 : -MK_EINVAL;
      if (bo)
         m->Address = (uint64_t)(uintptr_t)bo->cpu;
      break;
   }
   case IOCTL_MALIKM_UNMAP:
      h->Result = 0;
      break;
   case IOCTL_MALIKM_DRM:
      h->Result = fake_drm(h->Nr, (uint8_t *)(h + 1), h->Size);
      break;
   default:
      h->Result = -MK_ENOSYS;
      break;
   }
   if (fake_trace() && code != IOCTL_MALIKM_VERSION)
      fprintf(stderr, "malikm-fake:   -> %d\n", h->Result);
   ReleaseSRWLockExclusive(&fake_lock);
}
