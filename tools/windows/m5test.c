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
 * m5test perf [seconds], m5test scan [mode] and m5test mem (GPU memory by
 * process) only ask the driver; nothing is submitted.
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
    printf("  blt and fill presents %u, %llu us each\n", blts,
           each_us(b.PresentBltTicks - a.PresentBltTicks, b.PerfHz, blts));
    printf("  renders %u, submissions %u, interrupts %u, aperture maps %u, unmaps %u\n",
           b.Renders - a.Renders, b.Submits - a.Submits, b.Irqs - a.Irqs, b.Maps - a.Maps,
           b.Unmaps - a.Unmaps);
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
         * and at every start; scan 4: off, and not at start; scan: report. */
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
         * 1 CPLL 1000 MHz; keep stores it for every start. clk forget. */
        static const unsigned pll[2] = { 1188, 1000 };
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
        if (st < 0)
            return fail("GPU clock escape (at most 600 MHz: GPLL from /2, CPLL from /2)", st);
        if (d.Applied)
            printf("GPU clock: %u MHz (%s / %u), written by this driver\n",
                   pll[(d.Applied >> 8) & 1] / (d.Applied & 0xFF), (d.Applied >> 8) ? "CPLL" : "GPLL",
                   d.Applied & 0xFF);
        else
            printf("GPU clock: as the firmware left it (198 MHz on edk2-rk3576)\n");
        if (d.Stored)
            printf("  at every start: %u MHz\n", pll[(d.Stored >> 8) & 1] / (d.Stored & 0xFF));
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "mem") == 0)
        return mem(adapter);
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
