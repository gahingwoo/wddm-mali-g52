// SPDX-License-Identifier: GPL-2.0
/*
 * m5test: M5.2 step (b)'s acceptance test. Runs M1's WRITE_VALUE job on the
 * Mali through maliwddm using only the D3DKMT thunks (what a user-mode
 * driver's runtime calls end up as): a device, a context on node 0, one
 * allocation holding the job and its target, a render with one command, and
 * a lock that waits for the GPU before reading the value back.
 *
 * Exit: 0 the GPU wrote the value; 1 no maliwddm adapter; 2 a step failed.
 *
 * The thunks are resolved at run time from win32u (NtGdiDdDDI*, the system
 * calls themselves), falling back to gdi32 (D3DKMT*): WinPE's gdi32 forwards
 * every D3DKMT export to ext-ms-win-dx-d3dkmt-dxcore, and WinPE has no
 * dxcore, so a static gdi32 import never even loaded there
 * (STATUS_DLL_NOT_FOUND in every PE run).
 *
 * m5test perf [seconds], m5test prof [seconds] (DDI times, chains by slot,
 * latencies, clock and temperatures), m5test ring [csv] (the last 128
 * chains), m5test scan [mode] and m5test mem (GPU memory by process) only
 * ask the driver; nothing is submitted.
 *
 * Build: cl /W4 /I include tools\windows\m5test.c
 */
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "maliwddm_abi.h"

typedef NTSTATUS (APIENTRY *KMT_FN)(void *);
static KMT_FN pCreateDevice, pDestroyDevice, pCreateContext, pDestroyContext, pCreateAllocation,
              pDestroyAllocation, pLock, pUnlock, pRender, pEscape, pEnumAdapters2;

static KMT_FN resolve(const char *name)
{
    char nt[96];
    HMODULE w = LoadLibraryA("win32u.dll");
    HMODULE g = LoadLibraryA("gdi32.dll");
    FARPROC f = NULL;

    sprintf_s(nt, sizeof(nt), "NtGdiDdDDI%s", name);
    if (w != NULL)
        f = GetProcAddress(w, nt);
    if (f == NULL && g != NULL) {
        sprintf_s(nt, sizeof(nt), "D3DKMT%s", name);
        f = GetProcAddress(g, nt);
    }
    if (f == NULL)
        printf("cannot resolve %s\n", name);
    return (KMT_FN)(void *)f;
}

static int resolve_all(void)
{
    pCreateDevice = resolve("CreateDevice");
    pDestroyDevice = resolve("DestroyDevice");
    pCreateContext = resolve("CreateContext");
    pDestroyContext = resolve("DestroyContext");
    pCreateAllocation = resolve("CreateAllocation");
    pDestroyAllocation = resolve("DestroyAllocation");
    pLock = resolve("Lock");
    pUnlock = resolve("Unlock");
    pRender = resolve("Render");
    pEscape = resolve("Escape");
    pEnumAdapters2 = resolve("EnumAdapters2");
    return pCreateDevice && pDestroyDevice && pCreateContext && pDestroyContext && pCreateAllocation &&
           pDestroyAllocation && pLock && pUnlock && pRender && pEscape && pEnumAdapters2;
}

#define JOB_BYTES       4096
#define TARGET_OFFSET   1024
#define MAGIC           0xC0FFEE52u

static D3DKMT_HANDLE g_adapter;
static void print_stats(D3DKMT_HANDLE adapter, const char *when);
static void print_path(const MW_ESCAPE_STATS_DATA *s);
static void print_umd(const MW_ESCAPE_STATS_DATA *a, const MW_ESCAPE_STATS_DATA *b, double secs);

static int fail(const char *what, NTSTATUS st)
{
    printf("FAIL: %s: 0x%08lx\n", what, (unsigned long)st);
    if (g_adapter != 0)
        print_stats(g_adapter, "at the failure");
    return 2;
}

/* M1's job: a WRITE_VALUE header at +0, its payload at +32. */
static void write_job(volatile UINT32 *job, UINT64 target, UINT32 value)
{
    memset((void *)job, 0, 64);
    job[4] = 1u | (2u << 1) | (1u << 16);   /* is_64b, WRITE_VALUE, index 1 */
    job[8] = (UINT32)target;
    job[9] = (UINT32)(target >> 32);
    job[10] = 6;                             /* immediate 32 */
    job[12] = value;
}

static NTSTATUS escape(D3DKMT_HANDLE adapter, D3DKMT_HANDLE device, void *data, UINT size)
{
    D3DKMT_ESCAPE e = {0};
    e.hAdapter = adapter;
    e.hDevice = device;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = data;
    e.PrivateDriverDataSize = size;
    return pEscape(&e);
}

/* The adapter that answers our GPU-info escape with a G52's GPU_ID. */
static D3DKMT_HANDLE find_adapter(void)
{
    D3DKMT_ENUMADAPTERS2 en = {0};
    D3DKMT_ADAPTERINFO info[16];

    en.NumAdapters = 16;
    en.pAdapters = info;
    if (pEnumAdapters2(&en) < 0)
        return 0;
    for (ULONG i = 0; i < en.NumAdapters; i++) {
        MW_ESCAPE_GPU_INFO_DATA g = {0};
        g.Code = MW_ESCAPE_GPU_INFO;
        NTSTATUS st = escape(info[i].hAdapter, 0, &g, sizeof(g));
        printf("adapter %lu (luid %08lx:%08lx): escape 0x%08lx, %u params, GPU_ID 0x%llx\n",
               i, info[i].AdapterLuid.HighPart, info[i].AdapterLuid.LowPart, (unsigned long)st,
               g.Count, (unsigned long long)g.Value[0]);
        if (st >= 0 && g.Count != 0)
            return info[i].hAdapter;
    }
    return 0;
}

