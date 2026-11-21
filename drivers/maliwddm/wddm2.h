/* SPDX-License-Identifier: GPL-2.0 */
/*
 * maliwddm's WDDM 2.x work (docs/WDDM2.md). Kept in a header of its own so
 * that the 2.x changes touch the shared files (adapter.c, command.c,
 * maliwddm.h) in as few lines as possible while other branches change them.
 */
#pragma once

#ifndef MW_BUILD
#define MW_BUILD 0
#endif

/* version.c: what DriverEntry decided. */
extern ULONG MwWddmVersion;     /* DXGK_DRIVERCAPS::WDDMVersion */
extern ULONG MwDdiInterface;    /* DRIVER_INITIALIZATION_DATA::Version */
extern ULONG MwWddmProbe;       /* the WddmProbe value in effect, 0 for none */
extern ULONG MwWddmProbeResult;

#define MW_PROBE_NONE       0   /* no WddmProbe: WDDM 1.3 */
#define MW_PROBE_ACTIVE     1   /* WddmProbe in effect */
#define MW_PROBE_FELL_BACK  2   /* the last boot with it never confirmed: 1.3 */
#define MW_PROBE_UNKNOWN    3   /* a value that is no WDDM version: 1.3 */
#define MW_PROBE_NO_KEY     4   /* no service key path: 1.3 */

void MwVersionInit(PUNICODE_STRING RegistryPath);
void MwVersionRendered(void);
DXGKDDI_COLLECTDIAGNOSTICINFO MwCollectDiagnosticInfo;
NTSTATUS MwQueryPhysicalAdapterCaps(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info);

/* version.c: the probe's other settings (Services\maliwddm\Parameters). */
extern ULONG MwProbeWatchdog;           /* ProbeWatchdog: the watchdog on 1.3 too */
extern ULONG MwProbeUart;               /* ProbeUart: watchdog lines on the debug UART */
extern ULONG64 MwProbeUartBase;         /* ProbeUartBase: default UART0, 0x2AD40000 */

/* probe.c: breadcrumbs that survive a hang. One slot per DDI wrapper. */
typedef struct _MW_DDI_SLOT {
    PCWSTR Name;
    volatile LONG Registered;
    volatile LONG Enters, Exits, Fails;
    volatile LONG LastEnterSeq;
    volatile LONG LastStatus;
    volatile LONG64 LastEnterTick;
} MW_DDI_SLOT;

void MwDdiEnter(MW_DDI_SLOT *S);
NTSTATUS MwDdiExit(MW_DDI_SLOT *S, NTSTATUS St);
void MwProbePagingOp(ULONG Op, BOOLEAN Refused);
void MwProbeMirror(HANDLE Sub, PUNICODE_STRING Name, ULONG Value);
void MwProbeStart(MW_ADAPTER *A);
void MwProbeStop(MW_ADAPTER *A);

/* wddm2x.c: the 2.x shapes (Services\maliwddm\Parameters\Wddm2Shape). */
#define MW_SHAPE_PHYSICAL   0   /* physical mode, a copy node as the paging node */
#define MW_SHAPE_GPUMMU     1   /* the same and GpuMmu (VA caps, page tables) */
extern ULONG MwWddm2Shape;
extern ULONG MwGpuMmuPtSegment;         /* GpuMmuPtSegment: segment of the page tables (default 1) */
extern ULONG MwPagingMarker;            /* PagingMarker: 8 bytes into a paging DMA buffer (default 1) */

#define MW_COPY_NODE            1
#define MW_GPUMMU_LEVELS        4
#define MW_GPUMMU_INDEX_BITS    9
#define MW_GPUVA_INTERNAL_START 0x100000000ULL          /* 4 GB */
#define MW_GPUVA_INTERNAL_END   0x7FFFFFFFFULL          /* 2^35 - 1 */

ULONG MwNodeCount(void);
BOOLEAN MwGpuMmu(void);
NTSTATUS MwCopyNodeSubmit(MW_ADAPTER *A, ULONG Fence);
ULONG MwCopyNodeFence(void);
void Mw2xDriverCaps(MW_ADAPTER *A, DXGK_DRIVERCAPS *Caps, UINT Size);
NTSTATUS MwQueryGpuMmuCaps(const DXGKARG_QUERYADAPTERINFO *Info);
NTSTATUS MwQueryPageTableLevelDesc(const DXGKARG_QUERYADAPTERINFO *Info);
NTSTATUS MwQuery3xCaps(const DXGKARG_QUERYADAPTERINFO *Info);
DXGKDDI_CREATEPROCESS MwCreateProcess;
DXGKDDI_DESTROYPROCESS MwDestroyProcess;
DXGKDDI_SETROOTPAGETABLE MwSetRootPageTable;
DXGKDDI_GETROOTPAGETABLESIZE MwGetRootPageTableSize;
DXGKDDI_SUBMITCOMMANDVIRTUAL MwSubmitCommandVirtual;
BOOLEAN Mw2xPagingOp(MW_ADAPTER *A, DXGKARG_BUILDPAGINGBUFFER *Arg);
NTSTATUS Mw2xPagingFill(DXGKARG_BUILDPAGINGBUFFER *Arg);
void Mw2xCounters(ULONG *Out, ULONG Count);

/* memory.c */
NTSTATUS MwQuerySegment4(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info);
