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
 *
 * The boot journal (wddm2x build). The watchdog writes once a second from
 * a thread of its own, which a wedged system may never run again, and its
 * key hangs off Device Parameters, which exists only after AddDevice. So,
 * while a probe is in effect, there is also
 *
 *   Services\maliwddm\Probe\Boot<n % 8>     (n = Parameters\ProbeBoots)
 *     E0001, E0002, ...   REG_SZ, "<ms since DriverEntry> <event>"
 *     Last                the newest event
 *     Count               how many there are
 *   Services\maliwddm\Probe\LastBoot, LastBootKey
 *
 * written from DriverEntry on. An event is "> <ddi>" before a DDI runs,
 * "< <ddi> <status>" after it, and a line of its own for what each QAI was
 * asked and answered. The "> " line is flushed (ZwFlushKey) before the DDI
 * runs, so the last E value on disk names the DDI a hang is in. Only at
 * PASSIVE_LEVEL (the registry), only the first MW_BC_PER_DDI calls of each
 * DDI (Render would flood it) plus every failure, and at most
 * Mw2JournalMax events per boot (wddm2plan.h: ProbeJournalMax, default 1500
 * for a WddmProbe and 400 for the default 2.x boot, which also stops at
 * its confirm; never more than 1500). The 1.3 fallback boot never writes
 * there, and eight boots are kept, so the journal is bounded.
 */
#include "maliwddm.h"
#include "wddm2.h"

#define MW_DDI_MAX          96
#define MW_WD_PERIOD_MS     1000
#define MW_WD_STUCK_MS      2000
#define MW_WD_COUNTERS      20
#define MW_WD_EVERY_S_LATE  10      /* after the first 5 min, write every 10 s
                                     * unless something changed */

/* ---- the boot journal ---- */

#define MW_BC_SLOTS         MW2_JOURNAL_BOOTS
#define MW_BC_PER_DDI       64

static HANDLE BcKey;
static volatile LONG BcCount;
static ULONG BcMax;
static ULONG64 BcStart;

BOOLEAN MwBcOn(void)
{
    return BcKey != NULL;
}

static void BcPutSz(HANDLE Key, PCWSTR Name, PCWSTR Text)
{
    UNICODE_STRING name;
    size_t len = 0;

    RtlInitUnicodeString(&name, Name);
    (void)RtlStringCbLengthW(Text, 2048, &len);
    (void)ZwSetValueKey(Key, &name, 0, REG_SZ, (PVOID)Text, (ULONG)(len + sizeof(WCHAR)));
}

static void BcPutDword(HANDLE Key, PCWSTR Name, ULONG Value)
{
    UNICODE_STRING name;

    RtlInitUnicodeString(&name, Name);
    (void)ZwSetValueKey(Key, &name, 0, REG_DWORD, &Value, sizeof(Value));
}

static void BcWrite(BOOLEAN Flush, PCSTR Text)
{
    WCHAR w[512], vn[16];
    LONG n;
    ULONG ms;

    n = InterlockedIncrement(&BcCount);
    if ((ULONG)n > BcMax) {
        InterlockedDecrement(&BcCount);
        return;
    }
    ms = (ULONG)((KeQueryInterruptTime() - BcStart) / 10000);
    (void)RtlStringCbPrintfW(w, sizeof(w), L"%u %hs%hs", ms, Text,
                             (ULONG)n == BcMax ? " (journal full)" : "");
    (void)RtlStringCbPrintfW(vn, sizeof(vn), L"E%04d", n);
    BcPutSz(BcKey, vn, w);
    BcPutSz(BcKey, L"Last", w);
    BcPutDword(BcKey, L"Count", (ULONG)n);
    if (Flush)
        (void)ZwFlushKey(BcKey);
}

