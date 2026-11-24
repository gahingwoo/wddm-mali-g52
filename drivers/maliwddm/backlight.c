// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm: the backlight of a MIPI DSI panel, and Windows' brightness
 * control for it (Settings > System > Display > Brightness, the quick
 * settings slider, the brightness keys).
 *
 * Which mechanism. Windows shows a brightness slider for an internal panel
 * whose brightness it can set, by one of two routes: the display miniport
 * exposes the brightness interface (DxgkDdiQueryInterface with
 * GUID_DEVINTERFACE_BRIGHTNESS_2 or GUID_DEVINTERFACE_BRIGHTNESS) for a child
 * reported as D3DKMDT_VOT_INTERNAL, or ACPI _BCL/_BCM/_BQC on an output
 * device under the adapter whose _ADR the driver reports. This driver takes
 * the first. The ACPI route needs _DOD and output devices under GPU0, which
 * Windows evaluates whatever driver is bound to GPU0 and checks against the
 * driver's child IDs (edk2-rk3576 Gpu.asl keeps the EDID in a private object
 * for that reason), and it would put a per-step ACPI evaluation under a
 * smooth ramp. With the interface, the driver that already owns the panel
 * owns its backlight too, and nothing changes for an HDMI board.
 *
 * How the PWM is reached. The firmware (edk2-rk3576) adds the backlight PWM
 * channel's 4 KiB register block to GPU0's _CRS as a second memory resource,
 * only when the screen it hands over is a MIPI DSI panel whose backlight it
 * lit (Gpu.asl, BLPR). So that resource is the whole signal: with it the
 * child is an internal panel with brightness, without it (CM5-IO, ROCK 4D,
 * the TaishanPi 3M with no screen or with HDMI as its screen) the child is
 * HDMI and QueryInterface refuses the brightness GUIDs, as before. Mapping a
 * resource the PnP manager assigned, not an address the driver knows, keeps
 * the board's choice of channel in the firmware; and MMIO, not an ACPI
 * method with an OperationRegion, lets the ramp below write from a DPC.
 *
 * The channel is a PWM v4 block (RK3576 TRM part 1, 34.6; the register
 * comments of the mainline-bound pwm-rockchip-v4 driver): VERSION at 0x00,
 * ENABLE at 0x04 (bit 0 clock enable, bit 1 enable, bit 2 CTRL_UPDATE_EN,
 * write mask in the top half), PERIOD at 0x10 and DUTY at 0x14, in cycles of
 * the PWM clock. DUTY is a shadow register: a new value takes effect when 1
 * is written to CTRL_UPDATE_EN. Linux changes a running backlight the same
 * way (PERIOD, DUTY, OFFSET, CTRL, then CTRL_UPDATE_EN); here PERIOD, OFFSET
 * and CTRL stay as the firmware set them, so only DUTY and the commit are
 * written. The firmware left the channel running in continuous mode at the
 * brightness chosen in its setup, which is where Windows starts from: the
 * level is read back from DUTY / PERIOD.
 *
 * Smooth steps: a change is spread over BL_RAMP_TICKS timer ticks (about
 * 120 ms) unless Windows turns smoothing off (SetBrightnessState). The
 * driver says it smooths (DXGK_BRIGHTNESS_CAPS.SmoothBrightness), so Windows
 * sends the level it wants, not a staircase of its own.
 *
 * Levels are percent of the period, 10 to 100: the same numbers as the
 * firmware's setting, and no 0, which is a black screen. Windows keeps the
 * level the user chose in its power settings and sets it again at every
 * start; the firmware's setting is only where each start begins.
 */
#include "maliwddm.h"

