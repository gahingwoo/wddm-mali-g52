/* SPDX-License-Identifier: GPL-2.0 */
/*
 * maliwddm's WDDM 2.x work (docs/WDDM2.md). Kept in a header of its own so
 * that the 2.x changes touch the shared files (adapter.c, command.c,
 * maliwddm.h) in as few lines as possible while other branches change them.
 */
#pragma once

#include "wddm2plan.h"

#ifndef MW_BUILD
#define MW_BUILD 0
#endif

/* version.c: what DriverEntry decided. */
extern ULONG MwWddmVersion;     /* DXGK_DRIVERCAPS::WDDMVersion */
extern ULONG MwDdiInterface;    /* DRIVER_INITIALIZATION_DATA::Version */
extern ULONG MwWddmProbe;       /* the 2.x version in effect (WddmProbe or the default), 0 at 1.3 */
extern BOOLEAN MwWddmDefault;   /* it is the default: no WddmProbe value */
extern ULONG MwWddmProbeResult;

#define MW_PROBE_NONE       MW2_RESULT_NONE
#define MW_PROBE_ACTIVE     MW2_RESULT_ACTIVE
#define MW_PROBE_FELL_BACK  MW2_RESULT_FELL_BACK
#define MW_PROBE_UNKNOWN    MW2_RESULT_UNKNOWN
#define MW_PROBE_NO_KEY     MW2_RESULT_NO_KEY
#define MW_PROBE_ARM_FAILED MW2_RESULT_ARM_FAILED
#define MW_PROBE_STAYED_BACK MW2_RESULT_STAYED_BACK

/* What the 2.x driver says about itself (wddm2plan.h), set in DriverEntry. */
extern MW2_PLAN MwPlan;
extern ULONG MwWddm2Budget;     /* Wddm2Budget: segment budget flags at 2.x (default 1) */
extern ULONG MwWddm2LocalBudget; /* Wddm2LocalBudget: the local budget group, not non-local (default 1) */

void MwVersionInit(PUNICODE_STRING RegistryPath);
void MwVersionRendered(void);
void MwVersionConfirmCheck(ULONG Renders, ULONG Presents, ULONG Seconds, BOOLEAN Stuck, ULONG Tdrs);
extern ULONG MwWddmFallbackReason;
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

/* probe.c: the boot journal, Services\maliwddm\Probe\Boot<n>. One REG_SZ
 * per event, each flushed to disk before the DDI it names runs, from
 * DriverEntry on and only while a probe is in effect. PASSIVE_LEVEL only;
 * elsewhere a call does nothing (the watchdog's counters cover it). */
void MwBcInit(PUNICODE_STRING RegistryPath, ULONG BootNumber, ULONG Max);
void MwBc(PCSTR Fmt, ...);
BOOLEAN MwBcOn(void);
void MwBcClose(void);
void MwBcEnd(void);

/* probe.c: what happens above PASSIVE_LEVEL (the ISR, DPCs, the callbacks
 * into dxgkrnl made at interrupt level), kept in memory at any IRQL and
 * copied into the journal by the watchdog once a second. The first
 * MW_RING_PER_CODE of each kind per boot. */
#define MW_RING_ISR         0   /* a = ours */
#define MW_RING_DPC         1
#define MW_RING_DMA_DONE    2   /* a = node, b = fence */
#define MW_RING_PREEMPTED   3   /* a = node, b = preemption fence */
#define MW_RING_VSYNC       4   /* a = target, b = address */
#define MW_RING_CODES       5
void MwRing(ULONG Code, ULONG64 A, ULONG64 B);

/* wddm2x.c: the 2.x shapes (Services\maliwddm\Parameters\Wddm2Shape). */
#define MW_SHAPE_PHYSICAL   0   /* physical mode, a copy node as the paging node */
#define MW_SHAPE_GPUMMU     1   /* the same and GpuMmu (VA caps, page tables) */
extern ULONG MwWddm2Shape;
extern ULONG MwGpuMmuPtSegment;         /* GpuMmuPtSegment: segment of the page tables (default 1) */
extern ULONG MwPagingMarker;            /* PagingMarker: 8 bytes into a paging DMA buffer (default 1) */
extern ULONG MwWddm2AcquireHandle;      /* Wddm2AcquireHandle: AcquireHandleData at 2.x (default 1) */
extern ULONG MwWddm2RotOffset;          /* Wddm2RotOffset: Offset0 on every path at 2.x (default 1) */
extern ULONG MwWddm2PowerDdis;          /* Wddm2PowerDdis: CalibrateGpuClock, SetStablePowerState at 2.x (default 1) */

#define MW_COPY_NODE            MW2_NODE_COPY
#define MW_GPUMMU_INDEX_BITS    MW2_PT_INDEX_BITS

ULONG MwNodeCount(void);
BOOLEAN MwGpuMmu(void);
NTSTATUS MwCopyNodeSubmit(MW_ADAPTER *A, ULONG Fence);
ULONG MwCopyNodeFence(void);
NTSTATUS MwCopyNodePreempt(MW_ADAPTER *A, ULONG PreemptionFence);
void Mw2xDriverCaps(MW_ADAPTER *A, DXGK_DRIVERCAPS *Caps, UINT Size);
NTSTATUS MwQueryGpuMmuCaps(const DXGKARG_QUERYADAPTERINFO *Info);
NTSTATUS MwQueryPageTableLevelDesc(const DXGKARG_QUERYADAPTERINFO *Info);
NTSTATUS MwQuery3xCaps(const DXGKARG_QUERYADAPTERINFO *Info);
DXGKDDI_CREATEPROCESS MwCreateProcess;
DXGKDDI_DESTROYPROCESS MwDestroyProcess;
DXGKDDI_SETROOTPAGETABLE MwSetRootPageTable;
DXGKDDI_GETROOTPAGETABLESIZE MwGetRootPageTableSize;
DXGKDDI_SUBMITCOMMANDVIRTUAL MwSubmitCommandVirtual;
DXGKDDI_CALIBRATEGPUCLOCK MwCalibrateGpuClock;
DXGKDDI_SETSTABLEPOWERSTATE MwSetStablePowerState;
BOOLEAN Mw2xPagingOp(MW_ADAPTER *A, DXGKARG_BUILDPAGINGBUFFER *Arg);
NTSTATUS Mw2xPagingFill(DXGKARG_BUILDPAGINGBUFFER *Arg);
void Mw2xCounters(ULONG *Out, ULONG Count);
void Mw2xRotationSupport(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT *R);
void Mw2xBcQai(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info, NTSTATUS St);

/* memory.c */
MW_ALLOCATION *MwAllocFromHandle(MW_ADAPTER *A, D3DKMT_HANDLE H);
NTSTATUS MwQuerySegment4(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info);
