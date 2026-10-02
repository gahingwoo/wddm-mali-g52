// SPDX-License-Identifier: GPL-2.0
/*
 * malidod: milestone M5.1, a kernel-mode display-only driver (KMDOD) for the
 * RK3576. It is the display half of the future WDDM driver, built and tested
 * on its own first.
 *
 * This first version does what Microsoft's Basic Display driver does, from
 * the DDI documentation rather than its source: it takes over the
 * framebuffer the firmware left running (DxgkCbAcquirePostDisplayOwnership),
 * offers exactly that one mode, and presents by copying the dirty rectangles
 * into it. It does not touch VOP2: on this SoC even register reads can drop
 * the HDMI signal, so flipping by window address comes in a later step, with
 * writes only.
 *
 * Binds GPU0 (ACPI\RKCP7402), like m1probe and malikm; only one of the three
 * can be installed at a time. Progress goes to the device's registry key:
 *   HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters
 */
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>

typedef struct _MD_DEVICE {
    PDEVICE_OBJECT Pdo;
    DXGKRNL_INTERFACE Dxgk;
    DXGK_DISPLAY_INFORMATION Fb;   /* the firmware's framebuffer */
    PUCHAR FbVa;                   /* mapped write-combined */
    SIZE_T FbSize;
    BOOLEAN SourceVisible;
    BOOLEAN PathActive;
    ULONG Presents;
    ULONG64 Seen;                  /* DDIs already reported as called */
} MD_DEVICE, *PMD_DEVICE;

#define MD_TAG 'dDlM'

DRIVER_INITIALIZE DriverEntry;
DXGKDDI_ADD_DEVICE MdAddDevice;
DXGKDDI_START_DEVICE MdStartDevice;
DXGKDDI_STOP_DEVICE MdStopDevice;
DXGKDDI_REMOVE_DEVICE MdRemoveDevice;
DXGKDDI_DISPATCH_IO_REQUEST MdDispatchIoRequest;
DXGKDDI_QUERY_CHILD_RELATIONS MdQueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS MdQueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR MdQueryDeviceDescriptor;
DXGKDDI_SET_POWER_STATE MdSetPowerState;
DXGKDDI_RESET_DEVICE MdResetDevice;
DXGKDDI_UNLOAD MdUnload;
DXGKDDI_QUERY_INTERFACE MdQueryInterface;
DXGKDDI_QUERYADAPTERINFO MdQueryAdapterInfo;
DXGKDDI_SETPOINTERPOSITION MdSetPointerPosition;
DXGKDDI_SETPOINTERSHAPE MdSetPointerShape;
DXGKDDI_ISSUPPORTEDVIDPN MdIsSupportedVidPn;
DXGKDDI_RECOMMENDFUNCTIONALVIDPN MdRecommendFunctionalVidPn;
DXGKDDI_ENUMVIDPNCOFUNCMODALITY MdEnumVidPnCofuncModality;
DXGKDDI_SETVIDPNSOURCEVISIBILITY MdSetVidPnSourceVisibility;
DXGKDDI_COMMITVIDPN MdCommitVidPn;
DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH MdUpdateActiveVidPnPresentPath;
DXGKDDI_RECOMMENDMONITORMODES MdRecommendMonitorModes;
DXGKDDI_QUERYVIDPNHWCAPABILITY MdQueryVidPnHWCapability;
DXGKDDI_PRESENTDISPLAYONLY MdPresentDisplayOnly;
DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP MdStopDeviceAndReleasePostDisplayOwnership;
DXGKDDI_SYSTEM_DISPLAY_ENABLE MdSystemDisplayEnable;
DXGKDDI_SYSTEM_DISPLAY_WRITE MdSystemDisplayWrite;

/* ---- diagnostics ---- */

