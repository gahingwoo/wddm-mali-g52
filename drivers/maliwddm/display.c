// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm's display: malidod's, which drives the HDMI output under
 * Windows (M5.1). One HDMI child (an internal panel instead when the
 * firmware hands over a MIPI DSI panel's backlight, see backlight.c) with
 * the monitor's EDID if the firmware left it in the DSDT, the firmware's
 * framebuffer and timing, and as source modes the firmware's size and
 * (ScaledModes, on by default) smaller ones that the VOP2 scales up to it
 * (see "source modes" below), rotation by 0, 90, 180 or 270 degrees (see
 * "rotation" below). Flips land in MwSetVidPnSourceAddress, which records them;
 * scanout.c or the flip copy in command.c puts them on the screen.
 */
#include "maliwddm.h"
#include "wddm2.h"
#include <acpiioct.h>

static UINT BytesPerPixel(D3DDDIFORMAT F)
{
    return (F == D3DDDIFMT_A8R8G8B8 || F == D3DDDIFMT_X8R8G8B8) ? 4 : 0;
}

/* ---- rotation ----
 *
 * Offered on the path: identity, 90, 180 and 270 degrees (the Basic
 * Display Driver sample offers identity and 90 the same way:
 * Windows-driver-samples, video/KMDOD/bdd_dmm.cxx). Windows picks
 * one in Settings > Display > Display orientation; identity stays the
 * default, so an HDMI monitor and the panel come up as they did.
 *
 * The source mode is the panel's size at every rotation and dxgkrnl
 * creates the shared primary with those non-rotated dimensions ("Supporting
 * Rotation in a Display Miniport Driver", learn.microsoft.com). DWM draws
 * the desktop rotated into it ("the Desktop Window Manager (DWM) rotates
 * its content in advance", same page, primary clone path), so its flips
 * scan out or are copied as before, at no cost. The driver rotates only
 * where Windows gives it a picture that is not rotated: a present with the
 * Rotate flag (command.c), a primary of the rotated size (MwFlipDegrees)
 * and the bugcheck screen (below). */

/* Degrees for a path's rotation; ~0u for one that is not set or not ours. */
static UINT PathDegrees(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION R)
{
    switch (R) {
    case D3DKMDT_VPPR_IDENTITY:  return 0;
    case D3DKMDT_VPPR_ROTATE90:  return 90;
    case D3DKMDT_VPPR_ROTATE180: return 180;
    case D3DKMDT_VPPR_ROTATE270: return 270;
    default:                     return ~0u;
    }
}

/* The rotation of the path from our source in a VidPN, ~0u if there is no
 * such path or it has none. */
static UINT VidPnDegrees(const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                         D3DDDI_VIDEO_PRESENT_SOURCE_ID Src)
{
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    NTSTATUS st;

    if (!NT_SUCCESS(VidPn->pfnGetTopology(H, &topo, &topoIf)))
        return ~0u;
    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        if (path->VidPnSourceId == Src) {
            UINT deg = PathDegrees(path->ContentTransformation.Rotation);
            topoIf->pfnReleasePathInfo(topo, path);
            return deg;
        }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return ~0u;
}

/* The scaling of the path from our source in a VidPN, as scale.h's
 * MW_SCL_*: ~0u if there is no such path, MW_SCL_DEFAULT_ if its scaling is
 * not pinned or not one the window does (then the mode's own default). */
#define MW_SCL_DEFAULT_ 0xFFu
static UINT SclFromPath(D3DKMDT_VIDPN_PRESENT_PATH_SCALING S)
{
    switch (S) {
    case D3DKMDT_VPPS_IDENTITY:               return MW_SCL_IDENTITY;
    case D3DKMDT_VPPS_CENTERED:               return MW_SCL_CENTERED;
    case D3DKMDT_VPPS_STRETCHED:              return MW_SCL_STRETCHED;
    case D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX: return MW_SCL_ASPECT;
    default:                                  return MW_SCL_DEFAULT_;
    }
}

static UINT VidPnScaling(const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                         D3DDDI_VIDEO_PRESENT_SOURCE_ID Src)
{
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    NTSTATUS st;

    if (!NT_SUCCESS(VidPn->pfnGetTopology(H, &topo, &topoIf)))
        return ~0u;
    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        if (path->VidPnSourceId == Src) {
            UINT how = SclFromPath(path->ContentTransformation.Scaling);
            topoIf->pfnReleasePathInfo(topo, path);
            return how;
        }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return ~0u;
}

static void SetRotation(MW_ADAPTER *A, UINT Deg, PCWSTR Why)
{
    if (Deg == ~0u)
        return;
    if (Deg != A->RotDeg) {
        InterlockedExchange((volatile LONG *)&A->RotDeg, (LONG)Deg);
        MwLog(A, Why, Deg);
    }
}

/* For hpd.c, which has its own VidPN at HdmiHpd level 3. */
UINT MwPathDegrees(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION R)
{
    return PathDegrees(R);
}

void MwSetRotation(MW_ADAPTER *A, UINT Deg, PCWSTR Why)
{
    SetRotation(A, Deg, Why);
}

/* ---- source modes ----
 *
 * The firmware's mode is the only timing there is at HdmiHpd 0 to 2: the
 * HDMI link (or the panel) is never retimed from here, and the target has
 * that one mode. The source modes are the firmware's size and, with
 * ScaledModes (default 1), smaller ones that the VOP2's Esmart0 window
 * scales up to it (scale.h has the list: the sizes the EDID names and the
 * usual desktop sizes, no more than 8 times smaller; scanout.c's
 * MwScanScale sets the window). A smaller mode of the screen's shape is
 * offered stretched, one of another shape with its aspect ratio kept and
 * black bars (D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX). Settings > Display >
 * Display resolution lists them; "Recommended" is the firmware's.
 *
 * They need scan-out set up (the window's registers are mapped there) and
 * are not offered at HdmiHpd 3, where hpd.c has its own VidPN with real
 * HDMI modes. docs/DISPLAY-MODES.md has the rest. */

/* Mode Index of the source modes: 0 is the firmware's. FALSE past the end. */
static BOOLEAN SourceModeSize(MW_ADAPTER *Dev, ULONG Index, UINT *W, UINT *H)
{
    if (Index == 0) {
        *W = Dev->Fb.Width;
        *H = Dev->Fb.Height;
        return TRUE;
    }
    if (!Dev->ScaledModes || Index >= Dev->ModeCount)
        return FALSE;
    *W = Dev->Modes[Index].W;
    *H = Dev->Modes[Index].H;
    return TRUE;
}

static BOOLEAN IsSourceModeSize(MW_ADAPTER *Dev, UINT W, UINT H)
{
    UINT w, h;

    for (ULONG i = 0; SourceModeSize(Dev, i, &w, &h); i++)
        if (w == W && h == H)
            return TRUE;
    return FALSE;
}

static BOOLEAN IsNative(MW_ADAPTER *Dev, UINT W, UINT H)
{
    return W == Dev->Fb.Width && H == Dev->Fb.Height;
}

static void FillSourceMode(MW_ADAPTER *Dev, D3DKMDT_VIDPN_SOURCE_MODE *M, UINT W, UINT H)
{
    M->Type = D3DKMDT_RMT_GRAPHICS;
    M->Format.Graphics.PrimSurfSize.cx = W;
    M->Format.Graphics.PrimSurfSize.cy = H;
    M->Format.Graphics.VisibleRegionSize = M->Format.Graphics.PrimSurfSize;
    M->Format.Graphics.Stride = IsNative(Dev, W, H) ? Dev->Fb.Pitch : W * 4;
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
    MwBc("DxgkCbAcquirePostDisplayOwnership 0x%08x: %ux%u pitch %u format %u", (ULONG)st,
         A->Fb.Width, A->Fb.Height, A->Fb.Pitch, (ULONG)A->Fb.ColorFormat);
    if (!NT_SUCCESS(st) || A->Fb.Width == 0 || BytesPerPixel(A->Fb.ColorFormat) == 0)
        return STATUS_UNSUCCESSFUL;
    MwLog(A, L"FbWidth", A->Fb.Width);
    MwLog(A, L"FbHeight", A->Fb.Height);
    A->SrcWidth = A->Fb.Width;
    A->SrcHeight = A->Fb.Height;
    A->RotDeg = 0;
    A->RotateFlips = MwSetting(A, L"RotateFlips", 0) != 0;
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
    /* HDMI hot-plug (hpd.c): the setting, and at level 3 the framebuffer for
     * the driver's own mode sets, which MwScanStart enters in the IOMMU. At
     * HdmiHpd 0 this reads the setting and does nothing else. */
    MwHpdSettings(A);
    if (A->Hdmi.Level >= 3)
        (void)MwHpdReserve(A);
    MwScanStart(A);
    /* Only with scan-out set up and not at HdmiHpd 3 (Hdmi.Switch is decided
     * later, in MwHpdStart; the level is known now): see "source modes". */
    A->Scaled = FALSE;
    A->ScrW = A->Fb.Width;
    A->ScrH = A->Fb.Height;
    A->ScrHow = MW_SCL_IDENTITY;
    A->ScaledModes = MwSetting(A, L"ScaledModes", 1) != 0 && A->ScanReady && A->Hdmi.Level < 3;
    A->ModeCount = MwSclModes(A->Fb.Width, A->Fb.Height, A->EdidSize ? A->Edid : NULL, A->EdidSize,
                              A->ScaledModes, A->Modes, MW_SCL_MAX_MODES);
    MwLog(A, L"ScaledModes", A->ScaledModes);
    MwLog(A, L"SourceModes", A->ModeCount);
    if (A->ModeCount > 1) {
        MwLog(A, L"SourceMode1", (ULONG)A->Modes[1].W << 16 | A->Modes[1].H);
        MwLog(A, L"SourceModeLast", (ULONG)A->Modes[A->ModeCount - 1].W << 16 | A->Modes[A->ModeCount - 1].H);
    }
    return STATUS_SUCCESS;
}

void MwDisplayStop(MW_ADAPTER *A)
{
    MwScanStop(A);
    if (A->Hdmi.Level >= 3) {
        MwHpdDisplayStop(A);
        return;
    }
    if (A->FbVa != NULL) {
        MmUnmapIoSpace(A->FbVa, A->FbSize);
        A->FbVa = NULL;
    }
}


/* Hand the framebuffer back, e.g. to Basic Display when we are disabled.
 * dxgkrnl calls this instead of StopDevice, also when its own part of an
 * adapter start fails, so it stops everything StopDevice does
 * (MwAdapterStop, adapter.c), not only the display. */
NTSTATUS APIENTRY MwStopDeviceAndReleasePostDisplayOwnership(IN_CONST_PVOID MiniportDeviceContext,
                                                   IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(TargetId);
    *DisplayInfo = dev->Fb;
    MwAdapterStop(dev);
    return STATUS_SUCCESS;
}


/* ---- the monitor: one output, always connected, its EDID from the
 * firmware when the firmware has one. HDMI, or an internal panel when the
 * firmware handed over a MIPI DSI panel's backlight (backlight.c): Windows
 * offers brightness control only for an internal output, and asks the
 * driver for the brightness interface only then. ---- */

NTSTATUS APIENTRY MwQueryChildRelations(IN_CONST_PVOID MiniportDeviceContext,
                               PDXGK_CHILD_DESCRIPTOR ChildRelations, ULONG ChildRelationsSize)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
#if MW_RENDER_ONLY
    UNREFERENCED_PARAMETER(dev);
    if (ChildRelationsSize < sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(ChildRelations, sizeof(DXGK_CHILD_DESCRIPTOR)); /* the terminator alone */
    return STATUS_SUCCESS;
#else
    if (dev->Hdmi.Expose)
        return MwHpdQueryChildRelations(dev, ChildRelations, ChildRelationsSize);
    if (ChildRelationsSize < 2 * sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;   /* one child plus the zeroed terminator */
    RtlZeroMemory(ChildRelations, sizeof(DXGK_CHILD_DESCRIPTOR));
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
        dev->BlVa != NULL ? D3DKMDT_VOT_INTERNAL : D3DKMDT_VOT_HDMI;
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
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    if (dev != NULL && dev->Hdmi.Expose)
        return MwHpdQueryChildStatus(dev, ChildStatus);
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

    if (dev->Hdmi.Expose)
        InterlockedIncrement(&dev->Hdmi.DescCalls);
    if (dev->Hdmi.Expose && ChildUid == dev->Hdmi.HdmiChild)
        return MwHpdQueryDeviceDescriptor(dev, ChildUid, DeviceDescriptor);
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

    if (dev->Hdmi.Switch)
        return MwHpdIsSupportedVidPn(dev, Arg);
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
    /* Supported if nothing is pinned yet, or what is pinned is one of ours.
     * A smaller source mode cannot have identity scaling. */
    if (NT_SUCCESS(st)) {
        UINT w = 0, h = 0;
        if (pinned != NULL && pinned->Type == D3DKMDT_RMT_GRAPHICS) {
            w = pinned->Format.Graphics.PrimSurfSize.cx;
            h = pinned->Format.Graphics.PrimSurfSize.cy;
        }
        Arg->IsVidPnSupported = pinned == NULL || IsSourceModeSize(dev, w, h);
        if (Arg->IsVidPnSupported && pinned != NULL && !IsNative(dev, w, h) &&
            VidPnScaling(vidpn, Arg->hDesiredVidPn, 0) == MW_SCL_IDENTITY)
            Arg->IsVidPnSupported = FALSE;
        if (pinned != NULL)
            setIf->pfnReleaseModeInfo(set, pinned);
    }
    vidpn->pfnReleaseSourceModeSet(Arg->hDesiredVidPn, set);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRecommendFunctionalVidPn(IN_CONST_HANDLE hAdapter,
                                             const DXGKARG_RECOMMENDFUNCTIONALVIDPN *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    UNREFERENCED_PARAMETER(Arg);
    if (dev != NULL && dev->Hdmi.Switch)
        InterlockedIncrement(&dev->Hdmi.RecFunc);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

/* Give the source and target of a path our modes if nothing is pinned:
 * the firmware's, and with ScaledModes the smaller ones (SourceModeSize),
 * unless the path's scaling is already pinned to identity (Identity), which
 * only the firmware's size can have. The target has only the firmware's,
 * whatever the source. */
static NTSTATUS AddSourceMode(MW_ADAPTER *Dev, const DXGK_VIDPN_INTERFACE *VidPn, D3DKMDT_HVIDPN H,
                              D3DDDI_VIDEO_PRESENT_SOURCE_ID Src, BOOLEAN Identity)
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
    for (ULONG i = 0; NT_SUCCESS(st) && (i == 0 || !Identity) && SourceModeSize(Dev, i, &w, &h); i++) {
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

    if (dev->Hdmi.Switch)
        return MwHpdEnumVidPnCofuncModality(dev, Arg);
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
            r = AddSourceMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnSourceId,
                              path->ContentTransformation.Scaling == D3DKMDT_VPPS_IDENTITY);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddSourceMode", (ULONG)r);
        }
        if (NT_SUCCESS(r) && !tgtPivot) {
            r = AddTargetMode(dev, vidpn, Arg->hConstrainingVidPn, path->VidPnTargetId);
            if (!NT_SUCCESS(r))
                MwLog(dev, L"Fail_AddTargetMode", (ULONG)r);
        }

        /* Rotation by 0, 90, 180 or 270 degrees (see the top of this file).
         * Identity scaling for the firmware's mode; a smaller source mode
         * of its shape stretched to it, one of another shape with its
         * aspect ratio kept (MwSclDefault). Before a source mode is pinned
         * all three are possible. */
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
                if (!dev->ScaledModes || dev->ModeCount < 2 || (pinned && IsNative(dev, w, h))) {
                    upd.ContentTransformation.ScalingSupport.Identity = 1;
                } else if (pinned) {
                    if (MwSclDefault(w, h, dev->Fb.Width, dev->Fb.Height) == MW_SCL_STRETCHED)
                        upd.ContentTransformation.ScalingSupport.Stretched = 1;
                    else
                        upd.ContentTransformation.ScalingSupport.AspectRatioCenteredMax = 1;
                } else {
                    upd.ContentTransformation.ScalingSupport.Identity = 1;
                    upd.ContentTransformation.ScalingSupport.Stretched = 1;
                    upd.ContentTransformation.ScalingSupport.AspectRatioCenteredMax = 1;
                }
            }
            if (upd.ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.RotationSupport,
                              sizeof(upd.ContentTransformation.RotationSupport));
                upd.ContentTransformation.RotationSupport.Identity = 1;
                upd.ContentTransformation.RotationSupport.Rotate90 = 1;
                upd.ContentTransformation.RotationSupport.Rotate180 = 1;
                upd.ContentTransformation.RotationSupport.Rotate270 = 1;
                /* No Offset bits at 1.3: they belong to path-independent
                 * rotation, an interface (WDDM1_3_PATH_INDEPENDENT_ROTATION)
                 * after the WDDM1_3 this driver registers there. At 2.x the
                 * interface is past it and dxgkrnl requires Offset0
                 * (wddm2x.c). */
                Mw2xRotationSupport(&upd.ContentTransformation.RotationSupport);
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
    if (dev->Hdmi.Switch) {
        InterlockedIncrement(Arg->Visible ? &dev->Hdmi.VisOn : &dev->Hdmi.VisOff);
        if (!Arg->Visible) {
            LONG n = InterlockedIncrement(&dev->Hdmi.VisOffN);
            if (n <= 4)
                dev->Hdmi.VisOffMs[n - 1] = (ULONG)(KeQueryInterruptTime() / 10000);
            MwHpdBlackEvent(dev, 1);
        }
    }
    /* Invisible means blank: clear the framebuffer once, and show it if
     * the window was on a primary. HdmiDiag 2 keeps its pattern. */
    if (!Arg->Visible && dev->FbVa != NULL && !dev->Hdmi.Hold && MwHpdFbEnter(dev)) {
        RtlZeroMemory(dev->FbVa, dev->FbSize);
        dev->Hdmi.ToFbFrom = 1;
        MwScanToFb(dev);
        MwHpdFbLeave(dev);
    }
    /* Visible again: the current primary back on the screen if it is
     * scanned out (flips made meanwhile were not). Under CopyLock, which
     * keeps the primary from being freed under it. PASSIVE_LEVEL. */
    if (Arg->Visible) {
        ExAcquireFastMutex(&dev->CopyLock);
        if (MwScanUsable(dev, dev->ScanAlloc))
            MwScanFlip(dev, dev->ScanAlloc);
        else if (dev->Hdmi.Switch && dev->ScanAlloc != NULL)
            /* Not scanned out: copy it again at the next vsync, or the
             * framebuffer cleared above stays black until the next flip. */
            InterlockedExchange(&dev->ShownSeq, dev->FlipSeq - 1);
        ExReleaseFastMutex(&dev->CopyLock);
    }
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

    if (dev->Hdmi.Switch)
        return MwHpdCommitVidPn(dev, Arg);
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
        /* What primaries will now be, and the window that scales them up
         * to the firmware's size (scanout.c MwScanScale): set now, onto the
         * cleared framebuffer, so the copy and the next flip both land in
         * it. The scaling is the path's, or the mode's own default when it
         * is not one the window does. */
        if (ours) {
            UINT how = IsNative(dev, w, h) ? MW_SCL_IDENTITY : VidPnScaling(vidpn, Arg->hFunctionalVidPn, 0);
            if (how == ~0u || how == MW_SCL_DEFAULT_ || (how == MW_SCL_IDENTITY && !IsNative(dev, w, h)))
                how = MwSclDefault(w, h, dev->Fb.Width, dev->Fb.Height);
            if (!MwScanScale(dev, w, h, how)) {
                ours = FALSE;
                st = STATUS_GRAPHICS_INVALID_VIDPN;
                dev->PathActive = FALSE;
                MwLog(dev, L"Fail_CommitScale", w << 16 | (h & 0xFFFF));
            }
        }
        if (ours && (w != dev->SrcWidth || h != dev->SrcHeight)) {
            dev->SrcWidth = w;
            dev->SrcHeight = h;
            MwLog(dev, L"CommitSourceWidth", w);
            MwLog(dev, L"CommitSourceHeight", h);
        }
        /* The path's rotation: the driver must always take it here, and
         * at UpdateActiveVidPnPresentPath for a smooth rotation (WDDM 1.2,
         * "Optimized screen rotation support"). */
        if (ours)
            SetRotation(dev, VidPnDegrees(vidpn, Arg->hFunctionalVidPn, 0), L"CommitRotation");
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

/* A smooth rotation (SupportSmoothRotation, adapter.c) comes here, not
 * through a commit: the mode stays and only the path's rotation changes. A
 * rotation that is not one of ours is refused; anything else on the path
 * (scaling, gamma) is accepted as before. */
NTSTATUS APIENTRY MwUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter,
                                                 const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r = Arg->VidPnPresentPathInfo.ContentTransformation.Rotation;
    UINT deg = PathDegrees(r);

    if (Arg->VidPnPresentPathInfo.VidPnSourceId != 0)
        return STATUS_SUCCESS;
    if (deg == ~0u && r != D3DKMDT_VPPR_UNINITIALIZED && r != D3DKMDT_VPPR_NOTSPECIFIED &&
        r != D3DKMDT_VPPR_UNPINNED) {
        MwLog(dev, L"Fail_UpdatePathRotation", (ULONG)r);
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    /* The scaling of a smaller source mode can change here too. */
    if (!dev->Hdmi.Switch && !IsNative(dev, dev->SrcWidth, dev->SrcHeight)) {
        UINT how = SclFromPath(Arg->VidPnPresentPathInfo.ContentTransformation.Scaling);
        if (how == MW_SCL_IDENTITY) {
            MwLog(dev, L"Fail_UpdatePathScaling", how);
            return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
        }
        if (how != MW_SCL_DEFAULT_ && !MwScanScale(dev, dev->SrcWidth, dev->SrcHeight, how))
            return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    SetRotation(dev, deg, L"UpdatePathRotation");
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY MwRecommendMonitorModes(IN_CONST_HANDLE hAdapter,
                                          const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE *mode;
    NTSTATUS st;

    if (dev->Hdmi.Switch)
        return MwHpdRecommendMonitorModes(dev, Arg);
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

/* A rotated path is rotated by DWM on the GPU or by this driver on the CPU,
 * never by the display controller: DriverRotation says so, as the Basic
 * Display Driver sample does (bdd.cxx). An unrotated path reports what it
 * always did. */
NTSTATUS APIENTRY MwQueryVidPnHWCapability(IN_CONST_HANDLE hAdapter,
                                           DXGKARG_QUERYVIDPNHWCAPABILITY *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    UINT deg = ~0u;

    RtlZeroMemory(&Arg->VidPnHWCaps, sizeof(Arg->VidPnHWCaps));
    if (Arg->hFunctionalVidPn != 0 &&
        NT_SUCCESS(dev->Dxgk.DxgkCbQueryVidPnInterface(Arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn)))
        deg = VidPnDegrees(vidpn, Arg->hFunctionalVidPn, Arg->SourceId);
    if (deg != ~0u && deg != 0)
        Arg->VidPnHWCaps.DriverRotation = 1;
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
    dev->Hdmi.ToFbFrom = 7;
    MwScanToFb(dev);            /* the bugcheck screen is drawn in the framebuffer */
    /* Rotated, the screen is the size Windows sees (bdd.cxx does the same),
     * and SystemDisplayWrite turns it. A smaller source mode: its size, in
     * the top left of the framebuffer, which the window scales up as it
     * did the desktop (no register is written here beyond MwScanToFb's). */
    MwRotLogicalSize(dev->RotDeg, MwScrW(dev), MwScrH(dev), Width, Height);
    *ColorFormat = dev->Fb.ColorFormat;
    return STATUS_SUCCESS;
}

VOID APIENTRY MwSystemDisplayWrite(IN_CONST_PVOID MiniportDeviceContext, IN_CONST_PVOID Source,
                          UINT SourceWidth, UINT SourceHeight, UINT SourceStride,
                          UINT PositionX, UINT PositionY)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)MiniportDeviceContext;
    UINT deg = dev->RotDeg, sw = MwScrW(dev), sh = MwScrH(dev);
    if (dev->FbVa == NULL)
        return;
    if (deg != 0) {
        UINT lw, lh;
        MW_ROT_RECT l;
        MwRotLogicalSize(deg, sw, sh, &lw, &lh);
        if (PositionX >= lw || PositionY >= lh)
            return;
        l.Left = (long)PositionX;
        l.Top = (long)PositionY;
        l.Right = (long)min(PositionX + SourceWidth, lw);
        l.Bottom = (long)min(PositionY + SourceHeight, lh);
        MwRotCopy(deg, dev->FbVa, (long)dev->Fb.Pitch, sw, sh,
                  (const unsigned char *)Source, (long)SourceStride, -(long)PositionX, -(long)PositionY, &l);
        return;
    }
    for (UINT y = 0; y < SourceHeight && PositionY + y < sh; y++) {
        UINT w = min(SourceWidth, sw > PositionX ? sw - PositionX : 0);
        RtlCopyMemory(dev->FbVa + (SIZE_T)(PositionY + y) * dev->Fb.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR *)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
    }
}


/* Called at interrupt level for a flip. Scanned out, it is two register
 * writes here and nothing else (scanout.c); otherwise it is only recorded,
 * and the copy thread copies the pixels before the next emulated vsync
 * reports the flip done (command.c). Which way it went, and why, is
 * counted for the stats escape. */
NTSTATUS APIENTRY MwSetVidPnSourceAddress(IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *Arg)
{
    MW_ADAPTER *dev = (MW_ADAPTER *)hAdapter;
    MW_ALLOCATION *p = (MW_ALLOCATION *)Arg->hAllocation;
    ULONG64 t0 = MwTick();

    if (Arg->VidPnSourceId != 0)
        return STATUS_INVALID_PARAMETER;
    /* The gap since the previous flip: with FlipIntervals, the flip rate. */
    if (dev->FlipTick != 0) {
        ULONG64 gap = t0 - dev->FlipTick;
        dev->FlipIntervalTicks += gap;
        if (gap > dev->FlipIntervalMaxTicks)
            dev->FlipIntervalMaxTicks = gap;
        dev->FlipIntervals++;
    }
    dev->ScanAddr = Arg->PrimaryAddress;
    dev->ScanAlloc = p;
    dev->Flips++;
    dev->FlipTick = t0;
    if (dev->QCount != 0)
        dev->FlipsWhileBusy++;
    {
        LONG seq = InterlockedIncrement(&dev->FlipSeq);
        /* Scanned out: nothing to copy, and the flip is as good as shown.
         * Not while the source is invisible: the screen is to stay blank
         * (MwSetVidPnSourceVisibility puts the flip on it again). */
        ULONG why = dev->SourceVisible ? MwScanWhy(dev, p) : 0;
        if (!dev->SourceVisible) {
            dev->WhyInvisible++;
        } else if (why == 0 && MwScanFlip(dev, p)) {
            dev->ShownAddr = Arg->PrimaryAddress;
            dev->ShownFlipTick = t0;
            dev->ShownSeq = seq;
            dev->FlipsScanned++;
            dev->LastPath = 1;
            dev->CopyBytesAvoided += (ULONG64)(p->Pitch ? p->Pitch : p->Width * 4) * p->Height;
        } else if (why == MW_SCAN_WHY_OFF) {
            dev->WhyScanOff++;
        } else if (why == MW_SCAN_WHY_ROTATED) {
            dev->WhyRotated++;
        } else {
            dev->WhyNotMapped++;
        }
    }
    if (dev->Hdmi.Switch) {
        dev->Hdmi.LastFlipWH = p != NULL ? (LONG)(p->Width << 16 | (p->Height & 0xFFFF)) : 0;
        dev->Hdmi.LastFlipWhy = (LONG)(dev->SourceVisible ? MwScanWhy(dev, p) : 9);
    }
    if (Arg->hAllocation == NULL) dev->FlipsNullAlloc++;
    if (!dev->SourceVisible) dev->FlipsInvisible++;
    dev->LastFlipFlags = Arg->Flags.Value;
    dev->LastFlipContexts = Arg->ContextCount;
    MwTickAdd(&dev->FlipCallTicks, &dev->FlipCallMaxTicks, t0);
    return STATUS_SUCCESS;
}