#define BL_VERSION              0x000
#define BL_ENABLE               0x004
#define BL_PERIOD               0x010
#define BL_DUTY                 0x014
#define BL_VERSION_MAJOR(v)     (((v) >> 24) & 0xFF)
#define BL_ENABLE_RUNNING       0x3u            /* PWM_EN | PWM_CLK_EN */
#define BL_ENABLE_COMMIT        0x00040004u     /* CTRL_UPDATE_EN, with its write mask */
#define BL_MAP_SIZE             0x1000u

#define BL_LEVEL_MIN            10u
#define BL_LEVEL_MAX            100u
#define BL_RAMP_TICKS           8u
#define BL_TICK_100NS           (-150000LL)     /* 15 ms, about one clock tick */

/* GUID_DEVINTERFACE_BRIGHTNESS and _2 (dispmprt.h). Kept here so that this
 * file does not need INITGUID for the WDK's copies. */
static const GUID BlGuid1 = { 0xFDE5BBA4, 0xB3F9, 0x46FB, { 0xBD, 0xAA, 0x07, 0x28, 0xCE, 0x31, 0x00, 0xB4 } };
static const GUID BlGuid2 = { 0x148A3C98, 0x0ECD, 0x465A, { 0xB6, 0x34, 0xB0, 0x5F, 0x19, 0x5F, 0x77, 0x39 } };

static __inline void BlWrite(MW_ADAPTER *A, ULONG Off, ULONG V)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(A->BlVa + Off), V);
}

/* DUTY, committed. The period in progress runs out on the old duty or is
 * cut short on the new one, depending on when the block latches the shadow;
 * either way it is one period of 25 us, not a visible step. */
static void BlSetDuty(MW_ADAPTER *A, ULONG Duty)
{
    if (Duty == A->BlDuty)
        return;
    BlWrite(A, BL_DUTY, Duty);
    BlWrite(A, BL_ENABLE, BL_ENABLE_COMMIT);
    A->BlDuty = Duty;
}

static ULONG BlLevelDuty(MW_ADAPTER *A, ULONG Level)
{
    return (ULONG)(((ULONG64)A->BlPeriod * Level + 50) / 100);
}

static UCHAR BlClamp(ULONG Level)
{
    if (Level < BL_LEVEL_MIN)
        return BL_LEVEL_MIN;
    if (Level > BL_LEVEL_MAX)
        return BL_LEVEL_MAX;
    return (UCHAR)Level;
}

/* One step of a ramp, from the timer's DPC. */
static VOID BlRampDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    KeAcquireSpinLock(&a->BlLock, &irql);
    if (a->BlVa != NULL && a->BlTick < BL_RAMP_TICKS) {
        LONG64 from = a->BlFrom, to = a->BlTarget;
        a->BlTick++;
        BlSetDuty(a, (ULONG)(from + (to - from) * a->BlTick / BL_RAMP_TICKS));
        if (a->BlTick < BL_RAMP_TICKS) {
            LARGE_INTEGER due;
            due.QuadPart = BL_TICK_100NS;
            KeSetTimer(&a->BlTimer, due, &a->BlDpc);
        }
    }
    KeReleaseSpinLock(&a->BlLock, irql);
}

/* Head for Duty: at once, or over a ramp. Called with BlLock held. */
static void BlGoLocked(MW_ADAPTER *A, ULONG Duty, BOOLEAN Smooth)
{
    A->BlTarget = Duty;
    if (!Smooth || Duty == A->BlDuty) {
        KeCancelTimer(&A->BlTimer);
        A->BlTick = BL_RAMP_TICKS;
        BlSetDuty(A, Duty);
        return;
    }
    A->BlFrom = A->BlDuty;
    A->BlTick = 0;
    A->BlRamps++;
    {
        LARGE_INTEGER due;
        due.QuadPart = BL_TICK_100NS;
        KeSetTimer(&A->BlTimer, due, &A->BlDpc);
    }
}

/* ---- the brightness interface (PASSIVE_LEVEL, all of it) ---- */

