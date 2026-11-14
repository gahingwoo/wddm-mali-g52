// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's display: malidod's, which drives the HDMI output under
 * Windows (M5.1). One HDMI child with the monitor's EDID if the firmware
 * left it in the DSDT, the firmware's framebuffer and timing, identity
 * transforms (and, with ScaledModes, smaller source modes stretched to the
 * firmware's). Flips land in MwSetVidPnSourceAddress, which records them;
 * scanout.c or the flip copy in command.c puts them on the screen.
 */
#include "maliwddm.h"
#include <acpiioct.h>

#ifndef DISPLAY_ADAPTER_HW_ID
#define DISPLAY_ADAPTER_HW_ID 0xFFFFFFFFu
#endif

static UINT BytesPerPixel(D3DDDIFORMAT F)
{
    return (F == D3DDDIFMT_A8R8G8B8 || F == D3DDDIFMT_X8R8G8B8) ? 4 : 0;
}

/* ---- source modes ----
 *
 * The firmware's mode is the only timing there is: the HDMI link is never
 * retimed from Windows. With ScaledModes set the driver also offers
 * smaller source modes of the same shape, for the VOP2's window scaler to
 * stretch to the firmware's size. They are offered only when scan-out is
 * set up: the copy to the framebuffer cannot scale, and would put a small
 * picture in the top left corner. Scan-out does not scale yet either
 * (scanout.c refuses a primary of another size and it is copied), so for
 * now the setting is for trying the VidPN side alone. */

static const struct { USHORT W, H; } Ladder[] = {
    { 3840, 2160 }, { 3200, 1800 }, { 2560, 1600 }, { 2560, 1440 }, { 1920, 1200 },
    { 1920, 1080 }, { 1680, 1050 }, { 1600, 1200 }, { 1600, 900 },  { 1440, 900 },
    { 1280, 1024 }, { 1280, 800 },  { 1280, 720 },  { 1024, 768 },  { 800, 600 },
};

/* Mode Index of the source modes: 0 is the firmware's, then the ladder
 * sizes that are smaller, exactly the same shape (so "stretched" keeps
 * the aspect ratio) and no more than 8 times smaller in either direction
 * (the Esmart window's limit in mainline rockchip_vop2_reg.c). FALSE when
 * there is no such mode. */
static BOOLEAN SourceModeSize(MW_ADAPTER *Dev, ULONG Index, UINT *W, UINT *H)
{
    UINT fw = Dev->Fb.Width, fh = Dev->Fb.Height;

    if (Index == 0) {
        *W = fw;
        *H = fh;
        return TRUE;
    }
    if (!Dev->ScaledModes)
        return FALSE;
    for (ULONG i = 0; i < ARRAYSIZE(Ladder); i++) {
        UINT w = Ladder[i].W, h = Ladder[i].H;
        if (w >= fw || h >= fh || (ULONG64)w * fh != (ULONG64)h * fw || w * 8 < fw || h * 8 < fh)
            continue;
        if (--Index == 0) {
            *W = w;
            *H = h;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN IsSourceModeSize(MW_ADAPTER *Dev, UINT W, UINT H)
{
    UINT w, h;

    for (ULONG i = 0; SourceModeSize(Dev, i, &w, &h); i++)
        if (w == W && h == H)
            return TRUE;
    return FALSE;
}

static void FillSourceMode(MW_ADAPTER *Dev, D3DKMDT_VIDPN_SOURCE_MODE *M, UINT W, UINT H)
{
    M->Type = D3DKMDT_RMT_GRAPHICS;
    M->Format.Graphics.PrimSurfSize.cx = W;
    M->Format.Graphics.PrimSurfSize.cy = H;
    M->Format.Graphics.VisibleRegionSize = M->Format.Graphics.PrimSurfSize;
    M->Format.Graphics.Stride = (W == Dev->Fb.Width) ? Dev->Fb.Pitch : W * 4;
    M->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    M->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    M->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

/* The firmware's timing. A full WDDM driver may not leave the frequencies
 * unspecified the way a display-only one does: board run 4 got
 * STATUS_GRAPHICS_INVALID_FREQUENCY from pfnAddMode for the target mode,
 * and Windows stopped at boot. TimingFromEdid or TimingMadeUp sets it. */
static void FillSignal(MW_ADAPTER *Dev, D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    UINT totalW = Dev->TimingTotalW, totalH = Dev->TimingTotalH;

    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = totalW;
    S->TotalSize.cy = totalH;
    S->ActiveSize.cx = Dev->Fb.Width;
    S->ActiveSize.cy = Dev->Fb.Height;
    S->PixelRate = Dev->TimingPixelRate;
    S->HSyncFreq.Numerator = Dev->TimingPixelRate;
    S->HSyncFreq.Denominator = totalW;
    S->VSyncFreq.Numerator = Dev->TimingPixelRate;
    S->VSyncFreq.Denominator = totalW * totalH;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* 60 Hz with +280/+45 blanking: for 1920x1080 that is exactly the CEA-861
 * timing (VIC 16: 2200x1125 total, 148.5 MHz), and something plausible
 * for other sizes. What there was before the EDID. */
static void TimingMadeUp(MW_ADAPTER *Dev)
{
    Dev->TimingTotalW = Dev->Fb.Width + 280;
    Dev->TimingTotalH = Dev->Fb.Height + 45;
    Dev->TimingPixelRate = Dev->TimingTotalW * Dev->TimingTotalH * 60;
}

/* The first progressive detailed timing in the base block whose active
 * size is the framebuffer's. The firmware's native mode is the monitor's
 * preferred timing, which is the first detailed timing, so this is the
 * timing on the wire unless the firmware was set to a preset of the same
 * size (then only the refresh rate Windows shows would be off). With it
 * the target mode is one of the EDID's own modes, not a second, made-up
 * mode of the same size next to it. */
static BOOLEAN TimingFromEdid(MW_ADAPTER *Dev)
{
    for (ULONG off = 54; off + 18 <= 126; off += 18) {
        const UCHAR *d = Dev->Edid + off;
        ULONG clk = d[0] | (ULONG)d[1] << 8;            /* 10 kHz units; 0: not a timing */
        UINT ha = d[2] | (UINT)(d[4] & 0xF0) << 4, hb = d[3] | (UINT)(d[4] & 0x0F) << 8;
        UINT va = d[5] | (UINT)(d[7] & 0xF0) << 4, vb = d[6] | (UINT)(d[7] & 0x0F) << 8;

        if (clk == 0 || (d[17] & 0x80) || ha != Dev->Fb.Width || va != Dev->Fb.Height || hb == 0 || vb == 0)
            continue;
        Dev->TimingTotalW = ha + hb;
        Dev->TimingTotalH = va + vb;
        Dev->TimingPixelRate = clk * 10000;
        return TRUE;
    }
    return FALSE;
}

/* ---- the EDID, from the firmware ----
 *
 * The VOP2's HDMI DDC is not ours to drive, but the firmware read the
 * EDID over it at boot and leaves the bytes in the DSDT: GPU0.EDID returns
 * a 512-byte buffer, the EDID then zeros (edk2-rk3576, Gpu.asl). Firmware
 * from before that has no such method; firmware that read no EDID returns
 * zeros. Either way EdidSize stays 0 and the monitor is what it was
 * before: no descriptor, one made-up mode. */

/* The base block's header and checksum; the extensions it announces, as
 * far as the buffer has them and they are not zero (the firmware zeroes
 * the buffer after the EDID). Their checksums are the firmware's to have
 * checked: it lets a CEA-861 block with a wrong one through, as Linux
 * does, for KVM switches that edit the block and do not fix it. */
static ULONG EdidLength(const UCHAR *E, ULONG Len)
{
    static const UCHAR header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    UCHAR sum = 0;
    ULONG blocks, have;

    if (Len < 128 || RtlCompareMemory(E, header, sizeof(header)) != sizeof(header))
        return 0;
    for (ULONG i = 0; i < 128; i++)
        sum = (UCHAR)(sum + E[i]);
    if (sum != 0)
        return 0;
    blocks = 1 + (ULONG)E[126];
    have = min(Len, MW_EDID_MAX) / 128;
    for (ULONG b = 1; b < min(blocks, have); b++) {
        BOOLEAN zero = TRUE;
        for (ULONG i = 0; i < 128 && zero; i++)
            zero = E[b * 128 + i] == 0;
        if (zero)
            return b * 128;
    }
    return min(blocks, have) * 128;
}

static NTSTATUS EvalEdid(MW_ADAPTER *A, ULONG Signature, PACPI_EVAL_OUTPUT_BUFFER Out, ULONG OutSize)
{
    ACPI_EVAL_INPUT_BUFFER_COMPLEX in;

    RtlZeroMemory(&in, sizeof(in));
    in.Signature = Signature;
    in.MethodNameAsUlong = (ULONG)'DIDE';       /* "EDID" */
    in.Size = 0;
    in.ArgumentCount = 0;
    RtlZeroMemory(Out, OutSize);
    return A->Dxgk.DxgkCbEvalAcpiMethod(A->Dxgk.DeviceHandle, DISPLAY_ADAPTER_HW_ID, &in,
                                        sizeof(in), Out, OutSize);
}

/* PASSIVE_LEVEL, from StartDevice. Registry trace: EdidEval (the status of
 * the evaluation), EdidBytes, EdidTiming (1: the target mode is the EDID's
 * detailed timing, 0: made up). */
static void ReadEdid(MW_ADAPTER *A)
{
    const ULONG head = (ULONG)FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) +
                       (ULONG)FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data);
    const ULONG outSize = head + MW_EDID_MAX + 64;
    PACPI_EVAL_OUTPUT_BUFFER out;
    NTSTATUS st;

    A->EdidSize = 0;
    out = (PACPI_EVAL_OUTPUT_BUFFER)ExAllocatePool2(POOL_FLAG_PAGED, outSize, MW_TAG);
    if (out == NULL)
        return;
    /* dxgkrnl's documentation asks a driver with children to say
     * DXGK_ACPI_PASS_ARGS_TO_CHILDREN; this method has no arguments, and
     * the plain signature is tried if that is refused. */
#ifdef DXGK_ACPI_PASS_ARGS_TO_CHILDREN
    st = EvalEdid(A, DXGK_ACPI_PASS_ARGS_TO_CHILDREN, out, outSize);
    if (!NT_SUCCESS(st) && st != STATUS_OBJECT_NAME_NOT_FOUND)
#endif
        st = EvalEdid(A, ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE, out, outSize);
    MwLog(A, L"EdidEval", (ULONG)st);
    if (NT_SUCCESS(st) && out->Signature == ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE && out->Count >= 1 &&
        out->Argument[0].Type == ACPI_METHOD_ARGUMENT_BUFFER &&
        head + (ULONG)out->Argument[0].DataLength <= outSize) {
        ULONG len = EdidLength(out->Argument[0].Data, out->Argument[0].DataLength);
        if (len != 0) {
            RtlCopyMemory(A->Edid, out->Argument[0].Data, len);
            A->EdidSize = len;
        }
    }
    ExFreePoolWithTag(out, MW_TAG);
    MwLog(A, L"EdidBytes", A->EdidSize);
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
    A->SrcWidth = A->Fb.Width;
    A->SrcHeight = A->Fb.Height;
    ReadEdid(A);
    {
        BOOLEAN fromEdid = A->EdidSize != 0 && TimingFromEdid(A);
        if (!fromEdid)
            TimingMadeUp(A);
        MwLog(A, L"EdidTiming", fromEdid);
    }
    MwLog(A, L"TimingPixelRate", A->TimingPixelRate);
    A->FbSize = (SIZE_T)A->Fb.Pitch * A->Fb.Height;
    pa = A->Fb.PhysicAddress;
    A->FbVa = (PUCHAR)MmMapIoSpaceEx(pa, A->FbSize, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (A->FbVa == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    A->SourceVisible = TRUE;
    A->PathActive = TRUE;
    MwScanStart(A);
    /* Only with scan-out set up: see SourceModeSize. */
    A->ScaledModes = MwSetting(A, L"ScaledModes", 0) != 0 && A->ScanReady;
    {
        UINT w, h;
        ULONG n = 0;
        while (SourceModeSize(A, n, &w, &h))
            n++;
        MwLog(A, L"SourceModes", n);
    }
    return STATUS_SUCCESS;
}

void MwDisplayStop(MW_ADAPTER *A)
{
    MwScanStop(A);
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


/* ---- the monitor: one HDMI output, always connected, its EDID from the
 * firmware when the firmware has one ---- */

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

/* The EDID by offset and length: dxgkrnl asks for the base block, then
 * monitor.sys for whatever it wants of it, an extension block at a time.
 * Without one Windows falls back to the modes RecommendMonitorModes adds,
 * and the monitor is "Generic Non-PnP Monitor". */
NTSTATUS APIENTRY MwQueryDeviceDescriptor(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid,
                                 INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    ULONG off, len, n;

    if (ChildUid != 0 || dev->EdidSize == 0)
        return STATUS_MONITOR_NO_DESCRIPTOR;
    off = DeviceDescriptor->DescriptorOffset;
    len = DeviceDescriptor->DescriptorLength;
    if (off >= dev->EdidSize)
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    if (DeviceDescriptor->DescriptorBuffer == NULL || len == 0)
        return STATUS_INVALID_PARAMETER;
    n = min(len, dev->EdidSize - off);
    RtlCopyMemory(DeviceDescriptor->DescriptorBuffer, dev->Edid + off, n);
    if (n < len)        /* never more than DescriptorLength */
        RtlZeroMemory((PUCHAR)DeviceDescriptor->DescriptorBuffer + n, len - n);
    return STATUS_SUCCESS;
}


/* ---- VidPN: one source, one target, the firmware's timing on the target ---- */

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
    /* Supported if nothing is pinned yet, or what is pinned is one of ours. */
    if (NT_SUCCESS(st)) {
        Arg->IsVidPnSupported = pinned == NULL ||
            (pinned->Type == D3DKMDT_RMT_GRAPHICS &&
             IsSourceModeSize(dev, pinned->Format.Graphics.PrimSurfSize.cx,
                              pinned->Format.Graphics.PrimSurfSize.cy));
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

/* Give the source and target of a path our modes if nothing is pinned:
 * the firmware's, and with ScaledModes the smaller ones (SourceModeSize).
 * The target has only the firmware's, whatever the source. */
static NTSTATUS AddSourceMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_SOURCE_ID Src)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE *mode;
    NTSTATUS st;
    UINT w, h;

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
    for (ULONG i = 0; NT_SUCCESS(st) && SourceModeSize(Dev, i, &w, &h); i++) {
        st = setIf->pfnCreateNewModeInfo(set, &mode);
        if (!NT_SUCCESS(st))
            break;
        FillSourceMode(Dev, mode, w, h);
        st = setIf->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st)) {
            setIf->pfnReleaseModeInfo(set, mode);
            /* A scaled mode refused is that mode gone, not the set. */
            if (i != 0) {
                MwLog(Dev, L"Fail_AddScaledMode", (ULONG)st);
                st = STATUS_SUCCESS;
            }
        }
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

/* The size of the source mode pinned on a source, if one is. */
static BOOLEAN PinnedSourceSize(const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                                D3DDDI_VIDEO_PRESENT_SOURCE_ID Src, UINT *W, UINT *H2)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    BOOLEAN found = FALSE;

    if (!NT_SUCCESS(VidPn->pfnAcquireSourceModeSet(H, Src, &set, &setIf)))
        return FALSE;
    if (NT_SUCCESS(setIf->pfnAcquirePinnedModeInfo(set, &pinned)) && pinned != NULL) {
        if (pinned->Type == D3DKMDT_RMT_GRAPHICS) {
            *W = pinned->Format.Graphics.PrimSurfSize.cx;
            *H2 = pinned->Format.Graphics.PrimSurfSize.cy;
            found = TRUE;
        }
        setIf->pfnReleaseModeInfo(set, pinned);
    }
    VidPn->pfnReleaseSourceModeSet(H, set);
    return found;
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

        /* Identity rotation. Identity scaling for the firmware's mode; a
         * smaller source mode is stretched to it (the same shape, so this
         * keeps the aspect ratio). Before a source mode is pinned both are
         * possible. */
        if (NT_SUCCESS(r) && !xfPivot &&
            (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
             path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;
            if (upd.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
                UINT w = dev->Fb.Width, h = dev->Fb.Height;
                BOOLEAN pinned = dev->ScaledModes &&
                    PinnedSourceSize(vidpn, Arg->hConstrainingVidPn, path->VidPnSourceId, &w, &h);
                RtlZeroMemory(&upd.ContentTransformation.ScalingSupport,
                              sizeof(upd.ContentTransformation.ScalingSupport));
                if (!dev->ScaledModes || (pinned && w == dev->Fb.Width && h == dev->Fb.Height)) {
                    upd.ContentTransformation.ScalingSupport.Identity = 1;
                } else if (pinned) {
                    upd.ContentTransformation.ScalingSupport.Stretched = 1;
                } else {
                    upd.ContentTransformation.ScalingSupport.Identity = 1;
                    upd.ContentTransformation.ScalingSupport.Stretched = 1;
                }
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
    if (Arg->Visible) dev->VisibleOn++; else dev->VisibleOff++;
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
        dev->CommitsNoPath++;
        MwLog(dev, L"CommitNoPath", (ULONG)st);
        return STATUS_SUCCESS;
    }
    st = setIf->pfnAcquirePinnedModeInfo(set, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL) {
        UINT w = pinned->Format.Graphics.PrimSurfSize.cx, h = pinned->Format.Graphics.PrimSurfSize.cy;
        BOOLEAN ours = pinned->Type == D3DKMDT_RMT_GRAPHICS && IsSourceModeSize(dev, w, h);
        setIf->pfnReleaseModeInfo(set, pinned);
        st = ours ? STATUS_SUCCESS : STATUS_GRAPHICS_INVALID_VIDPN;
        dev->PathActive = ours;
        if (ours) dev->CommitsPinned++; else dev->CommitsNotOurs++;
        /* What primaries will now be, and what scan-out is to stretch to
         * the firmware's size. The window itself changes at the first flip
         * to a primary of this size; until then it shows what it did. */
        if (ours && (w != dev->SrcWidth || h != dev->SrcHeight)) {
            dev->SrcWidth = w;
            dev->SrcHeight = h;
            MwLog(dev, L"CommitSourceWidth", w);
            MwLog(dev, L"CommitSourceHeight", h);
        }
    } else if (NT_SUCCESS(st)) {
        /* Nothing pinned: the source is being turned off. AcquirePinnedModeInfo
         * says so with an informational status (0x401E0307), which is not
         * ours to return: dxgkrnl logged "Driver returned an invalid NTSTATUS
         * code: 0x401e0307" for every such commit. */
        dev->PathActive = FALSE;
        dev->CommitsUnpinned++;
        MwLog(dev, L"CommitUnpinned", (ULONG)st);
        st = STATUS_SUCCESS;
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
    MwScanToFb(dev);            /* the bugcheck screen is drawn in the framebuffer */
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


/* Called at interrupt level for a flip: only record it. The next emulated
 * vsync copies the pixels and reports the flip done (command.c). */
NTSTATUS APIENTRY MwSetVidPnSourceAddress(IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;

    if (Arg->VidPnSourceId != 0)
        return STATUS_INVALID_PARAMETER;
    dev->ScanAddr = Arg->PrimaryAddress;
    dev->ScanAlloc = (MW_ALLOCATION *)Arg->hAllocation;
    dev->Flips++;
    dev->FlipTick = MwTick();
    if (dev->QCount != 0)
        dev->FlipsWhileBusy++;
    {
        LONG seq = InterlockedIncrement(&dev->FlipSeq);
        /* Scanned out: nothing to copy, and the flip is as good as shown. */
        if (MwScanFlip(dev, dev->ScanAlloc)) {
            dev->ShownAddr = Arg->PrimaryAddress;
            dev->ShownSeq = seq;
        }
    }
    if (Arg->hAllocation == NULL) dev->FlipsNullAlloc++;
    if (!dev->SourceVisible) dev->FlipsInvisible++;
    dev->LastFlipFlags = Arg->Flags.Value;
    dev->LastFlipContexts = Arg->ContextCount;
    return STATUS_SUCCESS;
}
