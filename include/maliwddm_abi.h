/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The private data maliwddm (the WDDM 1.3 kernel driver) exchanges with its
 * user-mode side through the D3D runtime: allocation private data, the
 * command buffer of a render, and escapes. Shared by the KMD, the UMD's
 * Panfrost backend and the D3DKMT test (tools/windows/m5test.c).
 *
 * The model (docs/M5.2-PLAN.md): the KMD owns the Mali's GPU address space.
 * It reserves a GPU address for every allocation when it is created, maps
 * the allocation's pages there whenever VidMm makes it resident, and the UMD
 * asks for the address once (MW_ESCAPE_ALLOC_INFO). Addresses never move,
 * so nothing is ever patched.
 */
#pragma once

#ifndef _KERNEL_MODE
#include <stdint.h>
typedef uint32_t mw_u32;
typedef uint64_t mw_u64;
typedef int32_t mw_s32;
#else
typedef unsigned __int32 mw_u32;
typedef unsigned __int64 mw_u64;
typedef __int32 mw_s32;
#endif

/* Allocation private data (D3DDDI_ALLOCATIONINFO.pPrivateDriverData). */
#define MW_ALLOC_NOEXEC         0x1     /* not shader code: may lie anywhere, XN */
#define MW_ALLOC_PRIMARY        0x2     /* scan-out capable (step d) */

typedef struct _MW_ALLOCATION_INFO {
    mw_u32 Version;                     /* MW_ABI_VERSION */
    mw_u32 Flags;                       /* MW_ALLOC_* */
    mw_u64 Size;                        /* bytes; rounded up to pages */
    /* Surfaces the runtime creates itself (primary, shadow, staging); 0 for
     * plain buffers. Format is a D3DDDIFORMAT. */
    mw_u32 Width, Height, Pitch, Format;
} MW_ALLOCATION_INFO;

#define MW_ABI_VERSION          1

/* A render's command buffer is a sequence of these. */
#define MW_CMD_JOB              1       /* run one job chain, wait for it */

typedef struct _MW_COMMAND {
    mw_u32 Type;                        /* MW_CMD_* */
    mw_u32 Slot;                        /* 0: fragment, 1: vertex/tiler/compute */
    mw_u64 Jc;                          /* GPU address of the first job header */
} MW_COMMAND;

/* At most this many commands per render; also the DMA buffer private data. */
#define MW_MAX_COMMANDS         32

/* Escapes (D3DKMTEscape, private driver data starts with the code). */
#define MW_ESCAPE_ALLOC_INFO    1
#define MW_ESCAPE_GPU_INFO      2
#define MW_ESCAPE_STATS         3
#define MW_ESCAPE_SCANOUT       4
#define MW_ESCAPE_WDDM          8

/* The WDDM version the KMD reported at this start (DXGK_DRIVERCAPS), and
 * what that asks of the UMD. A KMD that does not know the code refuses it:
 * WDDM 1.3, nothing asked. */
#define MW_WDDM_NEEDS_RESIDENCY 0x1u    /* allocations must be made resident (MakeResident) before a
                                         * render names them: WDDM 2.x, docs/WDDM2.md 16.10 */
typedef struct _MW_ESCAPE_WDDM_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_WDDM */
    mw_u32 WddmVersion;                 /* out: 0x1300, 0x2000, ... */
    mw_u32 Needs;                       /* out: MW_WDDM_NEEDS_* */
    mw_u32 Reserved;
} MW_ESCAPE_WDDM_DATA;

/* Scan-out of primaries by the display controller, on and off.
 * Mode: 1 on, 0 off (flips are copied), 2 off and the IOMMU off too,
 * 3 only report; 5 on and the ScanOut setting 1, 4 off and the setting 0.
 * The setting decides the state at every start; without it scan-out is on
 * (it was off before 2026-11-21). */
