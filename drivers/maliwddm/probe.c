// SPDX-License-Identifier: GPL-2.0
/*
 * Breadcrumbs that survive a hang and a power cut (docs/WDDM2.md, P1).
 *
 * The first WDDM 2.0 probe hung Windows at the boot spinner, and nothing it
 * wrote survived: the fallback boot rewrote the trace, and the boot-time
 * ETW file was empty after the power cut. So, in probe mode:
 *
 *   - Every DDI wrapper counts its calls in memory (any IRQL, interlocked
 *     only): enters, exits, failures, the sequence number and time of the
 *     last enter. A DDI with more enters than exits is in progress.
 *   - Everything MwLog writes is mirrored to Device Parameters\maliwddm\
 *     Probe<version>, a key the 1.3 fallback boot never writes, and each
 *     value is flushed.
 *   - A watchdog thread wakes every second and writes, to the same key and
 *     flushed: how long it has been running, the last DDI entered, any DDI
 *     in progress for longer than MW_WD_STUCK_MS ("still in <DDI> after N
 *     ms"), the worst such case seen, per-DDI call counts, the adapter's
 *     submission and fence counters, and the paging operations VidMm asked
 *     for (with those this driver refused).
 *   - Optionally (ProbeUart = 1) the watchdog also prints one line a second
 *     to the debug UART, which needs no file system or registry at all.
 *
 * The watchdog runs when WddmProbe is in effect, or on 1.3 with
 * ProbeWatchdog = 1 (both in Services\maliwddm\Parameters, read in
 * DriverEntry by version.c).
 */
#include "maliwddm.h"
#include "wddm2.h"

#define MW_DDI_MAX          96
#define MW_WD_PERIOD_MS     1000
#define MW_WD_STUCK_MS      2000
#define MW_WD_EVERY_S_LATE  10      /* after the first 5 min, write every 10 s
                                     * unless something changed */

static MW_DDI_SLOT *Slots[MW_DDI_MAX];
static volatile LONG SlotCount;
static volatile LONG Seq;
static MW_DDI_SLOT *volatile LastSlot;
static volatile LONG LastSeq;

/* Paging operations VidMm built, by DXGK_BUILDPAGINGBUFFER_OPERATION, and
 * how many of each this driver answered STATUS_NOT_SUPPORTED. */
#define MW_PG_MAX 32
static volatile LONG PagingOps[MW_PG_MAX];
static volatile LONG PagingRefused[MW_PG_MAX];

void MwDdiEnter(MW_DDI_SLOT *S)
{
    LONG q;

    if (S->Registered == 0 && InterlockedCompareExchange(&S->Registered, 1, 0) == 0) {
        LONG i = InterlockedIncrement(&SlotCount) - 1;
        if (i < MW_DDI_MAX)
            Slots[i] = S;
    }
    q = InterlockedIncrement(&Seq);
    S->LastEnterTick = (LONG64)MwTick();
    S->LastEnterSeq = q;
    InterlockedIncrement(&S->Enters);
    LastSlot = S;
    LastSeq = q;
}

NTSTATUS MwDdiExit(MW_DDI_SLOT *S, NTSTATUS St)
{
    S->LastStatus = St;
    if (!NT_SUCCESS(St))
        InterlockedIncrement(&S->Fails);
    InterlockedIncrement(&S->Exits);
    return St;
}

void MwProbePagingOp(ULONG Op, BOOLEAN Refused)
{
    ULONG i = Op < MW_PG_MAX ? Op : MW_PG_MAX - 1;
    InterlockedIncrement(&PagingOps[i]);
    if (Refused)
        InterlockedIncrement(&PagingRefused[i]);
}

/* ---- the probe key ---- */

static WCHAR ProbeKeyName[16];

static BOOLEAN ProbeOn(void)
{
    return MwWddmProbe != 0 || MwProbeWatchdog != 0;
}

static NTSTATUS OpenProbeKey(HANDLE Sub, HANDLE *Key)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;

    if (ProbeKeyName[0] == 0)
        (void)RtlStringCbPrintfW(ProbeKeyName, sizeof(ProbeKeyName), L"Probe%04x",
                                 MwWddmProbe != 0 ? MwWddmProbe : (ULONG)DXGKDDI_WDDMv1_3);
    RtlInitUnicodeString(&name, ProbeKeyName);
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Sub, NULL);
    return ZwCreateKey(Key, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
}

