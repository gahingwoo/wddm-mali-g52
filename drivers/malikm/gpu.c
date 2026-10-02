// SPDX-License-Identifier: GPL-2.0
/*
 * malikm's side of the hardware: BO mapping and the job worker, on top of
 * drivers/common/maligpu (power-up, page tables, job slots, interrupts).
 */
#include "malikm.h"

void MkLog(PDEVICE_CONTEXT Dev, PCWSTR Name, ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING name;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "malikm: %ws = 0x%08x\n", Name, Value);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Dev->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&name, Name);
        (void)WdfRegistryAssignULong(key, &name, Value);
        WdfRegistryClose(key);
    }
}

void MkLogHook(void *Owner, PCWSTR Name, ULONG Value)
{
    MkLog((PDEVICE_CONTEXT)Owner, Name, Value);
}

/* ---- BOs in the GPU address space ---- */

NTSTATUS MkMmuMap(PDEVICE_CONTEXT Dev, MK_BO *Bo)
{
    ULONG pages = (ULONG)(Bo->Size >> PAGE_SHIFT);
    BOOLEAN noexec = (Bo->Flags & PANFROST_BO_NOEXEC) != 0;
    NTSTATUS st;

    if (!MkVaAlloc(&Dev->Gpu, pages, !noexec, &Bo->GpuVa))
        return STATUS_INSUFFICIENT_RESOURCES;
    st = MkMmuMapPages(&Dev->Gpu, Bo->GpuVa, MmGetMdlPfnArray(Bo->Mdl), pages, noexec);
    if (!NT_SUCCESS(st))
        MkVaFree(&Dev->Gpu, Bo->GpuVa, pages);
    return st;
}

void MkMmuUnmap(PDEVICE_CONTEXT Dev, MK_BO *Bo)
{
    ULONG pages = (ULONG)(Bo->Size >> PAGE_SHIFT);

    MkMmuUnmapPages(&Dev->Gpu, Bo->GpuVa, pages);
    MkVaFree(&Dev->Gpu, Bo->GpuVa, pages);
}

/* ---- jobs ---- */

#define JOB_TIMEOUT_MS  2000

static void RunJob(PDEVICE_CONTEXT Dev, MK_JOB *Job)
{
    MK_GPU *gpu = &Dev->Gpu;
    ULONG slot = (Job->Requirements & PANFROST_JD_REQ_FS) ? 0 : 1;
    ULONG done = 1u << slot, err = 1u << (16 + slot);
    ULONG bits;
    ULONG64 start = KeQueryInterruptTime();
    LARGE_INTEGER tick;

    (void)MkJobTakeStatus(gpu, slot);
    InterlockedExchange(&gpu->MmuIrqStatus, 0);
    KeClearEvent(&Dev->JobIrqEvent);
    MkJobStart(gpu, slot, Job->Jc, (Job->Seq & 1) != 0);

    /* The interrupt wakes us; the raw status is also read every tick. */
    tick.QuadPart = -10000;                 /* 1 ms, relative */
    for (;;) {
        (void)KeWaitForSingleObject(&Dev->JobIrqEvent, Executive, KernelMode, FALSE, &tick);
        bits = MkJobTakeStatus(gpu, slot);

        if (bits & err) {
            Dev->LastJsStatus = MkJobSlotStatus(gpu, slot);
            Dev->JobsFailed++;
            break;
        }
        if (bits & done) {
            Dev->JobsDone++;
            return;
        }
        if (gpu->MmuIrqStatus != 0) {
            Dev->JobsFailed++;
            break;
        }
        if ((KeQueryInterruptTime() - start) / 10000 > JOB_TIMEOUT_MS) {
            Dev->LastJsStatus = MkJobSlotStatus(gpu, slot);
            MkJobHardStop(gpu, slot);
            Dev->JobsTimedOut++;
            break;
        }
    }

    /* Something went wrong: record it and start the GPU over. */
    MkLog(Dev, L"JobsFailed", Dev->JobsFailed);
    MkLog(Dev, L"JobsTimedOut", Dev->JobsTimedOut);
    MkLog(Dev, L"LastJsStatus", Dev->LastJsStatus);
    MkLog(Dev, L"LastMmuFault", gpu->LastMmuFault);
    MkLog(Dev, L"LastFaultAddressLo", (ULONG)gpu->LastFaultAddress);
    MkLog(Dev, L"LastFaultAddressHi", (ULONG)(gpu->LastFaultAddress >> 32));
    MkLog(Dev, L"LastFailedJcLo", (ULONG)Job->Jc);
    gpu->Resets++;
    if (!MkGpuBringup(gpu))
        MkLog(Dev, L"ResetFailed", gpu->Resets);
    MkLog(Dev, L"Resets", gpu->Resets);
}

static void Complete(PDEVICE_CONTEXT Dev, MK_JOB *Job)
{
    InterlockedExchange64(&Dev->CompletedSeq, (LONG64)Job->Seq);
    KeSetEvent(&Dev->ProgressEvent, 0, FALSE);
    ExAcquireFastMutex(&Dev->Lock);
    MkReapZombies(Dev);
    ExReleaseFastMutex(&Dev->Lock);
    ExFreePoolWithTag(Job, 'jmlM');
}

static MK_JOB *Pop(PDEVICE_CONTEXT Dev)
{
    KIRQL irql;
    PLIST_ENTRY e = NULL;

    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    if (!IsListEmpty(&Dev->Queue))
        e = RemoveHeadList(&Dev->Queue);
    KeReleaseSpinLock(&Dev->QueueLock, irql);
    return e ? CONTAINING_RECORD(e, MK_JOB, Link) : NULL;
}

static VOID Worker(PVOID Context)
{
    PDEVICE_CONTEXT dev = (PDEVICE_CONTEXT)Context;
    MK_JOB *job;

    for (;;) {
        (void)KeWaitForSingleObject(&dev->QueueEvent, Executive, KernelMode, FALSE, NULL);
        while ((job = Pop(dev)) != NULL) {
            if (!dev->StopWorker) {
                KeClearEvent(&dev->ProgressEvent);
                RunJob(dev, job);
                if (dev->JobsDone == 1 || (dev->JobsDone & 255) == 0)
                    MkLog(dev, L"JobsDone", dev->JobsDone);
            }
            Complete(dev, job);
        }
        if (dev->StopWorker)
            break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS MkWorkerStart(PDEVICE_CONTEXT Dev)
{
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;

    Dev->StopWorker = FALSE;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, Worker, Dev);
    if (!NT_SUCCESS(st))
        return st;
    st = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                   (PVOID *)&Dev->Worker, NULL);
    ZwClose(h);
    return st;
}

void MkWorkerStop(PDEVICE_CONTEXT Dev)
{
    if (Dev->Worker == NULL)
        return;
    Dev->StopWorker = TRUE;
    KeSetEvent(&Dev->QueueEvent, 0, FALSE);
    (void)KeWaitForSingleObject(Dev->Worker, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(Dev->Worker);
    Dev->Worker = NULL;
    MkLog(Dev, L"JobsDone", Dev->JobsDone);
    MkLog(Dev, L"Irqs", Dev->Gpu.Irqs);
}

void MkQueueJob(PDEVICE_CONTEXT Dev, MK_JOB *Job)
{
    KIRQL irql;

    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    InsertTailList(&Dev->Queue, &Job->Link);
    KeReleaseSpinLock(&Dev->QueueLock, irql);
    KeSetEvent(&Dev->QueueEvent, 0, FALSE);
}
