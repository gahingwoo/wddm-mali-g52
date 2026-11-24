// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm: driver entry, PnP and power, adapter capabilities, interrupts,
 * escapes, and the registry trace that found every M5.1 bug.
 */
#include "maliwddm.h"
#include "wddm2.h"

/* ---- settings: DWORDs in the device's Device Parameters key ---- */

ULONG MwSetting(MW_ADAPTER *A, PCWSTR Name, ULONG Default)
{
    HANDLE key;
    UNICODE_STRING name;
    struct { KEY_VALUE_PARTIAL_INFORMATION Info; ULONG Pad; } v;
    ULONG len, value = Default;

    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return Default;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &key)))
        return Default;
    RtlInitUnicodeString(&name, Name);
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, &v, sizeof(v), &len)) &&
        v.Info.Type == REG_DWORD && v.Info.DataLength == sizeof(ULONG))
        value = *(ULONG UNALIGNED *)v.Info.Data;
    ZwClose(key);
    return value;
}

void MwSetSetting(MW_ADAPTER *A, PCWSTR Name, ULONG Value)
{
    HANDLE key;
    UNICODE_STRING name;

    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    RtlInitUnicodeString(&name, Name);
    (void)ZwSetValueKey(key, &name, 0, REG_DWORD, &Value, sizeof(Value));
    (void)ZwFlushKey(key);
    ZwClose(key);
}

/* ---- trace ---- */

void MwLog(MW_ADAPTER *A, PCWSTR Name, ULONG Value)
{
    HANDLE key, sub;
    UNICODE_STRING name, subName = RTL_CONSTANT_STRING(L"maliwddm");
    OBJECT_ATTRIBUTES oa;

    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "maliwddm: %ws = 0x%08x\n", Name, Value);
    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    /* A subkey of Device Parameters of its own, so a test run can delete
     * the previous run's trace (and malidod's) in one step. */
    InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, key, NULL);
    if (NT_SUCCESS(ZwCreateKey(&sub, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        RtlInitUnicodeString(&name, Name);
        (void)ZwSetValueKey(sub, &name, 0, REG_DWORD, &Value, sizeof(Value));
        /* To disk now: a bugcheck loses registry writes still in memory,
         * and the last value before one is the one that matters. */
        (void)ZwFlushKey(sub);
        /* In probe mode, again under Probe<version> (probe.c). */
        MwProbeMirror(sub, &name, Value);
        ZwClose(sub);
    }
    ZwClose(key);
}

/* The CI run that built this image (0 for a local build): WDK builds are
 * deterministic, so __DATE__ and __TIME__ do not exist. */
#ifndef MW_BUILD
#define MW_BUILD 0
#endif

void MwLogHook(void *Owner, PCWSTR Name, ULONG Value)
{
    MwLog((MW_ADAPTER *)Owner, Name, Value);
}

/* Seen_<ddi> on a DDI's first call, Fail_<ddi> on any failure; Name is
 * "Fail_<ddi>". Only at PASSIVE_LEVEL, where the registry can be written. */
/* Enter_<ddi> = call order, written BEFORE a DDI's first call runs, so a
 * DDI that never returns (a bugcheck inside it) is still on record. Name is
 * "Enter_<ddi>". */
void MwEnter(MW_ADAPTER *A, PCWSTR Name)
{
    if (A == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    ULONG h = 0;
    for (PCWSTR c = Name; *c; c++)
        h = h * 31 + *c;
    if (A->Entered & (1ULL << (h % 64)))
        return;
    A->Entered |= 1ULL << (h % 64);
    MwLog(A, Name, ++A->CallOrder);
}

NTSTATUS MwRet(MW_ADAPTER *A, PCWSTR Name, NTSTATUS St)
{
    if (A == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return St;
    ULONG h = 0;
    for (PCWSTR c = Name; *c; c++)
        h = h * 31 + *c;
    if (!(A->Seen & (1ULL << (h % 64)))) {
        WCHAR seen[64];
        UNICODE_STRING us;
        A->Seen |= 1ULL << (h % 64);
        RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
        if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Seen_%ws", Name + 5)))
            MwLog(A, seen, (ULONG)St);
        /* Which DDI dxgkrnl called first, second, ... */
        RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
        if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Order_%ws", Name + 5)))
            MwLog(A, seen, ++A->CallOrder);
    }
    if (!NT_SUCCESS(St)) {
        MwLog(A, Name, (ULONG)St);
        MwLog(A, L"LastFailedDdi", (ULONG)St);
    }
    return St;
}

/* ---- PnP ---- */

static NTSTATUS APIENTRY MwAddDevice(IN_CONST_PDEVICE_OBJECT Pdo, OUT_PPVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MW_ADAPTER), MW_TAG);
    if (a == NULL)
        return STATUS_NO_MEMORY;
    a->Pdo = (PDEVICE_OBJECT)Pdo;
    ExInitializeFastMutex(&a->MmuLock);
    ExInitializeFastMutex(&a->UmdLock);
    a->Gpu.Log = MwLogHook;
    a->Gpu.LogOwner = a;
    MwCommandInit(a);
    /* The probe's watchdog, from here so a hang in StartDevice is seen. */
    MwProbeStart(a);
    MwBc("AddDevice: adapter context %p", a);
    *Context = a;
    return STATUS_SUCCESS;
}

/* The GPU's clock.
 *
 * The firmware leaves clk_gpu_src_pre at GPLL / 6 = 198 MHz, which is the
 * rate the device tree assigns before Linux's devfreq takes over; the
 * operating points start at 300 MHz and go to 950. Nothing in Windows
 * raises it, so everything up to 2026-11-14 ran there: FurMark through the
 * OpenGL ICD had the GPU 97% busy with jobs of up to 425 ms.
 *
 * CRU_CLKSEL_CON165 (0x27200594): mux in [7:5] (0 gpll, 1 cpll, 2 aupll,
 * 3 spll, 4 lpll), divider minus one in [4:0], [8] the PVTPLL path, [9]
 * the PVTPLL source; write-enable mask in the top half (TRM, CRU chapter;
 * clk-rk3576.c, CLK_GPU_SRC_PRE). The operating points up to 600 MHz ask
 * for the lowest voltage (mainline 700 mV); above that the supply has to
 * be higher, and this driver never touches the regulator.
 *
 * So by default it takes GPLL (1188 MHz) or CPLL (1000 MHz) with a divider
 * that keeps the result at or below 600 MHz. With the setting GpuClkHigh
 * (default 0) it also allows SPLL (702 MHz, a fixed PLL in the secure CRU:
 * mainline's clock-spll, TF-A's SPLL_RATE) and any rate up to 950 MHz, but
 * only a rate whose operating point the supply already meets:
 *
 *   GpuClkHigh 1: mainline rk3576.dtsi gpu_opp_table
 *   GpuClkHigh 2: the vendor kernel's table for a plain RK3576 at its
 *                 slowest PVTM level, which asks more
 *
 * The rate is held to the lowest operating point at or above it, so 702 MHz
 * is held to the 800 MHz one, plus the chip's own OTP offset (gpu-opp-info)
 * when the firmware reports one. The supply comes from the firmware's
 * GpuRail variable (edk2-rk3576 GpuRail.c, RK806 BUCK5 read at boot) and
 * the setting GpuRailUv (microvolts, a value verified some other way); with
 * both, the lower counts; with neither, nothing above 600 MHz is allowed.
 * AUPLL (audio's) and LPLL (the A53 cluster's, which moves with the CPU)
 * are never taken, and neither is a CRU on the PVTPLL path.
 *
 * Above 600 MHz, a job that faults or times out puts the clock back to
 * GPLL / 2 = 594 MHz at once (GpuClkFallbacks counts it). */
#define MW_CRU_CLKSEL165   0x27200594u
#define MW_GPU_MAX_HZ      600000000u
#define MW_GPU_CEIL_HZ     950000000u   /* mainline's highest operating point */
#define MW_RAIL_UV_MIN     550000u      /* vdd_gpu_s0 regulator-min/max-microvolt */
#define MW_RAIL_UV_MAX     900000u

static const ULONG MwGpuPll[8] = { 1188000000u, 1000000000u, 0, 702000000u, 0, 0, 0, 0 };

/* MHz, mainline opp-microvolt, vendor plain-RK3576 slowest level (0: none). */
static const struct { ULONG Mhz, Mainline, Vendor; } MwGpuOpp[] = {
    { 300, 700000, 712500 }, { 400, 700000, 712500 }, { 500, 700000, 712500 },
    { 600, 700000, 712500 }, { 700, 725000, 750000 }, { 800, 775000, 812500 },
    { 900, 825000, 875000 }, { 950, 850000, 0 },
};

/* The firmware's report (edk2-rk3576 GpuRail.h, GPU_RAIL_REPORT). */
#pragma pack(push, 1)
typedef struct {
    ULONG Signature;
    USHORT Version, Size;
    ULONG RailUv;
    UCHAR OnVsel, SlpVsel, PowerEn1, Buck5Config;
    UCHAR Specification, GpuLeakage, Flags, Reserved0;
    USHORT OppInfoMinMhz, OppInfoMaxMhz;
    UCHAR OppInfoVoltMv, OppInfoLength;
    USHORT Reserved1;
    ULONG ClkSel165;
} MW_GPU_RAIL_REPORT;
#pragma pack(pop)
C_ASSERT(sizeof(MW_GPU_RAIL_REPORT) == 32);
#define MW_GPU_RAIL_SIGNATURE   0x52555047u   /* 'G' 'P' 'U' 'R' */
#define MW_GPU_RAIL_VALID       0x01u
#define MW_GPU_RAIL_OTP_VALID   0x04u
#define MW_GPU_RAIL_PLAIN       0x08u

/* 7701c75f-f454-4aab-9445-117f09fb87cf, edk2-rk3576 GpuRail.c */
static GUID MwGpuRailGuid = { 0x7701c75f, 0xf454, 0x4aab, { 0x94, 0x45, 0x11, 0x7f, 0x09, 0xfb, 0x87, 0xcf } };

/* The TSADC (TRM chapter 19): its CRU gates are CLKGATE_CON(13) bits 8
 * (pclk) and 9 (clk), its resets SOFTRST_CON(13) bits 8 and 9, all in the
 * CRU page this driver already maps for CLKSEL_CON165; sampling runs when
 * TSADC_AUTO_CON bit 0 is set (edk2-rk3576 Rk3576TsadcLib.c, which starts
 * it and arms TSHUT). A read of the TSADC while it is gated or in reset can
 * be an SError on this SoC, so the TSADC is read only after the CRU says it
 * runs (TsadcMapIfRunning) or with the setting PerfTemp 1. */
#define MW_TSADC_PA          0x2AE70000u
#define MW_TSADC_AUTO_CON    0x004u
#define MW_TSADC_DATA(n)     (0x02Cu + (n) * 4u)
#define MW_CRU_PA            0x27200000u
#define MW_CRU_GATE13        0x834u
#define MW_CRU_SOFTRST13     0xA34u
#define MW_TSADC_CRU_BITS    0x300u

static PUCHAR TsadcMapIfRunning(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR cru, ts;
    ULONG gate, rst, autocon;

    pa.QuadPart = MW_CRU_PA;
    cru = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (cru == NULL)
        return NULL;
    gate = READ_REGISTER_ULONG((PULONG)(cru + MW_CRU_GATE13));
    rst = READ_REGISTER_ULONG((PULONG)(cru + MW_CRU_SOFTRST13));
    MmUnmapIoSpace(cru, PAGE_SIZE);
    if ((gate & MW_TSADC_CRU_BITS) != 0 || (rst & MW_TSADC_CRU_BITS) != 0) {
        MwLog(A, L"TsadcNotRunningGate", gate);
        return NULL;
    }
    pa.QuadPart = MW_TSADC_PA;
    ts = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (ts == NULL)
        return NULL;
    autocon = READ_REGISTER_ULONG((PULONG)(ts + MW_TSADC_AUTO_CON));
    if ((autocon & 1u) == 0) {
        MmUnmapIoSpace(ts, PAGE_SIZE);
        MwLog(A, L"TsadcNotRunningAuto", autocon);
        return NULL;
    }
    return ts;
}

static BOOLEAN SyncThermDrop(PVOID Context);
static BOOLEAN SyncThermRestore(PVOID Context);

/* The TDR snapshots not written yet (command.c, TdrSnap), as
 * Tdr<n>_<field>, n = the snapshot's number mod MW_TDR_SNAPS. */
static void TdrLogNew(MW_ADAPTER *A)
{
    static const PCWSTR names[] = {
        L"Node", L"Running", L"Slot", L"RunMs", L"JsStatus", L"JsHeadLo", L"JsTailLo", L"JobRaw",
        L"JobPending", L"QCount", L"HeadFence", L"HeadNext", L"HeadCount", L"HeadStarted",
        L"HeadAgeMs", L"LastSubmitted", L"LastCompleted", L"SinceDoneMs", L"SinceChainEndMs",
        L"SinceIsrMs", L"SincePollMs", L"SinceSubmitMs", L"PreemptPending", L"PreemptMs",
        L"StopAsked", L"StopMs", L"GpuClk", L"MilliC", L"JobsDone", L"ClkPending",
    };
    LONG count = A->TdrCount;

    C_ASSERT(sizeof(MW_TDR_SNAP) == sizeof(names) / sizeof(names[0]) * sizeof(ULONG));
    if (A->TdrLogged < count - MW_TDR_SNAPS)
        A->TdrLogged = count - MW_TDR_SNAPS;
    for (; A->TdrLogged < count; A->TdrLogged++) {
        const ULONG *v = (const ULONG *)&A->Tdr[A->TdrLogged % MW_TDR_SNAPS];
        for (ULONG i = 0; i < ARRAYSIZE(names); i++) {
            WCHAR name[48];
            UNICODE_STRING us;
            RtlInitEmptyUnicodeString(&us, name, sizeof(name));
            if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Tdr%u_%ws", (ULONG)(A->TdrLogged % MW_TDR_SNAPS), names[i])))
                MwLog(A, name, v[i]);
        }
        MwLog(A, L"TdrCount", (ULONG)(A->TdrLogged + 1));
    }
}

/* The registry trace of the guard and of TDRs (MwLog needs PASSIVE_LEVEL),
 * from a work item queued when something changed. */
static VOID ClkLogWork(PVOID IoObject, PVOID Context, PIO_WORKITEM Item)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(IoObject);
    UNREFERENCED_PARAMETER(Item);

    MwLog(a, L"GpuClkThermalDrops", a->GpuClkThermalDrops);
    MwLog(a, L"GpuClkThermalRestores", a->GpuClkThermalRestores);
    MwLog(a, L"GpuClkHighFaults", a->Therm.HighFaults);
    MwLog(a, L"GpuClkThrottleReason", MwThrottleReason(&a->Therm));
    MwLog(a, L"GpuClkFaultLocked", a->Therm.FaultLocked ? 1 : 0);
    MwLog(a, L"GpuClkSwitches", a->ClkSwitches);
    MwLog(a, L"GpuClkDeferred", a->ClkDeferred);
    TdrLogNew(a);
    InterlockedExchange(&a->ClkLogQueued, 0);
}