typedef struct _MW_ESCAPE_SCANOUT_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_SCANOUT */
    mw_u32 Mode;
    mw_u32 Status;                      /* 1 on, 2 translating, 4 a primary is shown; ~0 not available */
    mw_u32 Mmu0Status, Mmu1Status;
    mw_u32 Flips, Maps, ToFbs, Mismatch, NoSlot;
    mw_u32 FbPa;
    mw_u32 Faults, FaultAddr;           /* IOMMU page faults seen and cleared; the last address */
} MW_ESCAPE_SCANOUT_DATA;

typedef struct _MW_ESCAPE_ALLOC_INFO_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_ALLOC_INFO */
    mw_u32 hAllocation;                 /* in: D3DKMT allocation handle */
    mw_u64 GpuVa;                       /* out */
    mw_u64 Size;                        /* out */
} MW_ESCAPE_ALLOC_INFO_DATA;

/* GET_PARAM's values for the UMD, by Panfrost parameter number. */
#define MW_GPU_PARAMS           45
typedef struct _MW_ESCAPE_GPU_INFO_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_GPU_INFO */
    mw_u32 Count;                       /* out: valid entries */
    mw_u64 Value[MW_GPU_PARAMS];        /* out; index = DRM_PANFROST_PARAM_* */
    mw_u64 Valid;                       /* out: bit n = Value[n] is meaningful */
} MW_ESCAPE_GPU_INFO_DATA;

/* What the user-mode driver does on the CPU and on the GPU, by kind: each
 * process's UMD counts these and hands the KMD what it counted since the
 * last time, about once a second (MW_ESCAPE_UMD); MW_ESCAPE_STATS returns
 * the sums. Bytes are what was copied, uploaded, mapped or presented;
 * Ticks the CPU time the calls took in the UMD (for a GPU copy that is the
 * time to queue it, not the GPU's). */
#define MW_UMD_COPY_GPU         0       /* CopyResource / CopySubresourceRegion as a GPU job */
#define MW_UMD_COPY_CPU         1       /* the same between textures, done on the CPU */
#define MW_UMD_COPY_CPU_BUFFER  2       /* the same between buffers, on the CPU */
#define MW_UMD_UPLOAD           3       /* UpdateSubresource (from application memory) */
#define MW_UMD_MAP_READ         4       /* Map for reading (staging read-back) */
#define MW_UMD_MAP_WRITE        5       /* Map for writing only */
#define MW_UMD_PRESENT_READBACK 6       /* a present read back on the CPU and blitted by the KMD */
#define MW_UMD_PRESENT_DIRECT   7       /* a present of a surface the runtime already has: no copy */
#define MW_UMD_GPU_WAIT         8       /* waits for the GPU to be idle (the shim's wddm_idle) */
#define MW_UMD_BLT              9       /* DXGI pfnBlt calls, which the UMD does not carry out */
#define MW_UMD_RESOLVE          10      /* ResolveSubresource */
#define MW_UMD_SUBMIT           11      /* pfnRenderCb from the Panfrost shim: Calls are renders,
                                         * Bytes the allocation-list entries they carried (not
                                         * bytes), Ticks the CPU time in the call */
#define MW_UMD_KINDS            12

typedef struct _MW_UMD_COUNT {
    mw_u64 Calls, Bytes, Ticks;
} MW_UMD_COUNT;

/* MW_ESCAPE_UMD: in, the UMD's counts since its previous report; out, the
 * switches the KMD's settings give (UmdCopyOnGpu, UmdReadbackOneCopy,
 * UmdMergeSubmit in Device Parameters, each on unless 0; UmdCopyBatch,
 * off unless 1). */
#define MW_ESCAPE_UMD           7
#define MW_UMD_FLAG_COPY_GPU    0x1     /* copies between textures may run on the GPU */
#define MW_UMD_FLAG_READBACK_1  0x2     /* a read-back present copies once, straight into the surface */
#define MW_UMD_FLAG_MERGE_SUBMIT 0x4    /* UmdMergeSubmit (on unless 0): a batch's vertex/tiler
                                         * chain and the fragment chain Panfrost submits right
                                         * after it go to the KMD as one render with two commands */