void MwBc(PCSTR Fmt, ...)
{
    char text[400];
    va_list ap;

    if (BcKey == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    va_start(ap, Fmt);
    (void)RtlStringCbVPrintfA(text, sizeof(text), Fmt, ap);
    va_end(ap);
    BcWrite(TRUE, text);
}

/* From version.c's DriverEntry part, with a probe in effect. */
void MwBcInit(PUNICODE_STRING RegistryPath, ULONG BootNumber, ULONG Max)
{
    WCHAR buf[600], slot[16];
    UNICODE_STRING path, name;
    OBJECT_ATTRIBUTES oa;
    HANDLE probe, boot;

    BcStart = KeQueryInterruptTime();
    BcMax = Max;
    if (RegistryPath == NULL || Max == 0)
        return;
    RtlInitEmptyUnicodeString(&path, buf, sizeof(buf));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&path, RegistryPath)) ||
        !NT_SUCCESS(RtlAppendUnicodeToString(&path, L"\\Probe")))
        return;
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&probe, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL)))
        return;
    (void)RtlStringCbPrintfW(slot, sizeof(slot), L"Boot%u", BootNumber % MW_BC_SLOTS);
    RtlInitUnicodeString(&name, slot);
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, probe, NULL);
    /* The boot eight boots ago used this slot: start it empty. */
    if (NT_SUCCESS(ZwOpenKey(&boot, KEY_ALL_ACCESS, &oa))) {
        (void)ZwDeleteKey(boot);
        ZwClose(boot);
    }
    if (NT_SUCCESS(ZwCreateKey(&boot, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        BcPutDword(boot, L"BootNumber", BootNumber);
        BcPutDword(probe, L"LastBoot", BootNumber);
        BcPutSz(probe, L"LastBootKey", slot);
        (void)ZwFlushKey(probe);
        BcKey = boot;
    }
    ZwClose(probe);
}

/* No more events this boot (the default 2.x boot, once confirmed). The key
 * stays open: another thread may be writing to it right now. */
void MwBcEnd(void)
{
    BcMax = 0;
}

void MwBcClose(void)
{
    HANDLE k = BcKey;

    BcKey = NULL;
    if (k != NULL)
        ZwClose(k);
}

/* ---- the ring: events above PASSIVE_LEVEL ---- */

#define MW_RING_SIZE        128
#define MW_RING_PER_CODE    32

typedef struct {
    volatile LONG Seq;          /* 0 = empty or being written */
    ULONG Code;
    ULONG64 Time;               /* KeQueryInterruptTime */
    ULONG64 A, B;
} MW_RING_ENTRY;

static MW_RING_ENTRY Ring[MW_RING_SIZE];
static volatile LONG RingSeq;
static volatile LONG RingPerCode[MW_RING_CODES];
static LONG RingDumped;

void MwRing(ULONG Code, ULONG64 A, ULONG64 B)
{
    MW_RING_ENTRY *e;
    LONG q;

    if (BcKey == NULL || Code >= MW_RING_CODES)
        return;
    if (InterlockedIncrement(&RingPerCode[Code]) > MW_RING_PER_CODE)
        return;
    q = InterlockedIncrement(&RingSeq);
    e = &Ring[q % MW_RING_SIZE];
    InterlockedExchange(&e->Seq, 0);
    e->Code = Code;
    e->Time = KeQueryInterruptTime();
    e->A = A;
    e->B = B;
    InterlockedExchange(&e->Seq, q);
}

/* From the watchdog, at PASSIVE_LEVEL: new ring entries into the journal,
 * at most 32 a second. */
static void RingDump(void)
{
    static const char *const names[MW_RING_CODES] = { "isr", "dpc", "dma-completed", "dma-preempted", "vsync" };
    LONG last = RingSeq;

    for (int n = 0; RingDumped < last && n < 32; n++) {
        LONG q = ++RingDumped;
        MW_RING_ENTRY *e = &Ring[q % MW_RING_SIZE];
        if (e->Seq != q)
            continue;       /* overwritten, or still being written */
        MwBc("ring %d at %u ms: %hs 0x%I64x 0x%I64x", q, (ULONG)((e->Time - BcStart) / 10000),
             names[e->Code], e->A, e->B);
    }
}

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

/* ---- who called: the last DDIs made below DISPATCH_LEVEL ----
 *
 * The live dump of the 2026-11-25 session hang (docs/EDGE-PRESENT.md, "The
 * WDDM 2.0 session hang") had every DDI counter in .data but not one of
 * the threads that made the last calls, so which process opened the last
 * allocation and which one made the screen invisible was guessed from the
 * order alone. Each DDI entered below DISPATCH_LEVEL (the ISR, the DPC and
 * the interrupt-level ones are left out, they would push everything else
 * out) is noted with its caller's process image name, pid and tid. The
 * watchdog writes the last ones to the registry every second (Wd_Recent),
 * and the next boot keeps the last boot's copy (Prev_*), so a hang that
 * ends in a power cut still says who called last. */
#define MW_RECENT           32
#define MW_RECENT_SHOWN     12
#define MW_STALL_S          10u     /* the screen invisible and no DDI for this long */

typedef struct {
    volatile LONG Seq;          /* 0 = empty or being written */
    ULONG Pid, Tid;
    LONG64 Tick;
    PCWSTR Name;
    CHAR Image[16];
} MW_RECENT_ENTRY;

static MW_RECENT_ENTRY Recent[MW_RECENT];
static volatile LONG RecentNext;
static volatile LONG64 RecentTick;      /* the last one's MwTick */

/* Exported by ntoskrnl, not declared in the WDK headers. */
NTKERNELAPI PCHAR PsGetProcessImageFileName(PEPROCESS Process);

static void RecentNote(MW_DDI_SLOT *S, LONG Q)
{
    LONG r = InterlockedIncrement(&RecentNext) - 1;
    MW_RECENT_ENTRY *e = &Recent[(ULONG)r % MW_RECENT];
    PCHAR img = PsGetProcessImageFileName(PsGetCurrentProcess());
    ULONG i;

    InterlockedExchange(&e->Seq, 0);
    e->Name = S->Name;
    e->Pid = HandleToULong(PsGetCurrentProcessId());
    e->Tid = HandleToULong(PsGetCurrentThreadId());
    e->Tick = S->LastEnterTick;
    for (i = 0; i < sizeof(e->Image) - 1 && img != NULL && img[i] != 0; i++)
        e->Image[i] = img[i];
    for (; i < sizeof(e->Image); i++)
        e->Image[i] = 0;
    InterlockedExchange64(&RecentTick, e->Tick);
    InterlockedExchange(&e->Seq, Q);
}

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
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
        RecentNote(S, q);
    /* Flushed before the DDI runs: a hang inside it leaves this line. */
    if (BcKey != NULL && S->Enters <= MW_BC_PER_DDI && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        char t[96];
        (void)RtlStringCbPrintfA(t, sizeof(t), "> %ws #%d seq %d", S->Name, S->Enters, q);
        BcWrite(TRUE, t);
    }
}

