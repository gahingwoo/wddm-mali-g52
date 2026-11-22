// SPDX-License-Identifier: GPL-2.0
/*
 * HDMI hot-plug as Windows sees it: a thread that polls HPD and reads the
 * EDID when a monitor appears, the children and their status, and (at
 * level 3) the VidPN of an output whose mode the driver sets itself.
 * hdmi.c does the hardware. The HdmiHpd setting (maliwddm.h) decides what
 * runs; at 0 none of this does, and display.c's DDIs are what they were.
 *
 * The children. An HDMI boot has one, the HDMI output, as before; from
 * level 2 it is hot-plug aware (HpdAwarenessInterruptible: the driver says
 * when it changes, DxgkCbIndicateChildStatus). A boot on the TaishanPi 3M's
 * DSI panel has, at level 3, a second child: child 0 is the panel as
 * before, child 1 the HDMI output. There is one VidPN source, so Windows
 * shows one of them at a time ("PC screen only" / "Second screen only",
 * Win+P); a clone or extend topology is refused in IsSupportedVidPn.
 *
 * Mode sets. VP0 is the only video port used, by the panel and by HDMI
 * alike, as in the firmware. Committing the HDMI target with a timing that
 * is not what VP0 drives runs hdmi.c's mode set and moves the scan-out to a
 * framebuffer of the driver's own (MwHpdReserve) of the new size; committing
 * the panel again puts VP0 back on MIPI0 at the panel's timing and the
 * firmware's framebuffer. Flips are held off while the VOP2 is written
 * (Hdmi.Busy, scanout.c).
 *
 * HPD is polled, every HdmiHpdPollMs (200 by default), and a level is acted
 * on only once it has held for HdmiHpdDebounceMs (1000 by default). Linux
 * on this board (Armbian 7.3.5, mainline dw-hdmi-qp, drm.debug) logged a
 * replug as connected, disconnected 464 ms later, connected again 200 ms
 * after that: 0.66 s of contact bounce. Mainline debounces 150 ms after each
 * HPD interrupt (HOTPLUG_DEBOUNCE_MS) and only then reads the level; a poll
 * cannot see every edge, so it waits longer. Unplugging is believed only after HPD has
 * been seen high once since the driver started: a sink without HPD that the
 * firmware lit anyway (its 5 s timeout, DwHdmiQpConnectorPreInit) is never
 * reported gone.
 *
 * Every stage leaves a value in the registry trace (Device Parameters,
 * maliwddm), flushed as it is written: Hpd_*, Edid_*, Mset_*, Dsi_*, Aud_*,
 * Scan_*. docs/HDMI-HPD.md says what each means.
 */
#include "maliwddm.h"

/* The TaishanPi 3M's panel, as Tspi3mDsiPanel.c's NativeMode has it:
 * 800x1280, 71 MHz, HFP 52 HSYNC 8 HBP 48, VFP 15 VSYNC 6 VBP 16, both syncs
 * negative. Only what VP0 is programmed with on the way back from HDMI;
 * Windows keeps seeing the panel's mode as display.c reports it. */
static void TspiPanel(MW_TIMING *T)
{
    RtlZeroMemory(T, sizeof(*T));
    T->Clock = 71000;
    T->HDisplay = 800;
    T->HSyncStart = 852;
    T->HSyncEnd = 860;
    T->HTotal = 908;
    T->VDisplay = 1280;
    T->VSyncStart = 1295;
    T->VSyncEnd = 1301;
    T->VTotal = 1317;
}

static void Lock(PKMUTEX M)
{
    KeWaitForSingleObject(M, Executive, KernelMode, FALSE, NULL);
}

static void Unlock(PKMUTEX M)
{
    KeReleaseMutex(M, FALSE);
}

/* ---- settings and the reserved framebuffer, from MwDisplayStart ---- */

void MwHpdSettings(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    H->Level = MwSetting(A, L"HdmiHpd", 0);
    if (H->Level == 0)
        return;
    if (H->Level > 3)
        H->Level = 3;
    H->Source = MwSetting(A, L"HdmiHpdSource", 0);
    H->PollMs = MwSetting(A, L"HdmiHpdPollMs", 200);
    if (H->PollMs < 50)
        H->PollMs = 50;
    if (H->PollMs > 2000)
        H->PollMs = 2000;
    H->DebounceMs = MwSetting(A, L"HdmiHpdDebounceMs", 1000);
    if (H->DebounceMs < 300)
        H->DebounceMs = 300;
    if (H->DebounceMs > 10000)
        H->DebounceMs = 10000;
    H->Audio = MwSetting(A, L"HdmiAudio", 1) != 0;
    H->Diag = MwSetting(A, L"HdmiDiag", 0);
    /* On by default since the board run of c435f07: with one VidPN source
     * and both displays connected the session keeps the HDMI desktop black;
     * reported one at a time it works (docs/HDMI-HPD.md). 0 to try the
     * two-display arrangement anyway. */
    H->Exclusive = MwSetting(A, L"HdmiExclusive", 1) != 0;
    H->LastSup = H->LastEnum = 0xFFFFFFFFu;
    H->ResMaxPixels = MwSetting(A, L"HdmiMaxPixels", 2560 * 1600);
    if (H->ResMaxPixels > MW_VP0_MAX_W * MW_VP0_MAX_H)
        H->ResMaxPixels = MW_VP0_MAX_W * MW_VP0_MAX_H;
    if (H->ResMaxPixels < 1280 * 720)
        H->ResMaxPixels = 1280 * 720;
    KeInitializeMutex(&H->HwLock, 0);
    KeInitializeMutex(&H->StateLock, 0);
    KeInitializeEvent(&H->Wake, SynchronizationEvent, FALSE);
    MwLog(A, L"Hpd_Level", H->Level);
    MwLog(A, L"Hpd_Source", H->Source);
    MwLog(A, L"Hpd_Diag", H->Diag);
    MwLog(A, L"Hpd_Exclusive", H->Exclusive);
}

/* Level 3: the framebuffer of the driver's own mode sets, allocated now,
 * while memory is not yet fragmented, and entered in the VOP2 IOMMU by
 * MwScanStart at its own address as the firmware's is. Trace: Hpd_Reserve
 * (its size, 0 if it could not be had), Hpd_ReservePa. */