static void print_stats(D3DKMT_HANDLE adapter, const char *when)
{
    MW_ESCAPE_STATS_DATA s = {0};
    s.Code = MW_ESCAPE_STATS;
    NTSTATUS st = escape(adapter, 0, &s, sizeof(s));
    if (st < 0) {
        printf("stats %s: escape 0x%08lx\n", when, (unsigned long)st);
        return;
    }
    printf("stats %s: gpu %u renders %u presents %u submits %u fence %u/%u queued %u running %u\n"
           "  jobs done %u failed %u timedout %u js 0x%x fault 0x%x @0x%llx irqs %u resets %u\n",
           when, s.GpuUp, s.Renders, s.Presents, s.Submits, s.LastCompletedFence, s.LastSubmittedFence,
           s.Queued, s.Running, s.JobsDone, s.JobsFailed, s.JobsTimedOut, s.LastJsStatus,
           s.LastFaultStatus, (unsigned long long)s.LastFaultAddress, s.Irqs, s.Resets);
    printf("  allocations %u (last at 0x%llx); aperture maps %u (fails %u, last 0x%llx x%u pages, 0x%08x), unmaps %u\n"
           "  paging ops by type:", s.Allocations, (unsigned long long)s.LastAllocVa, s.Maps, s.MapFails,
           (unsigned long long)s.LastMapVa, s.LastMapPages, s.LastMapStatus, s.Unmaps);
    for (int i = 0; i < 16; i++)
        if (s.PagingOps[i] != 0)
            printf(" [%d]=%u", i, s.PagingOps[i]);
    printf("\n  patches %u: last with %u allocations, %u patch locations; allocation 0 in segment %u at 0x%llx\n",
           s.Patches, s.PatchAllocations, s.PatchPatchLocations, s.PatchSegment0,
           (unsigned long long)s.PatchAddress0);
    printf("  presents: %u blts, %u fills, %u skipped\n", s.PresentBlts, s.PresentFills, s.PresentSkips);
    printf("  display: %u vsyncs, %u flips, %u copied to the framebuffer, %u not\n",
           s.Vsyncs, s.Flips, s.FlipCopies, s.FlipCopySkips);
    if (s.PerfHz != 0)
        printf("  longest since start, us: flip copy %u, flip to shown %u, job %u, between vsyncs %u;"
               " %u flips came with jobs outstanding\n",
               s.FlipCopyMaxUs, s.FlipWaitMaxUs, s.JobMaxUs, s.VsyncGapMaxUs, s.FlipsWhileBusy);
    printf("  preemption: TimeSlice %u ms, requests %u, soft-stops %u (stopped %u), resumed %u,"
           " unexpected %u\n", s.TimeSliceMs, s.PreemptRequests, s.SoftStops, s.StopsTaken, s.Resumes,
           s.ResumeMismatch + s.ResumeLost + s.StopBadTail + s.StopUnasked);
    print_path(&s);
    printf("  KMD blt presents: %.1f MB copied on the CPU since the start\n",
           (double)s.PresentBltBytes / 1e6);
    print_umd(NULL, &s, 0);
}

static const char *path_name(unsigned p)
{
    switch (p) {
    case 1: return "scanned out by the VOP2";
    case 2: return "copied to the framebuffer";
    case 3: return "copied and turned";
    default: return "none yet";
    }
}

/* How flips reach the screen, since the start (0 from an older driver). */
static void print_path(const MW_ESCAPE_STATS_DATA *s)
{
    printf("  present path: last flip %s; scan-out state 0x%x (1 on, 2 translating, 4 a primary shown,"
           " 8 set up, 16 ScanOut setting on)\n", path_name(s->LastPath), s->ScanState);
    printf("  flips scanned out %u (copies avoided, %.1f MiB), copied %u (turned %u); left to the copy"
           " because: scan-out off %u, no VOP2 address %u, needs turning %u, display invisible %u\n",
           s->FlipsScanned, (double)s->CopyBytesAvoided / (1024.0 * 1024.0), s->FlipCopies,
           s->FlipsRotCopied, s->WhyScanOff, s->WhyNotMapped, s->WhyRotated, s->WhyInvisible);
    printf("  IOMMU faults %u (last at 0x%08x), scan-out turned off by one %u times\n", s->ScanFaults,
           s->ScanFaultAddr, s->ScanAutoOff);
}

static const char *const umd_names[MW_UMD_KINDS] = {
    "copy on the GPU", "copy on the CPU", "buffer copy on the CPU", "upload (UpdateSubresource)",
    "map for reading", "map for writing", "present, read back", "present, no copy",
    "wait for GPU idle", "Blt (not carried out)", "resolve (not carried out)", "submit (pfnRenderCb)",
};

/* The UMDs' counts between two readings (or since the start, A NULL):
 * calls, MB, and microseconds a call in the UMD, by kind; then the
 * processes that reported, by name. */
