/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Just enough of the WDK for drivers/malikm/gpu.c and uapi.c to build and
 * run as a Linux program (tests/malikm-host). Physical addresses are host
 * addresses, so the simulated GPU (sim.c) can walk the driver's page tables.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <pthread.h>

#define __int32 int
#define __int64 long long

typedef void VOID, *PVOID;
typedef uint8_t UCHAR, *PUCHAR, BOOLEAN;
typedef uint32_t ULONG, *PULONG;
typedef int32_t LONG, NTSTATUS;
typedef uint64_t ULONG64, *PULONG64;
typedef int64_t LONG64;
typedef size_t SIZE_T;
typedef uintptr_t ULONG_PTR, PFN_NUMBER, *PPFN_NUMBER;
typedef const wchar_t *PCWSTR;
typedef uint8_t KIRQL;
typedef void *HANDLE, *PETHREAD, *WDFDEVICE, *WDFINTERRUPT, *WDFKEY;
typedef union { int64_t QuadPart; } PHYSICAL_ADDRESS, LARGE_INTEGER, *PLARGE_INTEGER;
typedef struct { int dummy; } OBJECT_ATTRIBUTES;
typedef struct { const wchar_t *Buffer; } UNICODE_STRING;

#define TRUE 1
#define FALSE 0
#define NTKERNELAPI
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS                  ((NTSTATUS)0)
#define STATUS_TIMEOUT                  ((NTSTATUS)0x102)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009A)
#define STATUS_DEVICE_NOT_READY         ((NTSTATUS)0xC00000A3)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

#define CTL_CODE(t, f, m, a) (((t) << 16) | ((a) << 14) | ((f) << 2) | (m))
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define ROUND_TO_PAGES(s) (((SIZE_T)(s) + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1))
#define PAGE_READWRITE 4
#define PAGE_NOCACHE 0x200
#define POOL_FLAG_NON_PAGED 0x40
#define MM_ALLOCATE_FULLY_REQUIRED 4
typedef enum { MmNonCached, MmCached, MmWriteCombined } MEMORY_CACHING_TYPE;
enum { KernelMode, UserMode };
enum { Executive };
#define NormalPagePriority 16
#define EXCEPTION_EXECUTE_HANDLER 1
#define __try if (1)
#define __except(x) else
#define OBJ_KERNEL_HANDLE 0x200
#define THREAD_ALL_ACCESS 0x1fffff
#define InitializeObjectAttributes(p, n, a, r, s) ((void)(p))
#define DPFLTR_IHVDRIVER_ID 77
#define DPFLTR_ERROR_LEVEL 0
#define PLUGPLAY_REGKEY_DEVICE 1
#define KEY_WRITE 0x20006
#define WDF_NO_OBJECT_ATTRIBUTES NULL
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type, name)
#define _ARM64_BARRIER_SY 0xF
#define __dsb(x) __sync_synchronize()
#define KeMemoryBarrier() __sync_synchronize()
#define CONTAINING_RECORD(a, t, f) ((t *)((char *)(a) - offsetof(t, f)))
#define RtlZeroMemory(d, n) memset((d), 0, (n))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))

/* ---- lists ---- */
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
static inline void InitializeListHead(PLIST_ENTRY h) { h->Flink = h->Blink = h; }
static inline BOOLEAN IsListEmpty(const LIST_ENTRY *h) { return h->Flink == h; }
static inline void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e)
{ e->Flink = h; e->Blink = h->Blink; h->Blink->Flink = e; h->Blink = e; }
static inline BOOLEAN RemoveEntryList(PLIST_ENTRY e)
{ e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink; return e->Flink == e->Blink; }
static inline PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h)
{ PLIST_ENTRY e = h->Flink; RemoveEntryList(e); return e; }