NTSTATUS MwHpdReserve(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    PHYSICAL_ADDRESS lo, hi, none;
    SIZE_T n = (SIZE_T)H->ResMaxPixels * 4;

    H->FwFb = A->Fb;
    H->FwFbVa = A->FbVa;
    H->FwFbSize = A->FbSize;
    n = (n + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    lo.QuadPart = 0x40000000ll;                 /* DRAM starts here; the IOMMU's IO addresses are below */
    hi.QuadPart = 0xFFFFFFFFll;
    none.QuadPart = 0;
    H->ResVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(n, lo, hi, none, MmWriteCombined);
    if (H->ResVa != NULL) {
        H->ResPa = MmGetPhysicalAddress(H->ResVa);
        H->ResBytes = n;
        RtlZeroMemory(H->ResVa, n);
    }
    MwLog(A, L"Hpd_Reserve", (ULONG)H->ResBytes);
    MwLog(A, L"Hpd_ReservePa", H->ResPa.LowPart);
    return H->ResVa != NULL ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

/* ---- the EDID and the modes it gives ---- */

static void LogEdid(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    const UCHAR *e = H->Edid;

    MwLog(A, L"Edid_Bytes", H->EdidSize);
    MwLog(A, L"Edid_Id", H->EdidSize ? ((ULONG)e[8] << 24 | (ULONG)e[9] << 16 | (ULONG)e[10] << 8 | e[11]) : 0);
    MwLog(A, L"Edid_Sink", H->Sink.IsHdmi | H->Sink.Scdc << 1 | H->Sink.BasicAudio << 2 | H->Sink.RgbQs << 3);
    MwLog(A, L"Edid_Modes", H->ModeCount);
    if (H->ModeCount != 0) {
        MwLog(A, L"Edid_Mode0", (ULONG)H->Modes[0].HDisplay << 16 | H->Modes[0].VDisplay);
        MwLog(A, L"Edid_Mode0Clock", H->Modes[0].Clock | (H->Modes[0].Fw ? 0x80000000u : 0));
    }
}

/* A new EDID (or none, Len 0). StateLock taken here. The HDMI modes:
 * - on an HDMI boot, for the monitor the firmware lit (the EDID read first,
 *   or none at all), the firmware's timing first. If one of the EDID's
 *   detailed timings is that timing it stands in for it, since it can be
 *   programmed again later; and the wire is then known to be that timing.
 * - at level 3, the EDID's detailed timings and the safe set (hdmi_calc.h).
 * - at level 3 with nothing at all, the safe set. */
static void UpdateEdid(MW_ADAPTER *A, const UCHAR *E, ULONG Len)
{
    MW_HDMI *H = &A->Hdmi;
    MW_TIMING m[MW_HDMI_MODES];
    ULONG n = 0, k = 0;
    BOOLEAN same;

    Lock(&H->StateLock);
    RtlZeroMemory(H->Edid, sizeof(H->Edid));
    H->EdidSize = 0;
    RtlZeroMemory(&H->Sink, sizeof(H->Sink));
    if (Len >= 128) {
        RtlCopyMemory(H->Edid, E, min(Len, (ULONG)sizeof(H->Edid)));
        H->EdidSize = min(Len, (ULONG)sizeof(H->Edid));
        MwEdidSink(H->Edid, H->EdidSize, &H->Sink);
    } else {
        H->Sink.IsHdmi = 1;                     /* the firmware's fallback marks the sink HDMI too */
    }
    if (!H->HaveBootEdid && !H->Dsi && H->EdidSize >= 128) {
        RtlCopyMemory(H->BootEdid, H->Edid, 128);
        H->HaveBootEdid = TRUE;
    }
    same = !H->Dsi && (H->EdidSize == 0 ||
                       (H->HaveBootEdid && RtlCompareMemory(H->BootEdid, H->Edid, 128) == 128));
    if (H->Switch)
        n = MwHdmiModes(H->Edid, H->EdidSize, H->ResMaxPixels, m, MW_HDMI_MODES);
    H->ModeCount = 0;
    if (same) {
        MW_TIMING fw = H->FwTiming;
        for (ULONG i = 0; i < n; i++)
            if (MwTimingEqual(&m[i], &fw)) {
                fw = m[i];
                if (H->WireKind == MW_WIRE_HDMI && H->Wire.Fw && MwTimingEqual(&H->Wire, &fw))
                    H->Wire = fw;
                break;
            }
        H->Modes[H->ModeCount++] = fw;
    }
    for (k = 0; k < n && H->ModeCount < MW_HDMI_MODES; k++) {
        BOOLEAN dup = FALSE;
        for (ULONG i = 0; i < H->ModeCount; i++)
            if (MwTimingEqual(&H->Modes[i], &m[k]))
                dup = TRUE;
        if (!dup)
            H->Modes[H->ModeCount++] = m[k];
    }
    Unlock(&H->StateLock);
    LogEdid(A);
}

/* PASSIVE_LEVEL. HwLock taken here. */
static void ReadEdid(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    UCHAR *buf;
    ULONG len = 0;
    NTSTATUS st;

    buf = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, MW_EDID_MAX, MW_TAG);
    if (buf == NULL)
        return;
    Lock(&H->HwLock);
    st = MwHdmiReadEdid(A, buf, MW_EDID_MAX, &len);
    Unlock(&H->HwLock);
    UpdateEdid(A, buf, NT_SUCCESS(st) ? len : 0);
    ExFreePoolWithTag(buf, MW_TAG);
}

/* ---- the poll thread ---- */

static void IndicateOne(MW_ADAPTER *A, ULONG Child, BOOLEAN Connected, PCWSTR Name)
{
    DXGK_CHILD_STATUS cs;
    NTSTATUS st;

    RtlZeroMemory(&cs, sizeof(cs));
    cs.Type = StatusConnection;
    cs.ChildUid = Child;
    cs.HotPlug.Connected = Connected;
    st = A->Dxgk.DxgkCbIndicateChildStatus(A->Dxgk.DeviceHandle, &cs);
    MwLog(A, Name, (ULONG)st);
}

/* HdmiExclusive (DSI boot): the panel is reported disconnected while a
 * monitor is on HDMI, so that Windows never has two displays to arrange on
 * one VidPN source. Plugged: HDMI there first, then the panel gone;
 * unplugged: the panel back first, then HDMI gone, so that Windows always
 * has a display to move to. */
static void Indicate(MW_ADAPTER *A, BOOLEAN Connected)
{
    MW_HDMI *H = &A->Hdmi;
    BOOLEAN excl = H->Exclusive && H->Dsi && H->Children == 2;

    if (!H->Expose)
        return;
    if (excl && !Connected)
        IndicateOne(A, 0, TRUE, L"Hpd_IndicatePanel");
    IndicateOne(A, H->HdmiChild, Connected, L"Hpd_Indicate");
    if (excl && Connected)
        IndicateOne(A, 0, FALSE, L"Hpd_IndicatePanel");
}

/* Something that can leave the screen black, stamped once after each mode
 * set: 1 SetVidPnSourceVisibility(FALSE), 2 the window moved off a primary
 * onto A->Fb (MwScanToFb), 3 a commit with no path. Any IRQL; the poll
 * thread logs it (Blk_*) with a snapshot of the state at that moment. */
void MwHpdBlackEvent(MW_ADAPTER *A, LONG Why)
{
    MW_HDMI *H = &A->Hdmi;

    if (H->MsetTick == 0)
        return;
    if (InterlockedCompareExchange64(&H->BlackTick, (LONG64)KeQueryInterruptTime(), 0) == 0)
        InterlockedExchange(&H->BlackWhy, Why);
}

/* The state of the HDMI scan-out, as registry values Prefix_<name>.
 * Deltas since the last mode set: Flips (SetVidPnSourceAddress calls),
 * Scanned, Copied, NotMapped, Invisible, Mismatch, Maps (primaries entered
 * in the IOMMU), Vsyncs (vsync interrupts the driver sent dxgkrnl, all for
 * target VsyncTgt), Presents and PresentFlips (DxgkDdiPresent, all and
 * flips), Creates and Destroys (allocations), VisOff (visibility-off
 * calls). Refused: the last primary refused at map time, W << 16 | H.
 * Showing: bit 0 window on a primary, 1 scan-out on, 2 IOMMU paging, 3
 * source visible, 4 path active, 5 our framebuffer, 6 Hold, 7 vsync
 * interrupt on, 15:8 rotation. ScanAddr: the last flip's address. Fb:
 * A->Fb. LastFlip: that flip's primary, LastWhy: 0 scanned, 1 scan-out
 * off, 2 not entered, 3 rotated, 4 another size, 9 source invisible.
 * VisOn: visibility-on calls since start. IsSup, Enum, RecFunc, Status,
 * Desc: DDI calls since start. Sup0..Sup5: IsSupportedVidPn answers by
 * reason since start (0 supported). IntOn, IntOff: vsync interrupt
 * switches since start. Mmu0, Mmu1, Fault: the IOMMU registers
 * MwScanCheck reads. Ms: ms since boot. */
static void Snapshot(MW_ADAPTER *A, PCWSTR Prefix)
{
    MW_HDMI *H = &A->Hdmi;
    static PCWSTR const names[] = {
        L"Flips", L"Scanned", L"Copied", L"NotMapped", L"Invisible", L"Mismatch", L"Refused", L"Maps",
        L"Vsyncs", L"VsyncTgt", L"Presents", L"PresentFlips", L"Creates", L"Destroys", L"VisOff",
        L"Showing", L"ScanAddr", L"Fb", L"LastFlip", L"LastWhy", L"VisOn",
        L"IsSup", L"Enum", L"RecFunc", L"Status", L"Desc",
        L"Sup0", L"Sup1", L"Sup2", L"Sup3", L"Sup4", L"Sup5", L"IntOn", L"IntOff",
        L"PrimCreates", L"LastPrim", L"LastPrimPitch", L"Describes", L"LastDesc", L"LastDescRef",
        L"Blts", L"Fills", L"LastPresent", L"LastPresentSrc", L"LastPresentDst", L"Ref", L"SharedRef",
        L"ToFbFrom", L"FbSkips", L"VisOff1Ms", L"VisOff2Ms", L"VisOff3Ms", L"VisOff4Ms", L"VisOffN",
        L"Mmu0", L"Mmu1", L"Fault", L"Ms" };
    ULONG v[ARRAYSIZE(names)], fault = 0, k = 0;
    WCHAR buf[48];
    UNICODE_STRING us;

    v[k++] = A->Flips - H->BaseFlips;
    v[k++] = A->ScanFlips - H->BaseScanFlips;
    v[k++] = A->FlipCopies - H->BaseCopies;
    v[k++] = A->WhyNotMapped - H->BaseNotMapped;
    v[k++] = A->WhyInvisible - H->BaseInvisible;
    v[k++] = A->ScanMismatch - H->BaseMismatch;
    v[k++] = A->ScanLastW << 16 | (A->ScanLastH & 0xFFFF);
    v[k++] = A->ScanMaps - H->BaseMaps;
    v[k++] = A->Vsyncs - H->BaseVsyncs;
    v[k++] = H->ActiveTarget;
    v[k++] = A->Presents - H->BasePresents;
    v[k++] = A->PresentFlips - H->BasePresentFlips;
    v[k++] = (ULONG)A->MemCreates - H->BaseCreates;
    v[k++] = (ULONG)A->MemDestroys - H->BaseDestroys;
    v[k++] = (ULONG)H->VisOff - H->BaseVisOff;
    v[k++] = (A->ScanShowing ? 1u : 0) | (A->ScanOn ? 2u : 0) | (A->ScanPaging ? 4u : 0) |
             (A->SourceVisible ? 8u : 0) | (A->PathActive ? 16u : 0) | (H->OnRes ? 32u : 0) |
             (H->Hold ? 64u : 0) | (A->VsyncOn ? 128u : 0) | A->RotDeg << 8;
    v[k++] = A->ScanAddr.LowPart;
    v[k++] = A->Fb.Width << 16 | (A->Fb.Height & 0xFFFF);
    v[k++] = (ULONG)H->LastFlipWH;
    v[k++] = (ULONG)H->LastFlipWhy;
    v[k++] = (ULONG)H->VisOn;
    v[k++] = (ULONG)H->IsSupCalls;
    v[k++] = (ULONG)H->EnumCalls;
    v[k++] = (ULONG)H->RecFunc;
    v[k++] = (ULONG)H->StatusCalls;
    v[k++] = (ULONG)H->DescCalls;
    for (ULONG r = 0; r < 6; r++)
        v[k++] = (ULONG)H->SupBy[r];
    v[k++] = (ULONG)H->IntOn;
    v[k++] = (ULONG)H->IntOff;
    v[k++] = (ULONG)H->PrimCreates;
    v[k++] = (ULONG)H->LastPrimWH;
    v[k++] = (ULONG)H->LastPrimPitch;
    v[k++] = (ULONG)H->Describes;
    v[k++] = (ULONG)H->LastDescWH;
    v[k++] = (ULONG)H->LastDescRef;
    v[k++] = (ULONG)H->PresentBltsH;
    v[k++] = (ULONG)H->PresentFillsH;
    v[k++] = (ULONG)H->LastPresentFlags;
    v[k++] = (ULONG)H->LastPresentSrcWH;
    v[k++] = (ULONG)H->LastPresentDstWH;
    v[k++] = H->RefDen ? (ULONG)(H->RefNum * 100ull / H->RefDen) : 0;
    v[k++] = H->SharedPrimRefDen ? (ULONG)(H->SharedPrimRefNum * 100ull / H->SharedPrimRefDen) : 0;
    v[k++] = (ULONG)H->ToFbFrom;
    v[k++] = (ULONG)H->FbSkips;
    for (ULONG i = 0; i < 4; i++)
        v[k++] = H->VisOffMs[i];
    v[k++] = (ULONG)H->VisOffN;
    v[k++] = MwScanMmuStatus(A, 0, &fault);
    v[k++] = MwScanMmuStatus(A, 1, &fault);
    v[k++] = fault;
    v[k++] = (ULONG)(KeQueryInterruptTime() / 10000);
    for (ULONG i = 0; i < k; i++) {
        RtlInitEmptyUnicodeString(&us, buf, sizeof(buf));
        if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"%ws_%ws", Prefix, names[i])))
            MwLog(A, buf, v[i]);
    }
}