static void ClkLogQueue(MW_ADAPTER *A)
{
    if (A->ClkLogItem != NULL && InterlockedCompareExchange(&A->ClkLogQueued, 1, 0) == 0)
        IoQueueWorkItemEx(A->ClkLogItem, ClkLogWork, DelayedWorkQueue, A);
}

/* After a TDR (command.c): the snapshot goes into the trace. */
void MwTdrLogQueue(MW_ADAPTER *A)
{
    ClkLogQueue(A);
}

/* Once a second while GpuClkHigh is on (thermal_calc.h): faults above
 * 600 MHz since the last reading first, then the temperature. */
static VOID ThermDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    KIRQL old;
    long mc = 0;
    int valid, act;
    BOOLEAN ret = FALSE, changed = FALSE;
    ULONG64 now = KeQueryInterruptTime() / 10000;
    LONG faults;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (!a->ThermOn || a->TsadcVa == NULL)
        return;
    valid = MwThermalHotter(READ_REGISTER_ULONG((PULONG)(a->TsadcVa + MW_TSADC_DATA(MW_THERMAL_CH_A))),
                            READ_REGISTER_ULONG((PULONG)(a->TsadcVa + MW_TSADC_DATA(MW_THERMAL_CH_B))), &mc);
    KeAcquireSpinLock(&a->ClkLock, &old);
    faults = a->HighFaultEvents;
    while (a->HighFaultsSeen != faults) {
        a->HighFaultsSeen++;
        (void)MwThermalFault(&a->Therm, now, 1);
        changed = TRUE;
    }
    act = MwThermalStep(&a->Therm, valid, mc, now, a->RestoreHoldMs);
    if (act == MW_THERMAL_DROP) {
        InterlockedIncrement((volatile LONG *)&a->GpuClkThermalDrops);
        (void)a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle, SyncThermDrop, a, 0, &ret);
        changed = TRUE;
    } else if (act == MW_THERMAL_RESTORE) {
        if (NT_SUCCESS(a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle, SyncThermRestore, a, 0, &ret)) && ret)
            InterlockedIncrement((volatile LONG *)&a->GpuClkThermalRestores);
        changed = TRUE;
    }
    KeReleaseSpinLock(&a->ClkLock, old);
    if (changed)
        ClkLogQueue(a);
}

/* Once per start, at passive level, before any stored clock is applied. */
static void GpuRailInit(MW_ADAPTER *A)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"GpuRail");
    MW_GPU_RAIL_REPORT r;
    ULONG len = sizeof(r), attr = 0, fw = 0, reg;

    A->RailUv = 0;
    A->RailSource = 0;
    A->RailOtpMv = 0;
    A->RailOtpMinMhz = A->RailOtpMaxMhz = 0;
    A->RailNotPlain = FALSE;
    A->GpuClkHigh = MwSetting(A, L"GpuClkHigh", 0);
    if (A->GpuClkHigh > 2)
        A->GpuClkHigh = 0;
    /* Temperature and clock for Task Manager (QAI 24 to 27, 2.x only). */
    A->PerfData = MwSetting(A, L"PerfData", 1);
    A->PerfSeen = 0;

    RtlZeroMemory(&r, sizeof(r));
    if (NT_SUCCESS(ExGetFirmwareEnvironmentVariable(&name, &MwGpuRailGuid, &r, &len, &attr)) &&
        len == sizeof(r) && r.Signature == MW_GPU_RAIL_SIGNATURE && r.Version == 1 &&
        r.Size == sizeof(r)) {
        MwLog(A, L"GpuRailFwFlags", r.Flags);
        if ((r.Flags & MW_GPU_RAIL_OTP_VALID) && !(r.Flags & MW_GPU_RAIL_PLAIN))
            A->RailNotPlain = TRUE;
        if ((r.Flags & MW_GPU_RAIL_OTP_VALID) && r.OppInfoVoltMv != 0) {
            A->RailOtpMv = r.OppInfoVoltMv;
            A->RailOtpMinMhz = r.OppInfoMinMhz;
            A->RailOtpMaxMhz = r.OppInfoMaxMhz;
        }
        if ((r.Flags & MW_GPU_RAIL_VALID) && r.RailUv >= MW_RAIL_UV_MIN && r.RailUv <= MW_RAIL_UV_MAX)
            fw = r.RailUv;
        MwLog(A, L"GpuRailFwUv", fw);
    } else {
        MwLog(A, L"GpuRailFwUv", 0);
    }
    reg = MwSetting(A, L"GpuRailUv", 0);
    if (reg < MW_RAIL_UV_MIN || reg > MW_RAIL_UV_MAX)
        reg = 0;
    if (fw != 0 && reg != 0) {
        A->RailUv = fw < reg ? fw : reg;
        A->RailSource = 3;
    } else if (fw != 0) {
        A->RailUv = fw;
        A->RailSource = 1;
    } else if (reg != 0) {
        A->RailUv = reg;
        A->RailSource = 2;
    }
    MwLog(A, L"GpuRailUv", A->RailUv);
    MwLog(A, L"GpuClkHighSetting", A->GpuClkHigh);

    /* Mapped for the whole start: every clock change is written from the
     * interrupt's level, between two job chains (gpuclk_calc.h). */
    if (A->CruClkVa == NULL) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = MW_CRU_CLKSEL165 & ~0xFFFu;
        A->CruClkVa = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }

    /* The temperature guard: without a TSADC that runs, no clock above
     * 600 MHz (GpuClkWrite checks ThermOn). The TSADC stays mapped for the
     * start either way once it runs, which also lets the perf escape show
     * temperatures without PerfTemp. */
    KeInitializeSpinLock(&A->ClkLock);
    RtlZeroMemory(&A->Therm, sizeof(A->Therm));
    A->GpuClkWanted = 0;
    A->ThermOn = FALSE;
    A->ClkPending = 0;
    A->HighFaultEvents = 0;
    A->HighFaultsSeen = 0;
    A->RestoreHoldMs = MwThermalHoldMs(MwSetting(A, L"GpuClkRestoreHoldSec", 0));
    MwLog(A, L"GpuClkRestoreHoldMs", (ULONG)A->RestoreHoldMs);
    if (A->ClkLogItem == NULL && MwDriverObject != NULL) {
        A->ClkLogItem = (PIO_WORKITEM)ExAllocatePool2(POOL_FLAG_NON_PAGED, IoSizeofWorkItem(), MW_TAG);
        if (A->ClkLogItem != NULL)
            IoInitializeWorkItem(MwDriverObject, A->ClkLogItem);
    }
    if (A->TsadcVa == NULL)
        A->TsadcVa = TsadcMapIfRunning(A);
    if (A->GpuClkHigh != 0 && A->TsadcVa != NULL) {
        LARGE_INTEGER due;
        KeInitializeTimer(&A->ThermTimer);
        KeInitializeDpc(&A->ThermDpc, ThermDpc, A);
        due.QuadPart = -10000LL * MW_THERMAL_PERIOD_MS;
        KeSetTimerEx(&A->ThermTimer, due, MW_THERMAL_PERIOD_MS, &A->ThermDpc);
        A->ThermOn = TRUE;
    }
    MwLog(A, L"GpuClkThermalGuard", A->ThermOn ? 1 : 0);
}

/* The supply the rate needs, by the table GpuClkHigh picks; 0 when no
 * operating point covers it. */
static ULONG GpuRequiredUv(MW_ADAPTER *A, ULONG Hz)
{
    ULONG mhz = (Hz + 999999u) / 1000000u;

    for (ULONG i = 0; i < ARRAYSIZE(MwGpuOpp); i++) {
        ULONG uv;
        if (MwGpuOpp[i].Mhz < mhz)
            continue;
        uv = A->GpuClkHigh == 2 ? MwGpuOpp[i].Vendor : MwGpuOpp[i].Mainline;
        if (uv != 0 && A->RailOtpMv != 0 &&
            MwGpuOpp[i].Mhz >= A->RailOtpMinMhz && MwGpuOpp[i].Mhz <= A->RailOtpMaxMhz)
            uv += A->RailOtpMv * 1000u;
        return uv;
    }
    return 0;
}

/* The highest rate GpuClkWrite would accept now, in Hz. */
static ULONG GpuAllowedHz(MW_ADAPTER *A)
{
    ULONG best = MW_GPU_MAX_HZ;

    if (A->GpuClkHigh == 0 || A->RailUv == 0 || A->RailNotPlain)
        return best;
    for (ULONG i = 0; i < ARRAYSIZE(MwGpuOpp); i++) {
        ULONG need = GpuRequiredUv(A, MwGpuOpp[i].Mhz * 1000000u);
        if (need != 0 && need <= A->RailUv && MwGpuOpp[i].Mhz * 1000000u > best)
            best = MwGpuOpp[i].Mhz * 1000000u;
    }
    return best;
}

/* ---- the clock switch (gpuclk_calc.h) ----
 * Every change goes through ClkRequest at the interrupt's level: it is
 * written at once when no job chain runs, else by Kick between the
 * running chain and the next (MwGpuClkBetweenChains). The writes are
 * mainline's order, one field at a time (MwClkSteps). */

typedef struct {
    MW_ADAPTER *A;
    ULONG Val;
} MW_CLK_REQ;

PDRIVER_OBJECT MwDriverObject;

static void ClkApply(MW_ADAPTER *A)
{
    ULONG want = A->ClkPending, cur, w[2], n;
    PUCHAR reg = A->CruClkVa;
    unsigned long hz;

    if (want == 0)
        return;
    A->ClkPending = 0;
    if (reg == NULL)
        return;
    want &= 0xFFu;
    cur = READ_REGISTER_ULONG((PULONG)(reg + (MW_CRU_CLKSEL165 & 0xFFFu))) & 0xFFu;
    n = MwClkSteps(cur, want, MwGpuPll, w);
    for (ULONG i = 0; i < n; i++)
        WRITE_REGISTER_ULONG((PULONG)(reg + (MW_CRU_CLKSEL165 & 0xFFFu)), w[i]);
    hz = MwClkRate(want, MwGpuPll);
    A->GpuClk = (((want >> MW_CK_MUX_SHIFT) & 7u) << 8) | ((want & MW_CK_DIV_MASK) + 1u);
    A->GpuClkIsHigh = hz > MW_GPU_MAX_HZ;
    A->ClkSwitches++;
}

/* Interrupt level, synchronised: ask for Val (MwClkVal). */
static BOOLEAN ClkRequest(MW_ADAPTER *A, ULONG Val)
{
    A->ClkPending = MW_CLK_PENDING | (Val & 0xFFu);
    if (A->Running) {
        A->ClkDeferred++;
        return FALSE;
    }
    ClkApply(A);
    return TRUE;
}

static BOOLEAN SyncClkRequest(PVOID Context)
{
    MW_CLK_REQ *r = (MW_CLK_REQ *)Context;
    return ClkRequest(r->A, r->Val);
}

/* From Kick (command.c), with no chain running, before the next starts. */
void MwGpuClkBetweenChains(MW_ADAPTER *A)
{
    if (A->ClkPending != 0)
        ClkApply(A);
}

/* The 594 MHz after a fault is above 600 only if pending says so. */
static BOOLEAN ClkHighNowOrSoon(MW_ADAPTER *A)
{
    ULONG p = A->ClkPending;
    if (p != 0)
        return MwClkRate(p & 0xFFu, MwGpuPll) > MW_GPU_MAX_HZ;
    return A->GpuClkIsHigh;
}

static NTSTATUS GpuClkWrite(MW_ADAPTER *A, ULONG Parent, ULONG Div)
{
    PUCHAR reg;
    ULONG hz, cur;
    BOOLEAN high;

    if (Parent > 7 || MwGpuPll[Parent] == 0 || Div < 1 || Div > 32)
        return STATUS_INVALID_PARAMETER;
    hz = MwGpuPll[Parent] / Div;
    high = hz > MW_GPU_MAX_HZ;
    if (high) {
        /* Every condition, each logged when it is the one that refuses. */
        if (A->GpuClkHigh == 0)
            return STATUS_INVALID_PARAMETER;
        if (hz > MW_GPU_CEIL_HZ || A->RailUv == 0 || A->RailNotPlain || A->CruClkVa == NULL) {
            MwLog(A, L"GpuClkRefusedHz", hz);
            return STATUS_INVALID_PARAMETER;
        }
        if (!A->ThermOn) {
            MwLog(A, L"GpuClkRefusedNoTemp", hz);
            return STATUS_INVALID_DEVICE_STATE;
        }
        if (GpuRequiredUv(A, hz) == 0 || GpuRequiredUv(A, hz) > A->RailUv) {
            MwLog(A, L"GpuClkRefusedHz", hz);
            MwLog(A, L"GpuClkNeedsUv", GpuRequiredUv(A, hz));
            return STATUS_INVALID_PARAMETER;
        }
    }
    reg = A->CruClkVa;
    if (reg == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    cur = READ_REGISTER_ULONG((PULONG)(reg + (MW_CRU_CLKSEL165 & 0xFFFu)));
    if (high && (cur & 0x100u) != 0) {
        /* On the PVTPLL path the mux written here is not the GPU's clock,
         * and what the GPU runs at is not known. */
        MwLog(A, L"GpuClkRefusedPvtpll", cur);
        return STATUS_INVALID_DEVICE_STATE;
    }
    {
        KIRQL old;
        BOOLEAN held, ret = FALSE;
        MW_CLK_REQ req;

        KeAcquireSpinLock(&A->ClkLock, &old);
        /* Hot, or locked after faults: the clock asked for is kept, and
         * comes back when the guard allows (thermal_calc.h). */
        held = high && MwThrottleReason(&A->Therm) != MW_THROTTLE_NONE;
        A->GpuClkWanted = high ? ((Parent << 8) | Div) : 0;
        if (!held) {
            req.A = A;
            req.Val = MwClkVal(Parent, Div);
            if (!NT_SUCCESS(A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, SyncClkRequest, &req, 0, &ret)) &&
                !A->Running) {
                /* No interrupt to synchronise with yet (StartDevice
                 * applying the stored GpuClk): nothing runs either. */
                A->ClkPending = MW_CLK_PENDING | req.Val;
                ClkApply(A);
            }
        }
        KeReleaseSpinLock(&A->ClkLock, old);
        if (held) {
            MwLog(A, L"GpuClkHeld", hz);
            MwLog(A, L"GpuClkThrottleReason", MwThrottleReason(&A->Therm));
            return STATUS_SUCCESS;
        }
    }
    MwLog(A, L"GpuClkHz", hz);
    return STATUS_SUCCESS;
}