static void Log(PMD_DEVICE Dev, PCWSTR Name, ULONG Value)
{
    HANDLE key;
    UNICODE_STRING name;

    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "malidod: %ws = 0x%08x\n", Name, Value);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Dev->Pdo == NULL)
        return;
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Dev->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key))) {
        RtlInitUnicodeString(&name, Name);
        (void)ZwSetValueKey(key, &name, 0, REG_DWORD, &Value, sizeof(Value));
        ZwClose(key);
    }
}

/* Record a DDI that returned failure, under its own name, so a device that
 * Windows stops (code 43) says which call it disliked. */
static NTSTATUS Ret(PMD_DEVICE Dev, PCWSTR Name, NTSTATUS St)
{
    /* The first call of each DDI: Seen_<name> = its status, so the order of
     * events up to a failure can be read back. Name is "Fail_<name>". */
    if (Dev != NULL) {
        ULONG h = 0;
        for (PCWSTR c = Name; *c; c++)
            h = h * 31 + *c;
        if (!(Dev->Seen & (1ULL << (h % 64)))) {
            WCHAR seen[64];
            UNICODE_STRING us;
            Dev->Seen |= 1ULL << (h % 64);
            RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
            if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Seen_%ws", Name + 5)))
                Log(Dev, seen, (ULONG)St);
        }
    }
    if (!NT_SUCCESS(St) && Dev != NULL) {
        Log(Dev, Name, (ULONG)St);
        Log(Dev, L"LastFailedDdi", (ULONG)St);
    }
    return St;
}

/* ---- the one mode: whatever the firmware set ---- */

static UINT BytesPerPixel(D3DDDIFORMAT F)
{
    return (F == D3DDDIFMT_A8R8G8B8 || F == D3DDDIFMT_X8R8G8B8) ? 4 : 0;
}

static void FillSourceMode(PMD_DEVICE Dev, D3DKMDT_VIDPN_SOURCE_MODE *M)
{
    M->Type = D3DKMDT_RMT_GRAPHICS;
    M->Format.Graphics.PrimSurfSize.cx = Dev->Fb.Width;
    M->Format.Graphics.PrimSurfSize.cy = Dev->Fb.Height;
    M->Format.Graphics.VisibleRegionSize = M->Format.Graphics.PrimSurfSize;
    M->Format.Graphics.Stride = Dev->Fb.Pitch;
    M->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    M->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    M->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

/* The firmware does not tell us the refresh rate; it sets 60 Hz modes. */
static void FillSignal(PMD_DEVICE Dev, D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = Dev->Fb.Width;
    S->TotalSize.cy = Dev->Fb.Height;
    S->ActiveSize = S->TotalSize;
    S->VSyncFreq.Numerator = 60;
    S->VSyncFreq.Denominator = 1;
    S->HSyncFreq.Numerator = 60 * Dev->Fb.Height;
    S->HSyncFreq.Denominator = 1;
    S->PixelRate = (SIZE_T)Dev->Fb.Width * Dev->Fb.Height * 60;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* ---- PnP ---- */

NTSTATUS MdAddDevice(IN_CONST_PDEVICE_OBJECT PhysicalDeviceObject, OUT_PPVOID MiniportDeviceContext)
{
    PMD_DEVICE dev = (PMD_DEVICE)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MD_DEVICE), MD_TAG);
    if (dev == NULL)
        return STATUS_NO_MEMORY;
    dev->Pdo = (PDEVICE_OBJECT)PhysicalDeviceObject;
    *MiniportDeviceContext = dev;
    return STATUS_SUCCESS;
}