NTSTATUS MwDdiExit(MW_DDI_SLOT *S, NTSTATUS St)
{
    S->LastStatus = St;
    if (!NT_SUCCESS(St))
        InterlockedIncrement(&S->Fails);
    InterlockedIncrement(&S->Exits);
    /* Not flushed: the next "> " line or the watchdog flushes it. A failure
     * is flushed at once. */
    if (BcKey != NULL && (S->Enters <= MW_BC_PER_DDI || !NT_SUCCESS(St)) &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        char t[96];
        (void)RtlStringCbPrintfA(t, sizeof(t), "< %ws 0x%08x", S->Name, (ULONG)St);
        BcWrite(!NT_SUCCESS(St), t);
    }
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

static NTSTATUS OpenProbeKey(HANDLE Sub, ACCESS_MASK Access, HANDLE *Key)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;

    if (ProbeKeyName[0] == 0)
        (void)RtlStringCbPrintfW(ProbeKeyName, sizeof(ProbeKeyName), L"Probe%04x",
                                 MwWddmProbe != 0 ? MwWddmProbe : (ULONG)DXGKDDI_WDDMv1_3);
    RtlInitUnicodeString(&name, ProbeKeyName);
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Sub, NULL);
    return ZwCreateKey(Key, Access, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
}

/* From MwLog, with Device Parameters\maliwddm open: the same value again,
 * under Probe<version>, flushed. */
void MwProbeMirror(HANDLE Sub, PUNICODE_STRING Name, ULONG Value)
{
    HANDLE key;

    if (!ProbeOn() || !NT_SUCCESS(OpenProbeKey(Sub, KEY_WRITE, &key)))
        return;
    (void)ZwSetValueKey(key, Name, 0, REG_DWORD, &Value, sizeof(Value));
    (void)ZwFlushKey(key);
    ZwClose(key);
}

