// SPDX-License-Identifier: GPL-2.0
/*
 * The video decoder's calls through maliwddm (docs/CHROMIUM-VIDEO.md, C2;
 * the protocol is video/vdec_kmd.h).
 *
 * The UMD's video backend (video/vdec.c) runs in the player's process,
 * which may be a sandbox (Chromium's GPU process: low integrity, restricted
 * token) that cannot and should not open \\.\VDPU383. Instead it makes a
 * D3D device of its own on this adapter and sends each vdpu383.sys call as
 * MW_ESCAPE_VDEC_CALL on it. For each such device this file opens a session
 * of vdpu383.sys from kernel mode (vdpu383_check.h), in the caller's
 * context, and makes the calls there:
 *
 *   - only the calls a session may make, with their exact sizes, and a
 *     decode with the session's run rules (vdec_kmd_check, on a copy of the
 *     escape taken once); vdpu383.sys applies the same rules again;
 *   - vdpu383.sys checks every address register of a decode against the
 *     session's own buffers and imports, and decodes with an IOMMU
 *     directory that holds nothing else, so a process reaches only its own
 *     memory: the buffers it allocated through the session (mapped into it)
 *     and the pages of its own allocations exported to it
 *     (MW_ESCAPE_VDEC_EXPORT, vdecexport.c, which checks the process);
 *   - the session ends when the device is destroyed (DestroyDevice, also at
 *     process exit, while the process's address space is still there, as
 *     dxgkrnl needs it for its own mappings) or the adapter stops; then
 *     vdpu383.sys frees its buffers and releases its imports. Revocation of
 *     an import when its allocation goes is vdecexport.c's, as before.
 *
 * At most MW_VDEC_SESSIONS sessions, MW_VDEC_SESSIONS_PROCESS per process.
 * Off with UmdVideo 0 or UmdVideoKmd 0 (read at the adapter's start): the
 * escape is then refused and the UMD falls back to opening \\.\VDPU383
 * itself, which needs vdpu383.sys's UserAccess.
 */
#include "maliwddm.h"
#include "../../video/vdpu383/vdpu383_ioctl.h"
#include "../../video/vdec_kmd.h"

#define MW_VDEC_SESSIONS            32
#define MW_VDEC_SESSIONS_PROCESS    4

typedef struct _MW_VDEC_SESSION {
    LIST_ENTRY Link;
    MW_DEVICE *Device;
    HANDLE Pid;
    HANDLE File;                        /* kernel handle of \Device\VDPU383 */
    PFILE_OBJECT Fo;
    PDEVICE_OBJECT Dev;
    EX_RUNDOWN_REF Rundown;             /* calls in progress */
} MW_VDEC_SESSION;

static void SessGateEnter(MW_ADAPTER *A)
{
    (void)KeWaitForSingleObject(&A->VdecSessGate, Executive, KernelMode, FALSE, NULL);
}

static void SessGateLeave(MW_ADAPTER *A)
{
    KeSetEvent(&A->VdecSessGate, IO_NO_INCREMENT, FALSE);
}

/* A refused escape or session, for a board with no debugger: the first 16,
 * then every 64th. */
static NTSTATUS Refuse(MW_ADAPTER *A, NTSTATUS St, ULONG Why)
{
    ULONG n = (ULONG)InterlockedIncrement((volatile LONG *)&A->VdecKmdRefused);

    A->VdecKmdLastStatus = (ULONG)St;
    A->VdecKmdLastPid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    if (n <= 16 || n % 64 == 0) {
        MwLog(A, L"VdecKmdRefused", n);
        MwLog(A, L"VdecKmdLastWhy", Why);
        MwLog(A, L"VdecKmdLastStatus", (ULONG)St);
        MwLog(A, L"VdecKmdLastPid", A->VdecKmdLastPid);
    }
    return St;
}

void MwVdecKmdInit(MW_ADAPTER *A)
{
    KeInitializeEvent(&A->VdecSessGate, SynchronizationEvent, TRUE);
    InitializeListHead(&A->VdecSessions);
}

/* StartDevice (MwVdecStart), with the GPU up. */
void MwVdecKmdStart(MW_ADAPTER *A)
{
    A->VdecKmdOn = MwSetting(A, L"UmdVideo", 0) != 0 && MwSetting(A, L"UmdVideoKmd", 1) != 0;
    MwLog(A, L"VdecKmd", A->VdecKmdOn);
}

/* No call in progress or to come (Rundown run down); not in the list. */
static void SessionClose(MW_ADAPTER *A, MW_VDEC_SESSION *S)
{
    ExWaitForRundownProtectionRelease(&S->Rundown);
    /* vdpu383.sys's cleanup: its buffers (unmapped from the process,
     * attached to it when this is another context) and imports go */
    ZwClose(S->File);
    ObDereferenceObject(S->Fo);
    InterlockedDecrement((volatile LONG *)&A->VdecKmdSessions);
    ExFreePoolWithTag(S, MW_TAG);
}

