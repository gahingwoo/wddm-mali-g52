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
 *   - DriverEntry with WddmProbe set writes WddmProbeArmed = WddmProbe.
 *   - When the adapter has done MW_PROBE_CONFIRM_RENDERS Render calls (DWM
 *     drawing for a few seconds), WddmProbeArmed goes back to 0 and
 *     WddmProbeConfirmed = WddmProbe is written.
 *   - If DriverEntry finds WddmProbeArmed equal to WddmProbe, the last boot
 *     with this value never got that far. It then deletes WddmProbe, writes
 *     WddmProbeFellBack = the refused value, and starts as WDDM 1.3.
 *
 * So a refused version costs one boot. WddmProbeKeep = 1 turns the arming
 * off (for repeating a probe that is known to start).
 *
 * Everything written here is in the service's Parameters key, not in the
 * device's Device Parameters: DriverEntry runs before there is a device.
 * StartDevice copies the result into the usual trace (Device
 * Parameters\maliwddm: WddmVersion, DdiInterface, WddmProbe).
 */
#include "maliwddm.h"
#include "wddm2.h"

/* How many Render calls count as "the desktop is up". DWM renders tens of
 * times a second on an idle desktop (PERF.md: 3.4 jobs per flip). */
#define MW_PROBE_CONFIRM_RENDERS    300

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
ULONG MwProbeNotifyResidencyOk = 1;

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
    ULONG want, armed;

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
    MwProbeNotifyResidencyOk = ParamRead(L"ProbeNotifyResidencyOk", 1);

    want = ParamRead(L"WddmProbe", 0);
    if (want == 0 || want == DXGKDDI_WDDMv1_3) {
        MwWddmProbeResult = MW_PROBE_NONE;
        return;
    }
    level = FindLevel(want);
    if (level == NULL) {
        ParamWrite(L"WddmProbeUnknown", want);
        MwWddmProbeResult = MW_PROBE_UNKNOWN;
        return;
    }
    armed = ParamRead(L"WddmProbeArmed", 0);
    if (armed == want && ParamRead(L"WddmProbeKeep", 0) == 0) {
        /* The last boot with this version never reached the desktop. */
        ParamDelete(L"WddmProbe");
        ParamWrite(L"WddmProbeArmed", 0);
        ParamWrite(L"WddmProbeFellBack", want);
        MwWddmProbeResult = MW_PROBE_FELL_BACK;
        return;
    }
    if (ParamRead(L"WddmProbeKeep", 0) == 0)
        ParamWrite(L"WddmProbeArmed", want);
    MwWddmVersion = level->Caps;
    MwDdiInterface = level->Interface;
    MwWddmProbe = want;
    MwWddmProbeResult = MW_PROBE_ACTIVE;
}

/* From MwRender, at PASSIVE_LEVEL. */
void MwVersionRendered(void)
{
    if (MwWddmProbe == 0 || ProbeConfirmed)
        return;
    if (InterlockedIncrement(&ProbeRenders) != MW_PROBE_CONFIRM_RENDERS)
        return;
    if (InterlockedExchange(&ProbeConfirmed, 1) != 0)
        return;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    ParamWrite(L"WddmProbeArmed", 0);
    ParamWrite(L"WddmProbeConfirmed", MwWddmProbe);
}

/* DXGKQAITYPE_PHYSICALADAPTERCAPS, asked of a 2.0 or later driver. One
 * physical adapter with one node, which is also the paging node, and no
 * GPU virtual addressing yet (P2 sets GpuMmuSupported here and in the
 * DRIVERCAPS). The structure grows with the interface version (VPRPagingNode
 * in 2.1, VirtualCopyNodeIndex in 2.7), so it is written field by field. */
NTSTATUS MwQueryPhysicalAdapterCaps(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_PHYSICALADAPTERCAPS *c = (DXGK_PHYSICALADAPTERCAPS *)Info->pOutputData;
    UINT size = Info->OutputDataSize;

    if (size < RTL_SIZEOF_THROUGH_FIELD(DXGK_PHYSICALADAPTERCAPS, Flags))
        return STATUS_BUFFER_TOO_SMALL;
    if (Info->pInputData != NULL && Info->InputDataSize >= sizeof(DXGK_QUERYPHYSICALADAPTERCAPSIN) &&
        ((const DXGK_QUERYPHYSICALADAPTERCAPSIN *)Info->pInputData)->PhysicalAdapterIndex != 0)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(c, size);
    c->NumExecutionNodes = 1;
    c->PagingNodeIndex = 0;
    c->DxgkPhysicalAdapterHandle = (HANDLE)A;
    c->Flags.Value = 0;
    return STATUS_SUCCESS;
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