static void print_umd(const MW_ESCAPE_STATS_DATA *a, const MW_ESCAPE_STATS_DATA *b, double secs)
{
    unsigned long long hz = b->PerfHz;

    if (b->UmdReports == 0) {
        printf("  UMD: no reports (a maliumd.dll from before MW_ESCAPE_UMD, or no D3D process)\n");
        return;
    }
    printf("  UMD (all processes%s; switches 0x%x: 1 GPU copies allowed, 2 one-copy read-back):\n",
           a ? ", this interval" : ", since the start", b->UmdFlags & 0xff);
    for (int k = 0; k < MW_UMD_KINDS; k++) {
        unsigned long long calls = b->Umd[k].Calls - (a ? a->Umd[k].Calls : 0);
        unsigned long long bytes = b->Umd[k].Bytes - (a ? a->Umd[k].Bytes : 0);
        unsigned long long ticks = b->Umd[k].Ticks - (a ? a->Umd[k].Ticks : 0);
        if (!umd_names[k][0] || calls == 0)
            continue;
        if (k == MW_UMD_SUBMIT) {
            /* Bytes are allocation-list entries here. */
            printf("    %-28s %8llu (%.1f/s)  %6.1f BOs listed each  %8llu us each  %6.0f ms in all\n",
                   umd_names[k], calls, secs > 0 ? (double)calls / secs : 0.0, (double)bytes / (double)calls,
                   hz ? ticks * 1000000ull / hz / calls : 0, hz ? (double)ticks * 1000.0 / (double)hz : 0.0);
            continue;
        }
        printf("    %-28s %8llu (%.1f/s)  %9.1f MB  %8llu us each  %6.0f ms in all\n", umd_names[k],
               calls, secs > 0 ? (double)calls / secs : 0.0, (double)bytes / 1e6,
               hz ? ticks * 1000000ull / hz / calls : 0, hz ? (double)ticks * 1000.0 / (double)hz : 0.0);
    }
    for (int i = 0; i < MW_UMD_PROCS; i++) {
        const MW_UMD_PROC *p = &b->UmdProc[i];
        const MW_UMD_PROC *q = NULL;
        char name[MAX_PATH] = "?";
        const char *base = name;
        HANDLE h;

        if (p->Pid == 0)
            continue;
        if (a)
            for (int j = 0; j < MW_UMD_PROCS; j++)
                if (a->UmdProc[j].Pid == p->Pid)
                    q = &a->UmdProc[j];
        if (a && q && q->Reports == p->Reports)
            continue;           /* nothing from it in this interval */
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->Pid);
        if (h) {
            DWORD len = sizeof(name);
            if (QueryFullProcessImageNameA(h, 0, name, &len))
                for (const char *c = name; *c; c++)
                    if (*c == '\\')
                        base = c + 1;
            CloseHandle(h);
        } else {
            base = "(gone or not openable)";
        }
        printf("    pid %5u %-20.20s", p->Pid, base);
        for (int k = 0; k < MW_UMD_KINDS; k++) {
            unsigned long long calls = p->Count[k].Calls - (q ? q->Count[k].Calls : 0);
            unsigned long long ticks = p->Count[k].Ticks - (q ? q->Count[k].Ticks : 0);
            static const char *const tag[MW_UMD_KINDS] = {
                "gpucopy", "cpucopy", "cpubuf", "upload", "mapR", "mapW", "readback", "direct",
                "idle", "blt", "resolve", "submit",
            };
            if (calls && tag[k][0])
                printf(" %s %llu/%llums", tag[k], calls, hz ? ticks * 1000ull / hz : 0);
        }
        printf("\n");
    }
}

/* Ticks over a count, as microseconds each. */
static unsigned long long each_us(unsigned long long ticks, unsigned long long hz, unsigned count)
{
    return count != 0 && hz != 0 ? ticks * 1000000ull / hz / count : 0;
}

/* m5test perf [seconds]: what the driver did in an interval, from two
 * readings of its counters. Nothing is submitted; run it while the desktop
 * is doing whatever is to be measured. */