NTSTATUS MdStartDevice(IN_CONST_PVOID MiniportDeviceContext, IN_PDXGK_START_INFO DxgkStartInfo,
                       IN_PDXGKRNL_INTERFACE DxgkInterface, OUT_PULONG NumberOfVideoPresentSources,
                       OUT_PULONG NumberOfChildren)
{
    PMD_DEVICE dev = (PMD_DEVICE)MiniportDeviceContext;
    PHYSICAL_ADDRESS pa;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(DxgkStartInfo);

    dev->Dxgk = *DxgkInterface;
    st = dev->Dxgk.DxgkCbAcquirePostDisplayOwnership(dev->Dxgk.DeviceHandle, &dev->Fb);
    Log(dev, L"AcquirePostDisplay", (ULONG)st);
    if (!NT_SUCCESS(st) || dev->Fb.Width == 0 || BytesPerPixel(dev->Fb.ColorFormat) == 0)
        return STATUS_UNSUCCESSFUL;
    Log(dev, L"FbWidth", dev->Fb.Width);
    Log(dev, L"FbHeight", dev->Fb.Height);
    Log(dev, L"FbPitch", dev->Fb.Pitch);
    Log(dev, L"FbFormat", (ULONG)dev->Fb.ColorFormat);
    Log(dev, L"FbPhysLo", (ULONG)dev->Fb.PhysicAddress.QuadPart);
    Log(dev, L"FbPhysHi", (ULONG)(dev->Fb.PhysicAddress.QuadPart >> 32));

    dev->FbSize = (SIZE_T)dev->Fb.Pitch * dev->Fb.Height;
    pa = dev->Fb.PhysicAddress;
    dev->FbVa = (PUCHAR)MmMapIoSpaceEx(pa, dev->FbSize, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (dev->FbVa == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    dev->SourceVisible = TRUE;
    dev->PathActive = TRUE;
    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = 1;
    Log(dev, L"Started", 1);
    return STATUS_SUCCESS;
}

static void UnmapFb(PMD_DEVICE Dev)
{
    if (Dev->FbVa != NULL) {
        MmUnmapIoSpace(Dev->FbVa, Dev->FbSize);
        Dev->FbVa = NULL;
    }
}

NTSTATUS MdStopDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    UnmapFb((PMD_DEVICE)MiniportDeviceContext);
    return STATUS_SUCCESS;
}

NTSTATUS MdRemoveDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    PMD_DEVICE dev = (PMD_DEVICE)MiniportDeviceContext;
    UnmapFb(dev);
    ExFreePoolWithTag(dev, MD_TAG);
    return STATUS_SUCCESS;
}

/* Hand the framebuffer back, e.g. to Basic Display when we are disabled. */
NTSTATUS MdStopDeviceAndReleasePostDisplayOwnership(IN_CONST_PVOID MiniportDeviceContext,
                                                   IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    PMD_DEVICE dev = (PMD_DEVICE)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    *DisplayInfo = dev->Fb;
    UnmapFb(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS MdDispatchIoRequestImpl(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG VidPnSourceId,
                             IN_PVIDEO_REQUEST_PACKET VideoRequestPacket)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(VideoRequestPacket);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS MdSetPowerState(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG DeviceUid,
                         IN_DEVICE_POWER_STATE DevicePowerState, IN_POWER_ACTION ActionType)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceUid);
    UNREFERENCED_PARAMETER(DevicePowerState);
    UNREFERENCED_PARAMETER(ActionType);
    return STATUS_SUCCESS;
}

VOID MdResetDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
}

VOID MdUnload(VOID)
{
}

static NTSTATUS MdQueryInterfaceImpl(IN_CONST_PVOID MiniportDeviceContext, IN_PQUERY_INTERFACE QueryInterface)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(QueryInterface);
    return STATUS_NOT_SUPPORTED;
}

/* ---- the monitor: one HDMI output, always connected, no EDID read ---- */