/* Open Device Parameters\maliwddm\Probe<version> for the watchdog. */
static NTSTATUS OpenForWatchdogEx(MW_ADAPTER *A, ACCESS_MASK Access, HANDLE *Key)
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
    st = OpenProbeKey(sub, Access, Key);
    ZwClose(sub);
    return st;
}

static NTSTATUS OpenForWatchdog(MW_ADAPTER *A, HANDLE *Key)
{
    return OpenForWatchdogEx(A, KEY_WRITE, Key);
}

/* Before this boot's watchdog writes anything: the last boot's values of
 * the ones below, again as Prev_*, so a boot that ended in a power cut
 * still says, from the next one, what it last saw. PASSIVE_LEVEL. */
static void KeepLastBoot(MW_ADAPTER *A)
{
    static const PCWSTR names[] = {
        L"Wd_Seconds", L"Wd_Recent", L"Wd_Stall", L"Wd_Visible", L"Wd_SinceDdiMs",
        L"Wd_Last", L"Wd_Stuck", L"Wd_Flips", L"Wd_Vsyncs",
        L"Wd_LastSubmittedFence", L"Wd_LastCompletedFence", L"Wd_CopySubmits", L"Wd_CopyFence",
    };
    HANDLE key;
    ULONG size = 4096, got;
    PKEY_VALUE_PARTIAL_INFORMATION v;

    if (!NT_SUCCESS(OpenForWatchdogEx(A, KEY_READ | KEY_WRITE, &key)))
        return;
    v = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, size, MW_TAG);
    if (v != NULL) {
        for (ULONG i = 0; i < RTL_NUMBER_OF(names); i++) {
            UNICODE_STRING from, to;
            WCHAR prev[48];
            RtlInitUnicodeString(&from, names[i]);
            if (!NT_SUCCESS(ZwQueryValueKey(key, &from, KeyValuePartialInformation, v, size, &got)))
                continue;
            if (!NT_SUCCESS(RtlStringCbPrintfW(prev, sizeof(prev), L"Prev_%ws", names[i] + 3)))
                continue;
            RtlInitUnicodeString(&to, prev);
            (void)ZwSetValueKey(key, &to, 0, v->Type, v->Data, v->DataLength);
        }
        ExFreePoolWithTag(v, MW_TAG);
    }
    (void)ZwFlushKey(key);
    ZwClose(key);
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
    (void)RtlStringCbLengthW(Value, 4096, &len);
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
    ULONG Counters[MW_WD_COUNTERS];
    ULONG WorstMs;
    BOOLEAN WasStuck, WasStall;
} MW_WATCHDOG;

static MW_WATCHDOG *Wd;

static ULONG MsSince(MW_ADAPTER *A, LONG64 Tick)
{
    ULONG64 d = MwTick() - (ULONG64)Tick;
    return A->PerfHz != 0 ? (ULONG)(d * 1000 / A->PerfHz) : 0;
}

/* The last MW_RECENT_SHOWN noted DDIs, newest first: "seq name image
 * pid.tid -ms". The watchdog's thread only (one static buffer). */
static WCHAR RecentText[MW_RECENT_SHOWN * 72];