static int perf(D3DKMT_HANDLE adapter, unsigned seconds)
{
    MW_ESCAPE_STATS_DATA a = {0}, b = {0};
    LARGE_INTEGER hz, t0, t1;
    double secs;

    a.Code = b.Code = MW_ESCAPE_STATS;
    QueryPerformanceFrequency(&hz);
    if (escape(adapter, 0, &a, sizeof(a)) < 0)
        return fail("Escape(STATS)", 0);
    QueryPerformanceCounter(&t0);
    if (a.PerfHz == 0) {
        printf("perf: this maliwddm.sys has no timing counters\n");
        return 1;
    }
    Sleep(seconds * 1000);
    if (escape(adapter, 0, &b, sizeof(b)) < 0)
        return fail("Escape(STATS)", 0);
    QueryPerformanceCounter(&t1);
    secs = (double)(t1.QuadPart - t0.QuadPart) / (double)hz.QuadPart;

    unsigned flips = b.Flips - a.Flips, copies = b.FlipCopies - a.FlipCopies;
    unsigned jobs = (b.JobsDone + b.JobsFailed) - (a.JobsDone + a.JobsFailed);
    unsigned vsyncs = b.Vsyncs - a.Vsyncs;
    unsigned blts = (b.PresentBlts + b.PresentFills) - (a.PresentBlts + a.PresentFills);
    unsigned long long jobTicks = b.JobTicks - a.JobTicks;

    printf("perf over %.2f s:\n", secs);
    printf("  flips %u (%.1f/s), copied to the framebuffer %u, with jobs outstanding %u\n",
           flips, flips / secs, copies, b.FlipsWhileBusy - a.FlipsWhileBusy);
    printf("  flip copy: %llu us each (longest since start %u)\n",
           each_us(b.FlipCopyTicks - a.FlipCopyTicks, b.PerfHz, copies), b.FlipCopyMaxUs);
    printf("  flip to shown, copy included: %llu us each (longest since start %u)\n",
           each_us(b.FlipWaitTicks - a.FlipWaitTicks, b.PerfHz, copies), b.FlipWaitMaxUs);
    printf("  vsyncs %u (%.1f/s), %llu us apart (longest since start %u)\n",
           vsyncs, vsyncs / secs, each_us(b.VsyncTicks - a.VsyncTicks, b.PerfHz, vsyncs),
           b.VsyncGapMaxUs);
    printf("  jobs %u (%.1f/s, %.1f a flip), %llu us each (longest since start %u), GPU busy %.0f%%\n",
           jobs, jobs / secs, flips ? (double)jobs / flips : 0.0,
           each_us(jobTicks, b.PerfHz, jobs), b.JobMaxUs,
           100.0 * (double)jobTicks / (double)b.PerfHz / secs);
    {
        unsigned scanned = b.FlipsScanned - a.FlipsScanned, gaps = b.FlipIntervals - a.FlipIntervals;
        unsigned shows = b.FlipShows - a.FlipShows;
        unsigned long long gapUs = each_us(b.FlipIntervalTicks - a.FlipIntervalTicks, b.PerfHz, gaps);
        printf("  present path: %u flips scanned out (%.1f MiB of copying avoided), %u copied (%u turned);"
               " last flip %s\n", scanned, (double)(b.CopyBytesAvoided - a.CopyBytesAvoided) / (1024.0 * 1024.0),
               copies, b.FlipsRotCopied - a.FlipsRotCopied, path_name(b.LastPath));
        printf("  left to the copy because: scan-out off %u, no VOP2 address %u, needs turning %u,"
               " display invisible %u; scan-out state 0x%x\n",
               b.WhyScanOff - a.WhyScanOff, b.WhyNotMapped - a.WhyNotMapped, b.WhyRotated - a.WhyRotated,
               b.WhyInvisible - a.WhyInvisible, b.ScanState);
        printf("  per flip: %llu us apart (%.1f/s; longest since start %u), %llu us from the flip to the vsync"
               " that showed it (longest since start %u), %llu us in SetVidPnSourceAddress (longest %u)\n",
               gapUs, gapUs ? 1e6 / (double)gapUs : 0.0, b.FlipIntervalMaxUs,
               each_us(b.FlipShowTicks - a.FlipShowTicks, b.PerfHz, shows), b.FlipShowMaxUs,
               each_us(b.FlipCallTicks - a.FlipCallTicks, b.PerfHz, flips), b.FlipCallMaxUs);
        printf("  CPU in flip copies: %.0f ms of the %.2f s (%.0f%% of one core); IOMMU faults %u,"
               " scan-out turned off by one %u times\n",
               (double)(b.FlipCopyTicks - a.FlipCopyTicks) * 1000.0 / (double)b.PerfHz, secs,
               100.0 * (double)(b.FlipCopyTicks - a.FlipCopyTicks) / (double)b.PerfHz / secs,
               b.ScanFaults - a.ScanFaults, b.ScanAutoOff - a.ScanAutoOff);
    }
    printf("  KMD blt presents (a window's frame into DWM's surface, on the CPU): %.1f MB\n",
           (double)(b.PresentBltBytes - a.PresentBltBytes) / 1e6);
    print_umd(&a, &b, secs);
    printf("  blt and fill presents %u, %llu us each\n", blts,
           each_us(b.PresentBltTicks - a.PresentBltTicks, b.PerfHz, blts));
    printf("  renders %u, submissions %u, interrupts %u, aperture maps %u, unmaps %u\n",
           b.Renders - a.Renders, b.Submits - a.Submits, b.Irqs - a.Irqs, b.Maps - a.Maps,
           b.Unmaps - a.Unmaps);
    {
        unsigned waits = b.WaitCount - a.WaitCount, preempts = b.PreemptRequests - a.PreemptRequests;
        printf("  submission to start: %u submissions, %llu us each (longest since start %u);"
               " %u arrived behind another context's chain (longest since start %u us)\n",
               waits, each_us(b.WaitTicks - a.WaitTicks, b.PerfHz, waits), b.WaitMaxUs,
               b.WaitOtherCount - a.WaitOtherCount, b.WaitOtherMaxUs);
        printf("  preemption: TimeSlice %u ms; requests %u (%u idle, %u overlapping), %llu us each"
               " to DMA_PREEMPTED (longest since start %u); submissions handed back %u\n",
               b.TimeSliceMs, preempts, b.PreemptIdle - a.PreemptIdle, b.PreemptOverlap - a.PreemptOverlap,
               each_us(b.PreemptTicks - a.PreemptTicks, b.PerfHz, preempts), b.PreemptMaxUs,
               b.PreemptDropped - a.PreemptDropped);
        printf("  soft-stops %u: stopped %u, finished first %u, slow %u; at a chain boundary %u;"
               " resumed %u\n",
               b.SoftStops - a.SoftStops, b.StopsTaken - a.StopsTaken, b.StopsRacedDone - a.StopsRacedDone,
               b.StopSlow - a.StopSlow, b.StopsAtBoundary - a.StopsAtBoundary, b.Resumes - a.Resumes);
        printf("  unexpected (each should stay 0): resume mismatch %u, resume lost %u, bad tail %u,"
               " stopped unasked %u, jobs timed out %u\n",
               b.ResumeMismatch - a.ResumeMismatch, b.ResumeLost - a.ResumeLost, b.StopBadTail - a.StopBadTail,
               b.StopUnasked - a.StopUnasked, b.JobsTimedOut - a.JobsTimedOut);
    }
    return 0;
}


/* ---- m5test prof: MW_ESCAPE_PERF ---- */

static const char *const ddi_names[MW_PERF_DDIS] = {
    "Render", "Present", "SubmitCommand", "BuildPagingBuffer", "Patch", "CreateAllocation",
    "DestroyAllocation", "OpenAllocation", "Escape", "SetVidPnSourceAddress", "interrupt", "DPC",
};

static double us_of(unsigned long long ticks, unsigned long long hz)
{
    return hz ? (double)ticks * 1e6 / (double)hz : 0.0;
}

/* A histogram's change between two readings, as "<2us:n 2-3us:n ...". */
static void print_hist(const char *what, const mw_u32 *a, const mw_u32 *b)
{
    printf("    %s:", what);
    for (int i = 0; i < MW_PERF_BUCKETS; i++) {
        unsigned n = b[i] - (a ? a[i] : 0);
        unsigned lo = i ? 1u << i : 0;
        if (n == 0)
            continue;
        if (i == MW_PERF_BUCKETS - 1)
            printf(" >=%uus:%u", lo, n);
        else if (lo >= 1000)
            printf(" %.1f-%.1fms:%u", lo / 1000.0, ((2u << i) - 1) / 1000.0, n);
        else
            printf(" %u-%uus:%u", lo, (2u << i) - 1, n);
    }
    printf("\n");
}

static NTSTATUS read_perf(D3DKMT_HANDLE adapter, MW_ESCAPE_PERF_DATA *d)
{
    memset(d, 0, sizeof(*d));
    d->Code = MW_ESCAPE_PERF;
    return escape(adapter, 0, d, sizeof(*d));
}

