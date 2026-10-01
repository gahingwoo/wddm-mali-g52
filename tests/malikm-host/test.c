// SPDX-License-Identifier: GPL-2.0
/*
 * malikm on the host: drivers/malikm/gpu.c and uapi.c built against a mock
 * WDK (mock/), driving a simulated G52 (sim.c) that walks the driver's own
 * page tables and runs WRITE_VALUE jobs. Run under ASan/UBSan, so a buffer
 * freed while a job still uses it, a leak at file cleanup or a wrong PTE
 * shows up here rather than as a bugcheck on the board.
 */
#include <stdio.h>
#include "mock/ntddk.h"
#include "../../drivers/malikm/malikm.h"
#include "sim.h"

static DEVICE_CONTEXT dev;
static FILE_CONTEXT file;
static int fails, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fails++;                                                           \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);             \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* As the shim marshals: the struct, then its arrays, offsets in the struct. */
static int drm(ULONG nr, void *arg, size_t size, const void *extra, size_t elen)
{
    unsigned char *buf = calloc(1, size + elen + 8);
    memcpy(buf, arg, size);
    if (elen)
        memcpy(buf + size, extra, elen);
    int r = MkDrmIoctl(&dev, &file, nr, buf, size + elen);
    memcpy(arg, buf, size);
    free(buf);
    return r;
}

static int create_bo(ULONG size, ULONG flags, mk_u32 *h, mk_u64 *va)
{
    MK_CREATE_BO c = {size, flags, 0, 0, 0};
    int r = drm(MALIKM_NR_PANFROST_CREATE_BO, &c, sizeof(c), NULL, 0);
    *h = c.handle;
    *va = c.offset;
    return r;
}

static volatile ULONG *map_bo(mk_u32 h, mk_u64 size)
{
    MK_MMAP_BO m = {h, 0, 0};
    if (drm(MALIKM_NR_PANFROST_MMAP_BO, &m, sizeof(m), NULL, 0))
        return NULL;
    MALIKM_MAP map = {m.offset, size, 0};
    if (MkMap(&dev, &file, &map))
        return NULL;
    return (volatile ULONG *)(uintptr_t)map.Address;
}

static void write_job(volatile ULONG *job, mk_u64 target, ULONG value)
{
    memset((void *)job, 0, 64);
    job[4] = 1u | (2u << 1) | (1u << 16);
    job[8] = (ULONG)target;
    job[9] = (ULONG)(target >> 32);
    job[10] = 6;
    job[12] = value;
}

static int submit(mk_u64 jc, const mk_u32 *bos, ULONG nbos, mk_u32 out, ULONG reqs)
{
    MK_SUBMIT s = {0};
    s.jc = jc;
    s.bo_handles = sizeof(s);
    s.bo_handle_count = nbos;
    s.out_sync = out;
    s.requirements = reqs;
    return drm(MALIKM_NR_PANFROST_SUBMIT, &s, sizeof(s), bos, nbos * 4);
}

static int sync_wait(mk_u32 h, mk_s64 timeout_ns, ULONG flags)
{
    MK_SYNCOBJ_WAIT w = {0};
    w.handles = sizeof(w);
    w.count_handles = 1;
    w.flags = flags;
    w.timeout_nsec = timeout_ns;
    return drm(MALIKM_NR_SYNCOBJ_WAIT, &w, sizeof(w), &h, 4);
}

static mk_u32 sync_create(ULONG flags)
{
    MK_SYNCOBJ_CREATE c = {0, flags};
    drm(MALIKM_NR_SYNCOBJ_CREATE, &c, sizeof(c), NULL, 0);
    return c.handle;
}

static int gem_close(mk_u32 h)
{
    MK_GEM_CLOSE c = {h, 0};
    return drm(MALIKM_NR_GEM_CLOSE, &c, sizeof(c), NULL, 0);
}

static mk_u64 param(ULONG p, int *r)
{
    MK_GET_PARAM g = {p, 0, 0};
    *r = drm(MALIKM_NR_PANFROST_GET_PARAM, &g, sizeof(g), NULL, 0);
    return g.value;
}

#define SEC 1000000000LL

