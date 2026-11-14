// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's command path (docs/M5.2-PLAN.md): Render copies the UMD's
 * MW_COMMANDs into the DMA buffer's private data, SubmitCommand queues them
 * with their fence, and the hardware works through the queue one job chain
 * at a time. When a submission's last chain finishes, its fence is reported
 * DMA_COMPLETED.
 *
 * The queue is shared with the ISR, so every change to it happens at the
 * device's interrupt level: inside DxgkCbSynchronizeExecution or the ISR.
 * Whether dxgkrnl connects the Mali's interrupts on this ACPI device is not
 * yet known, so a timer also polls the job status while work is in flight;
 * with working interrupts it only costs latency.
 *
 * Preemption (setting TimeSlice, off by default). The scheduler asks for it
 * with DxgkDdiPreemptCommand when a context of higher priority (dwm) has
 * work; the INF's EnablePreemption 0 stops it from ever asking, so both
 * have to be set. A request is answered with DMA_PREEMPTED once nothing of
 * the submissions before it is on the GPU: those not started are handed
 * back, a fragment chain that has run for at least TimeSlice ms is
 * soft-stopped (the Job Manager stops it between tiles and JS_TAIL says
 * where it stopped), and a vertex/tiler chain is let finish. The scheduler
 * resubmits what was handed back with new fences. A submission that was
 * stopped part way is remembered in its context, and its resubmission, the
 * next submission of that context with the same commands, continues from
 * there: running it again from the start would repeat tiler work and blend
 * finished tiles twice. Linux panfrost resumes a soft-stopped chain the same
 * way (panfrost_job_handle_err: job->jc = JS_TAIL), and Arm's kbase too
 * (kbase_gpu_complete_hw).
 */
#include "maliwddm.h"

#define MW_DMA_MAGIC            0x4D57444Du /* 'MWDM' in the private data */
#define MW_DMA_BUFFER_SIZE      (16 * 1024)
#define MW_LIST_SIZE            256     /* Panfrost submits reference many BOs */
#define MW_JOB_TIMEOUT_MS       2000
#define MW_POLL_MS              1
#define MW_STOP_SLOW_MS         100     /* a soft-stop not taken by then is counted */
#define MW_SLICE_MAX_MS         1000

/* ---- settings ---- */

/* TimeSlice, once: at passive level, from StartDevice or the DRIVERCAPS
 * query, whichever comes first. Changing it takes a restart of the adapter. */
void MwCommandSettings(MW_ADAPTER *A)
{
    ULONG v;

    if (A->SliceRead)
        return;
    v = MwSetting(A, L"TimeSlice", 0);
    A->Slice = v > MW_SLICE_MAX_MS ? MW_SLICE_MAX_MS : v;
    A->SliceRead = TRUE;
}

/* ---- devices and contexts ---- */