static NTSTATUS MdQueryChildRelationsImpl(IN_CONST_PVOID MiniportDeviceContext,
                               PDXGK_CHILD_DESCRIPTOR ChildRelations, ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    if (ChildRelationsSize < 2 * sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;   /* one child plus the zeroed terminator */
    RtlZeroMemory(ChildRelations, sizeof(DXGK_CHILD_DESCRIPTOR));
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HDMI;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS MdQueryChildStatusImpl(IN_CONST_PVOID MiniportDeviceContext, INOUT_PDXGK_CHILD_STATUS ChildStatus,
                            IN_BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    switch (ChildStatus->Type) {
    case StatusConnection:
        ChildStatus->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        ChildStatus->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* The firmware's EDID is not at hand here: Windows falls back to the modes
 * RecommendMonitorModes adds. */
static NTSTATUS MdQueryDeviceDescriptorImpl(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid,
                                 INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(DeviceDescriptor);
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

/* ---- adapter caps ---- */

static NTSTATUS QueryAdapterInfo(const DXGKARG_QUERYADAPTERINFO *Info);

NTSTATUS APIENTRY MdQueryAdapterInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *Info)
{
    PMD_DEVICE dev = (PMD_DEVICE)hAdapter;
    NTSTATUS st = QueryAdapterInfo(Info);
    WCHAR name[32];
    UNICODE_STRING us;

    /* Every type dxgkrnl asks about, and our answer: QAI_<type> = status. */
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u", (ULONG)Info->Type)))
        Log(dev, name, (ULONG)st);
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u_Size", (ULONG)Info->Type)))
        Log(dev, name, Info->OutputDataSize);
    return st;
}

/* A field of a structure dxgkrnl sized for our interface version: write it
 * only if it lies wholly inside the buffer we were given. */
#define FITS(Size, Type, Field) \
    (FIELD_OFFSET(Type, Field) + RTL_FIELD_SIZE(Type, Field) <= (Size))

static NTSTATUS QueryAdapterInfo(const DXGKARG_QUERYADAPTERINFO *Info)
{
    /* dxgkrnl passes the size of each structure as it was for the interface
     * version we declared (WIN8). Today's WDK headers make these structures
     * larger, so comparing against sizeof() rejects a buffer that is right
     * (it did: STATUS_BUFFER_TOO_SMALL for DRIVERCAPS, then code 43). Fill
     * what fits instead. */
    switch (Info->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)Info->pOutputData;
        UINT size = Info->OutputDataSize;
        if (!FITS(size, DXGK_DRIVERCAPS, HighestAcceptableAddress))
            return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(caps, size);
        caps->HighestAcceptableAddress.QuadPart = -1;
        /* WDDMVersion is reserved, and stays 0, for interface version WIN7
         * and later. No hardware cursor: MaxPointerWidth/Height stay 0. */
        if (FITS(size, DXGK_DRIVERCAPS, SupportNonVGA))
            caps->SupportNonVGA = TRUE;   /* we do release POST ownership */
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS MdSetPointerPositionImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

static NTSTATUS MdSetPointerShapeImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_NOT_SUPPORTED;
}

/* ---- VidPN: one source, one target, one mode, identity everything ---- */

static NTSTATUS MdIsSupportedVidPnImpl(IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *Arg)
{
    PMD_DEVICE dev = (PMD_DEVICE)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;

    Arg->IsVidPnSupported = FALSE;
    if (Arg->hDesiredVidPn == 0) {
        Arg->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnAcquireSourceModeSet(Arg->hDesiredVidPn, 0, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    /* Supported if nothing is pinned yet, or what is pinned is our mode. */
    if (NT_SUCCESS(st)) {
        Arg->IsVidPnSupported = pinned == NULL ||
            (pinned->Type == D3DKMDT_RMT_GRAPHICS &&
             pinned->Format.Graphics.PrimSurfSize.cx == dev->Fb.Width &&
             pinned->Format.Graphics.PrimSurfSize.cy == dev->Fb.Height);
        if (pinned != NULL)
            setIf->pfnReleaseModeInfo(set, pinned);
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hDesiredVidPn, set);
    return STATUS_SUCCESS;
}

static NTSTATUS MdRecommendFunctionalVidPnImpl(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_RECOMMENDFUNCTIONALVIDPN *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

/* Give the source and target of a path our mode if nothing is pinned. */
static NTSTATUS AddSourceMode(PMD_DEVICE Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_SOURCE_ID Src)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE *mode;
    NTSTATUS st;

    st = VidPn->pfnAcquireSourceModeSet(H, Src, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL)
        setIf->pfnReleaseModeInfo(set, pinned);
    VidPn->pfnReleaseSourceModeSet(H, set);
    if (!NT_SUCCESS(st) || pinned != NULL)
        return st;

    st = VidPn->pfnCreateNewSourceModeSet(H, Src, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSourceMode(Dev, mode);
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignSourceModeSet(H, Src, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseSourceModeSet(H, set);
    return st;
}

static NTSTATUS AddTargetMode(PMD_DEVICE Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_TARGET_ID Tgt)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_TARGET_MODE *pinned = NULL;
    D3DKMDT_VIDPN_TARGET_MODE *mode;
    NTSTATUS st;

    st = VidPn->pfnAcquireTargetModeSet(H, Tgt, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL)
        setIf->pfnReleaseModeInfo(set, pinned);
    VidPn->pfnReleaseTargetModeSet(H, set);
    if (!NT_SUCCESS(st) || pinned != NULL)
        return st;

    st = VidPn->pfnCreateNewTargetModeSet(H, Tgt, &set, &setIf);
    if (!NT_SUCCESS(st))
        return st;
    st = setIf->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSignal(Dev, &mode->VideoSignalInfo);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st))
            setIf->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st))
        st = VidPn->pfnAssignTargetModeSet(H, Tgt, set);
    if (!NT_SUCCESS(st))
        VidPn->pfnReleaseTargetModeSet(H, set);
    return st;
}

static NTSTATUS MdEnumVidPnCofuncModalityImpl(IN_CONST_HANDLE hAdapter,
                                            const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg)
{
    PMD_DEVICE dev = (PMD_DEVICE)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    NTSTATUS st;

    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnGetTopology(Arg->hConstrainingVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st))
        return st;

    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        /* The pivot is what the OS is enumerating against: the driver must
         * leave that mode set (or that path's transformation) alone. */
        D3DKMDT_ENUMCOFUNCMODALITY_PIVOT_TYPE pt = Arg->EnumPivotType;
        BOOLEAN srcPivot = pt == D3DKMDT_EPT_VIDPNSOURCE && Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId;
        BOOLEAN tgtPivot = pt == D3DKMDT_EPT_VIDPNTARGET && Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        BOOLEAN xfPivot = (pt == D3DKMDT_EPT_SCALING || pt == D3DKMDT_EPT_ROTATION) &&
                          Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
                          Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        NTSTATUS r = STATUS_SUCCESS;

        if (!srcPivot)
            r = AddSourceMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnSourceId);
        if (NT_SUCCESS(r) && !tgtPivot)
            r = AddTargetMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnTargetId);

        /* Identity scaling and rotation, and nothing else. */
        if (NT_SUCCESS(r) && !xfPivot &&
            (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
             path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;
            if (upd.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.ScalingSupport,
                              sizeof(upd.ContentTransformation.ScalingSupport));
                upd.ContentTransformation.ScalingSupport.Identity = 1;
            }
            if (upd.ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.RotationSupport,
                              sizeof(upd.ContentTransformation.RotationSupport));
                upd.ContentTransformation.RotationSupport.Identity = 1;
            }
            r = topoIf->pfnUpdatePathSupportInfo(topo, &upd);
        }
        if (!NT_SUCCESS(r)) {
            topoIf->pfnReleasePathInfo(topo, path);
            return r;
        }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET ? STATUS_SUCCESS : st;
}