#define MW_UMD_FLAG_COPY_BATCH  0x8     /* UmdCopyBatch (off unless 1): a GPU copy is a draw in
                                         * the batches the UMD has open, not two flushes and
                                         * a job of its own (docs/PERF.md, "DWM's copies") */
#define MW_UMD_FLAG_VALID       0x80000000u

typedef struct _MW_ESCAPE_UMD_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_UMD */
    mw_u32 Flags;                       /* out: MW_UMD_FLAG_* */
    MW_UMD_COUNT Delta[MW_UMD_KINDS];   /* in */
} MW_ESCAPE_UMD_DATA;

#define MW_UMD_PROCS            12
typedef struct _MW_UMD_PROC {
    mw_u32 Pid, Reports;
    MW_UMD_COUNT Count[MW_UMD_KINDS];
} MW_UMD_PROC;

/* What the UMD may offer, from the KMD: the answer to the UMD's
 * KMTQAITYPE_UMDRIVERPRIVATE query (pfnQueryAdapterInfoCb at
 * OpenAdapter10_2, before there is a device to escape through). The KMD
 * fills it from DWORD settings in Device Parameters, so each step of the
 * D3D11 work (docs/D3D11-UMD.md) can be turned back without reinstalling.
 * A KMD that refuses the query means: Ddi 100, as before it existed.
 * Fields are only ever added at the end; Size says how many there are. */
#define MW_UMD_INFO_MAGIC       0x3149574Du     /* 'MWI1' */
#define MW_UMD_DDI_D3D10_0      100     /* what the UMD always offered: feature level 10_0 */
#define MW_UMD_DDI_D3D10_1      101     /* the D3D10.1 DDI: feature level 10_1, ANGLE's ES 3.0 */
#define MW_UMD_DDI_D3D11_0      110
#define MW_UMD_DDI_D3D11_1      111
#define MW_UMD_NO_NODE          0xFFFFFFFFu

typedef struct _MW_UMD_ADAPTER_INFO {
    mw_u32 Magic;                       /* MW_UMD_INFO_MAGIC */
    mw_u32 Size;                        /* bytes filled in */
    mw_u32 Ddi;                         /* UmdDdi: the highest DDI, MW_UMD_DDI_* (default 100) */
    mw_u32 FeatureLevel;                /* UmdFeatureLevel: a D3D_FEATURE_LEVEL (default 0xa000) */
    mw_u32 Dxgi12;                      /* UmdDxgi12: the DXGI 1.2 DDI and flip model (default 0) */
    mw_u32 Video;                       /* UmdVideo: 0 none, 1 video DDI without profiles, 2 with (default 0) */
    mw_u32 DecodeNode;                  /* node ordinal of the VIDEO_DECODE engine, MW_UMD_NO_NODE none */
    mw_u32 VirtualMode;                 /* 1: WDDM 2.x with GPU virtual addressing (D3DWDDM2_0 DDI) */
    mw_u32 KmdBuild;                    /* the CI run that built the KMD (MW_BUILD) */
    mw_u32 Msaa;                        /* UmdMsaa: 0 none; 1 every format Panfrost renders
                                         * at 4x is a multisample render target at 1x and 4x,
                                         * in the browsers and d3dprobe only (ANGLE then
                                         * uses 1x, which Skia needs); 2 the same in every
                                         * process; 4 and 5 as 1 and 2 (default 1) */
    mw_u32 Reserved[6];
} MW_UMD_ADAPTER_INFO;