static void print_temps(const MW_ESCAPE_PERF_DATA *d)
{
    static const char *const ch[6] = { "centre", "big", "little", "GPU", "NPU", "DDR" };
    if (d->TempValid == 0) {
        printf("  temperatures: not read (the TSADC is not running, or set PerfTemp 1; needs firmware with TSHUT/_CRT)\n");
        return;
    }
    printf("  temperatures (TSADC, no OTP trim, +-3 C):");
    for (int n = 0; n < 6; n++)
        if (d->TempValid & (1u << n))
            printf(" %s %.1f C", ch[n], d->TempMilliC[n] / 1000.0);
    printf("\n");
    if (d->GpuClkThermalDrops)
        printf("  GPU clock held at 594 MHz for heat %u times (GpuClkThermalDrops)\n", d->GpuClkThermalDrops);
}

static void print_clock(const MW_ESCAPE_PERF_DATA *d)
{
    static const char *const mux[8] = { "GPLL", "CPLL", "AUPLL", "SPLL", "LPLL", "?", "?", "?" };
    if (d->ClkSel165 == 0xFFFFFFFFu)
        printf("  GPU clock: not read (GPU not up)\n");
    else if (d->GpuHz)
        printf("  GPU clock: %u MHz (CRU_CLKSEL_CON165 0x%08x: %s / %u)\n", d->GpuHz / 1000000,
               d->ClkSel165, mux[(d->ClkSel165 >> 5) & 7], (d->ClkSel165 & 0x1F) + 1);
    else
        printf("  GPU clock: CRU_CLKSEL_CON165 0x%08x (%s / %u, rate not known here)\n", d->ClkSel165,
               mux[(d->ClkSel165 >> 5) & 7], (d->ClkSel165 & 0x1F) + 1);
}

/* m5test prof [seconds]: two readings of MW_ESCAPE_PERF and what changed. */
static int prof(D3DKMT_HANDLE adapter, unsigned seconds)
{
    static MW_ESCAPE_PERF_DATA a, b;
    NTSTATUS st;
    double secs;
    unsigned long long hz;

    if ((st = read_perf(adapter, &a)) < 0 || a.Version == 0) {
        printf("prof: this maliwddm.sys has no MW_ESCAPE_PERF (0x%08lx)\n", (unsigned long)st);
        return 1;
    }
    Sleep(seconds * 1000);
    if ((st = read_perf(adapter, &b)) < 0)
        return fail("Escape(PERF)", st);
    hz = b.PerfHz;
    secs = hz ? (double)(b.Now - a.Now) / (double)hz : 0.0;
    if (secs <= 0)
        secs = 1;
    printf("prof over %.2f s:\n", secs);
    print_clock(&b);
    print_temps(&b);

    printf("  DDI CPU time (in the driver only):\n");
    printf("    %-22s %9s %9s %9s %10s\n", "", "calls/s", "avg us", "max us", "ms per s");
    for (int i = 0; i < MW_PERF_DDIS; i++) {
        unsigned long long n = b.Ddi[i].Calls - a.Ddi[i].Calls, t = b.Ddi[i].Ticks - a.Ddi[i].Ticks;
        if (n == 0)
            continue;
        printf("    %-22s %9.1f %9.1f %9.0f %10.2f\n", ddi_names[i], n / secs, us_of(t, hz) / (double)n,
               us_of(b.Ddi[i].MaxTicks, hz), us_of(t, hz) / 1000.0 / secs);
    }
    {
        unsigned long long r = b.Ddi[MW_DDI_RENDER].Calls - a.Ddi[MW_DDI_RENDER].Calls;
        if (r)
            printf("  renders: %.1f allocation-list entries each (most %u), %.2f chains each (most %u)\n",
                   (double)(b.RenderAllocs - a.RenderAllocs) / (double)r, b.RenderAllocsMax,
                   (double)(b.RenderCmds - a.RenderCmds) / (double)r, b.RenderCmdsMax);
        printf("  GPU page tables: %.0f pages mapped a second\n", (double)(b.MapPages - a.MapPages) / secs);
    }
    {
        static const char *const slot[2] = { "fragment", "vertex/tiler/compute" };
        unsigned long long busy = 0;
        for (int i = 0; i < 2; i++)
            busy += b.SlotTicks[i] - a.SlotTicks[i];
        printf("  GPU busy %.1f%% of the interval (one chain at a time):\n", 100.0 * us_of(busy, hz) / 1e6 / secs);
        for (int i = 0; i < 2; i++) {
            unsigned long long n = b.SlotJobs[i] - a.SlotJobs[i], t = b.SlotTicks[i] - a.SlotTicks[i];
            printf("    %-21s %8llu chains (%.1f/s), %.0f us each, busy %.1f%%, longest since start %.0f us\n",
                   slot[i], n, n / secs, n ? us_of(t, hz) / (double)n : 0.0, 100.0 * us_of(t, hz) / 1e6 / secs,
                   us_of(b.SlotMaxTicks[i], hz));
            if (n)
                print_hist("  run time", a.SlotHist[i], b.SlotHist[i]);
        }
    }
    {
        unsigned long long n = b.FenceCount - a.FenceCount, g = b.GapCount - a.GapCount;
        printf("  submission to completion: %llu (%.1f/s), %.0f us each, longest since start %.0f us\n", n,
               n / secs, n ? us_of(b.FenceTicks - a.FenceTicks, hz) / (double)n : 0.0, us_of(b.FenceMaxTicks, hz));
        if (n)
            print_hist("latency", a.FenceHist, b.FenceHist);
        printf("  bubbles (a chain ready when the one before it ended, to its start): %llu, %.0f us each,"
               " %.1f ms in all, longest since start %.0f us\n", g, g ? us_of(b.GapTicks - a.GapTicks, hz) / (double)g : 0.0,
               us_of(b.GapTicks - a.GapTicks, hz) / 1000.0, us_of(b.GapMaxTicks, hz));
        if (g)
            print_hist("gap", a.GapHist, b.GapHist);
    }
    printf("  chains collected by the interrupt %u, by the poll timer %u; poll timer ran %u times (%.0f/s),"
           " PollMs %u\n", b.ReapIsr - a.ReapIsr, b.ReapPoll - a.ReapPoll, b.PollRuns - a.PollRuns,
           (b.PollRuns - a.PollRuns) / secs, b.PollMs);
    {
        unsigned long long n = b.DpcCount - a.DpcCount;
        unsigned subs = b.Submissions - a.Submissions;
        printf("  completion to DPC: %llu, %.0f us each, longest since start %.0f us\n", n,
               n ? us_of(b.DpcTicks - a.DpcTicks, hz) / (double)n : 0.0, us_of(b.DpcMaxTicks, hz));
        printf("  submissions %u (%.1f/s), %.2f already queued in the driver at each (most %u)\n", subs, subs / secs,
               subs ? (double)(b.QueueDepthSum - a.QueueDepthSum) / subs : 0.0, b.QueueDepthMax);
    }
    return 0;
}

