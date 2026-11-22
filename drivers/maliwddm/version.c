// SPDX-License-Identifier: GPL-2.0
/*
 * The WDDM version this driver reports (docs/WDDM2.md, phase P1, the probe).
 *
 * Without a setting the driver is WDDM 1.3, exactly as before. With
 *
 *   HKLM\SYSTEM\CurrentControlSet\Services\maliwddm\Parameters
 *     WddmProbe   REG_DWORD   0x2000 (2.0), 0x2100 (2.1), ... 0x3100 (3.1)
 *
 * it reports that version in DXGK_DRIVERCAPS::WDDMVersion and passes the
 * matching DXGKDDI_INTERFACE_VERSION to DxgkInitialize. Nothing else about
 * the driver changes: the memory model is still WDDM 1.x physical mode (one
 * aperture segment, allocation and patch lists), so the probe measures only
 * what dxgkrnl and the D3D runtime say about the number.
 *
 * The value is read in DriverEntry, so it takes effect at the next boot (or
 * the next time the driver image is loaded).
 *
 * One-shot fallback. A version dxgkrnl refuses can leave the board without
 * a desktop, so a probe arms itself:
 *
 *   - DriverEntry with WddmProbe set writes WddmProbeArmed = WddmProbe,
 *     flushes it and reads it back. If it does not read back, the probe is
 *     not used this boot (WddmProbeArmFailed, WDDM 1.3): an unarmed 2.x
 *     boot that hangs would hang every boot after it.
 *   - The probe's watchdog thread (probe.c) clears WddmProbeArmed and
 *     writes WddmProbeConfirmed once the boot is good by Mw2Confirm
 *     (wddm2plan.h): 300 renders, 120 s up and no DDI stuck.
 *   - If DriverEntry finds WddmProbeArmed equal to WddmProbe, the last boot
 *     with this value never got that far. It then deletes WddmProbe, writes
 *     WddmProbeFellBack = the refused value and WddmProbeFellBackBoot = the
 *     journal number of that boot, and starts as WDDM 1.3.
 *   - Every 2.x boot re-arms, a confirmed one too: a later boot that hangs
 *     also costs only itself.
 *
 * So a refused version costs one boot. WddmProbeKeep = 1 turns the arming
 * off (for repeating a probe that is known to start; never for a first
 * test). The decisions are wddm2plan.h's, checked by tests/wddm2-host.
 *
 * Everything written here is in the service's Parameters key, not in the
 * device's Device Parameters: DriverEntry runs before there is a device.
 * StartDevice copies the result into the usual trace (Device
 * Parameters\maliwddm: WddmVersion, DdiInterface, WddmProbe).
 */
#include "maliwddm.h"
#include "wddm2.h"

C_ASSERT(DXGKDDI_WDDMv1_3 == MW2_WDDM_1_3);
C_ASSERT(DXGKDDI_WDDMv2 == MW2_WDDM_2_0);

typedef struct {
    ULONG Caps;         /* DXGK_DRIVERCAPS::WDDMVersion */
    ULONG Interface;    /* DRIVER_INITIALIZATION_DATA::Version */
} MW_WDDM_LEVEL;

static const MW_WDDM_LEVEL Levels[] = {
    { DXGKDDI_WDDMv1_3, DXGKDDI_INTERFACE_VERSION_WDDM1_3 },
    { DXGKDDI_WDDMv2,   DXGKDDI_INTERFACE_VERSION_WDDM2_0 },
    { DXGKDDI_WDDMv2_1, DXGKDDI_INTERFACE_VERSION_WDDM2_1 },
    { DXGKDDI_WDDMv2_2, DXGKDDI_INTERFACE_VERSION_WDDM2_2 },
    { DXGKDDI_WDDMv2_3, DXGKDDI_INTERFACE_VERSION_WDDM2_3 },
    { DXGKDDI_WDDMv2_4, DXGKDDI_INTERFACE_VERSION_WDDM2_4 },
    { DXGKDDI_WDDMv2_5, DXGKDDI_INTERFACE_VERSION_WDDM2_5 },
    { DXGKDDI_WDDMv2_6, DXGKDDI_INTERFACE_VERSION_WDDM2_6 },
    { DXGKDDI_WDDMv2_7, DXGKDDI_INTERFACE_VERSION_WDDM2_7 },
    { DXGKDDI_WDDMv2_8, DXGKDDI_INTERFACE_VERSION_WDDM2_8 },
    { DXGKDDI_WDDMv2_9, DXGKDDI_INTERFACE_VERSION_WDDM2_9 },
    { DXGKDDI_WDDMv3_0, DXGKDDI_INTERFACE_VERSION_WDDM3_0 },
    /* The highest Windows 11 23H2 knows (22H2 = 3.1; 23H2 did not change
     * it). 3.2 is 24H2, which cannot run on the RK3576's Cortex-A72. */
    { DXGKDDI_WDDMv3_1, DXGKDDI_INTERFACE_VERSION_WDDM3_1 },
};