/* At the interrupt's level: 594 MHz (GPLL / 2), between chains. TRUE when
 * the clock was or was going to be above 600 MHz. */
static BOOLEAN GpuClkSafe(MW_ADAPTER *A)
{
    if (!ClkHighNowOrSoon(A) || A->CruClkVa == NULL)
        return FALSE;
    (void)ClkRequest(A, MwClkVal(0, 2));
    return TRUE;
}

/* From Reap, at the interrupt: a job faulted or timed out, and the chain is
 * no longer running, so the drop is written at once. Above 600 MHz it
 * counts for the fault policy (ThermDpc folds it in); the clock asked for
 * is kept. Nothing logged here (MwLog needs passive level). */
void MwGpuClkFallback(MW_ADAPTER *A)
{
    if (GpuClkSafe(A)) {
        A->GpuClkFallbacks++;
        InterlockedIncrement(&A->HighFaultEvents);
    }
}

/* From SyncReset (a TDR), the same. */
void MwGpuClkResetFault(MW_ADAPTER *A)
{
    MwGpuClkFallback(A);
}

/* At the interrupt's level (DxgkCbSynchronizeExecution), from ThermDpc. */
static BOOLEAN SyncThermDrop(PVOID Context)
{
    return GpuClkSafe((MW_ADAPTER *)Context);
}

static BOOLEAN SyncThermRestore(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    ULONG w = a->GpuClkWanted;

    /* Only the clock that was asked for, and only over a clock at or
     * below 600 MHz (594 from a drop). */
    if (w == 0 || ClkHighNowOrSoon(a) || a->CruClkVa == NULL)
        return FALSE;
    (void)ClkRequest(a, MwClkVal(w >> 8, w & 0xFF));
    return TRUE;
}

static NTSTATUS GpuClk(MW_ADAPTER *A, MW_ESCAPE_GPUCLK_DATA *D, ULONG Size)
{
    NTSTATUS st = STATUS_SUCCESS;

    if (D->Mode == 1 || D->Mode == 2) {
        st = GpuClkWrite(A, D->Parent, D->Div);
        if (NT_SUCCESS(st) && D->Mode == 2)
            MwSetSetting(A, L"GpuClk", (D->Parent << 8) | D->Div);
    } else if (D->Mode == 3) {
        MwSetSetting(A, L"GpuClk", 0);
    }
    D->Applied = A->GpuClk;
    D->Stored = MwSetting(A, L"GpuClk", 0);
    if (Size >= MW_ESCAPE_GPUCLK_V2_SIZE) {
        D->High = A->GpuClkHigh;
        D->RailUv = A->RailUv;
        D->RailSource = A->RailSource | (A->RailNotPlain ? 0x100u : 0);
        D->AllowedHz = GpuAllowedHz(A);
        D->NeedUv702 = A->GpuClkHigh != 0 ? GpuRequiredUv(A, 702000000u) : 0;
        D->Fallbacks = A->GpuClkFallbacks;
    }
    if (Size >= sizeof(MW_ESCAPE_GPUCLK_DATA)) {
        D->ThermalGuard = A->ThermOn ? 1 : 0;
        D->ThermalHeld = A->Therm.Held ? 1 : 0;
        D->ThermalMilliC = (mw_s32)A->Therm.LastMilliC;
        D->ThermalDrops = A->GpuClkThermalDrops;
        D->ThermalRestores = A->GpuClkThermalRestores;
        D->Wanted = A->GpuClkWanted;
    }
    if (Size >= MW_ESCAPE_GPUCLK_V3_SIZE) {
        ULONG64 now = KeQueryInterruptTime() / 10000;
        D->ThrottleReason = MwThrottleReason(&A->Therm);
        D->HighFaults = A->Therm.HighFaults;
        D->RestoreHoldSec = (mw_u32)(A->RestoreHoldMs / 1000);
        D->CoolForSec = A->Therm.Held && A->Therm.CoolSinceMs != 0 ? (mw_u32)((now - A->Therm.CoolSinceMs) / 1000) : 0;
        D->Switches = A->ClkSwitches;
        D->Deferred = A->ClkDeferred;
    }
    return st;
}

/* What Settings, dxdiag and Win32_VideoController show for an adapter comes
 * from HardwareInformation.* in the driver's software key; nothing fills
 * them in for a driver that does not, and they were blank. There is no
 * dedicated memory to report: every allocation is system memory. */
static void PublishIdentity(MW_ADAPTER *A)
{
    static const WCHAR chip[] = L"Arm Mali-G52 (Bifrost)";
    static const WCHAR name[] = L"Rockchip RK3576 Mali-G52";
    static const WCHAR dac[] = L"Rockchip VOP2, HDMI";
    static const WCHAR dsi[] = L"Rockchip VOP2, MIPI DSI";   /* an internal panel, backlight.c */
    static const WCHAR bios[] = L"UEFI (edk2-rk3576)";
    static const struct { PCWSTR Name; const WCHAR *Text; ULONG Bytes; } v[] = {
        { L"HardwareInformation.ChipType", chip, sizeof(chip) },
        { L"HardwareInformation.AdapterString", name, sizeof(name) },
        { L"HardwareInformation.DacType", dac, sizeof(dac) },
        { L"HardwareInformation.BiosString", bios, sizeof(bios) },
    };
    HANDLE key;
    UNICODE_STRING n;

    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DRIVER, KEY_WRITE, &key)))
        return;
    for (ULONG i = 0; i < ARRAYSIZE(v); i++) {
        BOOLEAN panel = v[i].Text == dac && A->BlVa != NULL;
        RtlInitUnicodeString(&n, v[i].Name);
        (void)ZwSetValueKey(key, &n, 0, REG_SZ, (PVOID)(panel ? dsi : v[i].Text),
                            panel ? sizeof(dsi) : v[i].Bytes);
    }
    ZwClose(key);
}

/* The first memory resource is the GPU's register block. */
static NTSTATUS MapGpu(MW_ADAPTER *A)
{
    DXGK_DEVICE_INFO info;
    NTSTATUS st = A->Dxgk.DxgkCbGetDeviceInformation(A->Dxgk.DeviceHandle, &info);
    if (!NT_SUCCESS(st))
        return st;
    PCM_RESOURCE_LIST list = info.TranslatedResourceList;
    for (ULONG l = 0; list && l < list->Count; l++) {
        PCM_PARTIAL_RESOURCE_LIST p = &list->List[l].PartialResourceList;
        for (ULONG i = 0; i < p->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &p->PartialDescriptors[i];
            if (d->Type == CmResourceTypeMemory && A->Gpu.Regs == NULL) {
                A->Gpu.RegsLength = d->u.Memory.Length;
                A->Gpu.Regs = (volatile ULONG *)MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length,
                                                               PAGE_READWRITE | PAGE_NOCACHE);
            }
        }
    }
    return A->Gpu.Regs ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS APIENTRY MwStartDevice(IN_CONST_PVOID Context, IN_PDXGK_START_INFO StartInfo,
                                       IN_PDXGKRNL_INTERFACE Dxgk, OUT_PULONG Sources, OUT_PULONG Children)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(StartInfo);

    a->Dxgk = *Dxgk;
    a->Stopped = FALSE;
    /* PHYSICALADAPTERCAPS hands this handle back to dxgkrnl (wddm2x.c). */
    MwBc("StartDevice: dxgkrnl DeviceHandle %p, interface version 0x%x", Dxgk->DeviceHandle, Dxgk->Version);
    {
        LARGE_INTEGER hz;
        (void)KeQueryPerformanceCounter(&hz);
        a->PerfHz = (ULONG64)hz.QuadPart;
    }
    MwLog(a, L"Build", MW_BUILD);
    MwLog(a, L"RenderOnly", MW_RENDER_ONLY);
    MwLog(a, L"WddmVersion", MwWddmVersion);
    MwLog(a, L"DdiInterface", MwDdiInterface);
    MwLog(a, L"WddmProbe", MwWddmProbe);
    MwLog(a, L"WddmProbeResult", MwWddmProbeResult);
    MwLog(a, L"Wddm2Shape", MwWddmProbe != 0 ? MwWddm2Shape : 0xFFFFFFFF);
    MwLog(a, L"Nodes", MwNodeCount());
    /* The LUID the "GPU Engine" counters name this adapter by (luid_0x...). */
    MwLog(a, L"AdapterLuidLow", StartInfo->AdapterLuid.LowPart);
    MwLog(a, L"AdapterLuidHigh", (ULONG)StartInfo->AdapterLuid.HighPart);
    MwCommandSettings(a);
    MwLog(a, L"TimeSlice", a->Slice);
#if !MW_RENDER_ONLY
    st = MwDisplayStart(a);
    if (!NT_SUCCESS(st))
        return MwRet(a, L"Fail_StartDisplay", st);
#endif

    /* The GPU is optional to starting: without it the adapter still
     * displays, and submissions complete without running. */
    st = MapGpu(a);
    if (NT_SUCCESS(st))
        st = MkMmuInit(&a->Gpu);
    if (NT_SUCCESS(st))
        st = MkGpuInit(&a->Gpu);
    a->GpuUp = NT_SUCCESS(st);
    MwLog(a, L"GpuUp", NT_SUCCESS(st) ? 1 : (ULONG)st);
#if !MW_RENDER_ONLY
    /* Before the children are asked for: the backlight decides whether the
     * one child is an internal panel or HDMI (display.c). */
    MwBacklightStart(a);
    /* HDMI hot-plug (hpd.c): after the backlight, which says whether the
     * screen is the panel. Nothing at HdmiHpd 0. */
    MwHpdStart(a);
#endif
    PublishIdentity(a);
    GpuRailInit(a);
    {
        ULONG clk = MwSetting(a, L"GpuClk", 0);
        if (a->GpuUp && clk != 0)
            (void)GpuClkWrite(a, clk >> 8, clk & 0xFF);
    }

    /* Render-only: no sources and no children, and the firmware framebuffer
     * stays with Basic Display, which keeps the desktop up. */
    *Sources = MW_RENDER_ONLY ? 0 : 1;
    *Children = MW_RENDER_ONLY ? 0 : a->Hdmi.Children;
    MwPcwAttach(a);
    MwLog(a, L"Started", 1);
    return STATUS_SUCCESS;
}

static void StopGpu(MW_ADAPTER *A)
{
    MkGpuStop(&A->Gpu);
    MkMmuFree(&A->Gpu);
    if (A->Gpu.Regs != NULL) {
        MmUnmapIoSpace((PVOID)A->Gpu.Regs, A->Gpu.RegsLength);
        A->Gpu.Regs = NULL;
    }
    A->GpuUp = FALSE;
    if (A->ThermOn) {
        A->ThermOn = FALSE;
        KeMemoryBarrier();
        KeCancelTimer(&A->ThermTimer);
        KeFlushQueuedDpcs();
    }
    /* The trace's work item, if one is queued, ends before the adapter's
     * state goes (bounded: it only writes the registry). */
    for (ULONG i = 0; i < 200 && A->ClkLogQueued != 0; i++) {
        LARGE_INTEGER d;
        d.QuadPart = -10LL * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }
    if (A->ClkLogQueued == 0 && A->ClkLogItem != NULL) {
        IoUninitializeWorkItem(A->ClkLogItem);
        ExFreePoolWithTag(A->ClkLogItem, MW_TAG);
        A->ClkLogItem = NULL;
    }
    if (A->TsadcVa != NULL) {
        MmUnmapIoSpace(A->TsadcVa, PAGE_SIZE);
        A->TsadcVa = NULL;
    }
    A->GpuClkWanted = 0;
    /* A clock above 600 MHz is allowed by this start's checks only: the
     * next start (another package, the setting removed) may not allow it.
     * The GPU is stopped: nothing runs, the write is made now. */
    if (ClkHighNowOrSoon(A) && A->CruClkVa != NULL) {
        A->ClkPending = MW_CLK_PENDING | MwClkVal(0, 2);
        ClkApply(A);
    }
    if (A->CruClkVa != NULL) {
        MmUnmapIoSpace(A->CruClkVa, PAGE_SIZE);
        A->CruClkVa = NULL;
    }
}

/* Everything StartDevice (and AddDevice's copy thread) set going, stopped:
 * from StopDevice, and from StopDeviceAndReleasePostDisplayOwnership, which
 * dxgkrnl calls INSTEAD of StopDevice when it gives the screen to Basic
 * Display, including after its own part of an adapter start has failed
 * (dxgkrnl's DpiFdoStartAdapter). Until 2026-11-23 that path stopped only
 * the scan-out, and RemoveDevice then freed the adapter under the running
 * HPD and copy threads, which wait on events inside it. The two 2.0 boots
 * that dxgkrnl refused that day bugchecked about a second later with 0x139
 * (LIST_ENTRY corruption) on what looks like a DPC stack, with no DDI of
 * ours in between; this is the likely cause (WDDM2.md 16.6). */
void MwAdapterStop(MW_ADAPTER *A)
{
    if (A->Stopped)
        return;
    A->Stopped = TRUE;
    /* Before StopGpu unmaps the TSADC and the CRU page (pcw.c). */
    MwPcwDetach(A);
    MwCommandStop(A);
    MwLog(A, L"Renders", A->Renders);
    MwLog(A, L"Submits", A->Submits);
    MwLog(A, L"JobsDone", A->JobsDone);
    MwLog(A, L"JobsFailed", A->JobsFailed);
    MwLog(A, L"Vsyncs", A->Vsyncs);
    MwLog(A, L"Flips", A->Flips);
    MwLog(A, L"FlipCopies", A->FlipCopies);
    MwLog(A, L"FlipCopySkips", A->FlipCopySkips);
    StopGpu(A);
    MwHpdStop(A);
    MwBacklightStop(A);
    MwDisplayStop(A);
}

