// SPDX-License-Identifier: GPL-2.0
/*
 * The "RK3576 GPU" performance counter set (PCW, kernel mode), for PDH
 * consumers: perfmon, typeperf, a tray monitor. One instance, "gpu0":
 *
 *   \RK3576 GPU(gpu0)\Temperature (C)   the hotter of TSADC 3 and 5, whole C
 *   \RK3576 GPU(gpu0)\Clock (MHz)       CRU_CLKSEL_CON165 now
 *   \RK3576 GPU(gpu0)\% busy            job chains on the GPU, per interval
 *   \RK3576 GPU(gpu0)\Throttled         1 while the 85 C fallback holds 594 MHz
 *
 * The set is described in maliwddm.man; ctrpp turns it into
 * maliwddmCounters.h (the Register/Add/Unregister helpers used here) and
 * maliwddmCounters.rc (the names, in maliwddm.sys). docs/PERFDATA.md
 * section 6.
 *
 * Independent of the WDDM version (1.3 and 2.x alike) and of dxgkrnl's
 * performance queries. It costs nothing until a consumer collects: PCW
 * calls PcwCallback, at PASSIVE_LEVEL, only then, and the callback reads
 * two TSADC registers and one CRU register. A failed registration is
 * logged (PcwRegister in the trace) and changes nothing else.
 *
 * The callback runs on a consumer's thread, at any time, also while the
 * adapter stops. MwPcwAdapter, under MwPcwLock, is the one started
 * adapter: set at the end of a successful StartDevice, cleared before the
 * TSADC and CRU mappings go away (MwAdapterStop) and at RemoveDevice. With
 * no adapter, the set has no instance.
 */
#include "maliwddm.h"

/* The structure maliwddm.man names (struct GpuValues). */
typedef struct _MW_PCW_GPU_VALUES {
    ULONG TemperatureC;
    ULONG ClockMhz;
    ULONGLONG Busy100ns;
    ULONG Throttled;
} MW_PCW_GPU_VALUES;

#pragma warning(push)
#pragma warning(disable: 4201 4214)
#include "maliwddmCounters.h"
#pragma warning(pop)

static FAST_MUTEX MwPcwLock;
static MW_ADAPTER *MwPcwAdapter;
static BOOLEAN MwPcwRegistered;
static NTSTATUS MwPcwStatus = STATUS_UNSUCCESSFUL;

static void Values(MW_ADAPTER *A, MW_PCW_GPU_VALUES *V)
{
    long mc = 0;
    int valid = MwGpuTempMilliC(A, &mc);
    ULONG64 ticks = A->SlotTicks[0] + A->SlotTicks[1];

    RtlZeroMemory(V, sizeof(*V));
    V->TemperatureC = (ULONG)MwPcwTempC(valid, mc);
    V->ClockMhz = (ULONG)MwPcwMhz(MwGpuClkHzNow(A));
    V->Busy100ns = MwPcwTicksTo100ns(ticks, A->PerfHz);
    V->Throttled = A->ThermOn && A->Therm.Held ? 1u : 0u;
}

static NTSTATUS NTAPI PcwCallback(PCW_CALLBACK_TYPE Type, PPCW_CALLBACK_INFORMATION Info, PVOID Context)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"gpu0");
    MW_PCW_GPU_VALUES v;
    NTSTATUS st = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Context);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_SUCCESS;
    ExAcquireFastMutex(&MwPcwLock);
    if (MwPcwAdapter != NULL) {
        if (Type == PcwCallbackEnumerateInstances) {
            st = MwAddRk3576Gpu(Info->EnumerateInstances.Buffer, &name, 0, NULL);
        } else if (Type == PcwCallbackCollectData) {
            Values(MwPcwAdapter, &v);
            st = MwAddRk3576Gpu(Info->CollectData.Buffer, &name, 0, &v);
        }
    }
    ExReleaseFastMutex(&MwPcwLock);
    return st;
}

/* DriverEntry. */
void MwPcwInit(void)
{
    ExInitializeFastMutex(&MwPcwLock);
    MwPcwAdapter = NULL;
    MwPcwStatus = MwRegisterRk3576Gpu(PcwCallback, NULL);
    MwPcwRegistered = NT_SUCCESS(MwPcwStatus);
}

/* DriverUnload: after every adapter is gone. */
void MwPcwExit(void)
{
    if (MwPcwRegistered) {
        MwUnregisterRk3576Gpu();
        MwPcwRegistered = FALSE;
    }
}

/* The end of a successful StartDevice. */
void MwPcwAttach(MW_ADAPTER *A)
{
    MwLog(A, L"PcwRegister", (ULONG)MwPcwStatus);
    if (!MwPcwRegistered)
        return;
    ExAcquireFastMutex(&MwPcwLock);
    MwPcwAdapter = A;
    ExReleaseFastMutex(&MwPcwLock);
}

/* Before the adapter's mappings go (MwAdapterStop) and at RemoveDevice.
 * Waits for a callback that is reading this adapter. */
void MwPcwDetach(MW_ADAPTER *A)
{
    if (!MwPcwRegistered)
        return;
    ExAcquireFastMutex(&MwPcwLock);
    if (MwPcwAdapter == A)
        MwPcwAdapter = NULL;
    ExReleaseFastMutex(&MwPcwLock);
}