/* From the poll thread: Chk1_* and the one VOP2 read 3 s after a mode set;
 * Per_* every 5 s for 2 minutes, then every 30 s (Per_N counts them); Blk_*
 * once, at the first poll after a blanking event, with Blk_Why and
 * Blk_EventMs (ms since boot) and Blk_AfterMs (ms after the mode set). */
static void Periodic(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    ULONG64 now = KeQueryInterruptTime();

    if (H->MsetTick == 0)
        return;
    if (H->CheckStage == 1 && now >= H->MsetTick + 3000ull * 10000) {
        Snapshot(A, L"Chk1");
        MwLog(A, L"Chk1_VpRaw", MwHdmiVpRawStatus(A));
        H->CheckStage = 2;
    }
    if (!H->BlackLogged && H->BlackTick != 0) {
        H->BlackLogged = TRUE;
        MwLog(A, L"Blk_VisOff1Ms", H->VisOffMs[0]);
        MwLog(A, L"Blk_VisOff2Ms", H->VisOffMs[1]);
        MwLog(A, L"Blk_VisOff3Ms", H->VisOffMs[2]);
        MwLog(A, L"Blk_VisOff4Ms", H->VisOffMs[3]);
        MwLog(A, L"Blk_Why", (ULONG)H->BlackWhy);
        MwLog(A, L"Blk_EventMs", (ULONG)((ULONG64)H->BlackTick / 10000));
        MwLog(A, L"Blk_AfterMs", (ULONG)(((ULONG64)H->BlackTick - H->MsetTick) / 10000));
        Snapshot(A, L"Blk");
    }
    if (now >= H->NextPer) {
        Snapshot(A, L"Per");
        MwLog(A, L"Per_Sup2Path", (ULONG)H->Sup2Path);
        MwLog(A, L"Per_N", ++H->PerN);
        H->NextPer = now + (now - H->MsetTick < 120000ull * 10000 ? 5000ull : 30000ull) * 10000;
    }
}

