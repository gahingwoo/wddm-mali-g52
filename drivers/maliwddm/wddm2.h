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

/* memory.c */
NTSTATUS MwQuerySegment4(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info);