/* m5test ring: the last chains the GPU ran, oldest first, in microseconds
 * from the first one's submission. CSV with "csv". */
static int ring(D3DKMT_HANDLE adapter, int csv)
{
    static MW_ESCAPE_PERF_DATA d;
    NTSTATUS st = read_perf(adapter, &d);
    unsigned n, first;
    unsigned long long hz, base;

    if (st < 0 || d.Version == 0) {
        printf("ring: this maliwddm.sys has no MW_ESCAPE_PERF (0x%08lx)\n", (unsigned long)st);
        return 1;
    }
    hz = d.PerfHz;
    n = d.RingNext < MW_PERF_RING ? d.RingNext : MW_PERF_RING;
    first = d.RingNext - n;
    if (n == 0) {
        printf("ring: no chain has run since the driver started\n");
        return 0;
    }
    base = d.Ring[first % MW_PERF_RING].SubmitTick;
    printf(csv ? "fence,slot,flags,status,submit_us,start_us,end_us,queued_us,run_us\n"
               : "  fence slot flags status   submit_us    start_us      end_us  queued_us     run_us\n");
    for (unsigned i = first; i != d.RingNext; i++) {
        const MW_PERF_JOB *r = &d.Ring[i % MW_PERF_RING];
        double s = us_of(r->SubmitTick - base, hz), b = us_of(r->StartTick - base, hz), e = us_of(r->EndTick - base, hz);
        printf(csv ? "%u,%u,0x%x,0x%x,%.1f,%.1f,%.1f,%.1f,%.1f\n"
                   : "%7u %4u 0x%03x   0x%02x %11.1f %11.1f %11.1f %10.1f %10.1f\n",
               r->Fence, r->SlotFlags >> 16, r->SlotFlags & 0xF00, r->SlotFlags & 0xFF, s, b, e, b - s, e - b);
    }
    if (!csv)
        printf("  flags: 0x100 failed, 0x200 soft-stopped, 0x400 collected by the poll timer,"
               " 0x800 not its submission's first chain\n");
    return 0;
}

static double mib(unsigned long long bytes)
{
    return (double)bytes / (1024.0 * 1024.0);
}

/* m5test mem: GPU memory as maliwddm counts it, by the process that
 * created it, next to what Windows says each process uses. GPU bytes are
 * allocations in the aperture segment; their pages are system memory, and
 * they count in a process's working set only where its UMD has them mapped
 * (Panfrost keeps a CPU view of every BO it has touched). Run elevated to
 * see the working sets of dwm.exe and the others. */
static int mem(D3DKMT_HANDLE adapter)
{
    static MW_ESCAPE_MEMORY_DATA d;
    MW_MEM_PROC *p[MW_MEM_PROCS];
    NTSTATUS st;
    unsigned n;

    memset(&d, 0, sizeof(d));
    d.Code = MW_ESCAPE_MEMORY;
    st = escape(adapter, 0, &d, sizeof(d));
    if (st < 0) {
        printf("mem: escape 0x%08lx (a maliwddm.sys without MW_ESCAPE_MEMORY?)\n", (unsigned long)st);
        return 2;
    }
    printf("GPU memory: %.1f MiB in %u allocations (most ever %.1f MiB), %.1f MiB resident in %u;"
           " segment %.0f MiB\n", mib(d.Bytes), d.Count, mib(d.PeakBytes), mib(d.ResidentBytes),
           d.ResidentCount, mib(d.SegmentSize));
    printf("  %u created, %u destroyed, %u refused; %u made by a process with no entry left\n",
           d.Creates, d.Destroys, d.CreateFails, d.NoSlot);

    n = d.Procs <= MW_MEM_PROCS ? d.Procs : MW_MEM_PROCS;
    for (unsigned i = 0; i < n; i++)
        p[i] = &d.Proc[i];
    for (unsigned i = 1; i < n; i++) {         /* most bytes first */
        MW_MEM_PROC *t = p[i];
        unsigned j = i;
        while (j > 0 && p[j - 1]->Bytes < t->Bytes) {
            p[j] = p[j - 1];
            j--;
        }
        p[j] = t;
    }
    printf("\n  %6s %-28s %9s %6s %9s %9s %8s | %9s %9s\n", "pid", "process", "GPU MiB", "allocs",
           "resident", "peak MiB", "created", "WS MiB", "priv MiB");
    for (unsigned i = 0; i < n; i++) {
        char name[MAX_PATH] = "?";
        const char *base = name;
        double ws = -1, priv = -1;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, p[i]->Pid);

        if (h == NULL)
            h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p[i]->Pid);
        if (h != NULL) {
            DWORD len = sizeof(name);
            PROCESS_MEMORY_COUNTERS_EX pmc;
            if (QueryFullProcessImageNameA(h, 0, name, &len)) {
                for (const char *c = name; *c; c++)
                    if (*c == '\\')
                        base = c + 1;
            }
            memset(&pmc, 0, sizeof(pmc));
            if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
                ws = mib(pmc.WorkingSetSize);
                priv = mib(pmc.PrivateUsage);
            }
            CloseHandle(h);
        } else if (p[i]->Pid == 4) {
            base = "System";
        } else {
            base = p[i]->Count ? "(cannot open)" : "(gone)";
        }
        printf("  %6u %-28.28s %9.1f %6u %9.1f %9.1f %8u |", p[i]->Pid, base, mib(p[i]->Bytes),
               p[i]->Count, mib(p[i]->ResidentBytes), mib(p[i]->PeakBytes), p[i]->Creates);
        if (ws >= 0)
            printf(" %9.1f %9.1f\n", ws, priv);
        else
            printf(" %9s %9s\n", "-", "-");
    }
    return 0;
}