static VOID HpdThread(PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    MW_HDMI *H = &a->Hdmi;

    for (;;) {
        LARGE_INTEGER t;
        ULONG raw;
        BOOLEAN hpd;

        t.QuadPart = -(LONGLONG)H->PollMs * 10000;
        KeWaitForSingleObject(&H->Wake, Executive, KernelMode, FALSE, &t);
        if (H->Stop)
            break;
        Periodic(a);
        hpd = MwHdmiReadHpd(a, &raw);
        H->Polls++;
        if (raw != H->LastRaw) {
            H->LastRaw = raw;
            MwLog(a, L"Hpd_Raw", raw);
        }
        if (hpd != H->Last) {
            /* An edge: start the debounce again. */
            H->Last = hpd;
            H->LastEdge = KeQueryInterruptTime();
            H->Edges++;
            continue;
        }
        if (KeQueryInterruptTime() - H->LastEdge < (ULONG64)H->DebounceMs * 10000)
            continue;
        if (hpd)
            H->SeenHigh = TRUE;
        if (hpd == H->Connected || (!hpd && !H->SeenHigh))
            continue;
        H->Changes++;
        MwLog(a, L"Hpd_Changes", H->Changes);
        MwLog(a, L"Hpd_Edges", H->Edges);       /* raw edges seen so far: bounce shows as more than Changes */
        MwLog(a, L"Hpd_State", hpd);
        if (hpd)
            ReadEdid(a);
        H->Connected = hpd;
        Indicate(a, hpd);
    }
    MwLog(a, L"Hpd_Polls", H->Polls);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* From StartDevice, after the backlight is known (it says whether the
 * firmware's screen is the panel), before the children are counted. */
void MwHpdStart(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    ULONG raw = 0;
    BOOLEAN hpd;

    H->Children = 1;
    if (H->Level == 0)
        return;
    InterlockedExchange(&H->Stop, 0);           /* a restart of the adapter */
    H->Dsi = A->BlVa != NULL;
    if (!NT_SUCCESS(MwHdmiMap(A))) {
        /* Nothing is reported or set; MwHpdDisplayStop still frees. */
        MwLog(A, L"Hpd_Off", 1);
        MwHdmiUnmap(A);
        return;
    }
    MwLog(A, L"Hpd_Powered", MwHdmiPowered(A));

    /* The firmware's timing, as display.c reports it. */
    RtlZeroMemory(&H->FwTiming, sizeof(H->FwTiming));
    H->FwTiming.Clock = A->TimingPixelRate / 1000;
    H->FwTiming.HDisplay = (mwh_u16)A->Fb.Width;
    H->FwTiming.VDisplay = (mwh_u16)A->Fb.Height;
    H->FwTiming.HTotal = (mwh_u16)A->TimingTotalW;
    H->FwTiming.VTotal = (mwh_u16)A->TimingTotalH;
    H->FwTiming.Fw = 1;
    if (H->Dsi && A->Fb.Width == 800 && A->Fb.Height == 1280) {
        TspiPanel(&H->Panel);
        H->HavePanel = TRUE;
    }
    H->Switch = H->Level >= 3 && H->ResVa != NULL && (!H->Dsi || H->HavePanel);
    H->Expose = H->Level >= 2 && (!H->Dsi || H->Switch);
    H->HdmiChild = H->Dsi ? 1 : 0;
    H->Children = (H->Expose && H->Dsi) ? 2 : 1;
    H->WireKind = H->Dsi ? MW_WIRE_DSI : MW_WIRE_HDMI;
    H->Wire = H->FwTiming;
    H->ActiveTarget = 0;

    /* The state at start: two readings 10 ms apart. An HDMI boot's output
     * is connected whatever they say (the firmware lit it). */
    (void)MwHdmiReadHpd(A, &raw);
    {
        LARGE_INTEGER t;
        t.QuadPart = -100000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    hpd = MwHdmiReadHpd(A, &raw);
    H->LastRaw = raw;
    H->Last = hpd;
    H->LastEdge = KeQueryInterruptTime();
    H->SeenHigh = hpd;
    H->Connected = H->Dsi ? hpd : TRUE;
    MwLog(A, L"Hpd_Raw", raw);
    MwLog(A, L"Hpd_State", hpd);
    if (hpd)
        ReadEdid(A);
    else
        UpdateEdid(A, NULL, 0);
    MwLog(A, L"Hpd_Dsi", H->Dsi);
    MwLog(A, L"Hpd_Expose", H->Expose);
    MwLog(A, L"Hpd_Switch", H->Switch);
    MwLog(A, L"Hpd_Children", H->Children);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, HpdThread, A))) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                                  (PVOID *)&H->Thread, NULL)))
            H->Thread = NULL;
        ZwClose(h);
    }
    H->Started = TRUE;
    MwLog(A, L"Hpd_Thread", H->Thread != NULL);
}