/* DestroyDevice: the device's session ends. */
void MwVdecKmdDeviceGone(MW_DEVICE *Dev)
{
    MW_ADAPTER *a = Dev->Adapter;
    MW_VDEC_SESSION *s;

    if (Dev->Vdec == NULL)
        return;
    SessGateEnter(a);
    s = Dev->Vdec;
    Dev->Vdec = NULL;
    if (s != NULL)
        RemoveEntryList(&s->Link);
    SessGateLeave(a);
    if (s != NULL)
        SessionClose(a, s);
}

/* StopDevice, RemoveDevice: every session ends, before the exports do
 * (vdecexport.c), so vdpu383.sys releases its imports while the callback
 * is still registered. Safe to repeat. */
void MwVdecKmdStop(MW_ADAPTER *A)
{
    if (A->VdecKmdOn || A->VdecKmdSessionsMade != 0) {
        MwLog(A, L"VdecKmdSessionsMade", A->VdecKmdSessionsMade);
        MwLog(A, L"VdecKmdCalls", A->VdecKmdCalls);
        MwLog(A, L"VdecKmdRuns", A->VdecKmdRuns);
        MwLog(A, L"VdecKmdRefused", A->VdecKmdRefused);
    }
    A->VdecKmdOn = FALSE;
    for (;;) {
        MW_VDEC_SESSION *s = NULL;

        SessGateEnter(A);
        if (!IsListEmpty(&A->VdecSessions)) {
            s = CONTAINING_RECORD(RemoveHeadList(&A->VdecSessions), MW_VDEC_SESSION, Link);
            s->Device->Vdec = NULL;
        }
        SessGateLeave(A);
        if (s == NULL)
            break;
        SessionClose(A, s);
    }
}

/* The device's session, opened on its first call; with a call's rundown
 * protection held. Gate held. */
static NTSTATUS SessionGet(MW_ADAPTER *A, MW_DEVICE *Dev, MW_VDEC_SESSION **Out)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\VDPU383");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE pid = PsGetCurrentProcessId(), h = NULL;
    MW_VDEC_SESSION *s = Dev->Vdec;
    PFILE_OBJECT fo = NULL;
    PLIST_ENTRY l;
    ULONG mine = 0;
    NTSTATUS st;

    *Out = NULL;
    if (s != NULL) {
        if (s->Pid != pid)
            return STATUS_ACCESS_DENIED;        /* a device is its process's */
        if (!ExAcquireRundownProtection(&s->Rundown))
            return STATUS_DEVICE_NOT_READY;
        *Out = s;
        return STATUS_SUCCESS;
    }
    if (!A->VdecKmdOn)
        return STATUS_NOT_SUPPORTED;
    for (l = A->VdecSessions.Flink; l != &A->VdecSessions; l = l->Flink)
        mine += CONTAINING_RECORD(l, MW_VDEC_SESSION, Link)->Pid == pid;
    if ((ULONG)A->VdecKmdSessions >= MW_VDEC_SESSIONS || mine >= MW_VDEC_SESSIONS_PROCESS)
        return STATUS_TOO_MANY_SESSIONS;

    s = (MW_VDEC_SESSION *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*s), MW_TAG);
    if (s == NULL)
        return STATUS_NO_MEMORY;
    /* From kernel mode (the Zw call), so vdpu383.sys makes it a session
     * and its security descriptor (administrators and SYSTEM) is not
     * checked against the caller; a kernel handle, which the process
     * cannot use or see. */
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateFile(&h, GENERIC_READ | GENERIC_WRITE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                      FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (NT_SUCCESS(st))
        st = ObReferenceObjectByHandle(h, 0, *IoFileObjectType, KernelMode, (PVOID *)&fo, NULL);
    if (!NT_SUCCESS(st)) {
        if (h != NULL)
            ZwClose(h);
        ExFreePoolWithTag(s, MW_TAG);
        return st;
    }
    s->Device = Dev;
    s->Pid = pid;
    s->File = h;
    s->Fo = fo;
    s->Dev = IoGetRelatedDeviceObject(fo);
    ExInitializeRundownProtection(&s->Rundown);
    (void)ExAcquireRundownProtection(&s->Rundown);
    InsertTailList(&A->VdecSessions, &s->Link);
    Dev->Vdec = s;
    InterlockedIncrement((volatile LONG *)&A->VdecKmdSessions);
    if ((ULONG)InterlockedIncrement((volatile LONG *)&A->VdecKmdSessionsMade) <= 4) {
        MwLog(A, L"VdecKmdSessionsMade", A->VdecKmdSessionsMade);
        MwLog(A, L"VdecKmdSessionPid", (ULONG)(ULONG_PTR)pid);
    }
    *Out = s;
    return STATUS_SUCCESS;
}

static IO_COMPLETION_ROUTINE CallDone;