static NTSTATUS APIENTRY MwStopDevice(IN_CONST_PVOID Context)
{
    MwAdapterStop((MW_ADAPTER *)Context);
    return STATUS_SUCCESS;
}

/* RemoveDevice may come without either stop: after AddDevice alone (the
 * copy thread already runs), or after a StartDevice that failed. Nothing
 * of ours may still run, wait or be queued in the adapter when it is
 * freed: the copy and HPD threads, the poll, vsync and backlight timers
 * and their DPCs. Each of these stops is safe to repeat and safe on an
 * adapter that never started; the screen and backlight are left as they
 * are. */
static NTSTATUS APIENTRY MwRemoveDevice(IN_CONST_PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;

    MwPcwDetach(a);
    MwBc("RemoveDevice: %hs", a->Stopped ? "already stopped" : "not stopped, stopping the threads and timers first");
    if (!a->Stopped) {
        MwCommandStop(a);
        MwHpdStop(a);
        MwBacklightStop(a);
    }
    KeFlushQueuedDpcs();
    MwProbeStop(a);
    ExFreePoolWithTag(a, MW_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwDispatchIoRequest(IN_CONST_PVOID Context, IN_ULONG Source,
                                             IN_PVIDEO_REQUEST_PACKET Vrp)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Source);
    UNREFERENCED_PARAMETER(Vrp);
    return STATUS_NOT_SUPPORTED;
}

/* The child's power is the panel's: its backlight goes off with it
 * (backlight.c; nothing happens on HDMI). The adapter itself leaving D0
 * for a shutdown, a restart or sleep: scan-out off and the IOMMU no longer
 * translating, so the firmware, the bugcheck screen or the next driver
 * finds the VOP2 reading the framebuffer at its physical address
 * (scanout.c). */
static NTSTATUS APIENTRY MwSetPowerState(IN_CONST_PVOID Context, IN_ULONG Uid,
                                         IN_DEVICE_POWER_STATE State, IN_POWER_ACTION Action)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(Action);
    if (Uid != DISPLAY_ADAPTER_HW_ID)
        MwBacklightPower(a, State == PowerDeviceD0);
    else if (a != NULL) {
        MwScanPower(a, State == PowerDeviceD0);
        MwLog(a, L"AdapterPowerState", (ULONG)State);
    }
    return STATUS_SUCCESS;
}

static VOID APIENTRY MwResetDevice(IN_CONST_PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static VOID APIENTRY MwUnload(VOID)
{
    MwPcwExit();
    MwBcClose();
}

/* The brightness interface, for an internal panel only (backlight.c). */
static NTSTATUS APIENTRY MwQueryInterface(IN_CONST_PVOID Context, IN_PQUERY_INTERFACE Qi)
{
    NTSTATUS st = MwBacklightQueryInterface((MW_ADAPTER *)Context, Qi);
    /* Which interface was asked for and refused (P1: a 2.0 boot logged
     * Fail_QueryInterface 0xc00000bb, of an unknown interface). */
    if (!NT_SUCCESS(st) && Qi != NULL && Qi->InterfaceType != NULL) {
        MwLog((MW_ADAPTER *)Context, L"QI_RefusedData1", Qi->InterfaceType->Data1);
        MwLog((MW_ADAPTER *)Context, L"QI_RefusedVersion", Qi->Version);
    }
    return st;
}

/* ---- interrupts ---- */

/* Counted for the probe's watchdog (probe.c), at any IRQL. */
static MW_DDI_SLOT S_Interrupt = { L"Interrupt" };
static MW_DDI_SLOT S_Dpc = { L"Dpc" };

static BOOLEAN APIENTRY MwInterruptRoutine(IN_CONST_PVOID Context, IN_ULONG MessageNumber)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    ULONG64 t0 = MwTick();
    BOOLEAN mine = FALSE;
    UNREFERENCED_PARAMETER(MessageNumber);
    MwDdiEnter(&S_Interrupt);
    if (a->GpuUp && MkIsr(&a->Gpu)) {
        a->LastIsrTick = t0;
        (void)MwJobInterrupt(a);
        mine = TRUE;
    }
    MwRing(MW_RING_ISR, mine, 0);
    MwProfEnd(&a->Prof[MW_DDI_ISR], t0);
    (void)MwDdiExit(&S_Interrupt, STATUS_SUCCESS);
    return mine;
}

static VOID APIENTRY MwDpcRoutine(IN_CONST_PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    ULONG64 t0 = MwTick();
    LONG64 noticed = InterlockedExchange64(&a->NotifyTick, 0);

    MwDdiEnter(&S_Dpc);
    MwRing(MW_RING_DPC, 0, 0);
    /* How long a completion waited for this DPC to tell dxgkrnl. One DPC
     * of a device runs at a time, so the sums need no interlocks. */
    if (noticed != 0 && t0 >= (ULONG64)noticed) {
        ULONG64 d = t0 - (ULONG64)noticed;
        a->DpcCount++;
        a->DpcTicks += d;
        if (d > a->DpcMaxTicks)
            a->DpcMaxTicks = d;
    }
    a->Dxgk.DxgkCbNotifyDpc(a->Dxgk.DeviceHandle);
    MwProfEnd(&a->Prof[MW_DDI_DPC], t0);
    (void)MwDdiExit(&S_Dpc, STATUS_SUCCESS);
}

/* ---- adapter information ---- */

/* A field of a structure dxgkrnl sized for our interface version: write it
 * only if it lies inside the buffer (M5.1: the WDK's sizeof is larger). */
#define FITS(Size, Type, Field) (FIELD_OFFSET(Type, Field) + RTL_FIELD_SIZE(Type, Field) <= (Size))

static NTSTATUS DriverCaps(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)Info->pOutputData;
    UINT size = Info->OutputDataSize;

    if (!FITS(size, DXGK_DRIVERCAPS, WDDMVersion))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(caps, size);
    caps->WDDMVersion = (DXGK_WDDMVERSION)MwWddmVersion; /* 1.3 unless WddmProbe says otherwise (version.c) */
    caps->HighestAcceptableAddress.QuadPart = (1LL << 40) - 1;   /* the Mali sees 40 bits */
    caps->MaxAllocationListSlotId = 256;   /* Render patches every allocation, SlotId = index */
    caps->SchedulingCaps.MultiEngineAware = 1;
    /* Preemption is off system-wide (the INF sets EnablePreemption 0), as
     * viogpu3d does; DMA-buffer-boundary preemption comes later. */
    caps->SchedulingCaps.PreemptionAware = 1;
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
    /* The four caps WDDM 1.2's table marks mandatory for a render-only
     * driver. Without them, every render-only start failed with
     * STATUS_GRAPHICS_INVALID_DRIVER_MODEL right after dxgkrnl read these
     * caps (WinPE runs 5-9, ETW events 24/110/250 then the failure); a full
     * driver without them was accepted. */
    /* This one is for the full driver and must sit outside the render-only
     * block below. It was added inside that block (35d6adb) and so was
     * never compiled into the full build: the 2026-10-17 result "the
     * section-backed primary alone did not do it" (2290ca5) was measured
     * with this cap still 0. */
    /* The shared primary lives in our only segment, an aperture, and the
     * UMD does not set UseAlternateVA; dxgkrnl then insists that "all
     * segments in the supported write segment set for a shared primary be
     * CPU visible" (an aperture segment never is) and the primary is never
     * created: installed Windows, 2026-10-17, black screen, no
     * SetVidPnSourceAddress. DXGK_SEGMENTFLAGS' remarks give the way out:
     * a section-backed primary. */
    caps->MemoryManagementCaps.SectionBackedPrimary = 1;
    /* Flips are memory-mapped and take effect at the vertical sync: dxgkrnl
     * calls SetVidPnSourceAddress for each one, and the emulated vsync shows
     * the new primary and reports it (display.c, command.c). This too sat
     * inside the render-only block below (c389f13) and so was 0 in the only
     * build there is. With it 0, on the TaishanPi 3M, dwm's presents were
     * all accepted (pfnPresentCb S_OK, three primaries in rotation) and
     * neither Present nor SetVidPnSourceAddress was called for them. */
    caps->FlipCaps.FlipOnVSyncMmIo = 1;
    /* With TimeSlice set, a running chain can be stopped part way and
     * continued (command.c): a fragment job between tiles, a vertex or
     * tiler chain only at its end. Without it the cap stays as it was. */
    MwCommandSettings(A);
    if (A->Slice != 0 && FITS(size, DXGK_DRIVERCAPS, PreemptionCaps)) {
        caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_PRIMITIVE_BOUNDARY;
        caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DISPATCH_BOUNDARY;
    }
#if MW_RENDER_ONLY
    caps->PresentationCaps.SupportKernelModeCommandBuffer = 1; /* see MwRenderKm */
    if (FITS(size, DXGK_DRIVERCAPS, PreemptionCaps)) {
        /* Honest: a DMA buffer always runs to its end. */
        caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
    }
    if (FITS(size, DXGK_DRIVERCAPS, SupportPerEngineTDR))
        caps->SupportPerEngineTDR = TRUE;   /* ResetEngine, QueryEngineStatus, QueryDependentEngineGroup */
#endif
    if (FITS(size, DXGK_DRIVERCAPS, SupportNonVGA))
        caps->SupportNonVGA = TRUE;
    if (FITS(size, DXGK_DRIVERCAPS, SupportSmoothRotation))
        caps->SupportSmoothRotation = TRUE;
    /* WddmProbe only: the copy node, and GpuMmu in shape 1 (wddm2x.c). */
    Mw2xDriverCaps(A, caps, size);
    return STATUS_SUCCESS;
}

static void GpuInfo(MW_ADAPTER *A, MW_ESCAPE_GPU_INFO_DATA *D)
{
    const GPU_FEATURES *f = &A->Gpu.F;
    mw_u64 v[MW_GPU_PARAMS] = {0};

    /* Indices are DRM_PANFROST_PARAM_*, as malikm's GET_PARAM. */
    v[0] = f->Id; v[1] = f->Revision; v[2] = f->ShaderPresent; v[3] = f->TilerPresent;
    v[4] = f->L2Present; v[5] = f->StackPresent; v[6] = f->AsPresent; v[7] = f->JsPresent;
    v[8] = f->L2Features; v[9] = f->CoreFeatures; v[10] = f->TilerFeatures; v[11] = f->MemFeatures;
    v[12] = f->MmuFeatures; v[13] = f->ThreadFeatures; v[14] = f->MaxThreads;
    v[15] = f->MaxWorkgroupSize; v[16] = f->MaxBarrierSize; v[17] = f->CoherencyFeatures;
    for (ULONG i = 0; i < 4; i++)
        v[18 + i] = f->TextureFeatures[i];
    for (ULONG i = 0; i < 16; i++)
        v[22 + i] = f->JsFeatures[i];
    v[38] = f->NrCoreGroups; v[39] = f->ThreadTlsAlloc; v[40] = f->AfbcFeatures;
    v[44] = 31;                                  /* SELECTED_COHERENCY: none */
    RtlCopyMemory(D->Value, v, sizeof(v));
    D->Count = MW_GPU_PARAMS;
    D->Valid = (1ULL << 41) - 1;                 /* 0..40 */
    D->Valid |= 1ULL << 44;
    if (!A->GpuUp)
        D->Valid = 0;
}

/* KMTQAITYPE_UMDRIVERPRIVATE: what the UMD may offer (MW_UMD_ADAPTER_INFO,
 * docs/D3D11-UMD.md). Read from the settings at each query, PASSIVE.
 * The UMD's input, if any, is not looked at. */
static NTSTATUS UmdAdapterInfo(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    MW_UMD_ADAPTER_INFO u;
    UINT n = Info->OutputDataSize;

    if (Info->pOutputData == NULL || n < 2 * sizeof(mw_u32))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(&u, sizeof(u));
    u.Magic = MW_UMD_INFO_MAGIC;
    u.Ddi = MwSetting(A, L"UmdDdi", MW_UMD_DDI_D3D10_0);
    u.FeatureLevel = MwSetting(A, L"UmdFeatureLevel", 0xa000);
    u.Dxgi12 = MwSetting(A, L"UmdDxgi12", 0);
    u.Video = MwSetting(A, L"UmdVideo", 0);
    u.DecodeNode = MW_UMD_NO_NODE;      /* no decode engine yet (U5) */
    u.VirtualMode = 0;                  /* WDDM 1.3, physical (WDDM2.md P2 sets it) */
    u.KmdBuild = MW_BUILD;
    u.Msaa = MwSetting(A, L"UmdMsaa", 1);
    if (n > sizeof(u))
        n = sizeof(u);
    u.Size = n;
    RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
    RtlCopyMemory(Info->pOutputData, &u, n);
    return STATUS_SUCCESS;
}

/* ---- DxgkDdiQueryAdapterInfo 24 to 27: temperature and clock ----
 * What dxgkrnl and Task Manager do with these is in perfdata_calc.h and
 * docs/PERFDATA.md. In short: Task Manager's GPU page asks dxgkrnl for
 * KMTQAITYPE_ADAPTERPERFDATA, dxgkrnl asks this driver for type 25 at
 * most every 450 ms (at WDDM 2.0 too: run G's trace has QAI_25, size 53,
 * answered STATUS_NOT_SUPPORTED), and the page shows "GPU Temperature"
 * when the answer is not 0. All at PASSIVE_LEVEL, from a user thread. */

C_ASSERT(sizeof(DXGK_NODE_PERFDATA) == MW_PD_NODE_SIZE);
C_ASSERT(sizeof(DXGK_ADAPTER_PERFDATA) == MW_PD_ADAPTER_SIZE);
C_ASSERT(sizeof(DXGK_ADAPTER_PERFDATACAPS) == MW_PD_CAPS_SIZE);
C_ASSERT(sizeof(DXGK_GPUVERSION) == MW_PD_GPUVERSION_SIZE);
C_ASSERT(DXGKQAITYPE_NODEPERFDATA == MW_QAI_NODEPERFDATA);
C_ASSERT(DXGKQAITYPE_ADAPTERPERFDATA == MW_QAI_ADAPTERPERFDATA);
C_ASSERT(DXGKQAITYPE_ADAPTERPERFDATA_CAPS == MW_QAI_ADAPTERPERFDATA_CAPS);
C_ASSERT(DXGKQAITYPE_GPUVERSION == MW_QAI_GPUVERSION);