void MwHpdStop(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    if (!H->Started)
        return;
    InterlockedExchange(&H->Stop, 1);
    KeSetEvent(&H->Wake, IO_NO_INCREMENT, FALSE);
    if (H->Thread != NULL) {
        KeWaitForSingleObject(H->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(H->Thread);
        H->Thread = NULL;
    }
    MwLog(A, L"Hpd_ModeSets", H->ModeSets);
    MwLog(A, L"Hpd_ModeSetFails", H->ModeSetFails);
    MwLog(A, L"Hpd_Restores", H->Restores);
    H->Started = FALSE;
    /* At level 3 the registers stay mapped until MwHpdDisplayStop, since
     * MwScanStop runs between the two; below it nothing needs them. */
    if (H->Level < 3)
        MwHdmiUnmap(A);
}

/* From MwDisplayStop, after MwScanStop has put the window on A->Fb and
 * turned translation off. The firmware's framebuffer is unmapped. Ours is
 * freed unless it is the one on the screen: a hand-back gives it to the
 * next driver (MwStopDeviceAndReleasePostDisplayOwnership), which then
 * draws in it, so it is left allocated. */
void MwHpdDisplayStop(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    if (H->FwFbVa != NULL) {
        MmUnmapIoSpace(H->FwFbVa, H->FwFbSize);
        H->FwFbVa = NULL;
    }
    if (H->ResVa != NULL && !H->OnRes && !A->ScanPaging) {
        MmFreeContiguousMemorySpecifyCache(H->ResVa, H->ResBytes, MmWriteCombined);
        H->ResVa = NULL;
    }
    A->FbVa = NULL;
    MwHdmiUnmap(A);
}

/* ---- children ---- */

NTSTATUS MwHpdQueryChildRelations(MW_ADAPTER *A, PDXGK_CHILD_DESCRIPTOR C, ULONG Size)
{
    MW_HDMI *H = &A->Hdmi;
    ULONG n = H->Children;

    if (Size < (n + 1) * sizeof(DXGK_CHILD_DESCRIPTOR))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(C, (n + 1) * sizeof(DXGK_CHILD_DESCRIPTOR));
    for (ULONG i = 0; i < n; i++) {
        BOOLEAN panel = H->Dsi && i == 0;
        C[i].ChildDeviceType = TypeVideoOutput;
        C[i].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = panel ? D3DKMDT_VOT_INTERNAL : D3DKMDT_VOT_HDMI;
        C[i].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        C[i].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        C[i].ChildCapabilities.HpdAwareness = (panel && !H->Exclusive) ? HpdAwarenessAlwaysConnected
                                                                       : HpdAwarenessInterruptible;
        C[i].AcpiUid = 0;
        C[i].ChildUid = i;
    }
    MwLog(A, L"Hpd_ChildRelations", n);
    return STATUS_SUCCESS;
}

NTSTATUS MwHpdQueryChildStatus(MW_ADAPTER *A, PDXGK_CHILD_STATUS S)
{
    MW_HDMI *H = &A->Hdmi;

    InterlockedIncrement(&H->StatusCalls);
    switch (S->Type) {
    case StatusConnection:
        if (S->ChildUid == H->HdmiChild)
            S->HotPlug.Connected = H->Connected;
        else
            S->HotPlug.Connected = !(H->Exclusive && H->Dsi && H->Children == 2 && H->Connected);
        return STATUS_SUCCESS;
    case StatusRotation:
        S->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* The HDMI child's EDID: the one DDC returned, else (an HDMI boot) the
 * firmware's from the DSDT, as display.c serves it. */
NTSTATUS MwHpdQueryDeviceDescriptor(MW_ADAPTER *A, ULONG ChildUid, PDXGK_DEVICE_DESCRIPTOR D)
{
    MW_HDMI *H = &A->Hdmi;
    const UCHAR *e = NULL;
    ULONG size = 0, off, len, n;
    NTSTATUS st = STATUS_SUCCESS;

    if (ChildUid != H->HdmiChild)
        return STATUS_MONITOR_NO_DESCRIPTOR;
    Lock(&H->StateLock);
    if (H->EdidSize != 0) {
        e = H->Edid;
        size = H->EdidSize;
    } else if (!H->Dsi && A->EdidSize != 0) {
        e = A->Edid;
        size = A->EdidSize;
    }
    off = D->DescriptorOffset;
    len = D->DescriptorLength;
    if (size == 0) {
        st = STATUS_MONITOR_NO_DESCRIPTOR;
    } else if (off >= size) {
        st = STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    } else if (D->DescriptorBuffer == NULL || len == 0) {
        st = STATUS_INVALID_PARAMETER;
    } else {
        n = min(len, size - off);
        RtlCopyMemory(D->DescriptorBuffer, e + off, n);
        if (n < len)
            RtlZeroMemory((PUCHAR)D->DescriptorBuffer + n, len - n);
    }
    Unlock(&H->StateLock);
    return st;
}

/* ---- level 3: the VidPN ---- */

/* The timings a target offers. The panel: the firmware's, as display.c
 * reports it. HDMI: UpdateEdid's, else the safe set. */
static ULONG TargetModes(MW_ADAPTER *A, ULONG Target, MW_TIMING *Out)
{
    MW_HDMI *H = &A->Hdmi;
    ULONG n = 0;

    if (H->Dsi && Target == 0) {
        Out[0] = H->FwTiming;
        return 1;
    }
    if (Target != H->HdmiChild)
        return 0;
    Lock(&H->StateLock);
    n = H->ModeCount;
    RtlCopyMemory(Out, H->Modes, n * sizeof(MW_TIMING));
    Unlock(&H->StateLock);
    if (n == 0)
        n = MwHdmiModes(NULL, 0, H->ResMaxPixels, Out, MW_HDMI_MODES);
    return n;
}

static ULONG PixelRate(MW_ADAPTER *A, const MW_TIMING *T)
{
    return T->Fw ? A->TimingPixelRate : T->Clock * 1000;
}

static void Signal(MW_ADAPTER *A, const MW_TIMING *T, D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    ULONG rate = PixelRate(A, T);

    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = T->HTotal;
    S->TotalSize.cy = T->VTotal;
    S->ActiveSize.cx = T->HDisplay;
    S->ActiveSize.cy = T->VDisplay;
    S->PixelRate = rate;
    S->HSyncFreq.Numerator = rate;
    S->HSyncFreq.Denominator = T->HTotal;
    S->VSyncFreq.Numerator = rate;
    S->VSyncFreq.Denominator = (ULONG)T->HTotal * T->VTotal;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static BOOLEAN SignalIs(MW_ADAPTER *A, const MW_TIMING *T, const D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    return S->ActiveSize.cx == T->HDisplay && S->ActiveSize.cy == T->VDisplay &&
           S->TotalSize.cx == T->HTotal && S->TotalSize.cy == T->VTotal && S->PixelRate == PixelRate(A, T);
}

static UINT StrideFor(MW_ADAPTER *A, UINT W, UINT H2)
{
    MW_HDMI *H = &A->Hdmi;
    return (W == H->FwFb.Width && H2 == H->FwFb.Height) ? H->FwFb.Pitch : W * 4;
}

static BOOLEAN PinnedSource(const DXGK_VIDPN_INTERFACE *V, D3DKMDT_HVIDPN Vp, D3DDDI_VIDEO_PRESENT_SOURCE_ID Src,
                            UINT *W, UINT *H2)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si;
    const D3DKMDT_VIDPN_SOURCE_MODE *p = NULL;
    BOOLEAN found = FALSE;

    if (!NT_SUCCESS(V->pfnAcquireSourceModeSet(Vp, Src, &set, &si)))
        return FALSE;
    if (NT_SUCCESS(si->pfnAcquirePinnedModeInfo(set, &p)) && p != NULL) {
        if (p->Type == D3DKMDT_RMT_GRAPHICS) {
            *W = p->Format.Graphics.PrimSurfSize.cx;
            *H2 = p->Format.Graphics.PrimSurfSize.cy;
            found = TRUE;
        }
        si->pfnReleaseModeInfo(set, p);
    }
    V->pfnReleaseSourceModeSet(Vp, set);
    return found;
}

/* The timing pinned on a target, by index into its list; -1 none or not ours. */
static LONG PinnedTarget(MW_ADAPTER *A, const DXGK_VIDPN_INTERFACE *V, D3DKMDT_HVIDPN Vp,
                         D3DDDI_VIDEO_PRESENT_TARGET_ID Tgt, const MW_TIMING *M, ULONG N, BOOLEAN *Any)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
    const D3DKMDT_VIDPN_TARGET_MODE *p = NULL;
    LONG idx = -1;

    *Any = FALSE;
    if (!NT_SUCCESS(V->pfnAcquireTargetModeSet(Vp, Tgt, &set, &ti)))
        return -1;
    if (NT_SUCCESS(ti->pfnAcquirePinnedModeInfo(set, &p)) && p != NULL) {
        *Any = TRUE;
        for (ULONG i = 0; i < N; i++)
            if (SignalIs(A, &M[i], &p->VideoSignalInfo)) {
                idx = (LONG)i;
                break;
            }
        ti->pfnReleaseModeInfo(set, p);
    }
    V->pfnReleaseTargetModeSet(Vp, set);
    return idx;
}

static NTSTATUS AddSourceModes(MW_ADAPTER *A, const DXGK_VIDPN_INTERFACE *V, D3DKMDT_HVIDPN Vp,
                               D3DDDI_VIDEO_PRESENT_SOURCE_ID Src, const MW_TIMING *M, ULONG N, LONG Only)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si;
    NTSTATUS st;

    st = V->pfnCreateNewSourceModeSet(Vp, Src, &set, &si);
    if (!NT_SUCCESS(st))
        return st;
    for (ULONG i = 0; i < N && NT_SUCCESS(st); i++) {
        D3DKMDT_VIDPN_SOURCE_MODE *mode;
        BOOLEAN dup = FALSE;
        if (Only >= 0 && (M[i].HDisplay != M[Only].HDisplay || M[i].VDisplay != M[Only].VDisplay))
            continue;
        for (ULONG k = 0; k < i; k++)
            if (M[k].HDisplay == M[i].HDisplay && M[k].VDisplay == M[i].VDisplay)
                dup = TRUE;
        if (dup)
            continue;
        st = si->pfnCreateNewModeInfo(set, &mode);
        if (!NT_SUCCESS(st))
            break;
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        mode->Format.Graphics.PrimSurfSize.cx = M[i].HDisplay;
        mode->Format.Graphics.PrimSurfSize.cy = M[i].VDisplay;
        mode->Format.Graphics.VisibleRegionSize = mode->Format.Graphics.PrimSurfSize;
        mode->Format.Graphics.Stride = StrideFor(A, M[i].HDisplay, M[i].VDisplay);
        mode->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
        mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        st = si->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st)) {
            si->pfnReleaseModeInfo(set, mode);
            if (st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
                st = STATUS_SUCCESS;
        }
    }
    if (NT_SUCCESS(st))
        st = V->pfnAssignSourceModeSet(Vp, Src, set);
    if (!NT_SUCCESS(st))
        V->pfnReleaseSourceModeSet(Vp, set);
    return st;
}

