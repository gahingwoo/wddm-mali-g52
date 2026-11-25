/*
 * vdpu383_import.h - the kernel interface between vdpu383.sys and the
 * display driver (wddm-mali-g52's maliwddm.sys) for zero-copy decoding:
 * the decoder writes the picture straight into the pages of a D3D11
 * texture, mapped through the decoder's own IOMMU.
 *
 * Kernel mode only; shared by both drivers (wddm-mali-g52 keeps a copy in
 * video/vdpu383/, identical, as for vdpu383_ioctl.h). Include after
 * <ntddk.h>.
 *
 * How a texture gets to the decoder:
 *
 *   1. The display driver's user-mode part (in the player's process) asks
 *      maliwddm, by a private escape, to export one of its allocations.
 *      maliwddm resolves the allocation handle in that process's context,
 *      checks the allocation is resident (its pages known), and returns a
 *      token: a 64-bit number naming the export, valid for that process
 *      only.
 *   2. The same process hands the token to vdpu383.sys (IOCTL_VDPU383_IMPORT).
 *      vdpu383.sys asks maliwddm for the pages through the callback object
 *      named below (ExCreateCallback / ExNotifyCallback; maliwddm registers
 *      the callback while its adapter runs). maliwddm checks the token and
 *      the process and copies the page frame numbers. vdpu383.sys enters
 *      them in its IOMMU at a free run of IO addresses.
 *   3. When the pages stop being the allocation's (VidMm unmaps it from the
 *      aperture, the allocation is destroyed, the adapter stops), maliwddm
 *      calls the importer's revoke routine first, synchronously. vdpu383.sys
 *      waits for a decode in progress, clears the mapping and marks the
 *      import dead: a decode naming its addresses is refused from then on
 *      (VDPU383_RUN_REVOKED). The user-mode part exports again and
 *      refreshes the import (same IO addresses, the new pages).
 *   4. The importer releases the import (VDPU383_IMPORT_OP_RELEASE) before
 *      it forgets it; after the release returns, maliwddm never calls the
 *      revoke routine for it again.
 *
 * Lock rules, both sides: maliwddm calls revoke routines holding only its
 * own revoke lock (an event, PASSIVE_LEVEL), never the lock that guards its
 * export list; a release takes the revoke lock, so it returns only after any
 * revoke in progress is over. vdpu383.sys may hold its decode gate while it
 * imports (maliwddm then takes only the list lock), and holds nothing of
 * its own while it releases.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_IMPORT_H
#define VDPU383_IMPORT_H

#define VDPU383_IMPORT_CALLBACK_NAME    L"\\Callback\\MaliwddmVdecExport"
#define VDPU383_IMPORT_VERSION          1

/* Argument2 of ExNotifyCallback */
#define VDPU383_IMPORT_OP_IMPORT        1
#define VDPU383_IMPORT_OP_RELEASE       2

/* Called by the exporter, at PASSIVE_LEVEL, when the pages of an import go
 * away; must not return before the decoder has stopped using them. */
typedef VOID VDPU383_REVOKE_ROUTINE(_In_ PVOID Context, _In_ ULONG64 Cookie);
typedef VDPU383_REVOKE_ROUTINE *PVDPU383_REVOKE_ROUTINE;

/* Argument1 of ExNotifyCallback. The caller sets Status to
 * STATUS_NOT_FOUND first: with nobody registered it stays that way. */
typedef struct _VDPU383_IMPORT_REQUEST {
    ULONG Version;                      /* VDPU383_IMPORT_VERSION */
    ULONG Size;                         /* sizeof(VDPU383_IMPORT_REQUEST) */
    ULONG64 Token;                      /* from the exporter's escape */
    HANDLE ProcessId;                   /* the process the token was given to */
    ULONG64 Cookie;                     /* the importer's name for the import */
    /* IMPORT: in */
    PVDPU383_REVOKE_ROUTINE Revoke;
    PVOID RevokeContext;
    PPFN_NUMBER Pfns;                   /* MaxPages entries, nonpaged */
    ULONG MaxPages;
    /* IMPORT: out */
    ULONG Pages;                        /* the allocation's pages, all of them */
    NTSTATUS Status;
} VDPU383_IMPORT_REQUEST, *PVDPU383_IMPORT_REQUEST;

#endif /* VDPU383_IMPORT_H */