/* The hotter of TSADC channels 3 and 5 (thermal_calc.h), through the
 * mapping GpuRailInit made once the CRU said the TSADC runs; no reading
 * without it (a read of a gated TSADC can be an SError). */
int MwGpuTempMilliC(MW_ADAPTER *A, long *MilliC)
{
    PUCHAR ts = A->TsadcVa;

    *MilliC = 0;
    if (ts == NULL)
        return 0;
    return MwThermalHotter(READ_REGISTER_ULONG((PULONG)(ts + MW_TSADC_DATA(MW_THERMAL_CH_A))),
                           READ_REGISTER_ULONG((PULONG)(ts + MW_TSADC_DATA(MW_THERMAL_CH_B))), MilliC);
}

/* The GPU clock as the CRU has it now, in Hz; 0 when not known. */
ULONG MwGpuClkHzNow(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR reg = A->CruClkVa;
    ULONG v;

    if (reg == NULL) {
        pa.QuadPart = MW_CRU_CLKSEL165 & ~0xFFFu;
        reg = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (reg == NULL)
            return 0;
    }
    v = READ_REGISTER_ULONG((PULONG)(reg + (MW_CRU_CLKSEL165 & 0xFFFu)));
    if (reg != A->CruClkVa)
        MmUnmapIoSpace(reg, PAGE_SIZE);
    return (ULONG)MwPerfClkHz(v, MwGpuPll);
}

/* Zero the whole buffer, copy at most Size bytes of the answer. */
static NTSTATUS PerfOut(const DXGKARG_QUERYADAPTERINFO *Info, const void *Src, ULONG Size, ULONG Need)
{
    if (Info->pOutputData == NULL || Info->OutputDataSize < Need)
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
    RtlCopyMemory(Info->pOutputData, Src, Info->OutputDataSize < Size ? Info->OutputDataSize : Size);
    return STATUS_SUCCESS;
}

static NTSTATUS MwQueryPerfData(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    ULONG in = 0;

    if (Info->pInputData != NULL && Info->InputDataSize >= sizeof(ULONG))
        in = *(const ULONG UNALIGNED *)Info->pInputData;
    switch ((ULONG)Info->Type) {
    case MW_QAI_NODEPERFDATA: {
        /* Both nodes run on the one GPU clock (the copy node is the
         * driver's own paging node, wddm2x.c). The supply is the rail the
         * firmware measured at boot (GpuRail) or GpuRailUv; this driver
         * never changes it. */
        DXGK_NODE_PERFDATA d;
        ULONG oc = A->ThermOn ? GpuAllowedHz(A) : MW_GPU_MAX_HZ;

        if (!MwPerfNodeOk(in, MwNodeCount()))
            return STATUS_INVALID_PARAMETER;
        RtlZeroMemory(&d, sizeof(d));
        d.Frequency = MwGpuClkHzNow(A);
        d.MaxFrequency = MwPerfBestHz(MW_GPU_MAX_HZ, MwGpuPll);
        d.MaxFrequencyOC = MwPerfBestHz(oc, MwGpuPll);
        d.Voltage = A->RailUv / 1000u;
        return PerfOut(Info, &d, sizeof(d), MW_PD_NODE_SIZE);
    }
    case MW_QAI_ADAPTERPERFDATA: {
        DXGK_ADAPTER_PERFDATA d;
        long mc;
        int valid;

        if (in != 0)                    /* one physical adapter */
            return STATUS_INVALID_PARAMETER;
        valid = MwGpuTempMilliC(A, &mc);
        RtlZeroMemory(&d, sizeof(d));
        d.Temperature = (ULONG)MwPerfDeciC(valid, mc);
        d.PowerStateOverride = (UCHAR)(A->GpuUp ? 1 : 0);
        return PerfOut(Info, &d, sizeof(d), MW_PD_ADAPTER_SIZE);
    }
    case MW_QAI_ADAPTERPERFDATA_CAPS: {
        /* No fan of the GPU's own, no PCIe, no memory counter. */
        DXGK_ADAPTER_PERFDATACAPS d;

        RtlZeroMemory(&d, sizeof(d));
        d.TemperatureMax = MW_PD_TEMP_MAX_DC;
        d.TemperatureWarning = MW_PD_TEMP_WARNING_DC;
        return PerfOut(Info, &d, sizeof(d), MW_PD_CAPS_SIZE);
    }
    case MW_QAI_GPUVERSION: {
        /* An SoC GPU has no video BIOS. */
        DXGK_GPUVERSION d;

        RtlZeroMemory(&d, sizeof(d));
        (void)RtlStringCbCopyW(d.BiosVersion, sizeof(d.BiosVersion), L"none (SoC GPU)");
        (void)RtlStringCbCopyW(d.GpuArchitecture, sizeof(d.GpuArchitecture), L"Mali-G52 MC3 (Bifrost)");
        return PerfOut(Info, &d, sizeof(d), MW_PD_GPUVERSION_SIZE);
    }
    }
    return STATUS_NOT_SUPPORTED;
}

/* Once per type, the first answer, in the registry trace: what dxgkrnl
 * asked with and what it got. */
static void PerfTraceFirst(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info, NTSTATUS St)
{
    ULONG t = (ULONG)Info->Type - MW_QAI_NODEPERFDATA;

    MwLog(A, t == 0 ? L"PerfNodeIn" : t == 1 ? L"PerfAdapterIn" : t == 2 ? L"PerfCapsIn" : L"PerfVersionIn",
          Info->InputDataSize >= sizeof(ULONG) && Info->pInputData != NULL ?
          *(const ULONG UNALIGNED *)Info->pInputData : 0xFFFFFFFFu);
    if (NT_SUCCESS(St) && t == 0)
        MwLog(A, L"PerfNodeMhz", (ULONG)(((const DXGK_NODE_PERFDATA *)Info->pOutputData)->Frequency / 1000000u));
    if (NT_SUCCESS(St) && t == 1)
        MwLog(A, L"PerfTempDeciC", ((const DXGK_ADAPTER_PERFDATA *)Info->pOutputData)->Temperature);
}

static NTSTATUS APIENTRY MwQueryAdapterInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *Info)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    NTSTATUS st;

    /* QAI 24 to 27 (temperature and clock), 2.x only: at 1.3 they are
     * STATUS_NOT_SUPPORTED as before. 24 and 25 come twice a second while
     * something watches, so only their first answer goes into the journal
     * and the registry trace. */
    if (MwPerfAnswers((ULONG)Info->Type, MwWddmProbe, a->PerfData)) {
        ULONG t = (ULONG)Info->Type - MW_QAI_NODEPERFDATA;
        BOOLEAN first = (InterlockedOr(&a->PerfSeen, 1L << t) & (1L << t)) == 0;

        if (first)
            MwBc("QAI %u asked: in %u bytes, out %u bytes", (ULONG)Info->Type, Info->InputDataSize, Info->OutputDataSize);
        st = MwQueryPerfData(a, Info);
        InterlockedIncrement((volatile LONG *)&a->PerfAnswers[t]);
        if (first || !MwPerfPolled((ULONG)Info->Type)) {
            PerfTraceFirst(a, Info, st);
            MwLog(a, t == 0 ? L"QAI_24" : t == 1 ? L"QAI_25" : t == 2 ? L"QAI_26" : L"QAI_27", (ULONG)st);
            Mw2xBcQai(a, Info, st);
        }
        return st;
    }

    /* The boot journal (probe.c): which QAI, flushed before it is answered. */
    MwBc("QAI %u asked: in %u bytes, out %u bytes", (ULONG)Info->Type, Info->InputDataSize, Info->OutputDataSize);
    switch (Info->Type) {
    case DXGKQAITYPE_DRIVERCAPS:
        st = DriverCaps(a, Info);
        break;
    case DXGKQAITYPE_QUERYSEGMENT3:
        st = MwQuerySegment(a, Info);
        break;
    case DXGKQAITYPE_QUERYSEGMENT4:         /* WDDM 2.x only (version.c) */
        st = MwQuerySegment4(a, Info);
        break;
    case DXGKQAITYPE_PHYSICALADAPTERCAPS:   /* WDDM 2.x only */
        st = MwQueryPhysicalAdapterCaps(a, Info);
        break;
    case DXGKQAITYPE_GPUMMUCAPS:            /* Wddm2Shape 1 only */
        st = MwQueryGpuMmuCaps(Info);
        break;
    case DXGKQAITYPE_PAGETABLELEVELDESC:    /* Wddm2Shape 1 only */
        st = MwQueryPageTableLevelDesc(Info);
        break;
    case DXGKQAITYPE_HARDWARERESERVEDRANGES:
    case DXGKQAITYPE_WDDMDEVICECAPS:        /* WDDM 3.x only, before StartDevice */
    case DXGKQAITYPE_PHYSICAL_MEMORY_CAPS:
    case DXGKQAITYPE_IOMMU_CAPS:
    case DXGKQAITYPE_HARDWARERESERVEDRANGES2:
        st = MwWddmProbe != 0 ? MwQuery3xCaps(Info) : STATUS_NOT_SUPPORTED;
        break;
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:
        /* WDDM 1.3 (Windows 8.1) asks this of every 1.3 driver. Answering
         * STATUS_NOT_SUPPORTED was logged by dxgkrnl as "Driver returned an
         * invalid NTSTATUS code" right before every render-only start
         * failed with STATUS_GRAPHICS_INVALID_DRIVER_MODEL (WinPE runs 5-8).
         * No history buffers are written; 64 bits is the honest width of
         * anything that would be. */
        /* One entry per node (wddm2plan.h): at WDDM 2.x there are two, and
         * build 310 answered only the first (0x139 at the next step). */
        {
            ULONG n = Mw2PerNodeEntries(Info->OutputDataSize, sizeof(DXGKARG_HISTORYBUFFERPRECISION));
            if (Info->pOutputData == NULL || n == 0) {
                st = STATUS_BUFFER_TOO_SMALL;
            } else {
                RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
                for (ULONG i = 0; i < n; i++)
                    ((DXGKARG_HISTORYBUFFERPRECISION *)Info->pOutputData)[i].PrecisionBits = 64;
                st = STATUS_SUCCESS;
            }
        }
        break;
    case 47: /* DXGKQAITYPE_64BITONLYCAPS: zero = not restricted to 64-bit */
        RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
        st = STATUS_SUCCESS;
        break;
    case DXGKQAITYPE_UMDRIVERPRIVATE:
        st = UmdAdapterInfo(a, Info);
        break;
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
        st = STATUS_SUCCESS;
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    WCHAR name[32];
    UNICODE_STRING us;
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u", (ULONG)Info->Type)))
        MwLog(a, name, (ULONG)st);
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u_Size", (ULONG)Info->Type)))
        MwLog(a, name, Info->OutputDataSize);
    /* And what was answered, in the journal (wddm2x.c). */
    Mw2xBcQai(a, Info, st);
    return st;
}

/* Ticks of the performance counter as microseconds, saturating. */
static ULONG TicksToUs(const MW_ADAPTER *A, ULONG64 Ticks)
{
    ULONG64 us = A->PerfHz != 0 ? Ticks * 1000000ULL / A->PerfHz : 0;
    return us > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)us;
}

/* A sum of ticks over a count, as microseconds each. */
static ULONG TicksEachUs(const MW_ADAPTER *A, ULONG64 Ticks, ULONG Count)
{
    return Count != 0 ? TicksToUs(A, Ticks / Count) : 0;
}

/* MW_ESCAPE_UMD: a UMD's counts since its last report go into the sums
 * and into its process's entry; the answer is the switches, read from the
 * settings each time (about once a second per process), so a change in
 * the registry reaches running processes without a restart. PASSIVE. */
static ULONG UmdFlags(MW_ADAPTER *A)
{
    ULONG f = MW_UMD_FLAG_VALID;
    if (MwSetting(A, L"UmdCopyOnGpu", 1) != 0)
        f |= MW_UMD_FLAG_COPY_GPU;
    if (MwSetting(A, L"UmdReadbackOneCopy", 1) != 0)
        f |= MW_UMD_FLAG_READBACK_1;
    /* On unless 0: three runs each way on the TaishanPi 3M on 2026-11-23
     * cut DWM's submit CPU per job by about a third, no errors. */
    if (MwSetting(A, L"UmdMergeSubmit", 1) != 0)
        f |= MW_UMD_FLAG_MERGE_SUBMIT;
    /* On unless 0: on the TaishanPi 3M on 2026-11-24 copytest passed both
     * ways, three automated drags each way cut DWM's GPU copy time from
     * about 1075 ms to 60 to 123 ms per 15 s and its submits by half, and
     * no stale or ghost rectangles were seen. */
    if (MwSetting(A, L"UmdCopyBatch", 1) != 0)
        f |= MW_UMD_FLAG_COPY_BATCH;
    return f;
}

static NTSTATUS UmdReport(MW_ADAPTER *A, MW_ESCAPE_UMD_DATA *D)
{
    ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    MW_UMD_PROC *e = NULL;
    MW_UMD_COUNT in[MW_UMD_KINDS];

    /* The caller's buffer, read once. */
    RtlCopyMemory(in, D->Delta, sizeof(in));
    ExAcquireFastMutex(&A->UmdLock);
    A->UmdReports++;
    for (ULONG i = 0; i < MW_UMD_PROCS && e == NULL; i++)
        if (A->UmdProc[i].Pid == pid)
            e = &A->UmdProc[i];
    if (e == NULL) {
        /* A new process takes the oldest entry. */
        e = &A->UmdProc[A->UmdProcNext];
        A->UmdProcNext = (A->UmdProcNext + 1) % MW_UMD_PROCS;
        RtlZeroMemory(e, sizeof(*e));
        e->Pid = pid;
    }
    e->Reports++;
    for (ULONG k = 0; k < MW_UMD_KINDS; k++) {
        A->Umd[k].Calls += in[k].Calls;
        A->Umd[k].Bytes += in[k].Bytes;
        A->Umd[k].Ticks += in[k].Ticks;
        e->Count[k].Calls += in[k].Calls;
        e->Count[k].Bytes += in[k].Bytes;
        e->Count[k].Ticks += in[k].Ticks;
    }
    ExReleaseFastMutex(&A->UmdLock);
    D->Flags = UmdFlags(A);
    return STATUS_SUCCESS;
}