static NTSTATUS AddTargetModes(MW_ADAPTER *A, const DXGK_VIDPN_INTERFACE *V, D3DKMDT_HVIDPN Vp,
                               D3DDDI_VIDEO_PRESENT_TARGET_ID Tgt, const MW_TIMING *M, ULONG N,
                               BOOLEAN SizePinned, UINT W, UINT H2)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
    NTSTATUS st;
    BOOLEAN first = TRUE;

    st = V->pfnCreateNewTargetModeSet(Vp, Tgt, &set, &ti);
    if (!NT_SUCCESS(st))
        return st;
    for (ULONG i = 0; i < N && NT_SUCCESS(st); i++) {
        D3DKMDT_VIDPN_TARGET_MODE *mode;
        if (SizePinned && (M[i].HDisplay != W || M[i].VDisplay != H2))
            continue;
        st = ti->pfnCreateNewModeInfo(set, &mode);
        if (!NT_SUCCESS(st))
            break;
        Signal(A, &M[i], &mode->VideoSignalInfo);
        mode->Preference = first ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        first = FALSE;
        st = ti->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st)) {
            ti->pfnReleaseModeInfo(set, mode);
            if (st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
                st = STATUS_SUCCESS;
        }
    }
    if (NT_SUCCESS(st))
        st = V->pfnAssignTargetModeSet(Vp, Tgt, set);
    if (!NT_SUCCESS(st))
        V->pfnReleaseTargetModeSet(Vp, set);
    return st;
}

/* A VidPN decision for the trace, written only when it differs from the
 * last one (dxgkrnl asks many times). */
static void LogOnChange(MW_ADAPTER *A, PCWSTR Name, ULONG *Last, ULONG V)
{
    if (*Last == V)
        return;
    *Last = V;
    MwLog(A, Name, V);
}

/* Hpd_IsSup: target | reason << 8 | supported << 16 | paths << 24. Reason:
 * 0 fine, 1 not source 0 or more than one path (clone/extend), 2 the target
 * has no modes, 3 the pinned source size is none of the target's (then
 * Hpd_IsSupSize is that size), 4 the pinned target mode is not ours, 5 the
 * pinned source and target sizes differ. */
NTSTATUS MwHpdIsSupportedVidPn(MW_ADAPTER *A, DXGKARG_ISSUPPORTEDVIDPN *Arg)
{
    MW_HDMI *H = &A->Hdmi;
    const DXGK_VIDPN_INTERFACE *v;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    MW_TIMING m[MW_HDMI_MODES];
    ULONG paths = 0, reason = 0, target = 0xFF, twoPath = 0;
    UINT w = 0, h = 0;
    NTSTATUS st;

    InterlockedIncrement(&H->IsSupCalls);
    Arg->IsVidPnSupported = FALSE;
    if (Arg->hDesiredVidPn == 0) {
        Arg->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    st = A->Dxgk.DxgkCbQueryVidPnInterface(Arg->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &v);
    if (!NT_SUCCESS(st))
        return st;
    st = v->pfnGetTopology(Arg->hDesiredVidPn, &topo, &ti);
    if (!NT_SUCCESS(st))
        return st;
    st = ti->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        ULONG n = TargetModes(A, path->VidPnTargetId, m);
        BOOLEAN any;
        LONG t;

        paths++;
        target = path->VidPnTargetId;
        if (paths <= 2)
            twoPath |= ((path->VidPnSourceId & 0xFF) << 8 | (path->VidPnTargetId & 0xFF)) << (paths == 1 ? 16 : 0);
        if (reason == 0 && (path->VidPnSourceId != 0 || paths > 1))
            reason = 1;
        if (reason == 0 && n == 0)
            reason = 2;
        if (reason == 0 && PinnedSource(v, Arg->hDesiredVidPn, 0, &w, &h)) {
            BOOLEAN size = FALSE;
            for (ULONG i = 0; i < n; i++)
                if (m[i].HDisplay == w && m[i].VDisplay == h)
                    size = TRUE;
            if (!size) {
                reason = 3;
                MwLog(A, L"Hpd_IsSupSize", w << 16 | (h & 0xFFFF));
            }
        }
        if (reason == 0) {
            t = PinnedTarget(A, v, Arg->hDesiredVidPn, path->VidPnTargetId, m, n, &any);
            if (any && t < 0)
                reason = 4;
            else if (any && w != 0 && (m[t].HDisplay != w || m[t].VDisplay != h))
                reason = 5;
        }
        st = ti->pfnAcquireNextPathInfo(topo, path, &next);
        ti->pfnReleasePathInfo(topo, path);
        path = next;
    }
    Arg->IsVidPnSupported = reason == 0;
    InterlockedIncrement(&H->SupBy[reason]);
    if (paths >= 2)
        InterlockedExchange(&H->Sup2Path, (LONG)twoPath);
    LogOnChange(A, L"Hpd_IsSup", &H->LastSup, (target & 0xFF) | reason << 8 | (reason == 0 ? 1u : 0) << 16 | paths << 24);
    return STATUS_SUCCESS;
}

NTSTATUS MwHpdEnumVidPnCofuncModality(MW_ADAPTER *A, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg)
{
    const DXGK_VIDPN_INTERFACE *v;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    D3DKMDT_HVIDPN vp = Arg->hConstrainingVidPn;
    MW_TIMING m[MW_HDMI_MODES];
    NTSTATUS st;

    InterlockedIncrement(&A->Hdmi.EnumCalls);
    st = A->Dxgk.DxgkCbQueryVidPnInterface(vp, DXGK_VIDPN_INTERFACE_VERSION_V1, &v);
    if (!NT_SUCCESS(st))
        return st;
    st = v->pfnGetTopology(vp, &topo, &ti);
    if (!NT_SUCCESS(st))
        return st;
    st = ti->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        D3DKMDT_ENUMCOFUNCMODALITY_PIVOT_TYPE pt = Arg->EnumPivotType;
        BOOLEAN srcPivot = pt == D3DKMDT_EPT_VIDPNSOURCE && Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId;
        BOOLEAN tgtPivot = pt == D3DKMDT_EPT_VIDPNTARGET && Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        BOOLEAN xfPivot = (pt == D3DKMDT_EPT_SCALING || pt == D3DKMDT_EPT_ROTATION) &&
                          Arg->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
                          Arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
        ULONG n = TargetModes(A, path->VidPnTargetId, m);
        UINT w = 0, h = 0;
        BOOLEAN srcPinned = PinnedSource(v, vp, path->VidPnSourceId, &w, &h), any;
        LONG tgt = PinnedTarget(A, v, vp, path->VidPnTargetId, m, n, &any);
        NTSTATUS r = STATUS_SUCCESS;

        /* Hpd_Enum: target | source pinned << 8 | target pinned << 9 |
         * pinned target is ours << 10 | modes << 16 | pinned source W/8 << 20. */
        LogOnChange(A, L"Hpd_Enum", &A->Hdmi.LastEnum,
                    (path->VidPnTargetId & 0xFF) | (srcPinned ? 1u << 8 : 0) | (any ? 1u << 9 : 0) |
                    (tgt >= 0 ? 1u << 10 : 0) | (n & 0xF) << 16 | ((w / 8) & 0xFFF) << 20);

        if (!srcPivot && !srcPinned && n != 0) {
            r = AddSourceModes(A, v, vp, path->VidPnSourceId, m, n, any ? tgt : -1);
            if (!NT_SUCCESS(r))
                MwLog(A, L"Fail_HpdAddSourceMode", (ULONG)r);
        }
        if (NT_SUCCESS(r) && !tgtPivot && !any && n != 0) {
            r = AddTargetModes(A, v, vp, path->VidPnTargetId, m, n, srcPinned, w, h);
            if (!NT_SUCCESS(r))
                MwLog(A, L"Fail_HpdAddTargetMode", (ULONG)r);
        }
        /* Identity scaling only; rotation as display.c offers it. */
        if (NT_SUCCESS(r) && !xfPivot &&
            (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
             path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;
            if (upd.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.ScalingSupport, sizeof(upd.ContentTransformation.ScalingSupport));
                upd.ContentTransformation.ScalingSupport.Identity = 1;
            }
            if (upd.ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
                RtlZeroMemory(&upd.ContentTransformation.RotationSupport, sizeof(upd.ContentTransformation.RotationSupport));
                upd.ContentTransformation.RotationSupport.Identity = 1;
                upd.ContentTransformation.RotationSupport.Rotate90 = 1;
                upd.ContentTransformation.RotationSupport.Rotate180 = 1;
                upd.ContentTransformation.RotationSupport.Rotate270 = 1;
            }
            r = ti->pfnUpdatePathSupportInfo(topo, &upd);
        }
        if (!NT_SUCCESS(r)) {
            ti->pfnReleasePathInfo(topo, path);
            return r;
        }
        st = ti->pfnAcquireNextPathInfo(topo, path, &next);
        ti->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET ? STATUS_SUCCESS : st;
}