static NTSTATUS MdSetVidPnSourceVisibilityImpl(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_SETVIDPNSOURCEVISIBILITY *Arg)
{
    PMD_DEVICE dev = (PMD_DEVICE)hAdapter;
    dev->SourceVisible = Arg->Visible;
    /* Invisible means blank: clear the framebuffer once. */
    if (!Arg->Visible && dev->FbVa != NULL)
        RtlZeroMemory(dev->FbVa, dev->FbSize);
    return STATUS_SUCCESS;
}

static NTSTATUS MdCommitVidPnImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *Arg)
{
    PMD_DEVICE dev = (PMD_DEVICE)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;

    st = dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st))
        return st;
    st = vidpn->pfnAcquireSourceModeSet(Arg->hFunctionalVidPn, 0, &set, &setIf);
    if (st == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE || !NT_SUCCESS(st)) {
        /* No path for our source: it is being turned off. */
        dev->PathActive = FALSE;
        Log(dev, L"CommitNoPath", (ULONG)st);
        return STATUS_SUCCESS;
    }
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL) {
        BOOLEAN ours = pinned->Type == D3DKMDT_RMT_GRAPHICS &&
                       pinned->Format.Graphics.PrimSurfSize.cx == dev->Fb.Width &&
                       pinned->Format.Graphics.PrimSurfSize.cy == dev->Fb.Height;
        setIf->pfnReleaseModeInfo(set, pinned);
        st = ours ? STATUS_SUCCESS : STATUS_GRAPHICS_INVALID_VIDPN;
        dev->PathActive = ours;
    } else if (NT_SUCCESS(st)) {
        dev->PathActive = FALSE;
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hFunctionalVidPn, set);
    Log(dev, L"CommitStatus", (ULONG)st);
    return st;
}

