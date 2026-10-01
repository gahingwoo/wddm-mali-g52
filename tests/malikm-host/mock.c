// SPDX-License-Identifier: GPL-2.0
/* The WDK functions mock/ntddk.h declares, on pthreads and the C heap. */
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include "mock/ntddk.h"

void *MockThreadType;

void KeInitializeEvent(PKEVENT e, int type, BOOLEAN state)
{
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->signalled = state;
    e->autoreset = type == SynchronizationEvent;
}

LONG KeSetEvent(PKEVENT e, LONG incr, BOOLEAN wait)
{
    (void)incr; (void)wait;
    pthread_mutex_lock(&e->m);
    LONG old = e->signalled;
    e->signalled = 1;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
    return old;
}

void KeClearEvent(PKEVENT e)
{
    pthread_mutex_lock(&e->m);
    e->signalled = 0;
    pthread_mutex_unlock(&e->m);
}

NTSTATUS KeWaitForSingleObject(PVOID obj, int reason, int mode, BOOLEAN alertable, PLARGE_INTEGER timeout)
{
    MOCK_OBJ *o = obj;
    struct timespec dl;
    NTSTATUS st = STATUS_SUCCESS;
    (void)reason; (void)mode; (void)alertable;

    if (timeout) {
        /* Relative only (negative, 100 ns units), as the driver uses. */
        int64_t ns = -timeout->QuadPart * 100;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += ns / 1000000000;
        dl.tv_nsec += ns % 1000000000;
        if (dl.tv_nsec >= 1000000000) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000;
        }
    }
    pthread_mutex_lock(&o->m);
    while (!o->signalled) {
        int r = timeout ? pthread_cond_timedwait(&o->c, &o->m, &dl) : pthread_cond_wait(&o->c, &o->m);
        if (r == ETIMEDOUT) {
            st = STATUS_TIMEOUT;
            break;
        }
    }
    if (st == STATUS_SUCCESS && o->autoreset)
        o->signalled = 0;
    pthread_mutex_unlock(&o->m);
    return st;
}

ULONG64 KeQueryInterruptTime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (ULONG64)t.tv_sec * 10000000ull + (ULONG64)t.tv_nsec / 100;
}

/* A thread is an object that becomes signalled when its routine returns. */
struct mock_thread {
    MOCK_OBJ obj;
    pthread_t t;
    PKSTART_ROUTINE start;
    PVOID ctx;
};
static __thread struct mock_thread *self;

static void *thread_main(void *arg)
{
    self = arg;
    self->start(self->ctx);
    KeSetEvent(&self->obj, 0, 0);
    return NULL;
}

NTSTATUS PsCreateSystemThread(HANDLE *h, ULONG access, OBJECT_ATTRIBUTES *oa, HANDLE proc,
                              void *cid, PKSTART_ROUTINE start, PVOID ctx)
{
    struct mock_thread *t = calloc(1, sizeof(*t));
    (void)access; (void)oa; (void)proc; (void)cid;
    KeInitializeEvent(&t->obj, NotificationEvent, 0);
    t->start = start;
    t->ctx = ctx;
    if (pthread_create(&t->t, NULL, thread_main, t))
        return STATUS_INSUFFICIENT_RESOURCES;
    *h = t;
    return STATUS_SUCCESS;
}

NTSTATUS PsTerminateSystemThread(NTSTATUS st)
{
    (void)st;
    KeSetEvent(&self->obj, 0, 0);
    pthread_exit(NULL);
}

NTSTATUS ObReferenceObjectByHandle(HANDLE h, ULONG access, void *type, int mode, PVOID *obj, void *info)
{
    (void)access; (void)type; (void)mode; (void)info;
    *obj = h;
    return STATUS_SUCCESS;
}

void ObDereferenceObject(PVOID obj)
{
    struct mock_thread *t = obj;
    pthread_join(t->t, NULL);
    free(t);
}

BOOLEAN PsIsThreadTerminating(PETHREAD t)
{
    (void)t;
    return 0;
}

/* ---- memory: physical address == host address ---- */

PVOID ExAllocatePool2(ULONG flags, SIZE_T size, ULONG tag)
{
    (void)flags; (void)tag;
    return calloc(1, size);
}

void ExFreePoolWithTag(PVOID p, ULONG tag) { (void)tag; free(p); }
void ExFreePool(PVOID p) { free(p); }

PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T n, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi,
                                             PHYSICAL_ADDRESS b, MEMORY_CACHING_TYPE c)
{
    (void)lo; (void)hi; (void)b; (void)c;
    void *p = aligned_alloc(PAGE_SIZE, ROUND_TO_PAGES(n));
    if (p)
        memset(p, 0xA5, n);   /* not zeroed, like the real one */
    return p;
}

void MmFreeContiguousMemorySpecifyCache(PVOID p, SIZE_T n, MEMORY_CACHING_TYPE c)
{
    (void)n; (void)c;
    free(p);
}

PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID p)
{
    PHYSICAL_ADDRESS pa = {(int64_t)(uintptr_t)p};
    return pa;
}

PMDL MmAllocatePagesForMdlEx(PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS skip,
                             SIZE_T n, MEMORY_CACHING_TYPE c, ULONG flags)
{
    SIZE_T pages = ROUND_TO_PAGES(n) / PAGE_SIZE;
    PMDL m = calloc(1, sizeof(*m) + pages * sizeof(PFN_NUMBER));
    (void)lo; (void)hi; (void)skip; (void)c; (void)flags;
    if (!m)
        return NULL;
    m->Base = aligned_alloc(PAGE_SIZE, pages * PAGE_SIZE);
    if (!m->Base) {
        free(m);
        return NULL;
    }
    memset(m->Base, 0, pages * PAGE_SIZE);
    m->ByteCount = pages * PAGE_SIZE;
    for (SIZE_T i = 0; i < pages; i++)
        m->Pfn[i] = ((uintptr_t)m->Base >> PAGE_SHIFT) + i;
    return m;
}

void MmFreePagesFromMdl(PMDL m)
{
    free(m->Base);
    m->Base = NULL;
}

PVOID MmMapLockedPagesSpecifyCache(PMDL m, int mode, MEMORY_CACHING_TYPE c, PVOID base,
                                   ULONG bug, ULONG prio)
{
    (void)mode; (void)c; (void)base; (void)bug; (void)prio;
    return m->Base;
}

void MmUnmapLockedPages(PVOID va, PMDL m) { (void)va; (void)m; }

/* The PMU: all zero reads as "PD_GPU on, bus not idle". */
static ULONG pmu[1024];
PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS pa, SIZE_T n, ULONG prot)
{
    (void)pa; (void)n; (void)prot;
    return pmu;
}
void MmUnmapIoSpace(PVOID va, SIZE_T n) { (void)va; (void)n; }

/* ---- bitmaps ---- */

static int bit(PRTL_BITMAP b, ULONG i) { return (b->Buffer[i / 32] >> (i % 32)) & 1; }

void RtlInitializeBitMap(PRTL_BITMAP b, PULONG buf, ULONG n) { b->Buffer = buf; b->SizeOfBitMap = n; }
void RtlClearAllBits(PRTL_BITMAP b) { memset(b->Buffer, 0, (b->SizeOfBitMap + 31) / 32 * 4); }

void RtlSetBits(PRTL_BITMAP b, ULONG s, ULONG n)
{
    for (ULONG i = s; i < s + n; i++)
        b->Buffer[i / 32] |= 1u << (i % 32);
}

void RtlClearBits(PRTL_BITMAP b, ULONG s, ULONG n)
{
    for (ULONG i = s; i < s + n; i++)
        b->Buffer[i / 32] &= ~(1u << (i % 32));
}

/* First run of N clear bits in [From, To). */
static ULONG find_in(PRTL_BITMAP b, ULONG n, ULONG from, ULONG to)
{
    ULONG run = 0;
    for (ULONG i = from; i < to; i++) {
        run = bit(b, i) ? 0 : run + 1;
        if (run == n)
            return i + 1 - n;
    }
    return 0xFFFFFFFF;
}

/* First run of N clear bits at or after Hint, then from 0, as Windows. */
ULONG RtlFindClearBits(PRTL_BITMAP b, ULONG n, ULONG hint)
{
    ULONG size = b->SizeOfBitMap;
    if (n == 0 || n > size)
        return 0xFFFFFFFF;
    if (hint >= size)
        hint = 0;
    ULONG r = find_in(b, n, hint, size);
    if (r != 0xFFFFFFFF)
        return r;
    ULONG to = hint + n - 1 < size ? hint + n - 1 : size;
    return find_in(b, n, 0, to);
}