/* ---- atomics ---- */
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement64(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
static inline BOOLEAN _BitScanReverse64(ULONG *idx, ULONG64 x)
{ if (!x) return 0; *idx = 63 - (ULONG)__builtin_clzll(x); return 1; }

/* ---- synchronisation: events and threads share a header so that
 * KeWaitForSingleObject can take either ---- */
enum { NotificationEvent, SynchronizationEvent };
typedef struct _MOCK_OBJ {
    pthread_mutex_t m;
    pthread_cond_t c;
    int signalled, autoreset;
} MOCK_OBJ, KEVENT, *PKEVENT, *PKTHREAD;
typedef pthread_mutex_t FAST_MUTEX, KSPIN_LOCK;
void KeInitializeEvent(PKEVENT e, int type, BOOLEAN state);
LONG KeSetEvent(PKEVENT e, LONG incr, BOOLEAN wait);
void KeClearEvent(PKEVENT e);
NTSTATUS KeWaitForSingleObject(PVOID obj, int reason, int mode, BOOLEAN alertable, PLARGE_INTEGER timeout);
#define ExInitializeFastMutex(m) pthread_mutex_init((m), NULL)
#define ExAcquireFastMutex(m) pthread_mutex_lock(m)
#define ExReleaseFastMutex(m) pthread_mutex_unlock(m)
#define KeInitializeSpinLock(l) pthread_mutex_init((l), NULL)
#define KeAcquireSpinLock(l, irql) (*(irql) = 0, pthread_mutex_lock(l))
#define KeReleaseSpinLock(l, irql) ((void)(irql), pthread_mutex_unlock(l))
static inline KIRQL KeGetCurrentIrql(void) { return PASSIVE_LEVEL; }
ULONG64 KeQueryInterruptTime(void);
static inline void KeStallExecutionProcessor(ULONG us) { (void)us; }

typedef void (*PKSTART_ROUTINE)(PVOID);
extern void *MockThreadType;
#define PsThreadType (&MockThreadType)
NTSTATUS PsCreateSystemThread(HANDLE *h, ULONG access, OBJECT_ATTRIBUTES *oa, HANDLE proc,
                              void *cid, PKSTART_ROUTINE start, PVOID ctx);
NTSTATUS PsTerminateSystemThread(NTSTATUS st);
NTSTATUS ObReferenceObjectByHandle(HANDLE h, ULONG access, void *type, int mode, PVOID *obj, void *info);
void ObDereferenceObject(PVOID obj);
static inline NTSTATUS ZwClose(HANDLE h) { (void)h; return 0; }
static inline PETHREAD PsGetCurrentThread(void) { return NULL; }

/* ---- memory ---- */
typedef struct _MDL {
    SIZE_T ByteCount;
    void *Base;
    PFN_NUMBER Pfn[];
} MDL, *PMDL;
PVOID ExAllocatePool2(ULONG flags, SIZE_T size, ULONG tag);
void ExFreePoolWithTag(PVOID p, ULONG tag);
void ExFreePool(PVOID p);
PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T n, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi,
                                             PHYSICAL_ADDRESS b, MEMORY_CACHING_TYPE c);
void MmFreeContiguousMemorySpecifyCache(PVOID p, SIZE_T n, MEMORY_CACHING_TYPE c);
PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID p);
PMDL MmAllocatePagesForMdlEx(PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS skip,
                             SIZE_T n, MEMORY_CACHING_TYPE c, ULONG flags);
void MmFreePagesFromMdl(PMDL m);
#define MmGetMdlPfnArray(m) ((m)->Pfn)
#define MmGetMdlByteCount(m) ((m)->ByteCount)
PVOID MmMapLockedPagesSpecifyCache(PMDL m, int mode, MEMORY_CACHING_TYPE c, PVOID base,
                                   ULONG bug, ULONG prio);
void MmUnmapLockedPages(PVOID va, PMDL m);
PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS pa, SIZE_T n, ULONG prot);
void MmUnmapIoSpace(PVOID va, SIZE_T n);

/* ---- registers: the simulated GPU sees every access in its window ---- */
ULONG READ_REGISTER_ULONG(volatile ULONG *p);
void WRITE_REGISTER_ULONG(volatile ULONG *p, ULONG v);

/* ---- bitmaps ---- */
typedef struct { ULONG SizeOfBitMap; PULONG Buffer; } RTL_BITMAP, *PRTL_BITMAP;
void RtlInitializeBitMap(PRTL_BITMAP b, PULONG buf, ULONG n);
void RtlClearAllBits(PRTL_BITMAP b);
ULONG RtlFindClearBits(PRTL_BITMAP b, ULONG n, ULONG hint);
void RtlSetBits(PRTL_BITMAP b, ULONG start, ULONG n);
void RtlClearBits(PRTL_BITMAP b, ULONG start, ULONG n);

/* ---- registry and debug output: discarded ---- */
static inline void DbgPrintEx(ULONG id, ULONG lvl, const char *fmt, ...) { (void)id; (void)lvl; (void)fmt; }
static inline void RtlInitUnicodeString(UNICODE_STRING *s, PCWSTR w) { s->Buffer = w; }
static inline NTSTATUS WdfDeviceOpenRegistryKey(WDFDEVICE d, ULONG t, ULONG a, void *attr, WDFKEY *k)
{ (void)d; (void)t; (void)a; (void)attr; (void)k; return STATUS_UNSUCCESSFUL; }
static inline NTSTATUS WdfRegistryAssignULong(WDFKEY k, UNICODE_STRING *n, ULONG v)
{ (void)k; (void)n; (void)v; return 0; }
static inline void WdfRegistryClose(WDFKEY k) { (void)k; }