static NTSTATUS MdUpdateActiveVidPnPresentPathImpl(IN_CONST_HANDLE hAdapter,
                                                 const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

static NTSTATUS MdRecommendMonitorModesImpl(IN_CONST_HANDLE hAdapter,
                                          const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    PMD_DEVICE dev = (PMD_DEVICE)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE *mode;
    NTSTATUS st;

    st = Arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(Arg->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(st))
        return st;
    FillSignal(dev, &mode->VideoSignalInfo);
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 0;
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;
    st = Arg->pMonitorSourceModeSetInterface->pfnAddMode(Arg->hMonitorSourceModeSet, mode);
    if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        Arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Arg->hMonitorSourceModeSet, mode);
    return STATUS_SUCCESS;
}

static NTSTATUS MdQueryVidPnHWCapabilityImpl(IN_CONST_HANDLE hAdapter,
                                           DXGKARG_QUERYVIDPNHWCAPABILITY *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&Arg->VidPnHWCaps, sizeof(Arg->VidPnHWCaps));
    return STATUS_SUCCESS;
}

/* ---- present: copy the changed rectangles into the framebuffer ---- */

static void CopyRect(PMD_DEVICE Dev, const UCHAR *Src, LONG SrcPitch, const RECT *R)
{
    LONG l = max(R->left, 0), t = max(R->top, 0);
    LONG r = min(R->right, (LONG)Dev->Fb.Width), b = min(R->bottom, (LONG)Dev->Fb.Height);
    if (r <= l || b <= t)
        return;
    SIZE_T bytes = (SIZE_T)(r - l) * 4;
    for (LONG y = t; y < b; y++)
        RtlCopyMemory(Dev->FbVa + (SIZE_T)y * Dev->Fb.Pitch + (SIZE_T)l * 4,
                      Src + (SIZE_T)y * SrcPitch + (SIZE_T)l * 4, bytes);
}