ULONG MwWddmVersion = DXGKDDI_WDDMv1_3;
ULONG MwDdiInterface = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
ULONG MwWddmProbe;              /* the armed value, 0 when nothing is armed */
ULONG MwWddmProbeResult;        /* see MW_PROBE_* in maliwddm.h */

ULONG MwProbeWatchdog;
ULONG MwProbeUart;
ULONG64 MwProbeUartBase = 0x2AD40000ULL;
ULONG MwWddm2Shape = MW_SHAPE_PHYSICAL;
ULONG MwWddm2Budget = 1;
MW2_PLAN MwPlan;
ULONG MwGpuMmuPtSegment = 1;
ULONG MwPagingMarker = 1;

static volatile LONG ProbeRenders;
static volatile LONG ProbeConfirmed;

/* "<service key>\Parameters", kept from DriverEntry for the confirm. */
static WCHAR ParamsBuf[512];
static UNICODE_STRING ParamsPath;

static NTSTATUS OpenParams(ACCESS_MASK Access, HANDLE *Key)
{
    OBJECT_ATTRIBUTES oa;

    if (ParamsPath.Length == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    InitializeObjectAttributes(&oa, &ParamsPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateKey(Key, Access, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
}

static ULONG ParamRead(PCWSTR Name, ULONG Default)
{
    HANDLE key;
    UNICODE_STRING name;
    struct { KEY_VALUE_PARTIAL_INFORMATION Info; ULONG Pad; } v;
    ULONG len, value = Default;

    if (!NT_SUCCESS(OpenParams(KEY_READ, &key)))
        return Default;
    RtlInitUnicodeString(&name, Name);
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, &v, sizeof(v), &len)) &&
        v.Info.Type == REG_DWORD && v.Info.DataLength == sizeof(ULONG))
        value = *(ULONG UNALIGNED *)v.Info.Data;
    ZwClose(key);
    return value;
}

static void ParamWrite(PCWSTR Name, ULONG Value)
{
    HANDLE key;
    UNICODE_STRING name;

    if (!NT_SUCCESS(OpenParams(KEY_WRITE, &key)))
        return;
    RtlInitUnicodeString(&name, Name);
    (void)ZwSetValueKey(key, &name, 0, REG_DWORD, &Value, sizeof(Value));
    /* To disk now: the point of the arming is to survive a bugcheck. */
    (void)ZwFlushKey(key);
    ZwClose(key);
}

static void ParamDelete(PCWSTR Name)
{
    HANDLE key;
    UNICODE_STRING name;

    if (!NT_SUCCESS(OpenParams(KEY_WRITE, &key)))
        return;
    RtlInitUnicodeString(&name, Name);
    (void)ZwDeleteValueKey(key, &name);
    (void)ZwFlushKey(key);
    ZwClose(key);
}

static const MW_WDDM_LEVEL *FindLevel(ULONG Caps)
{
    for (ULONG i = 0; i < RTL_NUMBER_OF(Levels); i++)
        if (Levels[i].Caps == Caps)
            return &Levels[i];
    return NULL;
}

