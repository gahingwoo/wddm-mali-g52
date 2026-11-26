// SPDX-License-Identifier: GPL-2.0
/*
 * Zero-copy decode (docs/VIDEO-DECODE.md, V3b): the pages of an allocation
 * handed to the video decoder's driver, vdpu383.sys, which maps them in the
 * decoder's own IOMMU so the VDPU383 writes the picture straight into the
 * application's NV12 texture. The protocol between the two drivers is
 * video/vdpu383/vdpu383_import.h (woa-rk3576's, copied unchanged).
 *
 *   MW_ESCAPE_VDEC_EXPORT   the UMD, in the player's process: export this
 *                           allocation. The pages are those VidMm gave
 *                           MAP_APERTURE_SEGMENT; a token comes back, for
 *                           this process only.
 *   IMPORT (callback)       vdpu383.sys, in the same process's context: the
 *                           pages of that token, and the routine to call
 *                           when they go away.
 *   MwVdecGone              the pages are no longer the allocation's
 *                           (UNMAP_APERTURE_SEGMENT, a new MAP, the
 *                           allocation destroyed, the adapter stopping):
 *                           the importer's revoke routine runs first, and it
 *                           returns only when the decoder no longer uses
 *                           them; then the export is gone.
 *   RELEASE (callback)      the importer lets go of an import; no revoke
 *                           for it after this returns.
 *
 * Nothing here runs unless UmdVideo and UmdVideoZeroCopy (default 1) are on
 * at the adapter's start: without the callback object vdpu383.sys finds no
 * exporter and the UMD copies, as before.
 *
 * The GPU and the decoder are both non-coherent with the CPU: the CPU's view
 * of these allocations is uncached (the segment is not cache coherent), the
 * decoder writes memory, and every job chain starts with an L2 clean and
 * invalidate (maligpu.c), so the Mali reads what the decoder wrote. The UMD
 * waits for the GPU to be done with a texture before the decoder writes it.
 */
#include "maliwddm.h"
#include "wddm2.h"
#include "../../video/vdpu383/vdpu383_import.h"

typedef struct _MW_VDEC_EXPORT {
    LIST_ENTRY Link;
    MW_ALLOCATION *Alloc;
    ULONG64 Token;
    HANDLE Pid;
    ULONG Pages;
    PPFN_NUMBER Pfns;                   /* a copy, Pages entries */
    /* the importer, while there is one */
    PVDPU383_REVOKE_ROUTINE Revoke;
    PVOID RevokeContext;
    ULONG64 Cookie;
} MW_VDEC_EXPORT;

static CALLBACK_FUNCTION VdecCallback;

/*
 * Why an export or an import was refused, for a board with no debugger:
 * VdecWhy and the values below in the log key (MwLog), for the first 16
 * refusals and every 64th after, and at stop. docs/VIDEO-DECODE.md.
 */
#define MW_VDEC_WHY_OFF             0x01    /* export: zero copy not on at the adapter's start */
#define MW_VDEC_WHY_HANDLE          0x02    /* export: the allocation handle is not resolved */
#define MW_VDEC_WHY_NOMEM           0x03    /* export: no pool */
#define MW_VDEC_WHY_NOT_RESIDENT    0x04    /* export: not mapped, or not all of it (Pages, MdlPages) */
#define MW_VDEC_WHY_OTHER_PROCESS   0x05    /* export: another process exported this allocation */
#define MW_VDEC_WHY_BAD_REQUEST     0x11    /* import: version (Pid) or size (ExportPid) differ */
#define MW_VDEC_WHY_NO_PFNS         0x12    /* import: no PFN array or no revoke routine */
#define MW_VDEC_WHY_NO_EXPORT       0x13    /* import: no export with this token (it ended, VdecLastGone) */
#define MW_VDEC_WHY_PID             0x14    /* import: the token is another process's (Pid, ExportPid) */
#define MW_VDEC_WHY_IMPORTED        0x15    /* import: imported already by another import */
#define MW_VDEC_WHY_TOO_SMALL       0x16    /* import: MaxPages (Pages) under the export's (ExportPages) */