int main(void)
{
    int r;

    /* What EvtDeviceAdd and PrepareHardware set up. */
    sim_reset();
    dev.Gpu = sim_regs;
    dev.GpuLength = sizeof(sim_regs);
    ExInitializeFastMutex(&dev.Lock);
    KeInitializeSpinLock(&dev.QueueLock);
    InitializeListHead(&dev.Queue);
    InitializeListHead(&dev.Zombies);
    KeInitializeEvent(&dev.QueueEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&dev.JobIrqEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&dev.ProgressEvent, NotificationEvent, FALSE);
    InitializeListHead(&file.Mappings);
    CHECK(MkMmuInit(&dev) == STATUS_SUCCESS, "MmuInit");
    CHECK(MkGpuInit(&dev) == STATUS_SUCCESS, "GpuInit");
    CHECK(dev.Ready, "not ready");
    CHECK(MkWorkerStart(&dev) == STATUS_SUCCESS, "WorkerStart");
    CHECK(sim.as_updates == 1, "AS0 programmed %u times", sim.as_updates);

    /* GET_PARAM, as Linux reports this G52. */
    CHECK(param(0, &r) == 0x7402 && r == 0, "prod id");
    CHECK(param(1, &r) == 0x1000, "revision");
    CHECK(param(2, &r) == 0x7, "shader present");
    CHECK(param(12, &r) == 0x2823, "mmu features");
    CHECK(param(38, &r) == 1, "core groups");
    param(41, &r);
    CHECK(r == -MK_EINVAL, "timestamp is past 1.1: %d", r);

    /* One BO, one WRITE_VALUE job on each slot. */
    mk_u32 bo, s = sync_create(0);
    mk_u64 va;
    CHECK(create_bo(4096, 0, &bo, &va) == 0, "create_bo");
    CHECK(va >= MK_VA_START && va < MK_VA_END, "va 0x%llx", (unsigned long long)va);
    volatile ULONG *cpu = map_bo(bo, 4096);
    CHECK(cpu != NULL, "map");
    for (ULONG reqs = 0; reqs < 2; reqs++) {
        write_job(cpu, va + 0x100, 0xC0FFEE00 | reqs);
        CHECK(submit(va, &bo, 1, s, reqs) == 0, "submit");
        CHECK(sync_wait(s, SEC, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) == 0, "wait");
        CHECK(cpu[0x40] == (0xC0FFEE00 | reqs), "value 0x%x", cpu[0x40]);
        CHECK(cpu[0] == 1, "exception status %u", cpu[0]);
    }
    CHECK(sim.jobs[0] == 1 && sim.jobs[1] == 1, "slots %u %u (FS must go to 0)", sim.jobs[0], sim.jobs[1]);

    MK_WAIT_BO wb = {bo, 0, 0};
    CHECK(drm(MALIKM_NR_PANFROST_WAIT_BO, &wb, sizeof(wb), NULL, 0) == 0, "WAIT_BO idle");

    /* 1000 jobs back to back. */
    int bad = 0;
    for (ULONG i = 0; i < 1000; i++) {
        write_job(cpu, va + 0x100, i);
        if (submit(va, &bo, 1, s, 0) || sync_wait(s, SEC, 1) || cpu[0x40] != i)
            bad++;
    }
    CHECK(bad == 0, "%d of 1000 jobs wrong", bad);

    /* Executable BOs stay inside one 16 MB window; NOEXEC ones get XN. */
    for (int i = 0; i < 24; i++) {
        mk_u32 h;
        mk_u64 v;
        CHECK(create_bo(3 << 20, 0, &h, &v) == 0, "exec bo %d", i);
        CHECK((v >> 24) == ((v + (3 << 20) - 1) >> 24), "exec bo at 0x%llx crosses 16 MB",
              (unsigned long long)v);
        ULONG64 leaf = 0;
        CHECK(sim_translate(v, 0, &leaf) && !(leaf & MK_PTE_XN), "exec leaf 0x%llx", (unsigned long long)leaf);
    }
    {
        mk_u32 h;
        mk_u64 v;
        ULONG64 leaf = 0;
        CHECK(create_bo(8192, PANFROST_BO_NOEXEC, &h, &v) == 0, "noexec");
        CHECK(sim_translate(v + 4096, 1, &leaf) && (leaf & MK_PTE_XN), "noexec leaf 0x%llx",
              (unsigned long long)leaf);
    }

    /* Heap BOs: must be NOEXEC, rounded to 2 MB, not mappable. */
    {
        mk_u32 h;
        mk_u64 v;
        CHECK(create_bo(5000, PANFROST_BO_HEAP, &h, &v) == -MK_EINVAL, "heap without noexec");
        CHECK(create_bo(5000, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, &h, &v) == 0, "heap");
        CHECK(sim_translate(v + 0x1FF000, 1, NULL) != NULL, "heap rounded to 2 MB");
        MK_MMAP_BO m = {h, 0, 0};
        CHECK(drm(MALIKM_NR_PANFROST_MMAP_BO, &m, sizeof(m), NULL, 0) == -MK_EINVAL, "heap mmap");
    }

    /* A BO closed while a queued job still uses it lives until the job ran:
     * the job ahead of it is slow, so the second is still queued at close. */
    {
        mk_u32 x, sx = sync_create(0);
        mk_u64 vx;
        CHECK(create_bo(4096, 0, &x, &vx) == 0, "zombie bo");
        volatile ULONG *cx = map_bo(x, 4096);
        write_job(cx, vx + 0x100, 0x5A5A5A5A);
        sim_delay_us = 50000;
        write_job(cpu, va + 0x100, 1);
        CHECK(submit(va, &bo, 1, 0, 0) == 0, "slow job");
        CHECK(submit(vx, &x, 1, sx, 0) == 0, "zombie job");
        CHECK(MkUnmap(&dev, &file, (PVOID)cx) == 0, "unmap");
        CHECK(gem_close(x) == 0, "close while queued");
        CHECK(!IsListEmpty(&dev.Zombies), "BO freed while a queued job uses it");
        CHECK(sync_wait(sx, 2 * SEC, 1) == 0, "zombie job wait");
        sim_delay_us = 0;
        CHECK(IsListEmpty(&dev.Zombies), "zombie never reaped");
    }

    /* A job that faults: the driver times out, resets, and carries on. */
    {
        mk_u32 sf = sync_create(0);
        write_job(cpu, 0xF0000000ull, 7);       /* nothing mapped there */
        CHECK(submit(va, &bo, 1, sf, 0) == 0, "faulting submit");
        CHECK(sync_wait(sf, 5 * SEC, 1) == 0, "faulting job completes");
        CHECK(dev.JobsTimedOut == 1 && dev.Resets == 1, "timed out %u resets %u",
              dev.JobsTimedOut, dev.Resets);
        CHECK(sim.faults >= 1, "no MMU fault seen");
        write_job(cpu, va + 0x100, 0xAB);
        CHECK(submit(va, &bo, 1, sf, 0) == 0 && sync_wait(sf, SEC, 1) == 0 && cpu[0x40] == 0xAB,
              "job after reset: 0x%x", cpu[0x40]);
    }

    /* Syncobj semantics, as drm_syncobj.c. */
    {
        mk_u32 a = sync_create(0), b = sync_create(DRM_SYNCOBJ_CREATE_SIGNALED);
        CHECK(sync_wait(a, 0, 1) == -MK_EINVAL, "no fence, no WAIT_FOR_SUBMIT");
        CHECK(sync_wait(a, 0, 1 | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) == -MK_ETIME, "wait for submit, poll");
        CHECK(sync_wait(b, 0, 1) == 0, "created signalled");
        MK_SYNCOBJ_ARRAY arr = {sizeof(arr), 1, 0};
        CHECK(drm(MALIKM_NR_SYNCOBJ_SIGNAL, &arr, sizeof(arr), &a, 4) == 0 && sync_wait(a, 0, 1) == 0, "signal");
        CHECK(drm(MALIKM_NR_SYNCOBJ_RESET, &arr, sizeof(arr), &a, 4) == 0 && sync_wait(a, 0, 1) == -MK_EINVAL, "reset");
        MALIKM_SYNCOBJ_SEQ q = {a, 0, 0};
        CHECK(drm(MALIKM_NR_SYNCOBJ_EXPORT_SEQ, &q, sizeof(q), NULL, 0) == -MK_EINVAL, "export without fence");
        q.Seq = MALIKM_SEQ_LATEST;
        CHECK(drm(MALIKM_NR_SYNCOBJ_IMPORT_SEQ, &q, sizeof(q), NULL, 0) == 0, "import latest");
        CHECK(drm(MALIKM_NR_SYNCOBJ_EXPORT_SEQ, &q, sizeof(q), NULL, 0) == 0 &&
              q.Seq == (ULONG64)dev.SubmittedSeq, "export %llu", (unsigned long long)q.Seq);
        CHECK(sync_wait(a, SEC, 1) == 0, "imported fence signalled");
    }

    /* Bad input. */
    {
        mk_u32 none = 999;
        CHECK(submit(va, &none, 1, 0, 0) == -MK_ENOENT, "unknown BO");
        CHECK(submit(0, &bo, 1, 0, 0) == -MK_EINVAL, "jc 0");
        CHECK(gem_close(999) == -MK_EINVAL, "close unknown");
        MK_SUBMIT sb = {0};
        sb.jc = va;
        sb.bo_handle_count = 4;
        sb.bo_handles = sizeof(sb);            /* but no array sent */
        CHECK(drm(MALIKM_NR_PANFROST_SUBMIT, &sb, sizeof(sb), NULL, 0) == -MK_EFAULT, "array outside the buffer");
        CHECK(drm(0x77, &sb, sizeof(sb), NULL, 0) == -MK_ENOSYS, "unknown ioctl");
    }

    /* The process exits with BOs, a mapping and syncobjs still open. */
    MkFileCleanup(&dev, &file);
    MkWorkerStop(&dev);
    CHECK(IsListEmpty(&dev.Zombies), "zombies left after cleanup and idle");
    MkGpuStop(&dev);
    MkMmuFree(&dev);

    CHECK(sim.bad_config == 0, "bad JS config/affinity %u", sim.bad_config);
    CHECK(sim.flush_without_lock == 0, "FLUSH_PT without LOCK %u", sim.flush_without_lock);
    CHECK(sim.started_while_locked == 0, "job started with AS locked %u", sim.started_while_locked);
    CHECK(sim.started_unpowered == 0, "job started unpowered %u", sim.started_unpowered);

    printf("%d checks, %d failed; jobs %u/%u, writes %u, MMU flushes %u, resets %u -> %s\n",
           checks, fails, sim.jobs[0], sim.jobs[1], sim.writes, sim.flushes, sim.resets,
           fails ? "FAIL" : "PASS");
    return fails != 0;
}
