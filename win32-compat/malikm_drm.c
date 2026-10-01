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
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_time.h"
#include "drm-uapi/drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "malikm_ioctl.h"
#include "xf86drm.h"
#include "sys/mman.h"

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

static HANDLE
fd_handle(int fd)
{
   intptr_t h = _get_osfhandle(fd);
   return h == -1 ? INVALID_HANDLE_VALUE : (HANDLE)h;
}

/* Send Buf (a MALIKM_DRM_HEADER and what follows) and return the driver's
 * result, 0 or -1 with errno set. */
static int fake_on(void);
static void fake_ioctl(DWORD code, void *buf, DWORD len);

static int
call(int fd, DWORD code, void *buf, DWORD len)
{
   HANDLE h = fd_handle(fd);
   DWORD got = 0;

   if (fake_on()) {
      fake_ioctl(code, buf, len);
      int r = ((MALIKM_DRM_HEADER *)buf)->Result;
      if (r < 0) {
         errno = crt_errno(-r);
         return -1;
      }
      return 0;
   }
   if (h == INVALID_HANDLE_VALUE) {
      errno = EBADF;
      return -1;
   }
   if (!DeviceIoControl(h, code, buf, len, buf, len, &got, NULL)) {
      errno = GetLastError() == ERROR_INVALID_FUNCTION ? ENOTTY : EIO;
      return -1;
   }
   int r = ((MALIKM_DRM_HEADER *)buf)->Result;
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
   else if (h == INVALID_HANDLE_VALUE ||
            !DeviceIoControl(h, IOCTL_MALIKM_VERSION, &v, sizeof(v), &v, sizeof(v), &got, NULL) ||
            got < sizeof(v)) {
      errno = ENODEV;
      return NULL;
   }
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

   if (call(fd, IOCTL_MALIKM_MAP, &m, sizeof(m)))
      return MAP_FAILED;
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
   ReleaseSRWLockExclusive(&fake_lock);
}