NTSTATUS APIENTRY MwCreateDevice(IN_CONST_HANDLE hAdapter, DXGKARG_CREATEDEVICE *Arg)
{
    MW_DEVICE *dev = (MW_DEVICE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*dev), MW_TAG);

    if (dev == NULL)
        return STATUS_NO_MEMORY;
    dev->Adapter = (MW_ADAPTER *)hAdapter;
    Arg->hDevice = dev;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyDevice(IN_CONST_HANDLE hDevice)
{
    ExFreePoolWithTag(hDevice, MW_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCreateContext(IN_CONST_HANDLE hDevice, DXGKARG_CREATECONTEXT *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), MW_TAG);

    if (ctx == NULL)
        return STATUS_NO_MEMORY;
    ctx->Device = (MW_DEVICE *)hDevice;
    Arg->hContext = ctx;
    Arg->ContextInfo.DmaBufferSize = MW_DMA_BUFFER_SIZE;
    Arg->ContextInfo.DmaBufferSegmentSet = 0;           /* system memory, as viogpu3d */
    Arg->ContextInfo.DmaBufferPrivateDataSize = sizeof(MW_DMA_PRIVATE);
    Arg->ContextInfo.AllocationListSize = MW_LIST_SIZE;
    Arg->ContextInfo.PatchLocationListSize = MW_LIST_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwDestroyContext(IN_CONST_HANDLE hContext)
{
    ExFreePoolWithTag(hContext, MW_TAG);
    return STATUS_SUCCESS;
}

/* ---- building DMA buffers ---- */

/* The DMA buffer itself is never read: SubmitCommand gets only its physical
 * address. It carries one marker word so that no submission is empty. */
static void MarkDmaBuffer(VOID **DmaBuffer)
{
    *(ULONG *)*DmaBuffer = MW_DMA_MAGIC;
    *DmaBuffer = (PUCHAR)*DmaBuffer + sizeof(ULONG);
}

/* One patch location per allocation the buffer references. Nothing in our
 * DMA buffer needs patching (GPU addresses are fixed), but WDDM 1.x VidMm
 * learns from the patch list what to make resident: with the UMD's empty
 * list the job's allocation was never mapped into the aperture (WinPE runs
 * 13-14: no MAP_APERTURE_SEGMENT for it, translation fault at its address). */
static void PatchEveryAllocation(UINT Allocations, D3DDDI_PATCHLOCATIONLIST **Out, UINT OutSize)
{
    for (UINT i = 0; i < Allocations && i < OutSize; i++) {
        RtlZeroMemory(*Out, sizeof(**Out));
        (*Out)->AllocationIndex = i;
        (*Out)->SlotId = i;
        (*Out)++;
    }
}

NTSTATUS APIENTRY MwRender(IN_CONST_HANDLE hContext, DXGKARG_RENDER *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    MW_DMA_PRIVATE *priv = (MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
    ULONG count;

    if (priv == NULL || Arg->DmaBufferPrivateDataSize < sizeof(*priv) || Arg->DmaSize < sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;
    if (Arg->CommandLength % sizeof(MW_COMMAND) != 0)
        return STATUS_INVALID_PARAMETER;
    count = Arg->CommandLength / sizeof(MW_COMMAND);
    if (count > MW_MAX_COMMANDS)
        return STATUS_INVALID_PARAMETER;

    /* pCommand is the UMD's buffer, mapped in the caller's process. */
    __try {
        for (ULONG i = 0; i < count; i++) {
            MW_COMMAND c = ((const MW_COMMAND *)Arg->pCommand)[i];
            if (c.Type != MW_CMD_JOB || c.Slot > 1 || c.Jc == 0)
                return STATUS_INVALID_PARAMETER;
            priv->Commands[i] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_USER_BUFFER;
    }
    priv->Count = count;
    priv->Reserved = MW_DMA_MAGIC;

    PatchEveryAllocation(Arg->AllocationListSize, &Arg->pPatchLocationListOut, Arg->PatchLocationListOutSize);
    MarkDmaBuffer(&Arg->pDmaBuffer);
    ctx->Device->Adapter->Renders++;
    return STATUS_SUCCESS;
}

/* Presents are done by the CPU at submission (MwSubmitCommand): a blt from
 * the source allocation's pages, or a colour fill, into the primary (the
 * POST framebuffer) or the destination allocation's pages. Render records
 * what to do in the DMA buffer's private data and patches both allocations
 * so that VidMm makes them resident, which is when their pages are known.
 * Flips are not handled yet. */
NTSTATUS APIENTRY MwPresent(IN_CONST_HANDLE hContext, DXGKARG_PRESENT *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    MW_ADAPTER *a = ctx->Device->Adapter;
    MW_DMA_PRIVATE *priv = (MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;

    if (Arg->Flags.Flip) {
        /* A flip is memory-mapped here (FlipOnVSyncMmIo): dxgkrnl calls
         * SetVidPnSourceAddress for it and there is nothing to put in a DMA
         * buffer. It also comes without the private data a blt has, which
         * the check below turned into STATUS_INVALID_PARAMETER for every
         * flip dwm made once SetDisplayMode worked (Fail_Present 0xC000000D
         * on the TaishanPi 3M; the UMD saw E_INVALIDARG and dwm rebuilt its
         * swap chain, 37 times in the trace). */
        if (priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv)) {
            RtlZeroMemory(priv, sizeof(*priv));
            priv->Reserved = MW_DMA_MAGIC;
        }
        a->PresentFlips++;
        a->Presents++;
        return STATUS_SUCCESS;
    }
    if (priv == NULL || Arg->DmaBufferPrivateDataSize < sizeof(*priv))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(priv, sizeof(*priv));
    priv->Reserved = MW_DMA_MAGIC;
    if (Arg->Flags.Blt || Arg->Flags.ColorFill) {
        MW_PRESENT_OP *op = &priv->Present;
        op->Kind = Arg->Flags.ColorFill ? MW_PRESENT_FILL : MW_PRESENT_BLT;
        op->Color = Arg->Color;
        /* A present's allocation list has fixed slots; an unused one has
         * no allocation. */
        op->Src = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
        op->Dst = (MW_ALLOCATION *)Arg->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
        op->SrcRect = Arg->SrcRect;
        op->DstRect = Arg->DstRect;
        op->NumRects = Arg->SubRectCnt <= MW_MAX_RECTS ? Arg->SubRectCnt : 0;
        for (ULONG i = 0; i < op->NumRects; i++)
            op->Rects[i] = Arg->pDstSubRects[i];
        /* Both allocations must be resident (and their pages known). */
        for (UINT i = DXGK_PRESENT_SOURCE_INDEX;
             i <= DXGK_PRESENT_DESTINATION_INDEX && Arg->PatchLocationListOutSize > 0; i++) {
            if (Arg->pAllocationList[i].hDeviceSpecificAllocation == NULL)
                continue;
            RtlZeroMemory(Arg->pPatchLocationListOut, sizeof(*Arg->pPatchLocationListOut));
            Arg->pPatchLocationListOut->AllocationIndex = i;
            Arg->pPatchLocationListOut->SlotId = i;
            Arg->pPatchLocationListOut++;
            Arg->PatchLocationListOutSize--;
        }
    }
    if (Arg->pDmaBuffer != NULL && Arg->DmaSize >= sizeof(ULONG))
        MarkDmaBuffer(&Arg->pDmaBuffer);
    a->Presents++;
    return STATUS_SUCCESS;
}

/* The CPU view of a resident allocation, or NULL. */
static PUCHAR AllocationVa(MW_ALLOCATION *Alloc)
{
    PUCHAR va;
    if (Alloc == NULL || Alloc->Mdl == NULL)
        return NULL;
    va = (PUCHAR)MmGetSystemAddressForMdlSafe(Alloc->Mdl, NormalPagePriority | MdlMappingNoExecute);
    return va != NULL ? va + (SIZE_T)Alloc->MdlPage * PAGE_SIZE : NULL;
}

static void ClipRect(RECT *R, LONG W, LONG H)
{
    if (R->left < 0) R->left = 0;
    if (R->top < 0) R->top = 0;
    if (R->right > W) R->right = W;
    if (R->bottom > H) R->bottom = H;
}

/* Carry out a present on the CPU. 32-bit pixels only. */
static void DoPresent(MW_ADAPTER *A, const MW_PRESENT_OP *Op)
{
    PUCHAR dst, src = NULL;
    LONG dstW, dstH, dstPitch, srcPitch = 0, srcW = 0, srcH = 0;
    ULONG n = Op->NumRects ? Op->NumRects : 1;
    ULONG64 t0 = MwTick(), longest = 0;

    if (Op->Dst != NULL && (Op->Dst->Flags & MW_ALLOC_PRIMARY) && A->FbVa != NULL) {
        MwScanToFb(A);          /* a blt goes to the framebuffer: show it */
        dst = A->FbVa;
        dstW = (LONG)A->Fb.Width;
        dstH = (LONG)A->Fb.Height;
        dstPitch = (LONG)A->Fb.Pitch;
    } else {
        dst = AllocationVa(Op->Dst);
        if (dst == NULL) {
            A->PresentSkips++;
            return;
        }
        dstW = (LONG)Op->Dst->Width;
        dstH = (LONG)Op->Dst->Height;
        dstPitch = (LONG)(Op->Dst->Pitch ? Op->Dst->Pitch : Op->Dst->Width * 4);
    }
    if (Op->Kind == MW_PRESENT_BLT) {
        src = AllocationVa(Op->Src);
        if (src == NULL) {
            A->PresentSkips++;
            return;
        }
        srcW = (LONG)Op->Src->Width;
        srcH = (LONG)Op->Src->Height;
        srcPitch = (LONG)(Op->Src->Pitch ? Op->Src->Pitch : Op->Src->Width * 4);
    }

    for (ULONG i = 0; i < n; i++) {
        RECT r = Op->NumRects ? Op->Rects[i] : Op->DstRect;
        LONG dx = Op->SrcRect.left - Op->DstRect.left, dy = Op->SrcRect.top - Op->DstRect.top;

        ClipRect(&r, dstW, dstH);
        if (Op->Kind == MW_PRESENT_BLT) {
            /* Keep the source side inside the source too. */
            if (r.left + dx < 0) r.left = -dx;
            if (r.top + dy < 0) r.top = -dy;
            if (r.right + dx > srcW) r.right = srcW - dx;
            if (r.bottom + dy > srcH) r.bottom = srcH - dy;
        }
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        for (LONG y = r.top; y < r.bottom; y++) {
            ULONG *d = (ULONG *)(dst + (SIZE_T)y * dstPitch) + r.left;
            if (Op->Kind == MW_PRESENT_BLT) {
                const ULONG *s = (const ULONG *)(src + (SIZE_T)(y + dy) * srcPitch) + r.left + dx;
                RtlCopyMemory(d, s, (SIZE_T)(r.right - r.left) * 4);
            } else {
                for (LONG x = 0; x < r.right - r.left; x++)
                    d[x] = Op->Color;
            }
        }
    }
    if (Op->Kind == MW_PRESENT_BLT)
        A->PresentBlts++;
    else
        A->PresentFills++;
    MwTickAdd(&A->PresentBltTicks, &longest, t0);
}

NTSTATUS APIENTRY MwPatch(IN_CONST_HANDLE hAdapter, const DXGKARG_PATCH *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    /* Nothing to patch; record where VidMm put the first allocation. */
    a->Patches++;
    a->PatchAllocations = Arg->AllocationListSize;
    a->PatchPatchLocations = Arg->PatchLocationListSubmissionLength;
    if (Arg->AllocationListSize != 0 && Arg->pAllocationList != NULL) {
        a->PatchSegment0 = Arg->pAllocationList[0].SegmentId;
        a->PatchAddress0 = (ULONG64)Arg->pAllocationList[0].PhysicalAddress.QuadPart;
    }
    return STATUS_SUCCESS;
}

/* ---- the queue, at interrupt level ---- */

static void NotifyCompleted(MW_ADAPTER *A, ULONG Fence)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    n.DmaCompleted.SubmissionFenceId = Fence;
    n.DmaCompleted.NodeOrdinal = 0;
    n.DmaCompleted.EngineOrdinal = 0;
    A->Dxgk.DxgkCbNotifyInterrupt(A->Dxgk.DeviceHandle, &n);
    A->LastCompletedFence = Fence;
    A->NeedDpc = TRUE;
}

/* Fence A was submitted before fence B (fences increase, and wrap). */
static BOOLEAN FenceBefore(ULONG A, ULONG B)
{
    return (LONG)(A - B) < 0;
}

/* Answer the waiting preemption request: nothing is running. Submissions
 * from before the request go back to the scheduler, which resubmits them
 * with new fences; one that ran part way leaves where it got to in its
 * context. Later submissions stay queued. */
static void FinishPreempt(MW_ADAPTER *A)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    while (A->QCount != 0) {
        MW_SUBMISSION *s = &A->Queue[A->QHead];

        if (!FenceBefore(s->FenceId, A->PreemptFence))
            break;
        if (s->Next < s->Count && (s->Next != 0 || s->ResumeTail != 0)) {
            MW_CONTEXT *c = s->Ctx;
            if (c != NULL) {
                c->ResumeValid = TRUE;
                c->ResumeGen = A->ResetGen;
                c->ResumeCount = s->Count;
                c->ResumeNext = s->Next;
                c->ResumeTail = s->ResumeTail;
                RtlCopyMemory(c->ResumeCommands, s->Commands, s->Count * sizeof(MW_COMMAND));
            } else {
                A->ResumeLost++;
            }
        }
        A->QHead = (A->QHead + 1) % MW_QUEUE_DEPTH;
        A->QCount--;
        A->PreemptDropped++;
    }
    n.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    n.DmaPreempted.PreemptionFenceId = A->PreemptFence;
    n.DmaPreempted.LastCompletedFenceId = A->LastCompletedFence;
    n.DmaPreempted.NodeOrdinal = 0;
    n.DmaPreempted.EngineOrdinal = 0;
    A->Dxgk.DxgkCbNotifyInterrupt(A->Dxgk.DeviceHandle, &n);
    A->PreemptPending = FALSE;
    MwTickAdd(&A->PreemptTicks, &A->PreemptMaxTicks, A->PreemptTick);
    A->NeedDpc = TRUE;
}

/* Start the next job chain, completing every submission that has none left
 * and answering a preemption request when nothing is running. Called with
 * the queue owned (interrupt level). */
static void Kick(MW_ADAPTER *A)
{
    while (!A->Running && A->QCount != 0) {
        MW_SUBMISSION *s = &A->Queue[A->QHead];

        if (!A->GpuUp || s->Next >= s->Count) {
            NotifyCompleted(A, s->FenceId);
            A->QHead = (A->QHead + 1) % MW_QUEUE_DEPTH;
            A->QCount--;
            continue;
        }
        if (A->PreemptPending) {
            /* Nothing is running: answer before anything else starts. */
            if (FenceBefore(s->FenceId, A->PreemptFence) && s->Next != 0 && s->ResumeTail == 0)
                A->StopsAtBoundary++;
            FinishPreempt(A);
            continue;
        }
        const MW_COMMAND *c = &s->Commands[s->Next++];
        ULONG64 jc = s->ResumeTail != 0 ? s->ResumeTail : c->Jc;
        s->ResumeTail = 0;
        if (!s->Started) {
            s->Started = TRUE;
            if (s->SubmitTick != 0) {
                A->WaitCount++;
                MwTickAdd(&A->WaitTicks, &A->WaitMaxTicks, s->SubmitTick);
                if (s->BehindOther) {
                    ULONG64 ignored = 0;
                    A->WaitOtherCount++;
                    MwTickAdd(&ignored, &A->WaitOtherMaxTicks, s->SubmitTick);
                }
            }
        }
        A->RunningSlot = c->Slot;
        A->RunningCtx = s->Ctx;
        A->RunningFlag = (A->ChainFlag++ & 1) != 0;
        A->StopAsked = FALSE;
        A->Running = TRUE;
        A->JobStart = KeQueryInterruptTime();
        A->JobStartTick = MwTick();
        (void)MkJobTakeStatus(&A->Gpu, c->Slot);
        InterlockedExchange(&A->Gpu.MmuIrqStatus, 0);
        MkJobStart(&A->Gpu, c->Slot, jc, A->RunningFlag);
    }
    if (!A->Running && A->PreemptPending)
        FinishPreempt(A);
}

/* With a preemption request waiting: soft-stop the running chain once it
 * has run for TimeSlice ms, so a chain just started or resumed always gets
 * that long. Vertex/tiler chains (slot 1) are short and are let finish;
 * the request is answered at the chain boundary. */
static void MaybeStop(MW_ADAPTER *A)
{
    ULONG64 now;

    if (!A->PreemptPending || !A->Running)
        return;
    now = KeQueryInterruptTime();
    if (A->StopAsked) {
        if (!A->StopSlowSeen && (now - A->StopAskedAt) / 10000 > MW_STOP_SLOW_MS) {
            A->StopSlowSeen = TRUE;
            A->StopSlow++;
        }
        return;
    }
    if (A->RunningSlot != 0 || (now - A->JobStart) / 10000 < A->Slice)
        return;
    if (A->QCount == 0 || !FenceBefore(A->Queue[A->QHead].FenceId, A->PreemptFence))
        return;
    MkJobSoftStop(&A->Gpu, A->RunningSlot, A->RunningFlag);
    A->StopAsked = TRUE;
    A->StopSlowSeen = FALSE;
    A->StopAskedAt = now;
    A->SoftStops++;
}

/* Collect the running chain's outcome. TRUE if it finished, either way, or
 * stopped. */
static BOOLEAN Reap(MW_ADAPTER *A, BOOLEAN CheckTimeout)
{
    MK_GPU *gpu = &A->Gpu;
    ULONG slot = A->RunningSlot;
    ULONG bits;
    BOOLEAN failed;

    if (!A->Running)
        return FALSE;
    bits = MkJobTakeStatus(gpu, slot);
    if (bits & (1u << (16 + slot)) || gpu->MmuIrqStatus != 0) {
        A->LastJsStatus = MkJobSlotStatus(gpu, slot);
        failed = TRUE;
        if (A->LastJsStatus == JS_STATUS_STOPPED && gpu->MmuIrqStatus == 0) {
            if (!A->StopAsked) {
                A->StopUnasked++;
            } else {
                /* kbase (kbase_job_done): a soft-stopped chain has JS_HEAD
                 * equal to JS_TAIL; if not, it was wrongly marked stopped
                 * and is not to be resumed. */
                ULONG64 tail = MkJobTail(gpu, slot), head = MkJobHead(gpu, slot);
                if (tail == 0 || tail != head) {
                    A->StopBadTail++;
                } else {
                    MW_SUBMISSION *s = &A->Queue[A->QHead];
                    A->Running = FALSE;
                    A->StopAsked = FALSE;
                    MwTickAdd(&A->JobTicks, &A->JobMaxTicks, A->JobStartTick);
                    A->StopsTaken++;
                    s->Next--;              /* this chain again, from its tail */
                    s->ResumeTail = tail;
                    return TRUE;
                }
            }
        }
    } else if (bits & (1u << slot)) {
        if (A->StopAsked)
            A->StopsRacedDone++;
        failed = FALSE;
    } else if (CheckTimeout && (KeQueryInterruptTime() - A->JobStart) / 10000 > MW_JOB_TIMEOUT_MS) {
        A->LastJsStatus = MkJobSlotStatus(gpu, slot);
        MkJobHardStop(gpu, slot);
        A->JobsTimedOut++;
        failed = TRUE;
    } else {
        return FALSE;
    }

    A->Running = FALSE;
    A->StopAsked = FALSE;
    MwTickAdd(&A->JobTicks, &A->JobMaxTicks, A->JobStartTick);
    if (!failed) {
        A->JobsDone++;
        return TRUE;
    }
    /* A fault leaves the address space stopped: start the GPU over, and
     * drop the rest of this submission (its later chains depend on it). */
    A->JobsFailed++;
    A->LastFaultStatus = gpu->LastMmuFault;
    A->LastFaultAddress = gpu->LastFaultAddress;
    A->Queue[A->QHead].Next = A->Queue[A->QHead].Count;
    A->Queue[A->QHead].ResumeTail = 0;
    gpu->Resets++;
    if (!MkGpuBringup(gpu))
        A->GpuUp = FALSE;
    return TRUE;
}

/* From the ISR, after MkIsr has collected the status. */
BOOLEAN MwJobInterrupt(MW_ADAPTER *A)
{
    if (Reap(A, FALSE))
        Kick(A);
    if (A->NeedDpc) {
        A->NeedDpc = FALSE;
        A->Dxgk.DxgkCbQueueDpc(A->Dxgk.DeviceHandle);
    }
    return TRUE;
}

typedef struct {
    MW_ADAPTER *A;
    const MW_SUBMISSION *New;
    NTSTATUS Status;
} MW_SYNC;

static BOOLEAN SyncSubmit(PVOID Context)
{
    MW_SYNC *s = (MW_SYNC *)Context;
    MW_ADAPTER *a = s->A;

    if (a->QCount == MW_QUEUE_DEPTH) {
        s->Status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        return FALSE;
    }
    MW_SUBMISSION *q = &a->Queue[(a->QHead + a->QCount) % MW_QUEUE_DEPTH];
    MW_CONTEXT *c = s->New->Ctx;

    *q = *s->New;
    q->BehindOther = a->Running && a->RunningCtx != c;
    /* The resubmission of a submission preempted part way: the context's
     * next one, with the same commands. Anything else and the saved point
     * is dropped. */
    if (c != NULL && c->ResumeValid) {
        BOOLEAN same = c->ResumeGen == a->ResetGen && c->ResumeCount == q->Count &&
                       c->ResumeNext < q->Count;
        for (ULONG i = 0; same && i < q->Count; i++)
            same = c->ResumeCommands[i].Slot == q->Commands[i].Slot &&
                   c->ResumeCommands[i].Jc == q->Commands[i].Jc;
        c->ResumeValid = FALSE;
        if (same) {
            q->Next = c->ResumeNext;
            q->ResumeTail = c->ResumeTail;
            a->Resumes++;
        } else {
            a->ResumeMismatch++;
        }
    }
    a->QCount++;
    a->LastSubmittedFence = s->New->FenceId;
    Kick(a);
    if (a->NeedDpc) {
        a->NeedDpc = FALSE;
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    s->Status = STATUS_SUCCESS;
    return TRUE;
}

static BOOLEAN SyncPoll(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    if (Reap(a, TRUE))
        Kick(a);
    MaybeStop(a);
    if (a->NeedDpc) {
        a->NeedDpc = FALSE;
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    return a->QCount != 0 || a->PreemptPending;
}

static BOOLEAN Synchronize(MW_ADAPTER *A, PKSYNCHRONIZE_ROUTINE Routine, PVOID Context)
{
    BOOLEAN ret = FALSE;
    NTSTATUS st = A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, Routine, Context, 0, &ret);
    return NT_SUCCESS(st) && ret;
}

/* The fallback for interrupts that never arrive, and the job timeout. */
static VOID PollDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (a->Stopping) {
        InterlockedExchange(&a->PollArmed, 0);
        return;
    }
    if (Synchronize(a, SyncPoll, a)) {
        MwArmPoll(a);
        return;
    }
    /* Idle. A submission that raced with this saw PollArmed set and did
     * not arm the timer, so look once more after letting go. */
    InterlockedExchange(&a->PollArmed, 0);
    KeMemoryBarrier();
    if ((a->QCount != 0 || a->PreemptPending) && InterlockedExchange(&a->PollArmed, 1) == 0)
        MwArmPoll(a);
}

void MwArmPoll(MW_ADAPTER *A)
{
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * MW_POLL_MS;
    KeSetTimer(&A->PollTimer, due, &A->PollDpc);
}

/* ---- VSync, emulated ---- */

static BOOLEAN SyncVsync(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {0};

    n.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    n.CrtcVsync.VidPnTargetId = 0;
    /* The flip this vsync completes: the one whose pixels are shown. */
    n.CrtcVsync.PhysicalAddress = a->CopyThread != NULL ? a->ShownAddr : a->ScanAddr;
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle, &n);
    return TRUE;
}

/* The primary just made current, to the framebuffer: there is no scanout
 * engine pointed at our allocations, so a flip is a CPU copy. Only when the
 * primary changes, not on every vsync. */
static void ShowPrimary(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    PUCHAR src;
    ULONG w, h, pitch;
    ULONG64 t0 = MwTick();

    if (A->FbVa == NULL || P->Mdl == NULL) {
        A->FlipCopySkips++;
        return;
    }
    src = (PUCHAR)MmGetSystemAddressForMdlSafe(P->Mdl, NormalPagePriority | MdlMappingNoExecute);
    if (src == NULL) {
        A->FlipCopySkips++;
        return;
    }
    src += (SIZE_T)P->MdlPage << PAGE_SHIFT;
    w = min(P->Width ? P->Width : A->Fb.Width, A->Fb.Width);
    h = min(P->Height ? P->Height : A->Fb.Height, A->Fb.Height);
    pitch = P->Pitch ? P->Pitch : w * 4;
    for (ULONG y = 0; y < h; y++)
        RtlCopyMemory(A->FbVa + (SIZE_T)y * A->Fb.Pitch, src + (SIZE_T)y * pitch, (SIZE_T)w * 4);
    A->FlipCopies++;
    MwTickAdd(&A->FlipCopyTicks, &A->FlipCopyMaxTicks, t0);
}

static VOID VsyncDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    MW_ALLOCATION *p;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (a->Stopping || !a->VsyncOn)
        return;
    /* The period the timer really keeps, which is the most flips a second
     * there can be. */
    if (a->LastVsyncTick != 0)
        MwTickAdd(&a->VsyncTicks, &a->VsyncGapMaxTicks, a->LastVsyncTick);
    a->LastVsyncTick = MwTick();
    p = a->ScanAlloc;
    /* Compare flips, not allocations. On the TaishanPi 3M the compositor
     * flipped 71 times and one frame reached the framebuffer: it flips to
     * the same primary again and again, with new pixels in it. */
    if (a->CopyThread != NULL) {
        LONG seq = a->FlipSeq;
        if (seq != a->ShownSeq) {
            if (InterlockedCompareExchange(&a->CopyBusy, 1, 0) == 0) {
                if (p != NULL && a->SourceVisible) {
                    KeSetEvent(&a->CopyEvent, IO_NO_INCREMENT, FALSE);
                } else {
                    /* Nothing to copy: the flip is done as it is. */
                    a->ShownAddr = a->ScanAddr;
                    a->ShownSeq = seq;
                    InterlockedExchange(&a->CopyBusy, 0);
                }
            } else {
                a->CopyDeferred++;      /* the previous frame is still being copied */
            }
        }
    } else {
        LONG seq = a->FlipSeq;
        if (p != NULL && seq != a->ShownSeq && a->SourceVisible) {
            ULONG64 flipped = a->FlipTick;
            ShowPrimary(a, p);
            a->ShownAlloc = p;
            a->ShownSeq = seq;
            /* How long the flip waited for this vsync, copy included. */
            if (flipped != 0)
                MwTickAdd(&a->FlipWaitTicks, &a->FlipWaitMaxTicks, flipped);
        }
    }
    a->Vsyncs++;
    if (Synchronize(a, SyncVsync, a))
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}

