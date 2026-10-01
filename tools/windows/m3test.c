// SPDX-License-Identifier: GPL-2.0
/*
 * m3test: milestone M3.1 on the board. Talks to \\.\MaliG52 (drivers/malikm)
 * with raw DeviceIoControl, no Mesa, and runs M1's WRITE_VALUE job through
 * the Panfrost uAPI: create a BO, map it, write the job, submit it with an
 * out-syncobj, wait, read the value back. Once on each job slot, then a
 * batch to time the round trip.
 *
 *   m3test            exit code 0 = PASS
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "malikm_ioctl.h"

typedef struct { mk_u64 jc, in_syncs; mk_u32 in_sync_count, out_sync; mk_u64 bo_handles;
                 mk_u32 bo_handle_count, requirements, jm_ctx_handle, pad; } SUBMIT;
typedef struct { mk_u32 handle, pad; mk_s64 timeout_ns; } WAIT_BO;
typedef struct { mk_u32 size, flags, handle, pad; mk_u64 offset; } CREATE_BO;
typedef struct { mk_u32 handle, flags; mk_u64 offset; } MMAP_BO;
typedef struct { mk_u32 param, pad; mk_u64 value; } GET_PARAM;
typedef struct { mk_u32 handle, pad; } HANDLE_PAD;
typedef struct { mk_u32 handle, flags; } SYNC_CREATE;
typedef struct { mk_u64 handles; mk_s64 timeout_nsec; mk_u32 count_handles, flags, first_signaled, pad;
                 mk_u64 deadline_nsec; } SYNC_WAIT;

static HANDLE dev;

/* One DRM ioctl: header, struct, then Extra bytes of inlined arrays. */
static int drm(mk_u32 nr, void *arg, mk_u32 size, const void *extra, mk_u32 extra_len)
{
    static unsigned char buf[4096];
    MALIKM_DRM_HEADER *h = (MALIKM_DRM_HEADER *)buf;
    DWORD got;

    memset(buf, 0, sizeof(*h) + size + extra_len);
    h->Nr = nr;
    h->Size = size;
    memcpy(h + 1, arg, size);
    if (extra_len)
        memcpy((unsigned char *)(h + 1) + size, extra, extra_len);
    if (!DeviceIoControl(dev, IOCTL_MALIKM_DRM, buf, sizeof(*h) + size + extra_len,
                         buf, sizeof(*h) + size + extra_len, &got, NULL)) {
        printf("  DeviceIoControl(nr 0x%x) failed: %lu\n", nr, GetLastError());
        return -1000;
    }
    memcpy(arg, h + 1, size);
    return h->Result;
}

static mk_u64 param(mk_u32 p)
{
    GET_PARAM g = {p};
    int r = drm(MALIKM_NR_PANFROST_GET_PARAM, &g, sizeof(g), NULL, 0);
    return r ? (mk_u64)-1 : g.value;
}

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* M1's job: a WRITE_VALUE header at +0, its payload at +32. */
static void write_job(volatile uint32_t *job, mk_u64 target, uint32_t value)
{
    memset((void *)job, 0, 64);
    job[4] = 1u | (2u << 1) | (1u << 16);   /* is_64b, WRITE_VALUE, index 1 */
    job[8] = (uint32_t)target;
    job[9] = (uint32_t)(target >> 32);
    job[10] = 6;                             /* immediate 32 */
    job[12] = value;
}

/* Submit the job at Va on the slot Reqs selects and wait for its syncobj. */
static int run(mk_u32 bo, mk_u64 va, mk_u32 reqs, mk_u32 sync)
{
    SUBMIT s = {0};
    SYNC_WAIT w = {0};
    mk_u32 handles[1] = {bo};
    int r;

    s.jc = va;
    s.bo_handle_count = 1;
    s.bo_handles = sizeof(s);               /* offset of the inlined array */
    s.out_sync = sync;
    s.requirements = reqs;
    r = drm(MALIKM_NR_PANFROST_SUBMIT, &s, sizeof(s), handles, sizeof(handles));
    if (r) {
        printf("  SUBMIT: %d\n", r);
        return r;
    }
    w.handles = sizeof(w);
    w.count_handles = 1;
    w.flags = 1;                            /* WAIT_ALL */
    w.timeout_nsec = 2000000000LL;          /* relative, 2 s */
    mk_u32 sh[1] = {sync};
    r = drm(MALIKM_NR_SYNCOBJ_WAIT, &w, sizeof(w), sh, sizeof(sh));
    if (r)
        printf("  SYNCOBJ_WAIT: %d\n", r);
    return r;
}