void MwVersionInit(PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING suffix = RTL_CONSTANT_STRING(L"\\Parameters");
    const MW_WDDM_LEVEL *level;
    MW2_BOOT_IN in;
    MW2_BOOT_OUT out;
    ULONG boots;

    Mw2Plan(DXGKDDI_WDDMv1_3, 0, 0, &MwPlan);
    RtlInitEmptyUnicodeString(&ParamsPath, ParamsBuf, sizeof(ParamsBuf));
    if (RegistryPath == NULL || !NT_SUCCESS(RtlAppendUnicodeStringToString(&ParamsPath, RegistryPath)) ||
        !NT_SUCCESS(RtlAppendUnicodeStringToString(&ParamsPath, &suffix))) {
        ParamsPath.Length = 0;
        MwWddmProbeResult = MW_PROBE_NO_KEY;
        return;
    }

    /* The probe's instruments (probe.c); read whatever the version. */
    MwProbeWatchdog = ParamRead(L"ProbeWatchdog", 0);
    MwProbeUart = ParamRead(L"ProbeUart", 0);
    MwProbeUartBase = ParamRead(L"ProbeUartBase", (ULONG)MwProbeUartBase);
    MwWddm2Shape = ParamRead(L"Wddm2Shape", MW_SHAPE_PHYSICAL);
    MwWddm2Budget = ParamRead(L"Wddm2Budget", 1);
    MwGpuMmuPtSegment = ParamRead(L"GpuMmuPtSegment", 1);
    MwPagingMarker = ParamRead(L"PagingMarker", 1);

    /* The decision itself is wddm2plan.h's, checked by tests/wddm2-host. */
    in.Want = ParamRead(L"WddmProbe", 0);
    level = in.Want != 0 ? FindLevel(in.Want) : NULL;
    in.Known = level != NULL;
    in.Armed = ParamRead(L"WddmProbeArmed", 0);
    in.Keep = ParamRead(L"WddmProbeKeep", 0);
    Mw2BootDecide(&in, &out);

    if (out.Result == MW_PROBE_UNKNOWN)
        ParamWrite(L"WddmProbeUnknown", in.Want);
    if (out.FallBack) {
        /* The last boot with this version never reached a confirmed
         * desktop. The boot journal of that boot (Probe\Boot<n>) is left
         * alone for reading. */
        ParamDelete(L"WddmProbe");
        ParamWrite(L"WddmProbeArmed", 0);
        ParamWrite(L"WddmProbeFellBack", in.Want);
        ParamWrite(L"WddmProbeFellBackBoot", ParamRead(L"ProbeBoots", 0));
    }
    if (out.Arm) {
        ParamWrite(L"WddmProbeArmed", in.Want);
        out.Result = Mw2BootArmed(out.Result, out.Arm, ParamRead(L"WddmProbeArmed", 0), in.Want);
        if (out.Result == MW_PROBE_ARM_FAILED)
            ParamWrite(L"WddmProbeArmFailed", in.Want);
    }
    MwWddmProbeResult = out.Result;
    if (out.Result != MW_PROBE_ACTIVE)
        return;

    MwWddmVersion = level->Caps;
    MwDdiInterface = level->Interface;
    MwWddmProbe = in.Want;
    Mw2Plan(MwWddmVersion, MwWddm2Shape, MwWddm2Budget, &MwPlan);

    /* The boot journal (probe.c), from here on: a hang anywhere after
     * DriverEntry leaves the last DDI it entered on disk. */
    boots = ParamRead(L"ProbeBoots", 0) + 1;
    ParamWrite(L"ProbeBoots", boots);
    MwBcInit(RegistryPath, boots, ParamRead(L"ProbeJournalMax", 1500));
    MwBc("DriverEntry: WddmProbe 0x%x armed (keep %u), interface 0x%x, shape %u, budget %u, build %u",
         MwWddmProbe, in.Keep, MwDdiInterface, MwWddm2Shape, MwWddm2Budget, (ULONG)MW_BUILD);
    MwBc("plan: nodes %u, paging node %u (engine %u), VPR paging node %u, GpuMmu %u, VA %u, "
         "TDR %u, segment aperture %u cpuvisible %u apptarget %u nonlocal %u",
         MwPlan.Nodes, MwPlan.PagingNode, MwPlan.Engine[MwPlan.PagingNode], MwPlan.VprPagingNode,
         MwPlan.GpuMmu, MwPlan.VirtualAddressing, MwPlan.PerEngineTdr, MwPlan.SegAperture,
         MwPlan.SegCpuVisible, MwPlan.SegApplicationTarget, MwPlan.SegNonLocalBudget);
}

/* From MwRender. Counting only: the confirm is the watchdog's (below),
 * which runs at PASSIVE_LEVEL. Before, the 300th render wrote it, and a
 * 300th render at raised IRQL meant no confirm ever, on a good boot. */
void MwVersionRendered(void)
{
    if (MwWddmProbe != 0)
        InterlockedIncrement(&ProbeRenders);
}

/* From the probe's watchdog thread, once a second, at PASSIVE_LEVEL. A boot
 * is good when wddm2plan.h's Mw2Confirm says so: renders, two minutes up,
 * and nothing stuck. Until then WddmProbeArmed stays set, and a hang, a
 * bugcheck or a power cut leads to 1.3 on the next boot. */
void MwVersionConfirmCheck(ULONG Renders, ULONG Seconds, BOOLEAN Stuck)
{
    UNREFERENCED_PARAMETER(Renders);
    if (MwWddmProbe == 0 || ProbeConfirmed || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!Mw2Confirm((unsigned)ProbeRenders, (unsigned)Seconds, Stuck ? 1u : 0u))
        return;
    if (InterlockedExchange(&ProbeConfirmed, 1) != 0)
        return;
    ParamWrite(L"WddmProbeArmed", 0);
    ParamWrite(L"WddmProbeConfirmed", MwWddmProbe);
    MwBc("confirmed: %u renders, %u s up; WddmProbeArmed cleared", (ULONG)ProbeRenders, Seconds);
}

/* WDDM 2.6 makes this DDI mandatory ("Collect Diagnostic Info"). Its
 * arguments name the adapter by PDO, not by our context, so this answers
 * without touching any adapter state: an identifying description and no
 * private blob. */
NTSTATUS APIENTRY MwCollectDiagnosticInfo(IN_CONST_PDEVICE_OBJECT Pdo,
                                          DXGKARG_COLLECTDIAGNOSTICINFO *Arg)
{
    UNREFERENCED_PARAMETER(Pdo);
    if (Arg == NULL)
        return STATUS_INVALID_PARAMETER;
    (void)RtlStringCbPrintfA(Arg->BucketingString, sizeof(Arg->BucketingString),
                             "maliwddm_%u", (ULONG)MW_BUILD);
    (void)RtlStringCbPrintfA(Arg->DescriptionString, sizeof(Arg->DescriptionString),
                             "maliwddm build %u, WDDM 0x%x, type %u",
                             (ULONG)MW_BUILD, MwWddmVersion, (ULONG)Arg->Type);
    Arg->BufferSizeOut = 0;
    return STATUS_SUCCESS;
}