static VOID BlReference(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static NTSTATUS BlGetPossible(PVOID Context, ULONG BufferSize, PUCHAR LevelCount, PUCHAR Levels)
{
    ULONG step, n = 0;
    UNREFERENCED_PARAMETER(Context);

    /* Every percent if the buffer takes it (dxgkrnl's is 101 or more), else
     * the tens. */
    if (BufferSize >= BL_LEVEL_MAX - BL_LEVEL_MIN + 1)
        step = 1;
    else if (BufferSize >= (BL_LEVEL_MAX - BL_LEVEL_MIN) / 10 + 1)
        step = 10;
    else
        return STATUS_BUFFER_TOO_SMALL;
    for (ULONG l = BL_LEVEL_MIN; l <= BL_LEVEL_MAX; l += step)
        Levels[n++] = (UCHAR)l;
    *LevelCount = (UCHAR)n;
    return STATUS_SUCCESS;
}

static NTSTATUS BlSet(PVOID Context, UCHAR Brightness)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    KIRQL irql;
    UCHAR level = BlClamp(Brightness);

    KeAcquireSpinLock(&a->BlLock, &irql);
    if (a->BlVa == NULL) {
        KeReleaseSpinLock(&a->BlLock, irql);
        return STATUS_DEVICE_NOT_READY;
    }
    a->BlLevel = level;
    a->BlSets++;
    /* Powered down: remembered, shown when the panel is powered up. */
    if (!a->BlOff)
        BlGoLocked(a, BlLevelDuty(a, level), a->BlSmooth);
    KeReleaseSpinLock(&a->BlLock, irql);
    return STATUS_SUCCESS;
}

static NTSTATUS BlGet(PVOID Context, PUCHAR Brightness)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    *Brightness = a->BlLevel;
    return STATUS_SUCCESS;
}

static NTSTATUS BlGetCaps(PVOID Context, DXGK_BRIGHTNESS_CAPS *Caps)
{
    UNREFERENCED_PARAMETER(Context);
    Caps->Value = 0;
    Caps->SmoothBrightness = 1;     /* BlGoLocked ramps */
    return STATUS_SUCCESS;
}

static NTSTATUS BlSetState(PVOID Context, DXGK_BRIGHTNESS_STATE *State)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    a->BlSmooth = State->SmoothBrightness != 0;
    return STATUS_SUCCESS;
}

/* Content-adaptive backlight: not offered (no AdaptiveBrightness cap), so
 * there is nothing to optimise and the effective level is the one set. */
static NTSTATUS BlSetOptimization(PVOID Context, DXGK_BACKLIGHT_OPTIMIZATION_LEVEL Level)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Level);
    return STATUS_SUCCESS;
}

static NTSTATUS BlGetReduction(PVOID Context, DXGK_BACKLIGHT_INFO *Info)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    USHORT v = (USHORT)((ULONG)a->BlLevel * 0xFFFF / 100);

    Info->BacklightUsersetting = v;
    Info->BacklightEffective = v;
    for (ULONG i = 0; i < 256; i++) {
        USHORT g = (USHORT)(i * 0x101);
        Info->GammaRamp.Red[i] = g;
        Info->GammaRamp.Green[i] = g;
        Info->GammaRamp.Blue[i] = g;
    }
    return STATUS_SUCCESS;
}