/* The KMD's counters, for tests and bring-up. */
typedef struct _MW_ESCAPE_STATS_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_STATS */
    mw_u32 GpuUp;
    mw_u32 Renders, Presents, Submits;
    mw_u32 LastSubmittedFence, LastCompletedFence;
    mw_u32 Queued, Running;
    mw_u32 JobsDone, JobsFailed, JobsTimedOut;
    mw_u32 LastJsStatus, LastFaultStatus;
    mw_u32 Irqs, Resets;
    mw_u64 LastFaultAddress;
    /* BuildPagingBuffer: calls by DXGK_BUILDPAGINGBUFFER_OPERATION (0-15),
     * and the last aperture map. */
    mw_u32 PagingOps[16];
    mw_u32 Maps, MapFails, Unmaps, LastMapPages, LastMapStatus, Allocations;
    mw_u64 LastMapVa, LastAllocVa;
    /* What VidMm handed DxgkDdiPatch: the allocation list of the last
     * patched DMA buffer, first entry. */
    mw_u32 Patches, PatchAllocations, PatchPatchLocations, PatchSegment0;
    mw_u64 PatchAddress0;
    /* Presents carried out on the CPU, and skipped (no pages known). */
    mw_u32 PresentBlts, PresentFills, PresentSkips, Reserved0;
    /* Emulated vsyncs, flips (SetVidPnSourceAddress), and flips whose
     * primary was copied to the framebuffer or could not be. */
    mw_u32 Vsyncs, Flips, FlipCopies, FlipCopySkips;
    /* Where the time goes, summed since the driver started, in ticks of the
     * performance counter (PerfHz a second; 0 from a driver without these).
     * Two readings a few seconds apart give the cost per flip, per job and
     * per vsync in between (m5test perf). The Max values are microseconds,
     * since the start. */
    mw_u64 PerfHz;
    mw_u64 FlipCopyTicks;               /* copying a primary to the framebuffer */
    mw_u64 FlipWaitTicks;               /* SetVidPnSourceAddress to the vsync that showed it */
    mw_u64 JobTicks;                    /* a job chain on the GPU, start to collected */
    mw_u64 PresentBltTicks;             /* blt and fill presents on the CPU */
    mw_u64 VsyncTicks;                  /* between emulated vsyncs; with Vsyncs, the period */
    mw_u32 FlipCopyMaxUs, FlipWaitMaxUs, JobMaxUs, VsyncGapMaxUs;
    /* Flips that arrived while a job chain was running or queued. */
    mw_u32 FlipsWhileBusy, Reserved1;
    /* Preemption (setting TimeSlice; 0 from a driver without it, or with
     * it off). Counts since the driver started. */
    mw_u32 TimeSliceMs;                 /* the setting in effect; 0 off */
    mw_u32 PreemptRequests;             /* DxgkDdiPreemptCommand calls acted on */
    mw_u32 PreemptIdle;                 /* of those, nothing was on the GPU: reported at once */
    mw_u32 PreemptOverlap;              /* a request came while one was still waiting */
    mw_u32 PreemptDropped;              /* submissions handed back to the scheduler */
    mw_u32 SoftStops;                   /* soft-stops written */
    mw_u32 StopsTaken;                  /* of those, the chain stopped (STOPPED, tail saved) */
    mw_u32 StopsRacedDone;              /* of those, the chain finished first */
    mw_u32 StopsAtBoundary;             /* preempted between chains, nothing stopped */
    mw_u32 StopSlow;                    /* a soft-stop not taken within 100 ms */
    mw_u32 Resumes;                     /* resubmissions recognised and continued */
    mw_u32 ResumeMismatch;              /* a saved point whose next submission differed */
    mw_u32 ResumeLost;                  /* a partly run submission with nowhere to save it */
    mw_u32 StopBadTail;                 /* STOPPED with a zero tail, or tail != head */
    mw_u32 StopUnasked;                 /* STOPPED without a stop asked for */
    mw_u32 PreemptMaxUs;                /* request to DMA_PREEMPTED, longest */
    mw_u64 PreemptTicks;                /* request to DMA_PREEMPTED, summed */
    /* Submission to its first chain starting: all submissions with jobs,
     * and those that arrived while another context's chain was running. */
    mw_u64 WaitTicks;
    mw_u32 WaitCount, WaitMaxUs;
    mw_u32 WaitOtherCount, WaitOtherMaxUs;
    /* How flips reach the screen (0 from a driver without these). A flip
     * is scanned out (the VOP2 is pointed at the primary, no pixel is
     * copied) or left to the copy thread, which copies it into the
     * firmware framebuffer, turned if it has to be. */
    mw_u32 ScanState;                   /* 1 on, 2 translating, 4 a primary shown, 8 set up, 16 ScanOut setting on */
    mw_u32 FlipsScanned;                /* flips scanned out at SetVidPnSourceAddress: copies avoided */
    mw_u32 FlipsRotCopied;              /* of FlipCopies, those turned on the CPU */
    mw_u32 WhyScanOff;                  /* flips not scanned out: scan-out off or not translating */
    mw_u32 WhyNotMapped;                /* ... the primary has no VOP2 address (size, slot, not resident) */
    mw_u32 WhyRotated;                  /* ... the driver has to turn it (a primary of the turned size) */
    mw_u32 WhyInvisible;                /* ... the source was invisible (display off) */
    mw_u32 ScanAutoOff;                 /* times an IOMMU fault turned scan-out off */
    mw_u32 ScanFaults, ScanFaultAddr;   /* IOMMU faults seen, the last address */
    mw_u32 FlipIntervals, FlipIntervalMaxUs;    /* gaps between flips: their count, the longest */
    mw_u32 FlipShows, FlipShowMaxUs;    /* flips a vsync reported shown; flip to that vsync, longest */
    mw_u32 FlipCallMaxUs;               /* the longest SetVidPnSourceAddress */
    mw_u32 LastPath;                    /* the last flip shown: 1 scanned out, 2 copied, 3 copied and turned */
    mw_u64 FlipIntervalTicks;           /* between flips, summed: with FlipIntervals, the flip rate */
    mw_u64 FlipShowTicks;               /* flip to the vsync that reported it, summed, either path */
    mw_u64 FlipCallTicks;               /* inside SetVidPnSourceAddress, summed over Flips */
    mw_u64 CopyBytesAvoided;            /* bytes the scanned-out flips did not copy */
    /* Blt presents carried out on the CPU (PresentBlts): the bytes copied. */
    mw_u64 PresentBltBytes;
    /* What the user-mode drivers reported (MW_ESCAPE_UMD), summed over
     * every process, and by process: UmdProc has up to MW_UMD_PROCS of the
     * processes that reported most recently. Ticks are of the performance
     * counter (PerfHz). */
    mw_u32 UmdReports;                  /* escapes received */
    mw_u32 UmdFlags;                    /* MW_UMD_FLAG_* the KMD hands out now */
    MW_UMD_COUNT Umd[MW_UMD_KINDS];
    MW_UMD_PROC UmdProc[MW_UMD_PROCS];
} MW_ESCAPE_STATS_DATA;