static NTSTATUS MdPresentDisplayOnlyImpl(IN_CONST_HANDLE hAdapter, const DXGKARG_PRESENT_DISPLAYONLY *Arg)
{
    PMD_DEVICE dev = (PMD_DEVICE)hAdapter;
    PMDL mdl;
    const UCHAR *src;
    SIZE_T size;

    if (!dev->SourceVisible || !dev->PathActive || dev->FbVa == NULL)
        return STATUS_SUCCESS;
    if (Arg->BytesPerPixel != 4 || Arg->Flags.Rotate)
        return STATUS_NOT_SUPPORTED;

    /* The source image is in the presenting process; lock and map it. */
    size = (SIZE_T)Arg->Pitch * dev->Fb.Height;
    mdl = IoAllocateMdl(Arg->pSource, (ULONG)size, FALSE, FALSE, NULL);
    if (mdl == NULL)
        return STATUS_NO_MEMORY;
    __try {
        MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(mdl);
        return STATUS_INVALID_PARAMETER;
    }
    src = (const UCHAR *)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (src != NULL) {
        /* Moved regions are already in place in the source image, so they
         * are copied like dirty ones. */
        for (ULONG i = 0; i < Arg->NumMoves; i++)
            CopyRect(dev, src, Arg->Pitch, &Arg->pMoves[i].DestRect);
        for (ULONG i = 0; i < Arg->NumDirtyRects; i++)
            CopyRect(dev, src, Arg->Pitch, &Arg->pDirtyRect[i]);
    }
    MmUnlockPages(mdl);
    IoFreeMdl(mdl);
    if (++dev->Presents == 1 || (dev->Presents & 1023) == 0)
        Log(dev, L"Presents", dev->Presents);
    return src != NULL ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* ---- the bugcheck screen ---- */

NTSTATUS MdSystemDisplayEnable(IN_CONST_PVOID MiniportDeviceContext,
                               IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                               PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                               OUT UINT *Width, OUT UINT *Height, OUT D3DDDIFORMAT *ColorFormat)
{
    PMD_DEVICE dev = (PMD_DEVICE)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    if (dev->FbVa == NULL)
        return STATUS_UNSUCCESSFUL;
    *Width = dev->Fb.Width;
    *Height = dev->Fb.Height;
    *ColorFormat = dev->Fb.ColorFormat;
    return STATUS_SUCCESS;
}

VOID MdSystemDisplayWrite(IN_CONST_PVOID MiniportDeviceContext, IN_CONST_PVOID Source,
                          UINT SourceWidth, UINT SourceHeight, UINT SourceStride,
                          UINT PositionX, UINT PositionY)
{
    PMD_DEVICE dev = (PMD_DEVICE)MiniportDeviceContext;
    if (dev->FbVa == NULL)
        return;
    for (UINT y = 0; y < SourceHeight && PositionY + y < dev->Fb.Height; y++) {
        UINT w = min(SourceWidth, dev->Fb.Width > PositionX ? dev->Fb.Width - PositionX : 0);
        RtlCopyMemory(dev->FbVa + (SIZE_T)(PositionY + y) * dev->Fb.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR *)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
    }
}

/* ---- the DDIs as registered: log any failure under the DDI's name ---- */

NTSTATUS MdDispatchIoRequest(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG VidPnSourceId, IN_PVIDEO_REQUEST_PACKET VideoRequestPacket)
{
    return Ret((PMD_DEVICE)MiniportDeviceContext, L"Fail_DispatchIoRequest", MdDispatchIoRequestImpl(MiniportDeviceContext, VidPnSourceId, VideoRequestPacket));
}

NTSTATUS MdQueryChildRelations(IN_CONST_PVOID MiniportDeviceContext, PDXGK_CHILD_DESCRIPTOR ChildRelations, ULONG ChildRelationsSize)
{
    return Ret((PMD_DEVICE)MiniportDeviceContext, L"Fail_QueryChildRelations", MdQueryChildRelationsImpl(MiniportDeviceContext, ChildRelations, ChildRelationsSize));
}

NTSTATUS MdQueryChildStatus(IN_CONST_PVOID MiniportDeviceContext, INOUT_PDXGK_CHILD_STATUS ChildStatus, IN_BOOLEAN NonDestructiveOnly)
{
    return Ret((PMD_DEVICE)MiniportDeviceContext, L"Fail_QueryChildStatus", MdQueryChildStatusImpl(MiniportDeviceContext, ChildStatus, NonDestructiveOnly));
}

NTSTATUS MdQueryDeviceDescriptor(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid, INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    return Ret((PMD_DEVICE)MiniportDeviceContext, L"Fail_QueryDeviceDescriptor", MdQueryDeviceDescriptorImpl(MiniportDeviceContext, ChildUid, DeviceDescriptor));
}

NTSTATUS MdQueryInterface(IN_CONST_PVOID MiniportDeviceContext, IN_PQUERY_INTERFACE QueryInterface)
{
    return Ret((PMD_DEVICE)MiniportDeviceContext, L"Fail_QueryInterface", MdQueryInterfaceImpl(MiniportDeviceContext, QueryInterface));
}

NTSTATUS APIENTRY MdSetPointerPosition(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_SetPointerPosition", MdSetPointerPositionImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdSetPointerShape(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_SetPointerShape", MdSetPointerShapeImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdIsSupportedVidPn(IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_IsSupportedVidPn", MdIsSupportedVidPnImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdRecommendFunctionalVidPn(IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_RecommendFunctionalVidPn", MdRecommendFunctionalVidPnImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdEnumVidPnCofuncModality(IN_CONST_HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_EnumVidPnCofuncModality", MdEnumVidPnCofuncModalityImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdSetVidPnSourceVisibility(IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_SetVidPnSourceVisibility", MdSetVidPnSourceVisibilityImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdCommitVidPn(IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_CommitVidPn", MdCommitVidPnImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_UpdateActiveVidPnPresentPath", MdUpdateActiveVidPnPresentPathImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdRecommendMonitorModes(IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_RecommendMonitorModes", MdRecommendMonitorModesImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdQueryVidPnHWCapability(IN_CONST_HANDLE hAdapter, DXGKARG_QUERYVIDPNHWCAPABILITY *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_QueryVidPnHWCapability", MdQueryVidPnHWCapabilityImpl(hAdapter, Arg));
}

NTSTATUS APIENTRY MdPresentDisplayOnly(IN_CONST_HANDLE hAdapter, const DXGKARG_PRESENT_DISPLAYONLY *Arg)
{
    return Ret((PMD_DEVICE)hAdapter, L"Fail_PresentDisplayOnly", MdPresentDisplayOnlyImpl(hAdapter, Arg));
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    KMDDOD_INITIALIZATION_DATA init = {0};

    /* KMDOD arrived with Windows 8; the board runs 23H2 and newer WDK
     * headers default to interface versions it may not know. */
    init.Version = DXGKDDI_INTERFACE_VERSION_WIN8;
    init.DxgkDdiAddDevice = MdAddDevice;
    init.DxgkDdiStartDevice = MdStartDevice;
    init.DxgkDdiStopDevice = MdStopDevice;
    init.DxgkDdiRemoveDevice = MdRemoveDevice;
    init.DxgkDdiDispatchIoRequest = MdDispatchIoRequest;
    init.DxgkDdiQueryChildRelations = MdQueryChildRelations;
    init.DxgkDdiQueryChildStatus = MdQueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = MdQueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = MdSetPowerState;
    init.DxgkDdiResetDevice = MdResetDevice;
    init.DxgkDdiUnload = MdUnload;
    init.DxgkDdiQueryInterface = MdQueryInterface;
    init.DxgkDdiQueryAdapterInfo = MdQueryAdapterInfo;
    init.DxgkDdiSetPointerPosition = MdSetPointerPosition;
    init.DxgkDdiSetPointerShape = MdSetPointerShape;
    init.DxgkDdiIsSupportedVidPn = MdIsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = MdRecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = MdEnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = MdSetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = MdCommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = MdUpdateActiveVidPnPresentPath;
    init.DxgkDdiRecommendMonitorModes = MdRecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = MdQueryVidPnHWCapability;
    init.DxgkDdiPresentDisplayOnly = MdPresentDisplayOnly;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = MdStopDeviceAndReleasePostDisplayOwnership;
    init.DxgkDdiSystemDisplayEnable = MdSystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = MdSystemDisplayWrite;
    /* No VSync control: DxgkDdiControlInterrupt and DxgkDdiGetScanLine must
     * stay NULL, and with them the interrupt and DPC routines. */

    return DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &init);
}