int main(int argc, char **argv)
{
    D3DKMT_HANDLE adapter;
    D3DKMT_CREATEDEVICE dev = {0};
    D3DKMT_CREATECONTEXT ctx = {0};
    D3DKMT_CREATEALLOCATION ca = {0};
    D3DDDI_ALLOCATIONINFO ai = {0};
    MW_ALLOCATION_INFO priv = {0};
    D3DKMT_LOCK lock = {0};
    D3DKMT_UNLOCK unlock = {0};
    D3DKMT_RENDER render = {0};
    MW_ESCAPE_ALLOC_INFO_DATA where = {0};
    NTSTATUS st;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!resolve_all())
        return 3;
    adapter = find_adapter();
    if (adapter == 0) {
        printf("no maliwddm adapter\n");
        return 1;
    }

    g_adapter = adapter;
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
        /* scan 1: primaries are scanned out by the display controller;
         * scan 0: copied again; scan 2: and its IOMMU off; scan 5: on, now
         * and at every start (the default since 2026-11-21 when there is
         * no setting); scan 4: off, and not at start; scan: report. */
        MW_ESCAPE_SCANOUT_DATA d = {0};
        d.Code = MW_ESCAPE_SCANOUT;
        d.Mode = argc >= 3 ? (unsigned)atoi(argv[2]) : 3;
        st = escape(adapter, 0, &d, sizeof(d));
        if (st < 0)
            return fail("scan-out escape", st);
        printf("scan-out: status 0x%x (1 on, 2 translating, 4 a primary is shown), mmu status 0x%08x 0x%08x\n",
               d.Status, d.Mmu0Status, d.Mmu1Status);
        printf("  %u flips scanned out, %u primaries mapped, %u returns to the framebuffer, "
               "%u wrong size, %u without a slot; framebuffer at 0x%08x\n",
               d.Flips, d.Maps, d.ToFbs, d.Mismatch, d.NoSlot, d.FbPa);
        printf("  %u IOMMU page faults cleared, the last at 0x%08x\n", d.Faults, d.FaultAddr);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "clk") == 0) {
        /* clk: report. clk <parent> <div> [keep]: parent 0 GPLL 1188 MHz,
         * 1 CPLL 1000 MHz, 3 SPLL 702 MHz (only with GpuClkHigh and a
         * supply that meets the operating point); keep stores it for every
         * start. clk forget. */
        static const unsigned pll[8] = { 1188, 1000, 0, 702, 0, 0, 0, 0 };
        static const char *const name[8] = { "GPLL", "CPLL", "?", "SPLL", "?", "?", "?", "?" };
        static const char *const src[4] = { "unknown", "firmware (GpuRail)", "the GpuRailUv setting",
                                            "firmware and GpuRailUv, the lower" };
        MW_ESCAPE_GPUCLK_DATA d = {0};
        d.Code = MW_ESCAPE_GPUCLK;
        if (argc >= 3 && strcmp(argv[2], "forget") == 0)
            d.Mode = 3;
        else if (argc >= 4) {
            d.Parent = (unsigned)atoi(argv[2]);
            d.Div = (unsigned)atoi(argv[3]);
            d.Mode = (argc >= 5 && strcmp(argv[4], "keep") == 0) ? 2 : 1;
        }
        st = escape(adapter, 0, &d, sizeof(d));
        if (st < 0) {
            fail("GPU clock escape (at most 600 MHz unless GpuClkHigh allows more; see below)", st);
            d.Mode = 0;
            if (escape(adapter, 0, &d, sizeof(d)) < 0)
                return 1;
        }
        if (d.Applied && (d.Applied & 0xFF) != 0 && pll[(d.Applied >> 8) & 7] != 0)
            printf("GPU clock: %u MHz (%s / %u), written by this driver\n",
                   pll[(d.Applied >> 8) & 7] / (d.Applied & 0xFF), name[(d.Applied >> 8) & 7],
                   d.Applied & 0xFF);
        else
            printf("GPU clock: as the firmware left it (198 MHz on edk2-rk3576)\n");
        if (d.Stored && (d.Stored & 0xFF) != 0 && pll[(d.Stored >> 8) & 7] != 0)
            printf("  at every start: %u MHz\n", pll[(d.Stored >> 8) & 7] / (d.Stored & 0xFF));
        printf("  GpuClkHigh %u (%s)\n", d.High,
               d.High == 1 ? "mainline's operating points" :
               d.High == 2 ? "the vendor's slowest level" : "off: 600 MHz at most");
        printf("  supply: %u uV from %s%s\n", d.RailUv, src[d.RailSource & 3],
               (d.RailSource & 0x100) ? "; not a plain RK3576, nothing above 600 MHz" : "");
        if (d.AllowedHz)
            printf("  allowed now: up to %u MHz; 702 MHz (SPLL / 1) needs %u uV\n",
                   d.AllowedHz / 1000000, d.NeedUv702);
        if (d.Fallbacks)
            printf("  %u times back to 594 MHz after a job fault\n", d.Fallbacks);
        if (d.High)
            printf("  temperature guard: %s; hotter of TSADC 3/5 %.1f C (no OTP trim); %s; "
                   "held for heat %u times, back %u times (drop at 85 C, back below 75 C)\n",
                   d.ThermalGuard ? "on, every 1 s" : "OFF, so nothing above 600 MHz",
                   d.ThermalMilliC / 1000.0,
                   d.ThermalHeld ? "HELD at 594 MHz now" : "not held",
                   d.ThermalDrops, d.ThermalRestores);
        return st < 0 ? 1 : 0;
    }
    if (argc >= 2 && strcmp(argv[1], "mem") == 0)
        return mem(adapter);
    if (argc >= 2 && strcmp(argv[1], "prof") == 0) {
        int seconds = argc >= 3 ? atoi(argv[2]) : 5;
        return prof(adapter, seconds > 0 && seconds <= 600 ? (unsigned)seconds : 5);
    }
    if (argc >= 2 && strcmp(argv[1], "ring") == 0)
        return ring(adapter, argc >= 3 && strcmp(argv[2], "csv") == 0);
    if (argc >= 2 && strcmp(argv[1], "perf") == 0) {
        int seconds = argc >= 3 ? atoi(argv[2]) : 5;
        return perf(adapter, seconds > 0 && seconds <= 600 ? (unsigned)seconds : 5);
    }
    print_stats(adapter, "at start");
    dev.hAdapter = adapter;
    if ((st = pCreateDevice(&dev)) < 0)
        return fail("CreateDevice", st);
    ctx.hDevice = dev.hDevice;
    ctx.NodeOrdinal = 0;
    ctx.EngineAffinity = 1;
    ctx.ClientHint = D3DKMT_CLIENTHINT_DX10;
    if ((st = pCreateContext(&ctx)) < 0)
        return fail("CreateContext", st);
    printf("device 0x%x context 0x%x, command buffer %u bytes, %u allocation slots\n",
           dev.hDevice, ctx.hContext, ctx.CommandBufferSize, ctx.AllocationListSize);

    priv.Version = MW_ABI_VERSION;
    priv.Flags = MW_ALLOC_NOEXEC;
    priv.Size = JOB_BYTES;
    ai.pPrivateDriverData = &priv;
    ai.PrivateDriverDataSize = sizeof(priv);
    ca.hDevice = dev.hDevice;
    ca.NumAllocations = 1;
    ca.pAllocationInfo = &ai;
    if ((st = pCreateAllocation(&ca)) < 0)
        return fail("CreateAllocation", st);

    where.Code = MW_ESCAPE_ALLOC_INFO;
    where.hAllocation = ai.hAllocation;
    if ((st = escape(adapter, dev.hDevice, &where, sizeof(where))) < 0)
        return fail("Escape(ALLOC_INFO)", st);
    printf("allocation 0x%x at GPU 0x%llx, %llu bytes\n", ai.hAllocation,
           (unsigned long long)where.GpuVa, (unsigned long long)where.Size);
    if (where.GpuVa == 0)
        return fail("allocation has no GPU address (GPU not up?)", 0);

    lock.hDevice = dev.hDevice;
    lock.hAllocation = ai.hAllocation;
    if ((st = pLock(&lock)) < 0)
        return fail("Lock", st);
    volatile UINT32 *cpu = (volatile UINT32 *)lock.pData;
    memset((void *)cpu, 0, JOB_BYTES);
    write_job(cpu, where.GpuVa + TARGET_OFFSET, MAGIC);
    unlock.hDevice = dev.hDevice;
    unlock.NumAllocations = 1;
    unlock.phAllocations = &ai.hAllocation;
    if ((st = pUnlock(&unlock)) < 0)
        return fail("Unlock", st);

    /* One command, one allocation it writes. */
    MW_COMMAND *cmd = (MW_COMMAND *)ctx.pCommandBuffer;
    cmd->Type = MW_CMD_JOB;
    cmd->Slot = 1;
    cmd->Jc = where.GpuVa;
    memset(&ctx.pAllocationList[0], 0, sizeof(ctx.pAllocationList[0]));
    ctx.pAllocationList[0].hAllocation = ai.hAllocation;
    ctx.pAllocationList[0].WriteOperation = 1;
    render.hContext = ctx.hContext;
    render.CommandLength = sizeof(*cmd);
    render.AllocationCount = 1;
    render.PatchLocationCount = 0;
    if ((st = pRender(&render)) < 0)
        return fail("Render", st);
    printf("rendered\n");

    /* Locking waits for the GPU to finish with the allocation. */
    ULONGLONG t0 = GetTickCount64();
    if ((st = pLock(&lock)) < 0)
        return fail("Lock after render", st);
    cpu = (volatile UINT32 *)lock.pData;
    UINT32 got = cpu[TARGET_OFFSET / 4];
    UINT32 status = cpu[0];
    (void)pUnlock(&unlock);
    print_stats(adapter, "after the job");
    printf("after %llu ms: target 0x%08x (want 0x%08x), job header status word 0x%08x\n",
           GetTickCount64() - t0, got, MAGIC, status);

    {
        D3DKMT_DESTROYALLOCATION da = {0};
        D3DKMT_DESTROYCONTEXT dc = {0};
        D3DKMT_DESTROYDEVICE dd = {0};
        da.hDevice = dev.hDevice;
        da.phAllocationList = &ai.hAllocation;
        da.AllocationCount = 1;
        (void)pDestroyAllocation(&da);
        dc.hContext = ctx.hContext;
        (void)pDestroyContext(&dc);
        dd.hDevice = dev.hDevice;
        (void)pDestroyDevice(&dd);
    }

    if (got != MAGIC) {
        printf("FAIL: the GPU did not write the value\n");
        return 2;
    }
    printf("PASS: the Mali ran a job submitted through WDDM\n");
    return 0;
}