/* Copies the current primary to the framebuffer when the vsync DPC asks,
 * and then lets the next vsync report that flip. It reads which primary
 * under CopyLock, so an allocation freed meanwhile is not touched. */
static VOID CopyThread(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    KAFFINITY big = (KAFFINITY)0xF0 & KeQueryActiveProcessors();

    /* The Cortex-A72 cores are 4 to 7 on the RK3576. */
    if (big != 0)
        KeSetSystemAffinityThreadEx(big);
    KeSetPriorityThread(KeGetCurrentThread(), 15);
    for (;;) {
        KeWaitForSingleObject(&a->CopyEvent, Executive, KernelMode, FALSE, NULL);
        if (a->CopyStop)
            break;
        ExAcquireFastMutex(&a->CopyLock);
        {
            LONG seq = a->FlipSeq;
            MW_ALLOCATION *p = a->ScanAlloc;
            PHYSICAL_ADDRESS addr = a->ScanAddr;
            ULONG64 flipped = a->FlipTick;

            if (MwScanUsable(a, p)) {
                /* The display controller reads it; a flip that raced the
                 * switch is put right here. */
                if (!a->ScanShowing)
                    MwScanFlip(a, p);
            } else if (p != NULL && a->SourceVisible && !a->Stopping) {
                ShowPrimary(a, p);
                MwScanToFb(a);
            }
            a->ShownAlloc = p;
            a->ShownAddr = addr;
            a->ShownSeq = seq;
            if (flipped != 0)
                MwTickAdd(&a->FlipWaitTicks, &a->FlipWaitMaxTicks, flipped);
        }
        ExReleaseFastMutex(&a->CopyLock);
        InterlockedExchange(&a->CopyBusy, 0);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* An allocation is going away or losing its pages: the copy thread must
 * not be reading it, and must not pick it up afterwards. */
void MwCopyForget(MW_ADAPTER *A, MW_ALLOCATION *Alloc, BOOLEAN Freed)
{
    ExAcquireFastMutex(&A->CopyLock);
    if (Freed) {
        InterlockedCompareExchangePointer((PVOID volatile *)&A->ScanAlloc, NULL, Alloc);
        if (A->ShownAlloc == Alloc)
            A->ShownAlloc = NULL;
    } else {
        Alloc->Mdl = NULL;
    }
    ExReleaseFastMutex(&A->CopyLock);
}

void MwCommandInit(MW_ADAPTER *A)
{
    HANDLE h;
    OBJECT_ATTRIBUTES oa;

    A->Stopping = FALSE;
    ExInitializeFastMutex(&A->CopyLock);
    KeInitializeEvent(&A->CopyEvent, SynchronizationEvent, FALSE);
    A->CopyStop = FALSE;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, CopyThread, A))) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                                  (PVOID *)&A->CopyThread, NULL)))
            A->CopyThread = NULL;       /* it runs, idle; the DPC copies */
        ZwClose(h);
    }
    KeInitializeTimer(&A->PollTimer);
    KeInitializeDpc(&A->PollDpc, PollDpc, A);
    KeInitializeTimer(&A->VsyncTimer);
    KeInitializeDpc(&A->VsyncDpc, VsyncDpc, A);
}