/* The CPU writers of A->FbVa that do not hold CopyLock: in while no switch
 * is under way. At level 0 to 2 the framebuffer never moves and this is
 * always TRUE with nothing counted. Any IRQL. */
BOOLEAN MwHpdFbEnter(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    if (!H->Switch)
        return TRUE;
    InterlockedIncrement(&H->FbUsers);
    if (H->FbSwitch) {
        InterlockedDecrement(&H->FbUsers);
        InterlockedIncrement(&H->FbSkips);
        return FALSE;
    }
    return TRUE;
}

void MwHpdFbLeave(MW_ADAPTER *A)
{
    if (A->Hdmi.Switch)
        InterlockedDecrement(&A->Hdmi.FbUsers);
}

/* Which framebuffer A->Fb is: ours at W x H, or the firmware's. Under
 * CopyLock, which the copy thread holds while it writes A->FbVa, and with
 * the other writers (MwHpdFbEnter) drained first: a blt present or the
 * clear at visibility off that read the new FbVa with the old size would
 * write past the end of the firmware's 4 MB mapping. PASSIVE_LEVEL. */
static void UseFb(MW_ADAPTER *A, BOOLEAN Res, UINT W, UINT H2)
{
    MW_HDMI *H = &A->Hdmi;

    InterlockedExchange(&H->FbSwitch, 1);
    for (ULONG i = 0; i < 5000 && H->FbUsers != 0; i++)
        KeStallExecutionProcessor(10);      /* a blt is at most a frame's copy */
    ExAcquireFastMutex(&A->CopyLock);
    if (Res) {
        A->Fb.PhysicAddress = H->ResPa;
        A->Fb.Width = W;
        A->Fb.Height = H2;
        A->Fb.Pitch = W * 4;
        A->FbVa = H->ResVa;
        A->FbSize = (SIZE_T)W * 4 * H2;
    } else {
        A->Fb = H->FwFb;
        A->FbVa = H->FwFbVa;
        A->FbSize = H->FwFbSize;
    }
    H->OnRes = Res;
    A->ScanShowing = FALSE;     /* the window is on A->Fb now */
    KeMemoryBarrier();
    ExReleaseFastMutex(&A->CopyLock);
    InterlockedExchange(&H->FbSwitch, 0);
}

/* HdmiDiag 2: eight vertical bars (white, yellow, cyan, green, magenta,
 * red, blue, black) and a white 8-pixel frame, in our framebuffer before
 * the window is pointed at it. On the monitor: the window, its address,
 * stride and size and the IOMMU entries of our framebuffer are right. */
static void Pattern(PUCHAR Va, UINT W, UINT H2)
{
    static const ULONG bar[8] = { 0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00,
                                  0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000 };
    for (UINT y = 0; y < H2; y++) {
        PULONG row = (PULONG)(Va + (SIZE_T)y * W * 4);
        for (UINT x = 0; x < W; x++)
            row[x] = (x < 8 || y < 8 || x >= W - 8 || y >= H2 - 8) ? 0xFFFFFFFF : bar[x * 8 / W];
    }
}

/* The counters the checkpoints compare against, and when to take them. */
static void Baseline(MW_ADAPTER *A)
{
    MW_HDMI *H = &A->Hdmi;

    H->BaseFlips = A->Flips;
    H->BaseScanFlips = A->ScanFlips;
    H->BaseCopies = A->FlipCopies;
    H->BaseNotMapped = A->WhyNotMapped;
    H->BaseInvisible = A->WhyInvisible;
    H->BaseMismatch = A->ScanMismatch;
    H->BaseMaps = A->ScanMaps;
    H->BaseVsyncs = A->Vsyncs;
    H->BasePresents = A->Presents;
    H->BasePresentFlips = A->PresentFlips;
    H->BaseCreates = (ULONG)A->MemCreates;
    H->BaseDestroys = (ULONG)A->MemDestroys;
    H->BaseVisOff = (ULONG)H->VisOff;
    InterlockedExchange(&H->VisOffN, 0);
    RtlZeroMemory(H->VisOffMs, sizeof(H->VisOffMs));
    H->MsetTick = KeQueryInterruptTime();
    H->CheckStage = 1;
    H->NextPer = H->MsetTick + 5000ull * 10000;
    H->PerN = 0;
    InterlockedExchange64(&H->BlackTick, 0);
    H->BlackLogged = FALSE;
}