/* GPU memory: what is allocated, what VidMm has made resident, and by which
 * process (m5test mem). A process is the one that was current when the
 * allocation was created: for a UMD's allocations, the process the UMD is
 * loaded in. Up to MW_MEM_PROCS processes; allocations made by any others
 * are counted in the totals and in NoSlot. A process's entry is reused once
 * it holds nothing, so an exited process with Count 0 may be gone. */
#define MW_ESCAPE_MEMORY        5
#define MW_ESCAPE_GPUCLK        6

/* The GPU's clock (RK3576: clk_gpu_src_pre, CRU_CLKSEL_CON165).
 * Parent: 0 GPLL (1188 MHz), 1 CPLL (1000 MHz), 3 SPLL (702 MHz); Div 1 to
 * 32. At most 600 MHz unless the setting GpuClkHigh allows more and the
 * supply meets the operating point (adapter.c, GpuClkWrite).
 * Mode: 0 report only; 1 set now; 2 set now and at every start; 3 forget
 * the stored setting (the firmware's rate is kept from the next start).
 * A caller may send only the first six fields (MW_ESCAPE_GPUCLK_V1_SIZE). */
typedef struct _MW_ESCAPE_GPUCLK_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_GPUCLK */
    mw_u32 Mode;
    mw_u32 Parent, Div;
    mw_u32 Applied;                     /* what this driver last wrote, or 0: (Parent << 8) | Div */
    mw_u32 Stored;                      /* the setting kept for the next start, same form, or 0 */
    mw_u32 High;                        /* the GpuClkHigh setting this start: 0 off, 1 mainline OPPs, 2 vendor slowest */
    mw_u32 RailUv;                      /* the supply the checks use, 0 unknown */
    mw_u32 RailSource;                  /* 1 firmware, 2 GpuRailUv, 3 both; 0x100: not a plain RK3576 */
    mw_u32 AllowedHz;                   /* the highest operating point allowed now */
    mw_u32 NeedUv702;                   /* what 702 MHz (SPLL / 1) needs, 0 with High 0 */
    mw_u32 Fallbacks;                   /* clocks dropped to 594 MHz by a job fault */
} MW_ESCAPE_GPUCLK_DATA;
#define MW_ESCAPE_GPUCLK_V1_SIZE  24
#define MW_MEM_PROCS            64