/* MW_ESCAPE_STATS_DATA.ScanState. */
static ULONG ScanState(const MW_ADAPTER *A)
{
    return (A->ScanOn ? 1u : 0) | (A->ScanPaging ? 2u : 0) | (A->ScanShowing ? 4u : 0) |
           (A->ScanReady ? 8u : 0) | (A->ScanSetting ? 16u : 0);
}

static void Stats(MW_ADAPTER *A, MW_ESCAPE_STATS_DATA *D, UINT Size)
{
    D->GpuUp = A->GpuUp;
    D->Renders = A->Renders;
    D->Presents = A->Presents;
    D->Submits = A->Submits;
    D->LastSubmittedFence = A->LastSubmittedFence;
    D->LastCompletedFence = A->LastCompletedFence;
    D->Queued = A->QCount;
    D->Running = A->Running;
    D->JobsDone = A->JobsDone;
    D->JobsFailed = A->JobsFailed;
    D->JobsTimedOut = A->JobsTimedOut;
    D->LastJsStatus = A->LastJsStatus;
    D->LastFaultStatus = A->LastFaultStatus;
    D->Irqs = A->Gpu.Irqs;
    D->Resets = A->Gpu.Resets;
    D->LastFaultAddress = A->LastFaultAddress;
    for (ULONG i = 0; i < 16; i++)
        D->PagingOps[i] = A->PagingOps[i];
    D->Maps = A->Maps;
    D->MapFails = A->MapFails;
    D->Unmaps = A->Unmaps;
    D->LastMapPages = A->LastMapPages;
    D->LastMapStatus = (mw_u32)A->LastMapStatus;
    D->Allocations = A->Allocations;
    D->LastMapVa = A->LastMapVa;
    D->LastAllocVa = A->LastAllocVa;
    D->Patches = A->Patches;
    D->PatchAllocations = A->PatchAllocations;
    D->PatchPatchLocations = A->PatchPatchLocations;
    D->PatchSegment0 = A->PatchSegment0;
    D->PatchAddress0 = A->PatchAddress0;
    D->PresentBlts = A->PresentBlts;
    D->PresentFills = A->PresentFills;
    D->PresentSkips = A->PresentSkips;
    /* Callers built before these existed pass the shorter structure. */
    if (Size >= RTL_SIZEOF_THROUGH_FIELD(MW_ESCAPE_STATS_DATA, FlipCopySkips)) {
        D->Vsyncs = A->Vsyncs;
        D->Flips = A->Flips;
        D->FlipCopies = A->FlipCopies;
        D->FlipCopySkips = A->FlipCopySkips;
    }
    if (Size >= RTL_SIZEOF_THROUGH_FIELD(MW_ESCAPE_STATS_DATA, Reserved1) && A->PerfHz != 0) {
        D->PerfHz = A->PerfHz;
        D->FlipCopyTicks = A->FlipCopyTicks;
        D->FlipWaitTicks = A->FlipWaitTicks;
        D->JobTicks = A->JobTicks;
        D->PresentBltTicks = A->PresentBltTicks;
        D->VsyncTicks = A->VsyncTicks;
        D->FlipCopyMaxUs = TicksToUs(A, A->FlipCopyMaxTicks);
        D->FlipWaitMaxUs = TicksToUs(A, A->FlipWaitMaxTicks);
        D->JobMaxUs = TicksToUs(A, A->JobMaxTicks);
        D->VsyncGapMaxUs = TicksToUs(A, A->VsyncGapMaxTicks);
        D->FlipsWhileBusy = A->FlipsWhileBusy;
    }
    if (Size >= RTL_SIZEOF_THROUGH_FIELD(MW_ESCAPE_STATS_DATA, WaitOtherMaxUs)) {
        D->TimeSliceMs = A->Slice;
        D->PreemptRequests = A->PreemptRequests;
        D->PreemptIdle = A->PreemptIdle;
        D->PreemptOverlap = A->PreemptOverlap;
        D->PreemptDropped = A->PreemptDropped;
        D->SoftStops = A->SoftStops;
        D->StopsTaken = A->StopsTaken;
        D->StopsRacedDone = A->StopsRacedDone;
        D->StopsAtBoundary = A->StopsAtBoundary;
        D->StopSlow = A->StopSlow;
        D->Resumes = A->Resumes;
        D->ResumeMismatch = A->ResumeMismatch;
        D->ResumeLost = A->ResumeLost;
        D->StopBadTail = A->StopBadTail;
        D->StopUnasked = A->StopUnasked;
        D->PreemptMaxUs = TicksToUs(A, A->PreemptMaxTicks);
        D->PreemptTicks = A->PreemptTicks;
        D->WaitTicks = A->WaitTicks;
        D->WaitCount = A->WaitCount;
        D->WaitMaxUs = TicksToUs(A, A->WaitMaxTicks);
        D->WaitOtherCount = A->WaitOtherCount;
        D->WaitOtherMaxUs = TicksToUs(A, A->WaitOtherMaxTicks);
    }
    if (Size >= sizeof(MW_ESCAPE_STATS_DATA)) {
        D->ScanState = ScanState(A);
        D->FlipsScanned = A->FlipsScanned;
        D->FlipsRotCopied = A->RotFlipCopies;
        D->WhyScanOff = A->WhyScanOff;
        D->WhyNotMapped = A->WhyNotMapped;
        D->WhyRotated = A->WhyRotated;
        D->WhyInvisible = A->WhyInvisible;
        D->ScanAutoOff = A->ScanAutoOff;
        D->ScanFaults = A->ScanFaults;
        D->ScanFaultAddr = A->ScanFaultAddr;
        D->FlipIntervals = A->FlipIntervals;
        D->FlipIntervalMaxUs = TicksToUs(A, A->FlipIntervalMaxTicks);
        D->FlipShows = A->FlipShows;
        D->FlipShowMaxUs = TicksToUs(A, A->FlipShowMaxTicks);
        D->FlipCallMaxUs = TicksToUs(A, A->FlipCallMaxTicks);
        D->LastPath = A->LastPath;
        D->FlipIntervalTicks = A->FlipIntervalTicks;
        D->FlipShowTicks = A->FlipShowTicks;
        D->FlipCallTicks = A->FlipCallTicks;
        D->CopyBytesAvoided = A->CopyBytesAvoided;
        D->PresentBltBytes = A->PresentBltBytes;
        ExAcquireFastMutex(&A->UmdLock);
        D->UmdReports = A->UmdReports;
        RtlCopyMemory(D->Umd, A->Umd, sizeof(D->Umd));
        RtlCopyMemory(D->UmdProc, A->UmdProc, sizeof(D->UmdProc));
        ExReleaseFastMutex(&A->UmdLock);
        D->UmdFlags = UmdFlags(A);
    }
}

/* ---- MW_ESCAPE_PERF ---- */

/* The GPU clock as the CRU has it now (MW_ESCAPE_PERF_DATA.ClkSel165). A
 * read of a CRU register this driver already writes (GpuClkWrite). */
static void GpuClkRead(mw_u32 *Raw, mw_u32 *Hz)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR reg;
    ULONG v;

    *Raw = *Hz = 0xFFFFFFFFu;
    pa.QuadPart = MW_CRU_CLKSEL165 & ~0xFFFu;
    reg = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (reg == NULL)
        return;
    v = READ_REGISTER_ULONG((PULONG)(reg + (MW_CRU_CLKSEL165 & 0xFFFu)));
    MmUnmapIoSpace(reg, PAGE_SIZE);
    *Raw = v;
    *Hz = MwGpuPll[(v >> 5) & 7] / ((v & 0x1F) + 1);
}

/* The TSADC's data registers, TSADC_DATA0..5 (TRM 19.4.2; the block the
 * firmware's ACPI thermal zones read, Thermal.asl in edk2-rk3576), through
 * the mapping GpuRailInit made once the CRU said the TSADC runs, or with
 * the setting PerfTemp 1 as before (a firmware from before the
 * thermal-safety work leaves the TSADC unclocked, and a read of an
 * unclocked block on this SoC can be an SError). */
static void TempRead(MW_ADAPTER *A, MW_ESCAPE_PERF_DATA *D)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR reg;

    D->TempValid = 0;
    D->GpuClkThermalDrops = A->GpuClkThermalDrops;
    if (A->TsadcVa != NULL) {
        reg = A->TsadcVa;
    } else {
        if (MwSetting(A, L"PerfTemp", 0) != 1)
            return;
        pa.QuadPart = MW_TSADC_PA;
        reg = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (reg == NULL)
            return;
    }
    for (ULONG n = 0; n < 6; n++) {
        long t = 0;
        D->TsadcCode[n] = READ_REGISTER_ULONG((PULONG)(reg + MW_TSADC_DATA(n)));
        if (MwTsadcToMilliC(D->TsadcCode[n], &t)) {
            D->TempMilliC[n] = (mw_s32)t;
            D->TempValid |= 1u << n;
        }
    }
    if (reg != A->TsadcVa)
        MmUnmapIoSpace(reg, PAGE_SIZE);
}

static NTSTATUS EscapePerf(MW_ADAPTER *A, MW_ESCAPE_PERF_DATA *D)
{
    NTSTATUS st;

    RtlZeroMemory((PUCHAR)D + sizeof(D->Code), sizeof(*D) - sizeof(D->Code));
    D->Version = MW_PERF_VERSION;
    D->PerfHz = A->PerfHz;
    for (ULONG i = 0; i < MW_PERF_DDIS; i++) {
        D->Ddi[i].Calls = (mw_u64)A->Prof[i].Calls;
        D->Ddi[i].Ticks = (mw_u64)A->Prof[i].Ticks;
        D->Ddi[i].MaxTicks = (mw_u64)A->Prof[i].MaxTicks;
    }
    D->RenderAllocs = (mw_u64)A->RenderAllocs;
    D->RenderCmds = (mw_u64)A->RenderCmds;
    D->RenderAllocsMax = (mw_u32)A->RenderAllocsMax;
    D->RenderCmdsMax = (mw_u32)A->RenderCmdsMax;
    D->MapPages = (mw_u64)A->MapPages;
    D->DpcCount = A->DpcCount;
    D->DpcTicks = A->DpcTicks;
    D->DpcMaxTicks = A->DpcMaxTicks;
    st = MwPerfSnapshot(A, D);
    if (A->GpuUp)
        GpuClkRead(&D->ClkSel165, &D->GpuHz);
    else
        D->ClkSel165 = D->GpuHz = 0xFFFFFFFFu;
    TempRead(A, D);
    D->Now = MwTick();
    return st;
}