/* From MwLog, with Device Parameters\maliwddm open: the same value again,
 * under Probe<version>, flushed. */
void MwProbeMirror(HANDLE Sub, PUNICODE_STRING Name, ULONG Value)
{
    HANDLE key;

    if (!ProbeOn() || !NT_SUCCESS(OpenProbeKey(Sub, &key)))
        return;
    (void)ZwSetValueKey(key, Name, 0, REG_DWORD, &Value, sizeof(Value));
    (void)ZwFlushKey(key);
    ZwClose(key);
}

/* Open Device Parameters\maliwddm\Probe<version> for the watchdog. */
static NTSTATUS OpenForWatchdog(MW_ADAPTER *A, HANDLE *Key)
{
    HANDLE dev, sub;
    UNICODE_STRING subName = RTL_CONSTANT_STRING(L"maliwddm");
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;

    st = IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &dev);
    if (!NT_SUCCESS(st))
        return st;
    InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, dev, NULL);
    st = ZwCreateKey(&sub, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    ZwClose(dev);
    if (!NT_SUCCESS(st))
        return st;
    st = OpenProbeKey(sub, Key);
    ZwClose(sub);
    return st;
}

static void PutDword(HANDLE Key, PCWSTR Name, ULONG Value)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    (void)ZwSetValueKey(Key, &name, 0, REG_DWORD, &Value, sizeof(Value));
}

static void PutString(HANDLE Key, PCWSTR Name, PCWSTR Value)
{
    UNICODE_STRING name;
    size_t len = 0;
    RtlInitUnicodeString(&name, Name);
    (void)RtlStringCbLengthW(Value, 1024, &len);
    (void)ZwSetValueKey(Key, &name, 0, REG_SZ, (PVOID)Value, (ULONG)(len + sizeof(WCHAR)));
}

/* ---- the debug UART (opt-in) ----
 * RK3576 UART0, a 16550-style DesignWare UART with 32-bit register stride,
 * left at 1500000 8n1 by the firmware. Only THR (0x00) is written and LSR
 * (0x14) read; no Windows driver owns this UART (it is described only in
 * SPCR and DBG2). */
#define UART_THR    0x00
#define UART_LSR    0x14
#define UART_THRE   0x20

static volatile ULONG *Uart;

static void UartPuts(const char *S)
{
    if (Uart == NULL)
        return;
    for (; *S; S++) {
        ULONG spin = 0;
        while ((READ_REGISTER_ULONG((PULONG)((PUCHAR)Uart + UART_LSR)) & UART_THRE) == 0 && ++spin < 100000)
            ;
        if (*S == '\n')
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Uart + UART_THR), '\r');
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Uart + UART_THR), (ULONG)(UCHAR)*S);
    }
}

/* ---- the watchdog ---- */

typedef struct {
    MW_ADAPTER *A;
    KEVENT Stop;
    PVOID Thread;
    /* What was last written, so unchanged values are not written again. */
    LONG Enters[MW_DDI_MAX];
    LONG Fails[MW_DDI_MAX];
    LONG Pg[MW_PG_MAX], PgRefused[MW_PG_MAX];
    ULONG Counters[12];
    ULONG WorstMs;
    BOOLEAN WasStuck;
} MW_WATCHDOG;

static MW_WATCHDOG *Wd;

static ULONG MsSince(MW_ADAPTER *A, LONG64 Tick)
{
    ULONG64 d = MwTick() - (ULONG64)Tick;
    return A->PerfHz != 0 ? (ULONG)(d * 1000 / A->PerfHz) : 0;
}