typedef struct _MW_MEM_PROC {
    mw_u32 Pid;
    mw_u32 Count;                       /* allocations it holds now */
    mw_u32 Creates;                     /* allocations it has made */
    mw_u32 ResidentCount;
    mw_u64 Bytes;                       /* bytes it holds now */
    mw_u64 ResidentBytes;               /* of those, mapped for the GPU now */
    mw_u64 PeakBytes;
} MW_MEM_PROC;

typedef struct _MW_ESCAPE_MEMORY_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_MEMORY */
    mw_u32 Procs;                       /* out: entries of Proc in use */
    mw_u64 Bytes, PeakBytes;            /* allocated now, and the most ever */
    mw_u64 ResidentBytes;               /* mapped at their GPU address now */
    mw_u64 SegmentSize;                 /* the aperture segment's size */
    mw_u32 Count, ResidentCount;
    mw_u32 Creates, Destroys, CreateFails, NoSlot;
    MW_MEM_PROC Proc[MW_MEM_PROCS];
} MW_ESCAPE_MEMORY_DATA;

/* Where the time goes, in more detail than MW_ESCAPE_STATS (m5test prof,
 * docs/PERF.md). Everything is a running total since the driver started:
 * two readings a few seconds apart give rates and averages. Ticks are of
 * the performance counter, PerfHz a second, Now the counter at the
 * reading. Each Max is the longest since the start.
 *
 * Histograms: bucket b counts durations of 2^b to 2^(b+1) - 1 us (bucket 0
 * also counts 0 us, the last one everything longer). */
#define MW_ESCAPE_PERF          0x20
#define MW_PERF_VERSION         1
#define MW_PERF_BUCKETS         20
#define MW_PERF_RING            128

/* The DDIs timed, by index into Ddi[]: CPU time inside the driver's own
 * function, from entry to return, whatever dxgkrnl does around it. */
#define MW_DDI_RENDER           0
#define MW_DDI_PRESENT          1
#define MW_DDI_SUBMIT           2       /* SubmitCommand, including a blt present done there */
#define MW_DDI_PAGING           3       /* BuildPagingBuffer: GPU page tables written here */
#define MW_DDI_PATCH            4
#define MW_DDI_CREATE_ALLOC     5
#define MW_DDI_DESTROY_ALLOC    6
#define MW_DDI_OPEN_ALLOC       7
#define MW_DDI_ESCAPE           8       /* includes the UMD's reports and its fence polls */
#define MW_DDI_SET_SOURCE       9       /* SetVidPnSourceAddress, a flip */
#define MW_DDI_ISR              10      /* the interrupt routine, at DIRQL, claimed or not */
#define MW_DDI_DPC              11
#define MW_PERF_DDIS            12

typedef struct _MW_PERF_TIME {
    mw_u64 Calls, Ticks, MaxTicks;
} MW_PERF_TIME;

