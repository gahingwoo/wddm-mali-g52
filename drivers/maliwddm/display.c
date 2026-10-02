// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's display: malidod's, which drives the HDMI output under
 * Windows (M5.1). One HDMI child, the firmware's framebuffer and mode,
 * identity transforms, unspecified target frequencies. Flips land in
 * MwSetVidPnSourceAddress; for now it records the address and the
 * framebuffer is written by presents (step d).
 */
#include "maliwddm.h"

static UINT BytesPerPixel(D3DDDIFORMAT F)
{
    return (F == D3DDDIFMT_A8R8G8B8 || F == D3DDDIFMT_X8R8G8B8) ? 4 : 0;
}

static void FillSourceMode(MW_ADAPTER *Dev, D3DKMDT_VIDPN_SOURCE_MODE *M)
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

/* A real 60 Hz timing. The firmware sets 1920x1080 with the CEA-861
 * timing (VIC 16: 2200x1125 total, 148.5 MHz); +280/+45 blanking gives
 * exactly that, and something plausible for other sizes. A full WDDM
 * driver may not leave the frequencies unspecified the way a display-only
 * one does: board run 4 got STATUS_GRAPHICS_INVALID_FREQUENCY from
 * pfnAddMode for the target mode, and Windows stopped at boot. */
static void FillSignal(MW_ADAPTER *Dev, D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    UINT totalW = Dev->Fb.Width + 280, totalH = Dev->Fb.Height + 45;

    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = totalW;
    S->TotalSize.cy = totalH;
    S->ActiveSize.cx = Dev->Fb.Width;
    S->ActiveSize.cy = Dev->Fb.Height;
    S->PixelRate = (SIZE_T)totalW * totalH * 60;
    S->HSyncFreq.Numerator = (UINT)S->PixelRate;
    S->HSyncFreq.Denominator = totalW;
    S->VSyncFreq.Numerator = (UINT)S->PixelRate;
    S->VSyncFreq.Denominator = totalW * totalH;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* ---- PnP ---- */


/* ---- the firmware framebuffer ---- */

NTSTATUS MwDisplayStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    NTSTATUS st;

    st = A->Dxgk.DxgkCbAcquirePostDisplayOwnership(A->Dxgk.DeviceHandle, &A->Fb);
    MwLog(A, L"AcquirePostDisplay", (ULONG)st);
    if (!NT_SUCCESS(st) || A->Fb.Width == 0 || BytesPerPixel(A->Fb.ColorFormat) == 0)
        return STATUS_UNSUCCESSFUL;
    MwLog(A, L"FbWidth", A->Fb.Width);
    MwLog(A, L"FbHeight", A->Fb.Height);
    A->FbSize = (SIZE_T)A->Fb.Pitch * A->Fb.Height;
    pa = A->Fb.PhysicAddress;
    A->FbVa = (PUCHAR)MmMapIoSpaceEx(pa, A->FbSize, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (A->FbVa == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    A->SourceVisible = TRUE;
    A->PathActive = TRUE;
    return STATUS_SUCCESS;
}

void MwDisplayStop(MW_ADAPTER *A)
{
    if (A->FbVa != NULL) {
        MmUnmapIoSpace(A->FbVa, A->FbSize);
        A->FbVa = NULL;
    }
}


/* Hand the framebuffer back, e.g. to Basic Display when we are disabled. */
NTSTATUS APIENTRY MwStopDeviceAndReleasePostDisplayOwnership(IN_CONST_PVOID MiniportDeviceContext,
                                                   IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    *DisplayInfo = dev->Fb;
    MwDisplayStop(dev);
    return STATUS_SUCCESS;
}


/* ---- the monitor: one HDMI output, always connected, no EDID read ---- */

NTSTATUS APIENTRY MwQueryChildRelations(IN_CONST_PVOID MiniportDeviceContext,
                               PDXGK_CHILD_DESCRIPTOR ChildRelations, ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
#if MW_RENDER_ONLY
    if (ChildRelationsSize < sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(ChildRelations, sizeof(DXGK_CHILD_DESCRIPTOR)); /* the terminator alone */
    return STATUS_SUCCESS;
#else
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
#endif
}

NTSTATUS APIENTRY MwQueryChildStatus(IN_CONST_PVOID MiniportDeviceContext, INOUT_PDXGK_CHILD_STATUS ChildStatus,
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
NTSTATUS APIENTRY MwQueryDeviceDescriptor(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid,
                                 INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(DeviceDescriptor);
    return STATUS_MONITOR_NO_DESCRIPTOR;
}


/* ---- VidPN: one source, one target, one mode, identity everything ---- */

NTSTATUS APIENTRY MwIsSupportedVidPn(IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
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

NTSTATUS APIENTRY MwRecommendFunctionalVidPn(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_RECOMMENDFUNCTIONALVIDPN *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

/* Give the source and target of a path our mode if nothing is pinned. */
static NTSTATUS AddSourceMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
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

static NTSTATUS AddTargetMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
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
        /* The same timing the monitor mode carries. */
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

NTSTATUS APIENTRY MwEnumVidPnCofuncModality(IN_CONST_HANDLE hAdapter,
                                            const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
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

        if (!srcPivot) {
            r = AddSourceMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnSourceId);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddSourceMode", (ULONG)r);
        }
        if (NT_SUCCESS(r) && !tgtPivot) {
            r = AddTargetMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnTargetId);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddTargetMode", (ULONG)r);
        }

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

NTSTATUS APIENTRY MwSetVidPnSourceVisibility(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_SETVIDPNSOURCEVISIBILITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    dev->SourceVisible = Arg->Visible;
    /* Invisible means blank: clear the framebuffer once. */
    if (!Arg->Visible && dev->FbVa != NULL)
        RtlZeroMemory(dev->FbVa, dev->FbSize);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwCommitVidPn(IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
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
        MwLog(dev, L"CommitNoPath", (ULONG)st);
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
    MwLog(dev, L"CommitStatus", (ULONG)st);
    return st;
}

NTSTATUS APIENTRY MwUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter,
                                                 const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRecommendMonitorModes(IN_CONST_HANDLE hAdapter,
                                          const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
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
    MwLog(dev, L"MonitorAddMode", (ULONG)st);
    if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        Arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Arg->hMonitorSourceModeSet, mode);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwQueryVidPnHWCapability(IN_CONST_HANDLE hAdapter,
                                           DXGKARG_QUERYVIDPNHWCAPABILITY *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&Arg->VidPnHWCaps, sizeof(Arg->VidPnHWCaps));
    return STATUS_SUCCESS;
}


/* ---- the bugcheck screen ---- */

NTSTATUS APIENTRY MwSystemDisplayEnable(IN_CONST_PVOID MiniportDeviceContext,
                               IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                               PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                               OUT UINT *Width, OUT UINT *Height, OUT D3DDDIFORMAT *ColorFormat)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    if (dev->FbVa == NULL)
        return STATUS_UNSUCCESSFUL;
    *Width = dev->Fb.Width;
    *Height = dev->Fb.Height;
    *ColorFormat = dev->Fb.ColorFormat;
    return STATUS_SUCCESS;
}

VOID APIENTRY MwSystemDisplayWrite(IN_CONST_PVOID MiniportDeviceContext, IN_CONST_PVOID Source,
                          UINT SourceWidth, UINT SourceHeight, UINT SourceStride,
                          UINT PositionX, UINT PositionY)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    if (dev->FbVa == NULL)
        return;
    for (UINT y = 0; y < SourceHeight && PositionY + y < dev->Fb.Height; y++) {
        UINT w = min(SourceWidth, dev->Fb.Width > PositionX ? dev->Fb.Width - PositionX : 0);
        RtlCopyMemory(dev->FbVa + (SIZE_T)(PositionY + y) * dev->Fb.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR *)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
    }
}


NTSTATUS APIENTRY MwSetVidPnSourceAddress(IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    /* Step d: the primary's pixels go to the framebuffer here. */
    return STATUS_SUCCESS;
}