static void WatchdogTick(MW_WATCHDOG *W, ULONG Seconds)
{
    MW_ADAPTER *A = W->A;
    HANDLE key;
    WCHAR text[512], name[64];
    char line[256];
    BOOLEAN changed = FALSE, stuck = FALSE, write;
    LONG n = SlotCount < MW_DDI_MAX ? SlotCount : MW_DDI_MAX;
    MW_DDI_SLOT *last = LastSlot;
    ULONG counters[12];
    size_t used = 0;

    if (A->PerfHz == 0) {
        LARGE_INTEGER hz;
        (void)KeQueryPerformanceCounter(&hz);
        A->PerfHz = (ULONG64)hz.QuadPart;
    }

    /* What is in progress, and for how long. */
    text[0] = 0;
    for (LONG i = 0; i < n; i++) {
        MW_DDI_SLOT *s = Slots[i];
        LONG inside;
        ULONG ms;
        if (s == NULL)
            continue;
        inside = s->Enters - s->Exits;
        if (inside <= 0)
            continue;
        ms = MsSince(A, s->LastEnterTick);
        if (ms < MW_WD_STUCK_MS)
            continue;
        stuck = TRUE;
        (void)RtlStringCbPrintfExW(text + used, sizeof(text) - used * sizeof(WCHAR), NULL, NULL, 0,
                                   L"%sstill in %ws after %u ms (%d in progress, seq %d)",
                                   used ? L"; " : L"", s->Name, ms, inside, s->LastEnterSeq);
        (void)RtlStringCbLengthW(text, sizeof(text), &used);
        used /= sizeof(WCHAR);
        if (ms > W->WorstMs) {
            W->WorstMs = ms;
            changed = TRUE;
        }
    }

    counters[0] = A->Renders;
    counters[1] = A->Submits;
    counters[2] = A->LastSubmittedFence;
    counters[3] = A->LastCompletedFence;
    counters[4] = A->Gpu.Irqs;
    counters[5] = A->JobsDone;
    counters[6] = A->JobsFailed;
    counters[7] = A->Vsyncs;
    counters[8] = A->Flips;
    counters[9] = A->Presents;
    counters[10] = (ULONG)Seq;
    counters[11] = A->GpuUp;
    if (RtlCompareMemory(counters, W->Counters, sizeof(counters)) != sizeof(counters))
        changed = TRUE;

    write = Seconds <= 300 || (Seconds % MW_WD_EVERY_S_LATE) == 0 || changed || stuck != W->WasStuck;
    if (write && NT_SUCCESS(OpenForWatchdog(A, &key))) {
        static const PCWSTR cname[12] = {
            L"Wd_Renders", L"Wd_Submits", L"Wd_LastSubmittedFence", L"Wd_LastCompletedFence",
            L"Wd_Irqs", L"Wd_JobsDone", L"Wd_JobsFailed", L"Wd_Vsyncs", L"Wd_Flips",
            L"Wd_Presents", L"Wd_Seq", L"Wd_GpuUp",
        };
        WCHAR lastText[160];

        PutDword(key, L"Wd_Seconds", Seconds);
        if (last != NULL) {
            (void)RtlStringCbPrintfW(lastText, sizeof(lastText), L"seq %d: %ws (enters %d, exits %d)",
                                     LastSeq, last->Name, last->Enters, last->Exits);
            PutString(key, L"Wd_Last", lastText);
        }
        PutString(key, L"Wd_Stuck", stuck ? text : L"none");
        if (stuck && W->WorstMs != 0 && changed)
            PutString(key, L"Wd_WorstStuck", text);
        PutDword(key, L"Wd_WorstStuckMs", W->WorstMs);
        for (ULONG i = 0; i < 12; i++)
            if (counters[i] != W->Counters[i] || Seconds == 1)
                PutDword(key, cname[i], counters[i]);
        for (LONG i = 0; i < n; i++) {
            MW_DDI_SLOT *s = Slots[i];
            if (s == NULL)
                continue;
            if (s->Enters != W->Enters[i]) {
                W->Enters[i] = s->Enters;
                if (NT_SUCCESS(RtlStringCbPrintfW(name, sizeof(name), L"Cnt_%ws", s->Name)))
                    PutDword(key, name, (ULONG)s->Enters);
                if (NT_SUCCESS(RtlStringCbPrintfW(name, sizeof(name), L"Seq_%ws", s->Name)))
                    PutDword(key, name, (ULONG)s->LastEnterSeq);
            }
            if (s->Fails != W->Fails[i]) {
                W->Fails[i] = s->Fails;
                if (NT_SUCCESS(RtlStringCbPrintfW(name, sizeof(name), L"Fails_%ws", s->Name)))
                    PutDword(key, name, (ULONG)s->Fails);
                if (NT_SUCCESS(RtlStringCbPrintfW(name, sizeof(name), L"LastFail_%ws", s->Name)))
                    PutDword(key, name, (ULONG)s->LastStatus);
            }
        }
        for (ULONG i = 0; i < MW_PG_MAX; i++) {
            if (PagingOps[i] != W->Pg[i]) {
                W->Pg[i] = PagingOps[i];
                if (NT_SUCCESS(RtlStringCbPrintfW(name, sizeof(name), L"Pg_%u", i)))
                    PutDword(key, name, (ULONG)PagingOps[i]);
            }
            if (PagingRefused[i] != W->PgRefused[i]) {
                W->PgRefused[i] = PagingRefused[i];
                if (NT_SUCCESS(RtlStringCbPrintfW(name, sizeof(name), L"PgRefused_%u", i)))
                    PutDword(key, name, (ULONG)PagingRefused[i]);
            }
        }
        (void)ZwFlushKey(key);
        ZwClose(key);
    }

    if (Uart != NULL && (write || stuck)) {
        (void)RtlStringCbPrintfA(line, sizeof(line),
                                 "mw: t=%us seq=%d last=%ws rend=%u sub=%u fence=%u/%u irq=%u %s\n",
                                 Seconds, LastSeq, last != NULL ? last->Name : L"-",
                                 counters[0], counters[1], counters[2], counters[3], counters[4],
                                 stuck ? "STUCK" : "");
        UartPuts(line);
        if (stuck) {
            char s8[256];
            (void)RtlStringCbPrintfA(s8, sizeof(s8), "mw:   %ws\n", text);
            UartPuts(s8);
        }
    }
    RtlCopyMemory(W->Counters, counters, sizeof(counters));
    W->WasStuck = stuck;
}