static NTSTATUS CallDone(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
 * One call in the session, in the caller's thread (vdpu383.sys maps a
 * buffer into the current process, and checks it is the session's). The
 * IRP is ours, with C->Data as its buffered-I/O buffer and our completion
 * routine: nothing depends on an APC to this thread, which an escape may
 * not get while dxgkrnl holds its locks.
 */
static void SessionCall(MW_VDEC_SESSION *S, VDEC_KMD_CALL *C)
{
    KEVENT ev;
    PIRP irp;
    PIO_STACK_LOCATION sl;

    C->Returned = 0;
    irp = IoAllocateIrp(S->Dev->StackSize, FALSE);
    if (irp == NULL) {
        C->Status = STATUS_INSUFFICIENT_RESOURCES;
        return;
    }
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp->AssociatedIrp.SystemBuffer = C->Data;
    irp->RequestorMode = KernelMode;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;
    sl = IoGetNextIrpStackLocation(irp);
    sl->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    sl->FileObject = S->Fo;
    sl->Parameters.DeviceIoControl.IoControlCode = C->Ioctl;
    sl->Parameters.DeviceIoControl.InputBufferLength = C->InSize;
    sl->Parameters.DeviceIoControl.OutputBufferLength = C->OutSize;
    IoSetCompletionRoutine(irp, CallDone, &ev, TRUE, TRUE, TRUE);
    (void)IoCallDriver(S->Dev, irp);
    (void)KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
    C->Status = irp->IoStatus.Status;
    if (NT_SUCCESS(C->Status) && irp->IoStatus.Information <= C->OutSize)
        C->Returned = (ULONG)irp->IoStatus.Information;
    IoFreeIrp(irp);
}

/* MW_ESCAPE_VDEC_CALL, in the caller's context, PASSIVE_LEVEL. */
NTSTATUS MwEscapeVdecCall(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc)
{
    VDEC_KMD_CALL *c, *out = (VDEC_KMD_CALL *)Esc->pPrivateDriverData;
    MW_VDEC_SESSION *s = NULL;
    NTSTATUS st;
    int why;

    if (Esc->PrivateDriverDataSize < sizeof(VDEC_KMD_CALL))
        return Refuse(A, STATUS_INVALID_PARAMETER, VDEC_KMD_E_SIZE);
    /* Our own copy: what is checked is what is used. */
    c = (VDEC_KMD_CALL *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), MW_TAG);
    if (c == NULL)
        return STATUS_NO_MEMORY;
    RtlCopyMemory(c, Esc->pPrivateDriverData, sizeof(*c));
    why = vdec_kmd_check(c, Esc->PrivateDriverDataSize, Esc->hDevice != NULL);
    if (why == VDEC_KMD_OK && c->Ioctl == VDEC_KMD_PROBE) {
        VDEC_KMD_COUNTERS k;

        RtlZeroMemory(&k, sizeof(k));
        k.On = A->VdecKmdOn;
        k.Sessions = (ULONG)A->VdecKmdSessions;
        k.SessionsMade = A->VdecKmdSessionsMade;
        k.Calls = A->VdecKmdCalls;
        k.Runs = A->VdecKmdRuns;
        k.Refused = A->VdecKmdRefused;
        k.LastStatus = (int)A->VdecKmdLastStatus;
        k.LastPid = A->VdecKmdLastPid;
        out->Status = STATUS_SUCCESS;
        out->Returned = sizeof(k);
        out->Magic = VDEC_KMD_MAGIC;
        RtlCopyMemory(out->Data, &k, sizeof(k));
        ExFreePoolWithTag(c, MW_TAG);
        return STATUS_SUCCESS;
    }
    if (!A->VdecKmdOn)
        st = STATUS_NOT_SUPPORTED;
    else if (why == VDEC_KMD_E_CALL)
        st = STATUS_ACCESS_DENIED;
    else if (why != VDEC_KMD_OK)
        st = STATUS_INVALID_PARAMETER;
    else {
        SessGateEnter(A);
        st = SessionGet(A, (MW_DEVICE *)Esc->hDevice, &s);
        SessGateLeave(A);
    }
    if (!NT_SUCCESS(st)) {
        ExFreePoolWithTag(c, MW_TAG);
        return Refuse(A, st, (ULONG)why);
    }

    SessionCall(s, c);
    ExReleaseRundownProtection(&s->Rundown);
    InterlockedIncrement((volatile LONG *)&A->VdecKmdCalls);
    if (c->Ioctl == IOCTL_VDPU383_RUN && NT_SUCCESS(c->Status) &&
        ((const VDPU383_RUN_OUT *)(const void *)c->Data)->result == VDPU383_RUN_OK)
        InterlockedIncrement((volatile LONG *)&A->VdecKmdRuns);

    out->Status = c->Status;
    out->Returned = c->Returned;
    out->Magic = VDEC_KMD_MAGIC;
    if (c->Returned)
        RtlCopyMemory(out->Data, c->Data, c->Returned);
    ExFreePoolWithTag(c, MW_TAG);
    return STATUS_SUCCESS;
}