void MwCommandStop(MW_ADAPTER *A)
{
    A->Stopping = TRUE;
    KeMemoryBarrier();
    KeCancelTimer(&A->PollTimer);
    InterlockedExchange(&A->VsyncOn, 0);
    KeCancelTimer(&A->VsyncTimer);
    KeFlushQueuedDpcs();
    InterlockedExchange(&A->PollArmed, 0);
    /* A preemption request still waiting is not answered: dxgkrnl is
     * stopping the adapter, and a chain stopped part way belongs to a
     * submission it already took back. */
    A->PreemptPending = FALSE;
    if (A->CopyThread != NULL) {
        PKTHREAD t = A->CopyThread;
        A->CopyStop = TRUE;
        KeSetEvent(&A->CopyEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(t, Executive, KernelMode, FALSE, NULL);
        A->CopyThread = NULL;
        ObDereferenceObject(t);
    }
}

NTSTATUS APIENTRY MwSubmitCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    const MW_DMA_PRIVATE *priv = (const MW_DMA_PRIVATE *)Arg->pDmaBufferPrivateData;
    MW_SUBMISSION sub;
    MW_SYNC sync;

    RtlZeroMemory(&sub, sizeof(sub));
    sub.FenceId = Arg->SubmissionFenceId;
    sub.SubmitTick = MwTick();
    /* The context: where a preempted submission leaves its resume point,
     * and which waits were behind another context's chain. Only compared
     * and written to with TimeSlice set. Paging submissions have none. */
    if (!Arg->Flags.Paging)
        sub.Ctx = (MW_CONTEXT *)Arg->hContext;
    /* A present: done here, on the CPU; the submission then carries no
     * job and completes at once. */
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Present.Kind != MW_PRESENT_NONE)
        DoPresent(a, &priv->Present);
    /* Paging buffers (empty: map/unmap happen on the CPU), presents and
     * anything without our private data carry no jobs. */
    if (!Arg->Flags.Paging && priv != NULL && Arg->DmaBufferPrivateDataSize >= sizeof(*priv) &&
        priv->Reserved == MW_DMA_MAGIC && priv->Count <= MW_MAX_COMMANDS) {
        sub.Count = priv->Count;
        RtlCopyMemory(sub.Commands, priv->Commands, sub.Count * sizeof(MW_COMMAND));
    }

    sync.A = a;
    sync.New = &sub;
    sync.Status = STATUS_UNSUCCESSFUL;
    (void)Synchronize(a, SyncSubmit, &sync);
    if (NT_SUCCESS(sync.Status) && sub.Count != 0 && !a->Stopping &&
        InterlockedExchange(&a->PollArmed, 1) == 0)
        MwArmPoll(a);
    a->Submits++;
    return sync.Status;
}