NTSTATUS MwBacklightQueryInterface(MW_ADAPTER *A, PQUERY_INTERFACE Qi)
{
    if (A->BlVa == NULL || Qi->InterfaceType == NULL || Qi->Interface == NULL)
        return STATUS_NOT_SUPPORTED;

    if (RtlCompareMemory(Qi->InterfaceType, &BlGuid2, sizeof(GUID)) == sizeof(GUID)) {
        DXGK_BRIGHTNESS_INTERFACE_2 *b = (DXGK_BRIGHTNESS_INTERFACE_2 *)Qi->Interface;
        if (Qi->Version < DXGK_BRIGHTNESS_INTERFACE_VERSION_2 || Qi->Size < sizeof(*b))
            return STATUS_NOT_SUPPORTED;
        RtlZeroMemory(b, sizeof(*b));
        b->Size = sizeof(*b);
        b->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_2;
        b->Context = A;
        b->InterfaceReference = BlReference;
        b->InterfaceDereference = BlReference;
        b->GetPossibleBrightness = BlGetPossible;
        b->SetBrightness = BlSet;
        b->GetBrightness = BlGet;
        b->GetBrightnessCaps = BlGetCaps;
        b->SetBrightnessState = BlSetState;
        b->SetBacklightOptimization = BlSetOptimization;
        b->GetBacklightReduction = BlGetReduction;
        MwLog(A, L"BacklightInterface", 2);
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(Qi->InterfaceType, &BlGuid1, sizeof(GUID)) == sizeof(GUID)) {
        DXGK_BRIGHTNESS_INTERFACE *b = (DXGK_BRIGHTNESS_INTERFACE *)Qi->Interface;
        if (Qi->Version < DXGK_BRIGHTNESS_INTERFACE_VERSION_1 || Qi->Size < sizeof(*b))
            return STATUS_NOT_SUPPORTED;
        RtlZeroMemory(b, sizeof(*b));
        b->Size = sizeof(*b);
        b->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
        b->Context = A;
        b->InterfaceReference = BlReference;
        b->InterfaceDereference = BlReference;
        b->GetPossibleBrightness = BlGetPossible;
        b->SetBrightness = BlSet;
        b->GetBrightness = BlGet;
        MwLog(A, L"BacklightInterface", 1);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

/* ---- start, stop, power ---- */

/* The second memory resource of GPU0, if there is one. */
static BOOLEAN BlFindResource(MW_ADAPTER *A, PHYSICAL_ADDRESS *Pa, ULONG *Len)
{
    DXGK_DEVICE_INFO info;
    ULONG seen = 0;

    if (!NT_SUCCESS(A->Dxgk.DxgkCbGetDeviceInformation(A->Dxgk.DeviceHandle, &info)))
        return FALSE;
    PCM_RESOURCE_LIST list = info.TranslatedResourceList;
    for (ULONG l = 0; list && l < list->Count; l++) {
        PCM_PARTIAL_RESOURCE_LIST p = &list->List[l].PartialResourceList;
        for (ULONG i = 0; i < p->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &p->PartialDescriptors[i];
            if (d->Type != CmResourceTypeMemory)
                continue;
            if (++seen == 2) {
                *Pa = d->u.Memory.Start;
                *Len = d->u.Memory.Length;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Whether the firmware handed over a DSI panel (the resource is there),
 * before MwBacklightStart: for the HdmiHpd default (hpd.c). Nothing is
 * mapped or read. PASSIVE_LEVEL. */
BOOLEAN MwBacklightPresent(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    ULONG len = 0;

    return BlFindResource(A, &pa, &len);
}

/* PASSIVE_LEVEL, from StartDevice. Registry trace: Backlight (1 found and
 * running, 0 none: an HDMI output), BacklightVersion, BacklightPeriod,
 * BacklightFirmwareLevel; Fail_Backlight with the reason when a resource was
 * there but the channel was not as the firmware should have left it. */
void MwBacklightStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS pa;
    ULONG len = 0, version, enable, period, duty;
    PUCHAR va;

    KeInitializeSpinLock(&A->BlLock);
    KeInitializeTimer(&A->BlTimer);
    KeInitializeDpc(&A->BlDpc, BlRampDpc, A);
    A->BlVa = NULL;
    A->BlOff = FALSE;
    A->BlSmooth = TRUE;
    A->BlTick = BL_RAMP_TICKS;

    if (!BlFindResource(A, &pa, &len)) {
        MwLog(A, L"Backlight", 0);
        return;
    }
    if (len < 0x18) {
        MwLog(A, L"Fail_Backlight", 1);
        return;
    }
    va = (PUCHAR)MmMapIoSpaceEx(pa, BL_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (va == NULL) {
        MwLog(A, L"Fail_Backlight", 2);
        return;
    }
    version = READ_REGISTER_ULONG((volatile ULONG *)(va + BL_VERSION));
    enable = READ_REGISTER_ULONG((volatile ULONG *)(va + BL_ENABLE));
    period = READ_REGISTER_ULONG((volatile ULONG *)(va + BL_PERIOD));
    duty = READ_REGISTER_ULONG((volatile ULONG *)(va + BL_DUTY));
    MwLog(A, L"BacklightVersion", version);
    MwLog(A, L"BacklightEnable", enable);
    MwLog(A, L"BacklightPeriod", period);
    MwLog(A, L"BacklightDuty", duty);
    /* A PWM v4 channel, running, with a period: anything else and the
     * resource is not what the firmware describes, and nothing is written. */
    if (BL_VERSION_MAJOR(version) != 4 || (enable & BL_ENABLE_RUNNING) != BL_ENABLE_RUNNING ||
        period == 0 || duty > period) {
        MmUnmapIoSpace(va, BL_MAP_SIZE);
        MwLog(A, L"Fail_Backlight", 3);
        return;
    }
    A->BlPeriod = period;
    A->BlDuty = duty;
    A->BlTarget = duty;
    A->BlLevel = BlClamp((ULONG)(((ULONG64)duty * 100 + period / 2) / period));
    A->BlVa = va;
    MwLog(A, L"BacklightFirmwareLevel", A->BlLevel);
    MwLog(A, L"Backlight", 1);
}

/* The ramp stops where it was going; the backlight stays lit at that level
 * for whoever has the screen next (Basic Display, or the next start), which
 * then reads it back as the firmware's. */
void MwBacklightStop(MW_ADAPTER *A)
{
    KIRQL irql;
    PUCHAR va;

    if (A->BlVa == NULL)
        return;
    /* A ramp step that is already running sees BlTick at the end and
     * neither writes nor arms the timer again. */
    KeAcquireSpinLock(&A->BlLock, &irql);
    A->BlTick = BL_RAMP_TICKS;
    KeReleaseSpinLock(&A->BlLock, irql);
    KeCancelTimer(&A->BlTimer);
    KeFlushQueuedDpcs();
    /* Lit again even if Windows had the panel powered down: whoever takes
     * the screen next (Basic Display after a driver update) does not know
     * about the backlight and would show a dark screen. */
    KeAcquireSpinLock(&A->BlLock, &irql);
    BlSetDuty(A, BlLevelDuty(A, A->BlLevel));
    A->BlOff = FALSE;
    va = A->BlVa;
    A->BlVa = NULL;
    KeReleaseSpinLock(&A->BlLock, irql);
    MwLog(A, L"BacklightSets", A->BlSets);
    MwLog(A, L"BacklightRamps", A->BlRamps);
    MwLog(A, L"BacklightLastLevel", A->BlLevel);
    MmUnmapIoSpace(va, BL_MAP_SIZE);
}

/* The panel's power, from SetPowerState on the child: off when Windows
 * turns the display off (idle timeout, the power button set to "turn off
 * the display"), so a blank screen is a dark one; back at the level set
 * when it turns it on again. */
void MwBacklightPower(MW_ADAPTER *A, BOOLEAN On)
{
    KIRQL irql;

    if (A->BlVa == NULL)
        return;
    KeAcquireSpinLock(&A->BlLock, &irql);
    if (A->BlVa != NULL && A->BlOff == On) {
        A->BlOff = !On;
        BlGoLocked(A, On ? BlLevelDuty(A, A->BlLevel) : 0, On && A->BlSmooth);
    }
    KeReleaseSpinLock(&A->BlLock, irql);
}
