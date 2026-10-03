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
#include "drm-uapi/drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "malikm_ioctl.h"
#include "maliwddm_abi.h"
#include "xf86drm.h"
#include "sys/mman.h"

/* The maliwddm backend, below. */
static int wddm_on(void);
static int wddm_init(void);
static void wddm_ioctl(DWORD code, void *buf, DWORD len);

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
         if (trace_on())
            fprintf(stderr, "malikm: no \\\\.\\MaliG52; using the maliwddm adapter\n");
         return _open("NUL", _O_RDWR | _O_NOINHERIT);
      }
      fprintf(stderr, "malikm: cannot open \\\\.\\MaliG52 (error %lu), and no maliwddm adapter\n",
              (unsigned long)err);
      errno = err == ERROR_FILE_NOT_FOUND ? ENOENT : ENODEV;
      return -1;
   }
   int fd = _open("NUL", _O_RDWR | _O_NOINHERIT);
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
         wddm_ioctl(code, buf, len);
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
      wddm_ioctl(IOCTL_MALIKM_VERSION, &v, sizeof(v));
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

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
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
   int sfd = _open("NUL", _O_RDWR | _O_NOINHERIT);
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
w_render(UINT commandLength, UINT allocations)
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

/* Everything submitted so far is done (one in-order queue in the KMD). */
static int
wddm_idle(void)
{
   int ret = 0;

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
   if (ret == 0)
      wdead_flush();
   return ret;
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

   AcquireSRWLockExclusive(&wddm_lock);
   if (!ddi.on || !ddi.dxgi || !ddi.dxgi->pfnPresentCb)
      goto out;
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
   ret = ddi.dxgi->pfnPresentCb(ddi.device, &p) >= 0 ? 0 : -1;
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

   if (!cb || !cb->pfnEscapeCb || !cb->pfnAllocateCb || !cb->pfnRenderCb || !cb->pfnCreateContextCb)
      return -1;
   AcquireSRWLockExclusive(&wddm_lock);
   /* Ask the adapter through the callbacks: a software runtime routes them
    * to d3d10umd's own D3DKMT stubs, which fail. */
   D3DDDICB_ESCAPE e = {0};
   e.hDevice = hRTDevice;
   e.pPrivateDriverData = &g;
   e.PrivateDriverDataSize = sizeof(g);
   g.Code = MW_ESCAPE_GPU_INFO;
   if (cb->pfnEscapeCb(hRTAdapter, &e) >= 0 && g.Count != 0 && g.Valid != 0) {
      D3DDDICB_CREATECONTEXT c = {0};
      c.NodeOrdinal = 0;
      c.EngineAffinity = 1;
      if (cb->pfnCreateContextCb(hRTDevice, &c) >= 0) {
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
   ReleaseSRWLockExclusive(&wddm_lock);
   if (trace_on())
      fprintf(stderr, "malikm: bind to the runtime device %p: %s\n", hRTDevice,
              ret == 0 ? "yes, through its callbacks" : "no (not maliwddm, or software runtime)");
   return ret;
}

static struct wbo *
wddm_bo(uint32_t h)
{
   return h && h <= wbo_cap && wbo[h - 1].h ? &wbo[h - 1] : NULL;
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
      MW_ALLOCATION_INFO pi = {0};
      pi.Version = MW_ABI_VERSION;
      pi.Flags = (a->flags & PANFROST_BO_NOEXEC) ? MW_ALLOC_NOEXEC : 0;
      pi.Size = sz;
      D3DDDI_ALLOCATIONINFO ai = {0};
      ai.pPrivateDriverData = &pi;
      ai.PrivateDriverDataSize = sizeof(pi);
      if (w_create(&ai) < 0)
         return -MK_ENOMEM;
      MW_ESCAPE_ALLOC_INFO_DATA where = {0};
      where.Code = MW_ESCAPE_ALLOC_INFO;
      where.hAllocation = ai.hAllocation;
      if (w_escape(&where, sizeof(where)) < 0 || where.GpuVa == 0) {
         w_destroy(ai.hAllocation);
         return -MK_ENOMEM;
      }
      uint32_t h = 0;
      for (uint32_t i = 0; i < wbo_cap && !h; i++)
         if (!wbo[i].h)
            h = i + 1;
      if (!h) {
         uint32_t cap = wbo_cap ? wbo_cap * 2 : 256;
         struct wbo *n = realloc(wbo, cap * sizeof(*n));
         if (!n)
            return -MK_ENOMEM;
         memset(n + wbo_cap, 0, (cap - wbo_cap) * sizeof(*n));
         wbo = n;
         h = wbo_cap + 1;
         wbo_cap = cap;
      }
      wbo[h - 1] = (struct wbo){ai.hAllocation, where.Size, where.GpuVa, NULL};
      a->handle = h;
      a->offset = where.GpuVa;
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
   case MALIKM_NR_PANFROST_WAIT_BO:
      if (!wddm_bo(((struct drm_panfrost_wait_bo *)b)->handle))
         return -MK_ENOENT;
      return wddm_idle();
   case MALIKM_NR_PANFROST_SUBMIT: {
      struct drm_panfrost_submit *a = (void *)b;
      const uint32_t *bos = (const uint32_t *)(b + a->bo_handles);
      if (!a->jc)
         return -MK_EINVAL;
      if (a->bo_handle_count > walloc_size || sizeof(MW_COMMAND) > wcmd_size)
         return -MK_E2BIG;
      for (uint32_t i = 0; i < a->bo_handle_count; i++) {
         struct wbo *bo = wddm_bo(bos[i]);
         if (!bo)
            return -MK_ENOENT;
         memset(&walloc[i], 0, sizeof(walloc[i]));
         walloc[i].hAllocation = bo->h;
         walloc[i].WriteOperation = 1;
      }
      MW_COMMAND *c = wcmd;
      c->Type = MW_CMD_JOB;
      c->Slot = (a->requirements & PANFROST_JD_REQ_FS) ? 0 : 1;
      c->Jc = a->jc;
      NTSTATUS st = w_render(sizeof(*c), a->bo_handle_count);
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
      if (bo->cpu)
         w_unlock(bo->h);
      /* Not destroyed yet: see wdead. The handle number is free at once. */
      if (wdead_n == wdead_cap) {
         uint32_t cap = wdead_cap ? wdead_cap * 2 : WDEAD_FLUSH;
         D3DKMT_HANDLE *n = realloc(wdead, cap * sizeof(*n));
         if (!n) {
            wddm_idle();
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
         wddm_idle();
      return 0;
   }
   case MALIKM_NR_SYNCOBJ_CREATE: {
      static uint32_t next = 1;
      ((struct drm_syncobj_create *)b)->handle = next++;
      return 0;
   }
   case MALIKM_NR_SYNCOBJ_WAIT:
      ((struct drm_syncobj_wait *)b)->first_signaled = 0;
      return wddm_idle();
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
wddm_ioctl(DWORD code, void *buf, DWORD len)
{
   MALIKM_DRM_HEADER *h = buf;
   (void)len;

   AcquireSRWLockExclusive(&wddm_lock);
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
      if (bo && m->Size <= bo->size) {
         if (!bo->cpu)
            bo->cpu = w_lock(bo->h);
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