static NTSTATUS APIENTRY MwEscape(IN_CONST_HANDLE hAdapter, const DXGKARG_ESCAPE *Esc)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    mw_u32 code;

    if (Esc->PrivateDriverDataSize < sizeof(code)) {
        /* Run 6: an escape we do not know (DWM's, by the timing) failed
         * here and its device was destroyed next. Record what it was. */
        MwLog(a, L"EscapeSmallSize", Esc->PrivateDriverDataSize);
        MwLog(a, L"EscapeSmallFlags", Esc->Flags.Value);
        return STATUS_INVALID_PARAMETER;
    }
    code = *(const mw_u32 *)Esc->pPrivateDriverData;
    switch (code) {
    case MW_ESCAPE_ALLOC_INFO:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_ALLOC_INFO_DATA))
            return STATUS_INVALID_PARAMETER;
        return MwEscapeAllocInfo(a, Esc, (MW_ESCAPE_ALLOC_INFO_DATA *)Esc->pPrivateDriverData);
    case MW_ESCAPE_GPU_INFO:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_GPU_INFO_DATA))
            return STATUS_INVALID_PARAMETER;
        GpuInfo(a, (MW_ESCAPE_GPU_INFO_DATA *)Esc->pPrivateDriverData);
        return STATUS_SUCCESS;
    case MW_ESCAPE_WDDM: {
        MW_ESCAPE_WDDM_DATA *w = (MW_ESCAPE_WDDM_DATA *)Esc->pPrivateDriverData;
        if (Esc->PrivateDriverDataSize < sizeof(*w))
            return STATUS_INVALID_PARAMETER;
        w->WddmVersion = MwWddmVersion;
        w->Needs = MwWddmVersion >= DXGKDDI_WDDMv2 ? MW_WDDM_NEEDS_RESIDENCY : 0;
        w->Reserved = 0;
        return STATUS_SUCCESS;
    }
    case MW_ESCAPE_GPUCLK:
        /* The first six fields are the 2026-11-14 layout, which m5test of
         * that time still sends; the rail fields are filled only when the
         * caller's buffer has them. */
        if (Esc->PrivateDriverDataSize < MW_ESCAPE_GPUCLK_V1_SIZE)
            return STATUS_INVALID_PARAMETER;
        return GpuClk(a, (MW_ESCAPE_GPUCLK_DATA *)Esc->pPrivateDriverData, Esc->PrivateDriverDataSize);
    case MW_ESCAPE_MEMORY:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_MEMORY_DATA))
            return STATUS_INVALID_PARAMETER;
        MwEscapeMemory(a, (MW_ESCAPE_MEMORY_DATA *)Esc->pPrivateDriverData);
        return STATUS_SUCCESS;
    case MW_ESCAPE_SCANOUT:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_SCANOUT_DATA))
            return STATUS_INVALID_PARAMETER;
        return MwScanSet(a, (MW_ESCAPE_SCANOUT_DATA *)Esc->pPrivateDriverData);
    case MW_ESCAPE_PERF:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_PERF_DATA))
            return STATUS_INVALID_PARAMETER;
        return EscapePerf(a, (MW_ESCAPE_PERF_DATA *)Esc->pPrivateDriverData);
    case MW_ESCAPE_UMD:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_UMD_DATA))
            return STATUS_INVALID_PARAMETER;
        return UmdReport(a, (MW_ESCAPE_UMD_DATA *)Esc->pPrivateDriverData);
    case MW_ESCAPE_STATS:
        if (Esc->PrivateDriverDataSize < (UINT)FIELD_OFFSET(MW_ESCAPE_STATS_DATA, Vsyncs))
            return STATUS_INVALID_PARAMETER;
        Stats(a, (MW_ESCAPE_STATS_DATA *)Esc->pPrivateDriverData, Esc->PrivateDriverDataSize);
        /* The registry trace only for a full reading (m5test), not for the
         * UMD's idle poll, which asks for the fences alone every
         * millisecond. */
        if (Esc->PrivateDriverDataSize <= (UINT)FIELD_OFFSET(MW_ESCAPE_STATS_DATA, Vsyncs))
            return STATUS_SUCCESS;
        MwLog(a, L"T_CommitsPinned", a->CommitsPinned);
        MwLog(a, L"T_CommitsUnpinned", a->CommitsUnpinned);
        MwLog(a, L"T_CommitsNoPath", a->CommitsNoPath);
        MwLog(a, L"T_CommitsNotOurs", a->CommitsNotOurs);
        MwLog(a, L"T_VisibleOn", a->VisibleOn);
        MwLog(a, L"T_VisibleOff", a->VisibleOff);
        MwLog(a, L"T_SourceVisible", a->SourceVisible);
        MwLog(a, L"T_PathActive", a->PathActive);
        MwLog(a, L"T_Flips", a->Flips);
        MwLog(a, L"T_FlipsNullAlloc", a->FlipsNullAlloc);
        MwLog(a, L"T_FlipsInvisible", a->FlipsInvisible);
        MwLog(a, L"T_LastFlipFlags", a->LastFlipFlags);
        MwLog(a, L"T_LastFlipContexts", a->LastFlipContexts);
        MwLog(a, L"T_ScanAllocSet", a->ScanAlloc != NULL);
        MwLog(a, L"T_ShownAllocSet", a->ShownAlloc != NULL);
        MwLog(a, L"T_ScanAddrLow", a->ScanAddr.LowPart);
        MwLog(a, L"T_FlipCopies", a->FlipCopies);
        MwLog(a, L"T_FlipCopySkips", a->FlipCopySkips);
        MwLog(a, L"T_PresentFlips", a->PresentFlips);
        MwLog(a, L"T_Presents", a->Presents);
        /* Rotation (display.c): the path's degrees, and how many flips and
         * presents the driver itself turned on the CPU. At 90 or 270 with
         * T_RotFlipCopies 0, DWM did the turning. */
        MwLog(a, L"T_Rotation", a->RotDeg);
        MwLog(a, L"T_RotFlipCopies", a->RotFlipCopies);
        MwLog(a, L"T_RotPresents", a->RotPresents);
        /* The DSI panel's backlight (backlight.c): Windows' level, the
         * DUTY written last, and how many levels and ramps there were. */
        MwLog(a, L"T_BacklightLevel", a->BlVa != NULL ? a->BlLevel : 0);
        MwLog(a, L"T_BacklightDuty", a->BlDuty);
        MwLog(a, L"T_BacklightSets", a->BlSets);
        MwLog(a, L"T_BacklightRamps", a->BlRamps);
        MwLog(a, L"T_BacklightOff", a->BlOff);
        /* Averages since the driver started, in microseconds; m5test perf
         * gives them for an interval. */
        MwLog(a, L"T_FlipCopyUsAvg", TicksEachUs(a, a->FlipCopyTicks, a->FlipCopies));
        MwLog(a, L"T_FlipCopyUsMax", TicksToUs(a, a->FlipCopyMaxTicks));
        MwLog(a, L"T_FlipWaitUsAvg", TicksEachUs(a, a->FlipWaitTicks, a->FlipCopies));
        MwLog(a, L"T_FlipWaitUsMax", TicksToUs(a, a->FlipWaitMaxTicks));
        MwLog(a, L"T_JobUsAvg", TicksEachUs(a, a->JobTicks, a->JobsDone + a->JobsFailed));
        MwLog(a, L"T_JobUsMax", TicksToUs(a, a->JobMaxTicks));
        MwLog(a, L"T_VsyncGapUsAvg", TicksEachUs(a, a->VsyncTicks, a->Vsyncs));
        MwLog(a, L"T_VsyncGapUsMax", TicksToUs(a, a->VsyncGapMaxTicks));
        MwLog(a, L"T_PresentBltUsAvg",
              TicksEachUs(a, a->PresentBltTicks, a->PresentBlts + a->PresentFills));
        MwLog(a, L"T_FlipsWhileBusy", a->FlipsWhileBusy);
        MwLog(a, L"T_CopyThread", a->CopyThread != NULL);
        MwLog(a, L"T_CopyDeferred", a->CopyDeferred);
        MwLog(a, L"T_TimeSlice", a->Slice);
        MwLog(a, L"T_PreemptRequests", a->PreemptRequests);
        MwLog(a, L"T_PreemptIdle", a->PreemptIdle);
        MwLog(a, L"T_PreemptOverlap", a->PreemptOverlap);
        MwLog(a, L"T_PreemptDropped", a->PreemptDropped);
        MwLog(a, L"T_PreemptUsAvg", TicksEachUs(a, a->PreemptTicks, a->PreemptRequests));
        MwLog(a, L"T_PreemptUsMax", TicksToUs(a, a->PreemptMaxTicks));
        MwLog(a, L"T_SoftStops", a->SoftStops);
        MwLog(a, L"T_StopsTaken", a->StopsTaken);
        MwLog(a, L"T_StopsRacedDone", a->StopsRacedDone);
        MwLog(a, L"T_StopsAtBoundary", a->StopsAtBoundary);
        MwLog(a, L"T_StopSlow", a->StopSlow);
        MwLog(a, L"T_Resumes", a->Resumes);
        MwLog(a, L"T_ResumeMismatch", a->ResumeMismatch);
        MwLog(a, L"T_ResumeLost", a->ResumeLost);
        MwLog(a, L"T_StopBadTail", a->StopBadTail);
        MwLog(a, L"T_StopUnasked", a->StopUnasked);
        MwLog(a, L"T_WaitUsAvg", TicksEachUs(a, a->WaitTicks, a->WaitCount));
        MwLog(a, L"T_WaitUsMax", TicksToUs(a, a->WaitMaxTicks));
        MwLog(a, L"T_WaitOtherUsMax", TicksToUs(a, a->WaitOtherMaxTicks));
        /* How flips reach the screen (scanout.c, display.c). T_PresentPath
         * is the last flip shown: 1 scanned out by the VOP2, 2 copied, 3
         * copied and turned. T_FlipsScanned are the copies avoided; each
         * T_CopyWhy* counts flips that were left to the copy, by reason.
         * T_ScanState: 1 on, 2 translating, 4 a primary shown, 8 set up,
         * 16 the ScanOut setting on. */
        MwLog(a, L"T_PresentPath", a->LastPath);
        MwLog(a, L"T_ScanState", ScanState(a));
        MwLog(a, L"T_FlipsScanned", a->FlipsScanned);
        MwLog(a, L"T_CopiesAvoided", a->FlipsScanned);
        MwLog(a, L"T_CopyMBAvoided", (ULONG)(a->CopyBytesAvoided >> 20));
        MwLog(a, L"T_CopyWhyScanOff", a->WhyScanOff);
        MwLog(a, L"T_CopyWhyNotMapped", a->WhyNotMapped);
        MwLog(a, L"T_CopyWhyRotated", a->WhyRotated);
        MwLog(a, L"T_CopyWhyInvisible", a->WhyInvisible);
        MwLog(a, L"T_ScanFlips", a->ScanFlips);
        MwLog(a, L"T_ScanMaps", a->ScanMaps);
        MwLog(a, L"T_ScanMismatch", a->ScanMismatch);
        MwLog(a, L"T_ScanNoSlot", a->ScanNoSlot);
        MwLog(a, L"T_ScanToFbs", a->ScanToFbs);
        MwLog(a, L"T_ScanFaults", a->ScanFaults);
        MwLog(a, L"T_ScanFaultAddr", a->ScanFaultAddr);
        MwLog(a, L"T_ScanAutoOff", a->ScanAutoOff);
        MwLog(a, L"T_ScanLastRefusedW", a->ScanLastW);
        MwLog(a, L"T_ScanLastRefusedH", a->ScanLastH);
        MwLog(a, L"T_ScanLastRefusedPitch", a->ScanLastPitch);
        MwLog(a, L"T_FbWidth", a->Fb.Width);
        MwLog(a, L"T_FbHeight", a->Fb.Height);
        MwLog(a, L"T_FbPitch", a->Fb.Pitch);
        /* Smaller source modes (scanout.c MwScanScale): the size the window
         * reads now (0 when it is the firmware's window) and how many flips
         * of such a primary were scanned out. */
        MwLog(a, L"T_SclSrc", a->Scaled ? (a->ScrW << 16 | (a->ScrH & 0xFFFF)) : 0);
        MwLog(a, L"T_SclFlips", a->SclFlips);
        /* Per flip: the time between flips (1000000 / it is the flip rate),
         * flip to the vsync that reported it shown, and the driver's own
         * time in SetVidPnSourceAddress; T_FlipCopyMsTotal is all the CPU
         * time the copies took since the start. */
        MwLog(a, L"T_FlipIntervalUsAvg", TicksEachUs(a, a->FlipIntervalTicks, a->FlipIntervals));
        MwLog(a, L"T_FlipIntervalUsMax", TicksToUs(a, a->FlipIntervalMaxTicks));
        MwLog(a, L"T_FlipShowUsAvg", TicksEachUs(a, a->FlipShowTicks, a->FlipShows));
        MwLog(a, L"T_FlipShowUsMax", TicksToUs(a, a->FlipShowMaxTicks));
        MwLog(a, L"T_FlipCallUsAvg", TicksEachUs(a, a->FlipCallTicks, a->Flips));
        MwLog(a, L"T_FlipCallUsMax", TicksToUs(a, a->FlipCallMaxTicks));
        MwLog(a, L"T_FlipCopyMsTotal", TicksToUs(a, a->FlipCopyTicks) / 1000);
        MwLog(a, L"T_PresentBltMB", (ULONG)(a->PresentBltBytes >> 20));
        /* The UMDs' counts since the start, all processes: calls, MB and
         * microseconds a call, by kind (MW_UMD_* in maliwddm_abi.h). */
        {
            static const PCWSTR names[MW_UMD_KINDS] = {
                L"CopyGpu", L"CopyCpu", L"CopyCpuBuffer", L"Upload", L"MapRead", L"MapWrite",
                L"PresentReadback", L"PresentDirect", L"GpuWait", L"Blt", L"Resolve", L"Submit",
            };
            MW_UMD_COUNT c[MW_UMD_KINDS];
            ExAcquireFastMutex(&a->UmdLock);
            RtlCopyMemory(c, a->Umd, sizeof(c));
            ExReleaseFastMutex(&a->UmdLock);
            MwLog(a, L"T_UmdReports", a->UmdReports);
            MwLog(a, L"T_UmdFlags", UmdFlags(a));
            for (ULONG k = 0; k < MW_UMD_KINDS; k++) {
                WCHAR n[48];
                UNICODE_STRING us;
                RtlInitEmptyUnicodeString(&us, n, sizeof(n));
                if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"T_Umd%wsCalls", names[k])))
                    MwLog(a, n, (ULONG)c[k].Calls);
                RtlInitEmptyUnicodeString(&us, n, sizeof(n));
                if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"T_Umd%wsMB", names[k])))
                    MwLog(a, n, (ULONG)(c[k].Bytes >> 20));
                RtlInitEmptyUnicodeString(&us, n, sizeof(n));
                if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"T_Umd%wsUsAvg", names[k])))
                    MwLog(a, n, c[k].Calls ? TicksToUs(a, c[k].Ticks / c[k].Calls) : 0);
            }
        }
        return STATUS_SUCCESS;
    default:
        MwLog(a, L"EscapeUnknownCode", code);
        MwLog(a, L"EscapeUnknownSize", Esc->PrivateDriverDataSize);
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS APIENTRY MwSetPointerPosition(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwSetPointerShape(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_NOT_SUPPORTED;
}

/* ---- the traced DDI table ---- */

/* The wrappers find the adapter from the DDI's first parameter, whose type
 * differs: an adapter, a device (OpenAllocation, CreateContext, ...), a
 * context (Render, Present, ...) or the miniport context. Build 97 traced
 * OpenAllocation as if its hDevice were the adapter: an out-of-bounds pool
 * write and a device pointer passed to IoOpenDeviceRegistryKey as a PDO,
 * from a system call -- SYSTEM_SERVICE_EXCEPTION. */
/* Each wrapper also counts its calls in memory for the probe's watchdog
 * (probe.c): enters, exits and the last enter, at any IRQL. */
#define TRACE_WITH(Name, Proto, Args, AdapterExpr)                             \
    static MW_DDI_SLOT S_##Name = { L"" MW_W(#Name) };                         \
    static NTSTATUS APIENTRY T_##Name Proto                                    \
    {                                                                          \
        MW_ADAPTER *a_ = (AdapterExpr);                                        \
        NTSTATUS st_;                                                          \
        MwDdiEnter(&S_##Name);                                                 \
        MwEnter(a_, L"Enter_" MW_W(#Name));                                    \
        st_ = MwRet(a_, L"Fail_" MW_W(#Name), Mw##Name Args);                  \
        return MwDdiExit(&S_##Name, st_);                                      \
    }
/* The same, and the DDI's CPU time into Prof[Slot] (MW_ESCAPE_PERF). The
 * watchdog's slot as TRACE_WITH, so a probe journal still sees these DDIs. */
#define TRACE_PROF(Name, Proto, Args, AdapterExpr, Slot)                       \
    static MW_DDI_SLOT S_##Name = { L"" MW_W(#Name) };                         \
    static NTSTATUS APIENTRY T_##Name Proto                                    \
    {                                                                          \
        MW_ADAPTER *a_ = (AdapterExpr);                                        \
        ULONG64 t0_ = MwTick();                                                \
        NTSTATUS st_;                                                          \
        MwDdiEnter(&S_##Name);                                                 \
        MwEnter(a_, L"Enter_" MW_W(#Name));                                    \
        st_ = Mw##Name Args;                                                   \
        MwProfEnd(&a_->Prof[Slot], t0_);                                       \
        return MwDdiExit(&S_##Name, MwRet(a_, L"Fail_" MW_W(#Name), st_));     \
    }
#define TRACE_ADAPTER_P(Name, Proto, Args, Slot) TRACE_PROF(Name, Proto, Args, (MW_ADAPTER *)hAdapter, Slot)
#define TRACE_ADAPTER(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, (MW_ADAPTER *)hAdapter)
#define TRACE_DEVICE(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, ((MW_DEVICE *)hDevice)->Adapter)
#define TRACE_CTX(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, ((MW_CONTEXT *)hContext)->Device->Adapter)

TRACE_ADAPTER(QueryAdapterInfo, (IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *a), (hAdapter, a))
TRACE_ADAPTER_P(Escape, (IN_CONST_HANDLE hAdapter, const DXGKARG_ESCAPE *a), (hAdapter, a), MW_DDI_ESCAPE)
TRACE_ADAPTER_P(CreateAllocation, (IN_CONST_HANDLE hAdapter, DXGKARG_CREATEALLOCATION *a), (hAdapter, a), MW_DDI_CREATE_ALLOC)
TRACE_ADAPTER_P(DestroyAllocation, (IN_CONST_HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *a), (hAdapter, a), MW_DDI_DESTROY_ALLOC)
TRACE_ADAPTER(DescribeAllocation, (IN_CONST_HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *a), (hAdapter, a))
TRACE_ADAPTER(GetStandardAllocationDriverData, (IN_CONST_HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *a), (hAdapter, a))
TRACE_ADAPTER_P(BuildPagingBuffer, (IN_CONST_HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *a), (hAdapter, a), MW_DDI_PAGING)
TRACE_ADAPTER(CreateDevice, (IN_CONST_HANDLE hAdapter, DXGKARG_CREATEDEVICE *a), (hAdapter, a))
TRACE_ADAPTER(IsSupportedVidPn, (IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(RecommendFunctionalVidPn, (IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(EnumVidPnCofuncModality, (IN_CONST_HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *a), (hAdapter, a))
TRACE_ADAPTER(SetVidPnSourceVisibility, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *a), (hAdapter, a))
TRACE_ADAPTER(CommitVidPn, (IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(UpdateActiveVidPnPresentPath, (IN_CONST_HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *a), (hAdapter, a))
TRACE_ADAPTER(RecommendMonitorModes, (IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *a), (hAdapter, a))
TRACE_ADAPTER(QueryVidPnHWCapability, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYVIDPNHWCAPABILITY *a), (hAdapter, a))
TRACE_ADAPTER_P(SetVidPnSourceAddress, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *a), (hAdapter, a), MW_DDI_SET_SOURCE)
TRACE_ADAPTER(ResetFromTimeout, (IN_CONST_HANDLE hAdapter), (hAdapter))
TRACE_ADAPTER(RestartFromTimeout, (IN_CONST_HANDLE hAdapter), (hAdapter))
TRACE_ADAPTER(ResetEngine, (IN_CONST_HANDLE hAdapter, DXGKARG_RESETENGINE *a), (hAdapter, a))
TRACE_ADAPTER(QueryEngineStatus, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *a), (hAdapter, a))
TRACE_ADAPTER(ControlInterrupt, (IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE t, IN_BOOLEAN e), (hAdapter, t, e))
TRACE_ADAPTER(GetScanLine, (IN_CONST_HANDLE hAdapter, DXGKARG_GETSCANLINE *a), (hAdapter, a))

#define TRACE_CONTEXT(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, (MW_ADAPTER *)Context)

TRACE_CONTEXT(QueryChildRelations, (IN_CONST_PVOID Context, PDXGK_CHILD_DESCRIPTOR d, IN_ULONG n), (Context, d, n))
TRACE_CONTEXT(QueryChildStatus, (IN_CONST_PVOID Context, INOUT_PDXGK_CHILD_STATUS c, IN_BOOLEAN b), (Context, c, b))
TRACE_CONTEXT(QueryDeviceDescriptor, (IN_CONST_PVOID Context, IN_ULONG u, INOUT_PDXGK_DEVICE_DESCRIPTOR d), (Context, u, d))
TRACE_CONTEXT(SetPowerState, (IN_CONST_PVOID Context, IN_ULONG u, IN_DEVICE_POWER_STATE p, IN_POWER_ACTION a), (Context, u, p, a))
TRACE_CONTEXT(QueryInterface, (IN_CONST_PVOID Context, IN_PQUERY_INTERFACE q), (Context, q))
TRACE_CONTEXT(DispatchIoRequest, (IN_CONST_PVOID Context, IN_ULONG s, IN_PVIDEO_REQUEST_PACKET v), (Context, s, v))
TRACE_CONTEXT(StopDevice, (IN_CONST_PVOID Context), (Context))
TRACE_PROF(OpenAllocation, (IN_CONST_HANDLE hDevice, const DXGKARG_OPENALLOCATION *a), (hDevice, a),
           ((MW_DEVICE *)hDevice)->Adapter, MW_DDI_OPEN_ALLOC)
TRACE_DEVICE(CloseAllocation, (IN_CONST_HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *a), (hDevice, a))
TRACE_DEVICE(CreateContext, (IN_CONST_HANDLE hDevice, DXGKARG_CREATECONTEXT *a), (hDevice, a))
TRACE_DEVICE(DestroyDevice, (IN_CONST_HANDLE hDevice), (hDevice))
TRACE_CTX(DestroyContext, (IN_CONST_HANDLE hContext), (hContext))
TRACE_PROF(Render, (IN_CONST_HANDLE hContext, DXGKARG_RENDER *a), (hContext, a),
           ((MW_CONTEXT *)hContext)->Device->Adapter, MW_DDI_RENDER)
TRACE_PROF(Present, (IN_CONST_HANDLE hContext, DXGKARG_PRESENT *a), (hContext, a),
           ((MW_CONTEXT *)hContext)->Device->Adapter, MW_DDI_PRESENT)
TRACE_ADAPTER(SetPointerPosition, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *a), (hAdapter, a))
TRACE_ADAPTER(SetPointerShape, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *a), (hAdapter, a))
TRACE_ADAPTER_P(Patch, (IN_CONST_HANDLE hAdapter, const DXGKARG_PATCH *a), (hAdapter, a), MW_DDI_PATCH)
TRACE_ADAPTER_P(SubmitCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *a), (hAdapter, a), MW_DDI_SUBMIT)
TRACE_ADAPTER(QueryCurrentFence, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *a), (hAdapter, a))
TRACE_ADAPTER(CollectDbgInfo, (IN_CONST_HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *a), (hAdapter, a))
TRACE_ADAPTER(CancelCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *a), (hAdapter, a))
TRACE_ADAPTER(PreemptCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *a), (hAdapter, a))
TRACE_ADAPTER(GetNodeMetadata, (IN_CONST_HANDLE hAdapter, UINT n, DXGKARG_GETNODEMETADATA *a), (hAdapter, n, a))
TRACE_ADAPTER(QueryDependentEngineGroup, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYDEPENDENTENGINEGROUP *a), (hAdapter, a))
TRACE_ADAPTER(CreateProcess, (IN_CONST_HANDLE hAdapter, DXGKARG_CREATEPROCESS *a), (hAdapter, a))
TRACE_ADAPTER(DestroyProcess, (IN_CONST_HANDLE hAdapter, IN_CONST_HANDLE p), (hAdapter, p))
TRACE_ADAPTER(SubmitCommandVirtual, (IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMANDVIRTUAL *a), (hAdapter, a))
static MW_DDI_SLOT S_SetRootPageTable = { L"SetRootPageTable" };
static VOID APIENTRY T_SetRootPageTable(IN_CONST_HANDLE hAdapter, const DXGKARG_SETROOTPAGETABLE *a)
{
    MwDdiEnter(&S_SetRootPageTable);
    MwSetRootPageTable(hAdapter, a);
    (void)MwDdiExit(&S_SetRootPageTable, STATUS_SUCCESS);
}
TRACE_CONTEXT(StartDevice, (IN_CONST_PVOID Context, IN_PDXGK_START_INFO s, IN_PDXGKRNL_INTERFACE d, OUT_PULONG n, OUT_PULONG c),
              (Context, s, d, n, c))
/* dxgkrnl's way out of a failed start (MwAdapterStop): journaled like the
 * rest. RemoveDevice frees the adapter, so its wrapper uses only the slot. */
TRACE_CONTEXT(StopDeviceAndReleasePostDisplayOwnership,
              (IN_CONST_PVOID Context, IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID t, PDXGK_DISPLAY_INFORMATION d),
              (Context, t, d))
static MW_DDI_SLOT S_RemoveDevice = { L"RemoveDevice" };
static NTSTATUS APIENTRY T_RemoveDevice(IN_CONST_PVOID Context)
{
    MwDdiEnter(&S_RemoveDevice);
    return MwDdiExit(&S_RemoveDevice, MwRemoveDevice(Context));
}
/* WDDM 2.x (wddm2x.c). SetStablePowerState returns nothing. */
TRACE_ADAPTER(CalibrateGpuClock, (IN_CONST_HANDLE hAdapter, IN UINT32 n, IN UINT32 e, DXGKARG_CALIBRATEGPUCLOCK *a),
              (hAdapter, n, e, a))
static MW_DDI_SLOT S_SetStablePowerState = { L"SetStablePowerState" };
static VOID APIENTRY T_SetStablePowerState(IN_CONST_HANDLE hAdapter, const DXGKARG_SETSTABLEPOWERSTATE *a)
{
    MwDdiEnter(&S_SetStablePowerState);
    MwSetStablePowerState(hAdapter, a);
    (void)MwDdiExit(&S_SetStablePowerState, STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA init = {0};

    MwDriverObject = DriverObject;     /* the guard's work item (ClkLogWork) */

    /* WDDM 1.3 unless the WddmProbe setting asks for another (version.c). */
    MwVersionInit(RegistryPath);
    init.Version = MwDdiInterface;
    init.DxgkDdiAddDevice = MwAddDevice;
    init.DxgkDdiStartDevice = T_StartDevice;
    init.DxgkDdiStopDevice = T_StopDevice;
    init.DxgkDdiRemoveDevice = T_RemoveDevice;
    init.DxgkDdiDispatchIoRequest = T_DispatchIoRequest;
    init.DxgkDdiInterruptRoutine = MwInterruptRoutine;
    init.DxgkDdiDpcRoutine = MwDpcRoutine;
    init.DxgkDdiQueryChildRelations = T_QueryChildRelations;
    init.DxgkDdiQueryChildStatus = T_QueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = T_QueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = T_SetPowerState;
    init.DxgkDdiResetDevice = MwResetDevice;
    init.DxgkDdiUnload = MwUnload;
    init.DxgkDdiQueryInterface = T_QueryInterface;

    init.DxgkDdiQueryAdapterInfo = T_QueryAdapterInfo;
    init.DxgkDdiEscape = T_Escape;
    init.DxgkDdiCreateAllocation = T_CreateAllocation;
    init.DxgkDdiDestroyAllocation = T_DestroyAllocation;
    init.DxgkDdiDescribeAllocation = T_DescribeAllocation;
    init.DxgkDdiGetStandardAllocationDriverData = T_GetStandardAllocationDriverData;
    init.DxgkDdiOpenAllocation = T_OpenAllocation;
    init.DxgkDdiCloseAllocation = T_CloseAllocation;
    init.DxgkDdiBuildPagingBuffer = T_BuildPagingBuffer;

    init.DxgkDdiCreateDevice = T_CreateDevice;
    init.DxgkDdiDestroyDevice = T_DestroyDevice;
    init.DxgkDdiCreateContext = T_CreateContext;
    init.DxgkDdiDestroyContext = T_DestroyContext;
    init.DxgkDdiRender = T_Render;
    init.DxgkDdiRenderKm = MwRenderKm;
    init.DxgkDdiPresent = T_Present;
    init.DxgkDdiPatch = T_Patch;
    init.DxgkDdiSubmitCommand = T_SubmitCommand;
    init.DxgkDdiPreemptCommand = T_PreemptCommand;
    init.DxgkDdiQueryCurrentFence = T_QueryCurrentFence;
    init.DxgkDdiResetFromTimeout = T_ResetFromTimeout;
    init.DxgkDdiRestartFromTimeout = T_RestartFromTimeout;
    init.DxgkDdiCollectDbgInfo = T_CollectDbgInfo;
    init.DxgkDdiGetNodeMetadata = T_GetNodeMetadata;
    init.DxgkDdiResetEngine = T_ResetEngine;
    init.DxgkDdiQueryEngineStatus = T_QueryEngineStatus;
    init.DxgkDdiQueryDependentEngineGroup = T_QueryDependentEngineGroup;
    init.DxgkDdiCancelCommand = T_CancelCommand;
    init.DxgkDdiControlInterrupt = T_ControlInterrupt;
    init.DxgkDdiGetScanLine = T_GetScanLine;

    init.DxgkDdiSetPointerPosition = T_SetPointerPosition;
    init.DxgkDdiSetPointerShape = T_SetPointerShape;
    init.DxgkDdiIsSupportedVidPn = T_IsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = T_RecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = T_EnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = T_SetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = T_CommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = T_UpdateActiveVidPnPresentPath;
    init.DxgkDdiSetVidPnSourceAddress = T_SetVidPnSourceAddress;
    init.DxgkDdiRecommendMonitorModes = T_RecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = T_QueryVidPnHWCapability;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = T_StopDeviceAndReleasePostDisplayOwnership;
    init.DxgkDdiSystemDisplayEnable = MwSystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = MwSystemDisplayWrite;
    /* Mandatory from WDDM 2.6; left out below it so that a 1.3 driver is
     * the same as before. */
    if (MwDdiInterface >= DXGKDDI_INTERFACE_VERSION_WDDM2_6)
        init.DxgkDdiCollectDiagnosticInfo = MwCollectDiagnosticInfo;
    /* GpuMmu (Wddm2Shape 1, wddm2x.c). Left NULL otherwise: a DDI in the
     * table is itself a claim. */
    if (MwGpuMmu()) {
        init.DxgkDdiCreateProcess = T_CreateProcess;
        init.DxgkDdiDestroyProcess = T_DestroyProcess;
        init.DxgkDdiSetRootPageTable = T_SetRootPageTable;
        init.DxgkDdiGetRootPageTableSize = MwGetRootPageTableSize;
        init.DxgkDdiSubmitCommandVirtual = T_SubmitCommandVirtual;
    }
    /* Mandatory from interface 0x5008 on: without them dxgkrnl fails the
     * adapter start after QAI 16 (wddm2plan.h, Mw2NeedsPowerDdis). Never at
     * 1.3, whose table stays as it was. */
    if (MwPlan.PowerDdis) {
        init.DxgkDdiCalibrateGpuClock = T_CalibrateGpuClock;
        init.DxgkDdiSetStablePowerState = T_SetStablePowerState;
    }

    /* The "RK3576 GPU" counters (pcw.c): whatever the version, and the
     * driver goes on without them if PCW refuses. */
    MwPcwInit();
    {
        NTSTATUS st = DxgkInitialize(DriverObject, RegistryPath, &init);
        /* No DriverUnload after a failed DriverEntry: nothing of PCW may
         * still point into this image. */
        if (!NT_SUCCESS(st))
            MwPcwExit();
        return st;
    }
}