static VOID WatchdogThread(PVOID Context)
{
    MW_WATCHDOG *w = (MW_WATCHDOG *)Context;
    LARGE_INTEGER period;
    ULONG seconds = 0;

    period.QuadPart = -10000LL * MW_WD_PERIOD_MS;
    UartPuts("mw: watchdog up\n");
    while (KeWaitForSingleObject(&w->Stop, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT)
        WatchdogTick(w, ++seconds);
    WatchdogTick(w, ++seconds);     /* a last snapshot at StopDevice/RemoveDevice */
    UartPuts("mw: watchdog stopped\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* From AddDevice: before StartDevice, so that a hang inside StartDevice is
 * caught too. */
void MwProbeStart(MW_ADAPTER *A)
{
    HANDLE h;
    MW_WATCHDOG *w;

    if (!ProbeOn() || Wd != NULL)
        return;
    if (MwProbeUart != 0 && Uart == NULL) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = MwProbeUartBase;
        Uart = (volatile ULONG *)MmMapIoSpaceEx(pa, 0x100, PAGE_READWRITE | PAGE_NOCACHE);
    }
    w = (MW_WATCHDOG *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*w), MW_TAG);
    if (w == NULL)
        return;
    w->A = A;
    KeInitializeEvent(&w->Stop, NotificationEvent, FALSE);
    if (!NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, WatchdogThread, w))) {
        ExFreePoolWithTag(w, MW_TAG);
        return;
    }
    (void)ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode, &w->Thread, NULL);
    ZwClose(h);
    Wd = w;
}

/* From RemoveDevice. */
void MwProbeStop(MW_ADAPTER *A)
{
    MW_WATCHDOG *w = Wd;

    if (w == NULL || w->A != A)
        return;
    KeSetEvent(&w->Stop, IO_NO_INCREMENT, FALSE);
    if (w->Thread != NULL) {
        (void)KeWaitForSingleObject(w->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(w->Thread);
    }
    Wd = NULL;
    ExFreePoolWithTag(w, MW_TAG);
    if (Uart != NULL) {
        MmUnmapIoSpace((PVOID)Uart, 0x100);
        Uart = NULL;
    }
}
