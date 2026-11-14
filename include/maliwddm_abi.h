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
#else
typedef unsigned __int32 mw_u32;
typedef unsigned __int64 mw_u64;
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

/* Scan-out of primaries by the display controller, on and off.
 * Mode: 1 on, 0 off (flips are copied), 2 off and the IOMMU off too,
 * 3 only report. */
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
 * Parent: 0 GPLL (1188 MHz), 1 CPLL (1000 MHz); Div 1 to 32.
 * Mode: 0 report only; 1 set now; 2 set now and at every start; 3 forget
 * the stored setting (the firmware's rate is kept from the next start). */
typedef struct _MW_ESCAPE_GPUCLK_DATA {
    mw_u32 Code;                        /* MW_ESCAPE_GPUCLK */
    mw_u32 Mode;
    mw_u32 Parent, Div;
    mw_u32 Applied;                     /* what this driver last wrote, or 0: (Parent << 8) | Div */
    mw_u32 Stored;                      /* the setting kept for the next start, same form, or 0 */
} MW_ESCAPE_GPUCLK_DATA;
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