static PCWSTR RecentFormat(MW_ADAPTER *A)
{
    LONG last = RecentNext;
    size_t used = 0;

    RecentText[0] = 0;
    for (LONG n = 0; n < MW_RECENT_SHOWN && last - 1 - n >= 0; n++) {
        const MW_RECENT_ENTRY *e = &Recent[(ULONG)(last - 1 - n) % MW_RECENT];
        LONG seq = e->Seq;
        if (seq == 0 || e->Name == NULL)
            continue;
        (void)RtlStringCbPrintfExW(RecentText + used, sizeof(RecentText) - used * sizeof(WCHAR), NULL, NULL, 0,
                                   L"%s%d %ws %hs %u.%u -%ums", used ? L"; " : L"", seq, e->Name,
                                   e->Image, e->Pid, e->Tid, MsSince(A, e->Tick));
        (void)RtlStringCbLengthW(RecentText, sizeof(RecentText), &used);
        used /= sizeof(WCHAR);
    }
    return RecentText[0] ? RecentText : L"none";
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
    ULONG counters[MW_WD_COUNTERS];
    size_t used = 0;
    BOOLEAN stall;
    static WCHAR stallText[96];

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

    /* The 2026-11-25 hang from here: the screen made invisible, then no
     * DDI below DISPATCH_LEVEL at all (only vsyncs) while every GUI thread
     * waited in win32k. Nothing is outstanding in this driver then, so
     * there is no timeout to detect; it is said, not acted on. */
    stall = !A->SourceVisible && RecentTick != 0 && MsSince(A, RecentTick) >= MW_STALL_S * 1000;
    if (stall)
        (void)RtlStringCbPrintfW(stallText, sizeof(stallText),
                                 L"screen invisible and no DDI for %u s (vsyncs %u, flips %u)",
                                 MsSince(A, RecentTick) / 1000, A->Vsyncs, A->Flips);
    if (stall != W->WasStall)
        changed = TRUE;

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
    /* wddm2x.c: copy-node submits and fence, processes, root page tables
     * set, virtual submits (all, node 0), PTEs written, page-table updates
     * refused. */
    Mw2xCounters(&counters[12], 8);
    if (RtlCompareMemory(counters, W->Counters, sizeof(counters)) != sizeof(counters))
        changed = TRUE;

    if (BcKey != NULL)
        RingDump();
    /* The one-shot fallback's confirm (version.c), at PASSIVE_LEVEL here. */
    MwVersionConfirmCheck(A->Renders, A->Flips + A->Presents, Seconds, stuck, (ULONG)A->TdrCount);
    if (stall && !W->WasStall)
        MwBc("watchdog %us: %ws; last DDIs: %ws", Seconds, stallText, RecentFormat(A));
    if (stuck && !W->WasStuck)
        MwBc("watchdog %us: %ws", Seconds, text);
    else if (!stuck && W->WasStuck)
        MwBc("watchdog %us: no longer stuck", Seconds);
    if (BcKey != NULL && (Seconds % 10) == 0)
        (void)ZwFlushKey(BcKey);

    write = Seconds <= 300 || (Seconds % MW_WD_EVERY_S_LATE) == 0 || changed || stuck != W->WasStuck;
    if (write && NT_SUCCESS(OpenForWatchdog(A, &key))) {
        static const PCWSTR cname[MW_WD_COUNTERS] = {
            L"Wd_Renders", L"Wd_Submits", L"Wd_LastSubmittedFence", L"Wd_LastCompletedFence",
            L"Wd_Irqs", L"Wd_JobsDone", L"Wd_JobsFailed", L"Wd_Vsyncs", L"Wd_Flips",
            L"Wd_Presents", L"Wd_Seq", L"Wd_GpuUp",
            L"Wd_CopySubmits", L"Wd_CopyFence", L"Wd_Processes", L"Wd_RootSets",
            L"Wd_VirtSubmits", L"Wd_VirtSubmits3d", L"Wd_PteWrites", L"Wd_PtRefused",
        };
        WCHAR lastText[160];

        PutDword(key, L"Wd_Seconds", Seconds);
        PutString(key, L"Wd_Recent", RecentFormat(A));
        PutDword(key, L"Wd_Visible", A->SourceVisible);
        PutDword(key, L"Wd_SinceDdiMs", RecentTick != 0 ? MsSince(A, RecentTick) : 0xFFFFFFFFu);
        PutString(key, L"Wd_Stall", stall ? stallText : L"none");
        if (last != NULL) {
            (void)RtlStringCbPrintfW(lastText, sizeof(lastText), L"seq %d: %ws (enters %d, exits %d)",
                                     LastSeq, last->Name, last->Enters, last->Exits);
            PutString(key, L"Wd_Last", lastText);
        }
        PutString(key, L"Wd_Stuck", stuck ? text : L"none");
        if (stuck && W->WorstMs != 0 && changed)
            PutString(key, L"Wd_WorstStuck", text);
        PutDword(key, L"Wd_WorstStuckMs", W->WorstMs);
        for (ULONG i = 0; i < MW_WD_COUNTERS; i++)
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
    if (Uart != NULL && stall && !W->WasStall) {
        char s9[160];
        (void)RtlStringCbPrintfA(s9, sizeof(s9), "mw:   STALL %ws\n", stallText);
        UartPuts(s9);
    }
    RtlCopyMemory(W->Counters, counters, sizeof(counters));
    W->WasStuck = stuck;
    W->WasStall = stall;
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
    KeepLastBoot(A);
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