/* One job chain as it ran, newest last (RingNext - 1). */
#define MW_PERF_JOB_FAILED      0x100   /* faulted or timed out; the low byte is JS_STATUS */
#define MW_PERF_JOB_STOPPED     0x200   /* soft-stopped, to be resumed */
#define MW_PERF_JOB_BY_POLL     0x400   /* collected by the poll timer, not the interrupt */
#define MW_PERF_JOB_SECOND      0x800   /* not the first chain of its submission */
typedef struct _MW_PERF_JOB {
    mw_u32 Fence;                       /* the submission's fence */
    mw_u32 SlotFlags;                   /* slot (0 fragment, 1 vertex/tiler/compute) << 16 | MW_PERF_JOB_* | JS_STATUS */
    mw_u64 SubmitTick;                  /* SubmitCommand of its submission */
    mw_u64 StartTick;                   /* written to JS_COMMAND_NEXT */
    mw_u64 EndTick;                     /* collected */
} MW_PERF_JOB;

typedef struct _MW_ESCAPE_PERF_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_PERF */
    mw_u32 Version;                     /* out: MW_PERF_VERSION */
    mw_u64 PerfHz, Now;
    MW_PERF_TIME Ddi[MW_PERF_DDIS];
    /* Renders: allocation-list entries and commands they carried. */
    mw_u64 RenderAllocs, RenderCmds;
    mw_u32 RenderAllocsMax, RenderCmdsMax;
    /* Aperture maps: pages written into the GPU page tables. */
    mw_u64 MapPages;
    /* Job chains by slot: 0 fragment, 1 vertex/tiler/compute. On the GPU
     * one at a time, so their ticks over the interval are its busy time. */
    mw_u64 SlotJobs[2], SlotTicks[2], SlotMaxTicks[2];
    mw_u32 SlotHist[2][MW_PERF_BUCKETS];
    /* Submissions with jobs, SubmitCommand to DMA_COMPLETED. */
    mw_u64 FenceCount, FenceTicks, FenceMaxTicks;
    mw_u32 FenceHist[MW_PERF_BUCKETS];
    /* Bubbles: a chain whose submission was already queued when the chain
     * before it ended; the time from that end to its start. This is the
     * driver's own latency (interrupt or poll, then the start). */
    mw_u64 GapCount, GapTicks, GapMaxTicks;
    mw_u32 GapHist[MW_PERF_BUCKETS];
    /* Who collected finished chains, and the poll timer's runs. */
    mw_u32 ReapIsr, ReapPoll, PollRuns, PollMs;
    /* A completion noticed (DMA_COMPLETED) to the DPC that tells dxgkrnl. */
    mw_u64 DpcCount, DpcTicks, DpcMaxTicks;
    /* Submissions already queued in the driver when one arrived, summed. */
    mw_u64 QueueDepthSum;
    mw_u32 QueueDepthMax, Submissions;
    /* The GPU clock as the CRU says now: CRU_CLKSEL_CON165, raw (mux
     * [7:5] 0 GPLL, 1 CPLL, 2 AUPLL, 3 SPLL; divider - 1 in [4:0]), and the
     * rate it gives with GPLL 1188, CPLL 1000 and SPLL 702 MHz; 0 when the
     * parent's rate is not known here. ~0 for both: not read. */
    mw_u32 ClkSel165, GpuHz;
    /* TSADC channels 0-5 (0 centre, 1 big cores, 2 little cores, 3 GPU by
     * the TRM, 4 NPU, 5 DDR): the raw 9-bit codes and the temperature in
     * millidegrees C by mainline's table, WITHOUT the per-chip OTP trim
     * (a few degrees). Read only with the setting PerfTemp 1; TempValid 0
     * otherwise, or when a code is not a temperature. */
    mw_u32 TempValid;                   /* bit n: channel n read and in the table */
    mw_u32 TsadcCode[6];
    mw_s32 TempMilliC[6];
    mw_u32 Reserved0;
    /* The last MW_PERF_RING chains: RingNext chains have been recorded
     * since the start; the newest is Ring[(RingNext - 1) % MW_PERF_RING]. */
    mw_u32 RingNext, Reserved1;
    MW_PERF_JOB Ring[MW_PERF_RING];
} MW_ESCAPE_PERF_DATA;