/* PASSIVE_LEVEL, no lock held. */
static void VdecNote(MW_ADAPTER *A, ULONG Why, NTSTATUS St, ULONG Pid, ULONG ExportPid, ULONG Pages,
                     ULONG ExportPages)
{
    ULONG n;

    A->VdecWhy = Why;
    A->VdecWhyStatus = (ULONG)St;
    A->VdecWhyPid = Pid;
    A->VdecWhyExportPid = ExportPid;
    A->VdecWhyPages = Pages;
    A->VdecWhyExportPages = ExportPages;
    n = (ULONG)InterlockedIncrement((volatile LONG *)&A->VdecNotes);
    if (n > 16 && n % 64)
        return;
    MwLog(A, L"VdecLastWhy", Why);
    MwLog(A, L"VdecLastStatus", (ULONG)St);
    MwLog(A, L"VdecLastPid", Pid);
    MwLog(A, L"VdecLastExportPid", ExportPid);
    MwLog(A, L"VdecLastPages", Pages);
    MwLog(A, L"VdecLastExportPages", ExportPages);
    MwLog(A, L"VdecLastGone", A->VdecLastGone);
    MwLog(A, L"VdecEndedUnimported", A->VdecEndedUnimported);
    MwLog(A, L"VdecExportsMade", A->VdecExportsMade);
    MwLog(A, L"VdecImports", A->VdecImports);
    MwLog(A, L"VdecRefused", A->VdecRefused);
}

void MwVdecInit(MW_ADAPTER *A)
{
    MwVdecKmdInit(A);
    ExInitializeFastMutex(&A->VdecLock);
    KeInitializeEvent(&A->VdecRevoke, SynchronizationEvent, TRUE);
    InitializeListHead(&A->VdecExports);
}

static void RevokeLock(MW_ADAPTER *A)
{
    (void)KeWaitForSingleObject(&A->VdecRevoke, Executive, KernelMode, FALSE, NULL);
}

static void RevokeUnlock(MW_ADAPTER *A)
{
    KeSetEvent(&A->VdecRevoke, IO_NO_INCREMENT, FALSE);
}

static void FreeExport(MW_VDEC_EXPORT *E)
{
    if (E->Pfns != NULL)
        ExFreePoolWithTag(E->Pfns, MW_TAG);
    ExFreePoolWithTag(E, MW_TAG);
}

/* The callback object vdpu383.sys calls; from StartDevice, when UmdVideo
 * and UmdVideoZeroCopy are on. */