/* Make VP0 drive Target at T. PASSIVE_LEVEL, from CommitVidPn. */
static NTSTATUS Apply(MW_ADAPTER *A, ULONG Target, const MW_TIMING *T)
{
    MW_HDMI *H = &A->Hdmi;
    NTSTATUS st = STATUS_SUCCESS;

    if (H->Dsi && Target == 0) {
        if (H->WireKind == MW_WIRE_DSI)
            return STATUS_SUCCESS;
        Lock(&H->HwLock);
        InterlockedExchange(&H->Busy, 1);
        st = MwHdmiDsiRestore(A, H->FwFb.PhysicAddress.LowPart, H->FwFb.Pitch);
        if (NT_SUCCESS(st)) {
            UseFb(A, FALSE, 0, 0);
            H->WireKind = MW_WIRE_DSI;
            H->Restores++;
        }
        InterlockedExchange(&H->Busy, 0);
        InterlockedExchange(&H->Hold, 0);
        Unlock(&H->HwLock);
        if (NT_SUCCESS(st))
            MwBacklightPower(A, TRUE);
        Baseline(A);
        MwLog(A, L"Scan_Switch", NT_SUCCESS(st) ? 1 : (ULONG)st);
        return st;
    }
    if (H->WireKind == MW_WIRE_HDMI && MwTimingEqual(&H->Wire, T))
        return STATUS_SUCCESS;
    if (T->Fw)                  /* the firmware's timing cannot be set again: its syncs are unknown */
        return STATUS_GRAPHICS_INVALID_VIDPN;
    if (H->ResVa == NULL || (SIZE_T)T->HDisplay * 4 * T->VDisplay > H->ResBytes)
        return STATUS_GRAPHICS_INVALID_VIDPN;
    if (H->Dsi)
        MwBacklightPower(A, FALSE);
    RtlZeroMemory(H->ResVa, (SIZE_T)T->HDisplay * 4 * T->VDisplay);
    if (H->Diag == 2) {
        Pattern(H->ResVa, T->HDisplay, T->VDisplay);
        InterlockedExchange(&H->Hold, 1);
    }
    Lock(&H->HwLock);
    InterlockedExchange(&H->Busy, 1);
    H->ModeSets++;
    st = MwHdmiModeSet(A, T, H->ResPa.LowPart, (ULONG)T->HDisplay * 4);
    /* Once VP0 was touched the window is on our framebuffer, whatever the
     * outcome; STATUS_DEVICE_NOT_READY is a refusal before any write. */
    if (st != STATUS_DEVICE_NOT_READY)
        UseFb(A, TRUE, T->HDisplay, T->VDisplay);
    if (NT_SUCCESS(st)) {
        H->WireKind = MW_WIRE_HDMI;
        H->Wire = *T;
    } else {
        H->ModeSetFails++;
        if (st != STATUS_DEVICE_NOT_READY) {
            H->WireKind = MW_WIRE_HDMI;
            RtlZeroMemory(&H->Wire, sizeof(H->Wire));
        }
    }
    InterlockedExchange(&H->Busy, 0);
    Unlock(&H->HwLock);
    if (st == STATUS_DEVICE_NOT_READY && H->Dsi)
        MwBacklightPower(A, TRUE);
    Baseline(A);
    MwLog(A, L"Scan_Switch", NT_SUCCESS(st) ? 2 : (ULONG)st);
    MwLog(A, L"Scan_FbPa", A->Fb.PhysicAddress.LowPart);
    return st;
}

/* The path from source 0: its target and rotation. */
static BOOLEAN PathOf(const DXGK_VIDPN_INTERFACE *V, D3DKMDT_HVIDPN Vp, ULONG *Target, UINT *Deg)
{
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next;
    NTSTATUS st;

    if (!NT_SUCCESS(V->pfnGetTopology(Vp, &topo, &ti)))
        return FALSE;
    st = ti->pfnAcquireFirstPathInfo(topo, &path);
    while (st == STATUS_SUCCESS && path != NULL) {
        if (path->VidPnSourceId == 0) {
            *Target = path->VidPnTargetId;
            *Deg = MwPathDegrees(path->ContentTransformation.Rotation);
            ti->pfnReleasePathInfo(topo, path);
            return TRUE;
        }
        st = ti->pfnAcquireNextPathInfo(topo, path, &next);
        ti->pfnReleasePathInfo(topo, path);
        path = next;
    }
    return FALSE;
}

/* CommitVidPn at level 3: display.c's, with the mode set first. */
NTSTATUS MwHpdCommitVidPn(MW_ADAPTER *A, const DXGKARG_COMMITVIDPN *Arg)
{
    MW_HDMI *H = &A->Hdmi;
    const DXGK_VIDPN_INTERFACE *v;
    MW_TIMING m[MW_HDMI_MODES];
    ULONG target = 0, n;
    UINT w = 0, h = 0, deg = ~0u;
    BOOLEAN any;
    LONG t;
    NTSTATUS st;

    MwLog(A, L"Hpd_Commits", ++H->Commits);
    st = A->Dxgk.DxgkCbQueryVidPnInterface(Arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &v);
    if (!NT_SUCCESS(st))
        return st;
    if (!PathOf(v, Arg->hFunctionalVidPn, &target, &deg) || !PinnedSource(v, Arg->hFunctionalVidPn, 0, &w, &h)) {
        /* No path for our source, or nothing pinned: it is being turned off. */
        MwHpdBlackEvent(A, 3);
        A->PathActive = FALSE;
        A->CommitsNoPath++;
        MwLog(A, L"CommitNoPath", 0);
        return STATUS_SUCCESS;
    }
    n = TargetModes(A, target, m);
    t = PinnedTarget(A, v, Arg->hFunctionalVidPn, target, m, n, &any);
    if (!any)
        for (ULONG i = 0; i < n && t < 0; i++)
            if (m[i].HDisplay == w && m[i].VDisplay == h)
                t = (LONG)i;
    MwLog(A, L"Hpd_CommitAsk", target << 28 | (w & 0x3FFF) << 14 | (h & 0x3FFF));
    MwLog(A, L"Hpd_CommitRot", deg);
    if (t < 0 || m[t].HDisplay != w || m[t].VDisplay != h) {
        A->CommitsNotOurs++;
        MwLog(A, L"CommitStatus", (ULONG)STATUS_GRAPHICS_INVALID_VIDPN);
        return STATUS_GRAPHICS_INVALID_VIDPN;
    }
    MwLog(A, L"Hpd_CommitTarget", target);
    MwLog(A, L"Hpd_CommitMode", (ULONG)w << 16 | h);
    st = Apply(A, target, &m[t]);
    if (!NT_SUCCESS(st)) {
        MwLog(A, L"CommitStatus", (ULONG)st);
        return st;
    }
    H->ActiveTarget = target;
    {
        /* What primaries report from now on (memory.c MwDescribeAllocation). */
        D3DKMDT_VIDEO_SIGNAL_INFO sig;
        RtlZeroMemory(&sig, sizeof(sig));
        Signal(A, &m[t], &sig);
        H->RefNum = sig.VSyncFreq.Numerator;
        H->RefDen = sig.VSyncFreq.Denominator;
        MwLog(A, L"Hpd_Refresh", H->RefDen ? (ULONG)(H->RefNum * 100ull / H->RefDen) : 0);
    }
    A->PathActive = TRUE;
    A->CommitsPinned++;
    if (w != A->SrcWidth || h != A->SrcHeight) {
        A->SrcWidth = w;
        A->SrcHeight = h;
        MwLog(A, L"CommitSourceWidth", w);
        MwLog(A, L"CommitSourceHeight", h);
    }
    MwSetRotation(A, deg, L"CommitRotation");
    MwLog(A, L"CommitStatus", 0);
    return STATUS_SUCCESS;
}

NTSTATUS MwHpdRecommendMonitorModes(MW_ADAPTER *A, const DXGKARG_RECOMMENDMONITORMODES *Arg)
{
    MW_TIMING m[MW_HDMI_MODES];
    ULONG n = TargetModes(A, Arg->VideoPresentTargetId, m);

    for (ULONG i = 0; i < n; i++) {
        D3DKMDT_MONITOR_SOURCE_MODE *mode;
        NTSTATUS st = Arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(Arg->hMonitorSourceModeSet, &mode);
        if (!NT_SUCCESS(st))
            return st;
        Signal(A, &m[i], &mode->VideoSignalInfo);
        mode->ColorBasis = D3DKMDT_CB_SRGB;
        mode->ColorCoeffDynamicRanges.FirstChannel = 8;
        mode->ColorCoeffDynamicRanges.SecondChannel = 8;
        mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
        mode->ColorCoeffDynamicRanges.FourthChannel = 0;
        mode->Origin = D3DKMDT_MCO_DRIVER;
        mode->Preference = i == 0 ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        st = Arg->pMonitorSourceModeSetInterface->pfnAddMode(Arg->hMonitorSourceModeSet, mode);
        if (!NT_SUCCESS(st))
            Arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Arg->hMonitorSourceModeSet, mode);
    }
    MwLog(A, L"Hpd_MonitorModes", Arg->VideoPresentTargetId << 16 | n);
    return STATUS_SUCCESS;
}