typedef struct {
    MW_ADAPTER *A;
    ULONG Fence;
} MW_PREEMPT;

static BOOLEAN SyncPreempt(PVOID Context)
{
    MW_PREEMPT *p = (MW_PREEMPT *)Context;
    MW_ADAPTER *a = p->A;

    a->PreemptRequests++;
    if (a->PreemptPending)
        a->PreemptOverlap++;            /* answered once, with the newer fence */
    else
        a->PreemptTick = MwTick();
    if (!a->Running && a->QCount == 0)
        a->PreemptIdle++;
    a->PreemptPending = TRUE;
    a->PreemptFence = p->Fence;
    if (Reap(a, FALSE) || !a->Running)
        Kick(a);
    MaybeStop(a);
    if (a->NeedDpc) {
        a->NeedDpc = FALSE;
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    return TRUE;
}

NTSTATUS APIENTRY MwPreemptCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    MW_PREEMPT p;

    /* TimeSlice 0: as before, when the INF's EnablePreemption 0 meant this
     * was never called. With EnablePreemption 1 and TimeSlice 0 the request
     * is never answered and dxgkrnl's timeout detection resets the GPU. */
    if (a->Slice == 0)
        return STATUS_SUCCESS;
    p.A = a;
    p.Fence = Arg->PreemptionFenceId;
    (void)Synchronize(a, SyncPreempt, &p);
    /* The poll timer takes the stop once the slice is up. */
    if (!a->Stopping && a->PreemptPending && InterlockedExchange(&a->PollArmed, 1) == 0)
        MwArmPoll(a);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryCurrentFence(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    Arg->CurrentFence = a->LastCompletedFence;
    return STATUS_SUCCESS;
}

/* ---- recovery ---- */

static BOOLEAN SyncReset(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    if (a->Running)
        MkJobHardStop(&a->Gpu, a->RunningSlot);
    a->Running = FALSE;
    a->QHead = a->QCount = 0;
    a->NeedDpc = FALSE;
    /* A request waiting is part of what the reset recovers from, and a
     * point saved in a context refers to GPU state the reset discards. */
    a->PreemptPending = FALSE;
    a->StopAsked = FALSE;
    a->ResetGen++;
    if (a->Gpu.Regs != NULL) {
        a->Gpu.Resets++;
        a->GpuUp = MkGpuBringup(&a->Gpu);
    }
    return TRUE;
}

NTSTATUS APIENTRY MwResetFromTimeout(IN_CONST_HANDLE hAdapter)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    (void)Synchronize(a, SyncReset, a);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRestartFromTimeout(IN_CONST_HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCollectDbgInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    if (Arg->pBuffer != NULL && Arg->BufferSize > 0)
        (void)RtlStringCbPrintfA((NTSTRSAFE_PSTR)Arg->pBuffer, Arg->BufferSize,
                                 "maliwddm: submitted %u completed %u running %u queued %u "
                                 "done %u failed %u timedout %u js 0x%x fault 0x%x @0x%llx "
                                 "slice %u preempt %u/%u stop %u/%u",
                                 a->LastSubmittedFence, a->LastCompletedFence, a->Running, a->QCount,
                                 a->JobsDone, a->JobsFailed, a->JobsTimedOut, a->LastJsStatus,
                                 a->LastFaultStatus, a->LastFaultAddress, a->Slice, a->PreemptPending,
                                 a->PreemptFence, a->StopAsked, a->SoftStops);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwGetNodeMetadata(IN_CONST_HANDLE hAdapter, UINT NodeOrdinal,
                                    DXGKARG_GETNODEMETADATA *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    RtlZeroMemory(Arg, sizeof(*Arg));
    Arg->EngineType = DXGK_ENGINE_TYPE_3D;
    (void)RtlStringCbCopyW(Arg->FriendlyName, sizeof(Arg->FriendlyName), L"Mali-G52 job manager");
    return STATUS_SUCCESS;
}

/* ---- what WDDM 1.2+ requires of a full driver ----
 * dxgkrnl failed the adapter (code 43) right after DRIVERCAPS while these
 * five were NULL; viogpu3d fills all of them. */

NTSTATUS APIENTRY MwResetEngine(IN_CONST_HANDLE hAdapter, DXGKARG_RESETENGINE *Arg)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    (void)Synchronize(a, SyncReset, a);
    Arg->LastAbortedFenceId = a->LastCompletedFence;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryEngineStatus(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCancelCommand(IN_CONST_HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

/* No vsync interrupt yet (the VOP2 is not ours to touch). */
/* Only VSync is controllable here. Before this, nothing ever raised one,
 * and on installed Windows (2026-10-17) the desktop hung right after the
 * commit: ControlInterrupt was called, then no SetVidPnSourceAddress or
 * Present ever came, while dxgkrnl's flip queue kept growing. */
NTSTATUS APIENTRY MwControlInterrupt(IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE Type,
                                     IN_BOOLEAN Enable)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;

    if (Type != DXGK_INTERRUPT_CRTC_VSYNC)
        return STATUS_SUCCESS;
    if (Enable) {
        if (InterlockedExchange(&a->VsyncOn, 1) == 0) {
            LARGE_INTEGER due;
            a->LastVsyncTick = 0;       /* the gap while it was off is not a period */
            due.QuadPart = -166667;           /* 16.7 ms, relative */
            KeSetTimerEx(&a->VsyncTimer, due, 16, &a->VsyncDpc);
        }
    } else if (InterlockedExchange(&a->VsyncOn, 0) != 0) {
        KeCancelTimer(&a->VsyncTimer);
    }
    MwLog(a, L"VsyncOn", (ULONG)a->VsyncOn);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwGetScanLine(IN_CONST_HANDLE hAdapter, DXGKARG_GETSCANLINE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->InVerticalBlank = FALSE;
    Arg->ScanLine = 0;
    return STATUS_SUCCESS;
}

/* GDI hardware acceleration (SupportKernelModeCommandBuffer). A render-only
 * adapter is never GDI's, so this is not expected to be called; it says so
 * instead of building a DMA buffer it cannot. */
NTSTATUS APIENTRY MwRenderKm(IN_CONST_HANDLE hContext, DXGKARG_RENDER *Arg)
{
    MW_CONTEXT *ctx = (MW_CONTEXT *)hContext;
    UNREFERENCED_PARAMETER(Arg);
    MwLog(ctx->Device->Adapter, L"RenderKmCalled", 1);
    return STATUS_NOT_SUPPORTED;
}

/* The third DDI SupportPerEngineTDR requires (with ResetEngine and
 * QueryEngineStatus): dxgkrnl said so in WinPE run 10 ("Driver reports
 * SupportPerEngineTDR cap but does not fill in all of the required DDIs")
 * and failed the start with STATUS_INVALID_PARAMETER. One engine, so
 * nothing depends on it. */
NTSTATUS APIENTRY MwQueryDependentEngineGroup(IN_CONST_HANDLE hAdapter,
                                              DXGKARG_QUERYDEPENDENTENGINEGROUP *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    Arg->DependentNodeOrdinalMask = 0;
    return STATUS_SUCCESS;
}