int main(void)
{
    MALIKM_VERSION v = {0};
    DWORD got;
    int fails = 0;

    dev = CreateFileW(MALIKM_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        printf("FAIL: cannot open \\\\.\\MaliG52 (%lu): is malikm installed and started?\n", GetLastError());
        return 1;
    }
    if (!DeviceIoControl(dev, IOCTL_MALIKM_VERSION, &v, sizeof(v), &v, sizeof(v), &got, NULL)) {
        printf("FAIL: VERSION (%lu)\n", GetLastError());
        return 1;
    }
    printf("driver: %s %d.%d\n", v.Name, v.Major, v.Minor);
    printf("GPU_PROD_ID 0x%llx  REVISION 0x%llx  SHADER_PRESENT 0x%llx  L2_PRESENT 0x%llx\n",
           param(0), param(1), param(2), param(4));
    printf("MMU_FEATURES 0x%llx  THREAD_FEATURES 0x%llx  MAX_THREADS %llu  JS_PRESENT 0x%llx\n",
           param(12), param(13), param(14), param(7));
    if (param(0) != 0x7402) {
        printf("FAIL: not a Mali-G52\n");
        return 1;
    }

    CREATE_BO c = {4096, 0};
    if (drm(MALIKM_NR_PANFROST_CREATE_BO, &c, sizeof(c), NULL, 0)) {
        printf("FAIL: CREATE_BO\n");
        return 1;
    }
    printf("BO handle %u at GPU 0x%llx\n", c.handle, c.offset);

    MMAP_BO m = {c.handle};
    if (drm(MALIKM_NR_PANFROST_MMAP_BO, &m, sizeof(m), NULL, 0)) {
        printf("FAIL: MMAP_BO\n");
        return 1;
    }
    struct { MALIKM_DRM_HEADER h; MALIKM_MAP m; } map = {{0}, {m.offset, 4096, 0}};
    if (!DeviceIoControl(dev, IOCTL_MALIKM_MAP, &map, sizeof(map), &map, sizeof(map), &got, NULL) ||
        map.h.Result) {
        printf("FAIL: MAP (%lu, %d)\n", GetLastError(), map.h.Result);
        return 1;
    }
    volatile uint32_t *cpu = (volatile uint32_t *)(uintptr_t)map.m.Address;
    printf("mapped at %p\n", (void *)cpu);

    SYNC_CREATE sc = {0, 0};
    if (drm(MALIKM_NR_SYNCOBJ_CREATE, &sc, sizeof(sc), NULL, 0)) {
        printf("FAIL: SYNCOBJ_CREATE\n");
        return 1;
    }

    /* Slot 0 (PANFROST_JD_REQ_FS), then slot 1. */
    for (mk_u32 reqs = 0; reqs < 2; reqs++) {
        mk_u32 slot = reqs ? 0 : 1, magic = 0xC0FFEE00u | reqs;
        cpu[0x100 / 4] = 0;
        write_job(cpu, c.offset + 0x100, magic);
        int r = run(c.handle, c.offset, reqs, sc.handle);
        uint32_t got32 = cpu[0x100 / 4];
        int ok = r == 0 && got32 == magic;
        printf("slot %u: result %d, read 0x%08x, want 0x%08x: %s\n", slot, r, got32, magic, ok ? "ok" : "WRONG");
        fails += !ok;
    }

    /* WAIT_BO on an idle BO, as a poll. */
    WAIT_BO wb = {c.handle, 0, 0};
    int r = drm(MALIKM_NR_PANFROST_WAIT_BO, &wb, sizeof(wb), NULL, 0);
    printf("WAIT_BO (poll, idle): %d\n", r);
    fails += r != 0;

    /* Round trip: 1000 jobs back to back, each waited for. */
    double t0 = now_ms();
    int bad = 0;
    for (int i = 0; i < 1000; i++) {
        write_job(cpu, c.offset + 0x100, (uint32_t)i);
        if (run(c.handle, c.offset, 0, sc.handle) || cpu[0x100 / 4] != (uint32_t)i)
            bad++;
    }
    double t1 = now_ms();
    printf("1000 jobs: %.1f ms (%.1f us each), %d wrong\n", t1 - t0, (t1 - t0), bad);
    fails += bad != 0;

    struct { MALIKM_DRM_HEADER h; MALIKM_UNMAP u; } um = {{0}, {map.m.Address}};
    DeviceIoControl(dev, IOCTL_MALIKM_UNMAP, &um, sizeof(um), &um, sizeof(um), &got, NULL);
    HANDLE_PAD gc = {c.handle};
    drm(MALIKM_NR_GEM_CLOSE, &gc, sizeof(gc), NULL, 0);
    HANDLE_PAD sd = {sc.handle};
    drm(MALIKM_NR_SYNCOBJ_DESTROY, &sd, sizeof(sd), NULL, 0);
    CloseHandle(dev);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