void MwVdecStart(MW_ADAPTER *A)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    PCALLBACK_OBJECT cb = NULL;
    NTSTATUS st;

    /* the decoder's calls through maliwddm (vdecproxy.c) */
    MwVdecKmdStart(A);
    if (A->VdecOn)
        return;
    if (MwSetting(A, L"UmdVideo", 0) == 0 || MwSetting(A, L"UmdVideoZeroCopy", 1) == 0) {
        MwLog(A, L"VdecExport", 0);
        return;
    }
    RtlInitUnicodeString(&name, VDPU383_IMPORT_CALLBACK_NAME);
    /* OBJ_PERMANENT: ExCreateCallback closes the handle it opened, and a
     * temporary object loses its name when its last handle closes, so
     * without it vdpu383.sys never finds the object by name (board,
     * 2026-11-26: every import failed with STATUS_NOT_FOUND, stage 5)
     * while our pointer and registration still worked. A permanent object
     * keeps the name; after our ExUnregisterCallback it has no callback
     * and vdpu383.sys's ExNotifyCallback finds nobody, which it treats as
     * a refused import. */
    InitializeObjectAttributes(&oa, &name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_PERMANENT, NULL, NULL);
    st = ExCreateCallback(&cb, &oa, TRUE, FALSE);
    if (!NT_SUCCESS(st)) {
        MwLog(A, L"VdecExport", (ULONG)st);
        return;
    }
    A->VdecRegistration = ExRegisterCallback(cb, VdecCallback, A);
    if (A->VdecRegistration == NULL) {
        ObDereferenceObject(cb);
        MwLog(A, L"VdecExport", (ULONG)STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    A->VdecCallback = cb;
    A->VdecOn = TRUE;
    MwLog(A, L"VdecExport", 1);
}

/* The export ends: its importer's revoke routine first, then the export is
 * freed. Called with VdecRevoke held and the export already out of the list
 * and off its allocation (under VdecLock), so no import or release can
 * reach it any more. */
static void EndExport(MW_ADAPTER *A, MW_VDEC_EXPORT *E)
{
    if (E->Revoke != NULL) {
        E->Revoke(E->RevokeContext, E->Cookie);
        InterlockedIncrement((volatile LONG *)&A->VdecRevokes);
    }
    FreeExport(E);
}

/* The allocation's pages stop being what was exported (memory.c: right
 * after the MmuLock section of an unmap or a map, and before destruction;
 * VidMm keeps the old pages until the paging operation returns, so the
 * decoder lets go of them in time). PASSIVE_LEVEL, no lock held. */
void MwVdecGone(MW_ADAPTER *A, MW_ALLOCATION *Alloc, ULONG Why)
{
    MW_VDEC_EXPORT *e;
    BOOLEAN unimported = FALSE;

    if (!A->VdecOn || Alloc == NULL || Alloc->Vdec == NULL)
        return;
    RevokeLock(A);
    ExAcquireFastMutex(&A->VdecLock);
    e = Alloc->Vdec;
    if (e != NULL) {
        RemoveEntryList(&e->Link);
        Alloc->Vdec = NULL;
    }
    ExReleaseFastMutex(&A->VdecLock);
    if (e != NULL) {
        /* MW_VDEC_GONE_*, and 0x100 when an import had the pages */
        A->VdecLastGone = Why | (e->Revoke != NULL ? 0x100 : 0);
        unimported = e->Revoke == NULL;
        EndExport(A, e);
    }
    RevokeUnlock(A);
    /* an export that ended before vdpu383.sys took it: an import with its
     * token is refused (MW_VDEC_WHY_NO_EXPORT) */
    if (unimported) {
        ULONG n = (ULONG)InterlockedIncrement((volatile LONG *)&A->VdecEndedUnimported);
        if (n <= 16 || n % 64 == 0) {
            MwLog(A, L"VdecEndedUnimported", n);
            MwLog(A, L"VdecLastGone", A->VdecLastGone);
        }
    }
}

/* StopDevice, RemoveDevice: every export ends (the importers let go of the
 * pages), then nobody can call in any more. Safe to repeat. */
void MwVdecStop(MW_ADAPTER *A)
{
    /* first the sessions: vdpu383.sys releases their imports through the
     * callback, which must still be registered */
    MwVdecKmdStop(A);
    if (!A->VdecOn)
        return;
    MwLog(A, L"VdecExportsMade", A->VdecExportsMade);
    MwLog(A, L"VdecImports", A->VdecImports);
    MwLog(A, L"VdecRevokes", A->VdecRevokes);
    MwLog(A, L"VdecRefused", A->VdecRefused);
    MwLog(A, L"VdecEndedUnimported", A->VdecEndedUnimported);
    if (A->VdecNotes != 0)
        MwLog(A, L"VdecLastWhy", A->VdecWhy);
    for (;;) {
        MW_VDEC_EXPORT *e = NULL;

        RevokeLock(A);
        ExAcquireFastMutex(&A->VdecLock);
        if (!IsListEmpty(&A->VdecExports)) {
            e = CONTAINING_RECORD(RemoveHeadList(&A->VdecExports), MW_VDEC_EXPORT, Link);
            if (e->Alloc != NULL)
                e->Alloc->Vdec = NULL;
        }
        ExReleaseFastMutex(&A->VdecLock);
        if (e != NULL)
            EndExport(A, e);
        RevokeUnlock(A);
        if (e == NULL)
            break;
    }
    /* waits for callbacks in progress */
    ExUnregisterCallback(A->VdecRegistration);
    ObDereferenceObject(A->VdecCallback);
    A->VdecRegistration = NULL;
    A->VdecCallback = NULL;
    A->VdecOn = FALSE;
}

/* Under VdecLock. */
static MW_VDEC_EXPORT *FindExport(MW_ADAPTER *A, ULONG64 Token)
{
    PLIST_ENTRY l;

    for (l = A->VdecExports.Flink; l != &A->VdecExports; l = l->Flink) {
        MW_VDEC_EXPORT *e = CONTAINING_RECORD(l, MW_VDEC_EXPORT, Link);
        if (e->Token == Token)
            return e;
    }
    return NULL;
}

/* vdpu383.sys, through ExNotifyCallback (vdpu383_import.h). It may hold its
 * decode gate: nothing here waits for anything but VdecLock (IMPORT), or
 * VdecRevoke (RELEASE, which vdpu383.sys makes holding nothing). */
static VOID VdecCallback(PVOID Context, PVOID Argument1, PVOID Argument2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    PVDPU383_IMPORT_REQUEST r = (PVDPU383_IMPORT_REQUEST)Argument1;
    ULONG op = (ULONG)(ULONG_PTR)Argument2;
    MW_VDEC_EXPORT *e;

    if (r == NULL)
        return;
    if (r->Version != VDPU383_IMPORT_VERSION || r->Size < sizeof(*r)) {
        InterlockedIncrement((volatile LONG *)&a->VdecRefused);
        VdecNote(a, MW_VDEC_WHY_BAD_REQUEST, STATUS_REVISION_MISMATCH, r->Version, r->Size, 0, (ULONG)sizeof(*r));
        return;
    }
    if (op == VDPU383_IMPORT_OP_IMPORT) {
        NTSTATUS st = STATUS_NOT_FOUND;
        ULONG why = 0, exportPid = 0, exportPages = 0;

        if (r->Pfns == NULL || r->Revoke == NULL) {
            r->Status = STATUS_INVALID_PARAMETER;
            InterlockedIncrement((volatile LONG *)&a->VdecRefused);
            VdecNote(a, MW_VDEC_WHY_NO_PFNS, r->Status, (ULONG)(ULONG_PTR)r->ProcessId, 0, r->MaxPages, 0);
            return;
        }
        ExAcquireFastMutex(&a->VdecLock);
        e = FindExport(a, r->Token);
        if (e != NULL) {
            exportPid = (ULONG)(ULONG_PTR)e->Pid;
            exportPages = e->Pages;
        }
        if (e == NULL) {
            st = STATUS_NOT_FOUND;              /* no such export (any more) */
            why = MW_VDEC_WHY_NO_EXPORT;
        } else if (e->Pid != r->ProcessId) {
            st = STATUS_NOT_FOUND;              /* not this process's token */
            why = MW_VDEC_WHY_PID;
        } else if (e->Revoke != NULL && e->Cookie != r->Cookie) {
            st = STATUS_SHARING_VIOLATION;      /* imported already, by another import */
            why = MW_VDEC_WHY_IMPORTED;
        } else if (r->MaxPages < e->Pages) {
            st = STATUS_BUFFER_TOO_SMALL;
            why = MW_VDEC_WHY_TOO_SMALL;
        } else {
            RtlCopyMemory(r->Pfns, e->Pfns, (SIZE_T)e->Pages * sizeof(PFN_NUMBER));
            r->Pages = e->Pages;
            e->Revoke = r->Revoke;
            e->RevokeContext = r->RevokeContext;
            e->Cookie = r->Cookie;
            st = STATUS_SUCCESS;
        }
        ExReleaseFastMutex(&a->VdecLock);
        r->Status = st;
        if (NT_SUCCESS(st)) {
            if (InterlockedIncrement((volatile LONG *)&a->VdecImports) <= 4)
                MwLog(a, L"VdecImports", a->VdecImports);
        } else {
            InterlockedIncrement((volatile LONG *)&a->VdecRefused);
            VdecNote(a, why, st, (ULONG)(ULONG_PTR)r->ProcessId, exportPid, r->MaxPages, exportPages);
        }
    } else if (op == VDPU383_IMPORT_OP_RELEASE) {
        /* After a revoke in progress (which holds VdecRevoke) is over. */
        RevokeLock(a);
        ExAcquireFastMutex(&a->VdecLock);
        e = FindExport(a, r->Token);
        if (e != NULL && e->Revoke != NULL && e->Cookie == r->Cookie) {
            e->Revoke = NULL;
            e->RevokeContext = NULL;
            e->Cookie = 0;
        }
        ExReleaseFastMutex(&a->VdecLock);
        RevokeUnlock(a);
        r->Status = STATUS_SUCCESS;
    }
}

/* MW_ESCAPE_VDEC_EXPORT, in the calling process's context. */
NTSTATUS MwEscapeVdecExport(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_VDEC_EXPORT_DATA *D)
{
    MW_ALLOCATION *alloc;
    MW_VDEC_EXPORT *e, *n;
    HANDLE pid = PsGetCurrentProcessId();
    NTSTATUS st = STATUS_SUCCESS;
    ULONG why = 0, otherPid = 0, mdlPages = 0;
    BOOLEAN made = FALSE;

    D->Token = 0;
    D->Pages = 0;
    if (!A->VdecOn) {
        if (InterlockedIncrement((volatile LONG *)&A->VdecRefused) <= 4)
            MwLog(A, L"VdecLastWhy", MW_VDEC_WHY_OFF);
        return STATUS_NOT_SUPPORTED;
    }
    if (Esc->hDevice == NULL)
        return STATUS_INVALID_PARAMETER;
    alloc = MwAllocFromHandle(A, (D3DKMT_HANDLE)D->hAllocation);
    if (alloc == NULL) {
        InterlockedIncrement((volatile LONG *)&A->VdecRefused);
        VdecNote(A, MW_VDEC_WHY_HANDLE, STATUS_INVALID_HANDLE, (ULONG)(ULONG_PTR)pid, 0, 0, 0);
        return STATUS_INVALID_HANDLE;
    }

    /* Made outside the locks; thrown away if the allocation has an export
     * already. */
    n = (MW_VDEC_EXPORT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*n), MW_TAG);
    if (n != NULL) {
        n->Pfns = (PPFN_NUMBER)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)alloc->Pages * sizeof(PFN_NUMBER), MW_TAG);
        if (n->Pfns == NULL) {
            FreeExport(n);
            n = NULL;
        }
    }
    if (n == NULL) {
        InterlockedIncrement((volatile LONG *)&A->VdecRefused);
        VdecNote(A, MW_VDEC_WHY_NOMEM, STATUS_NO_MEMORY, (ULONG)(ULONG_PTR)pid, 0, alloc->Pages, 0);
        return STATUS_NO_MEMORY;
    }

    /*
     * MmuLock, across the copy and the insertion: Mapped, Mdl and MdlPages
     * change only under it (memory.c), and memory.c ends an export
     * (MwVdecGone) after the MmuLock section of every map and unmap. So the
     * export either is in place before such a section, and that Gone ends
     * it, or comes after it and sees the pages as they now are.
     */
    ExAcquireFastMutex(&A->MmuLock);
    if (!alloc->Mapped || alloc->Mdl == NULL || alloc->MdlPages != alloc->Pages) {
        st = STATUS_NOT_SUPPORTED;              /* not resident, or not all of it */
        why = MW_VDEC_WHY_NOT_RESIDENT;
        mdlPages = alloc->Mapped ? alloc->MdlPages : 0;
        goto out;
    }
    ExAcquireFastMutex(&A->VdecLock);
    e = alloc->Vdec;
    if (e != NULL && e->Pid != pid) {
        st = STATUS_ACCESS_DENIED;              /* another process's export of a shared allocation */
        why = MW_VDEC_WHY_OTHER_PROCESS;
        otherPid = (ULONG)(ULONG_PTR)e->Pid;
    } else if (e == NULL) {
        LARGE_INTEGER t = KeQueryPerformanceCounter(NULL);

        RtlCopyMemory(n->Pfns, MmGetMdlPfnArray(alloc->Mdl) + alloc->MdlPage,
                      (SIZE_T)alloc->Pages * sizeof(PFN_NUMBER));
        n->Alloc = alloc;
        n->Pid = pid;
        n->Pages = alloc->Pages;
        /* unique by the sequence, not guessable from it */
        n->Token = (((ULONG64)InterlockedIncrement64(&A->VdecSeq) << 24) ^
                    ((ULONG64)t.QuadPart & 0xFFFFFFull) ^ ((ULONG64)(ULONG_PTR)n << 40)) | 1;
        InsertTailList(&A->VdecExports, &n->Link);
        alloc->Vdec = n;
        e = n;
        n = NULL;
        made = ++A->VdecExportsMade <= 4;
    }
    if (NT_SUCCESS(st)) {
        D->Token = e->Token;
        D->Pages = e->Pages;
    }
    ExReleaseFastMutex(&A->VdecLock);
out:
    ExReleaseFastMutex(&A->MmuLock);
    if (n != NULL)
        FreeExport(n);
    if (!NT_SUCCESS(st)) {
        InterlockedIncrement((volatile LONG *)&A->VdecRefused);
        /* not resident: Pages the allocation's, ExportPages what is mapped */
        VdecNote(A, why, st, (ULONG)(ULONG_PTR)pid, otherPid, alloc->Pages, mdlPages);
    } else if (made) {
        MwLog(A, L"VdecExportsMade", A->VdecExportsMade);
        MwLog(A, L"VdecExportPid", (ULONG)(ULONG_PTR)pid);
        MwLog(A, L"VdecExportPages", D->Pages);
    }
    return st;
}
