/* SPDX-License-Identifier: GPL-2.0 */
/*
 * maliwddm: the WDDM 1.3 driver for the RK3576's Mali-G52 and its display
 * (milestone M5.2, docs/M5.2-PLAN.md). The display half is malidod's; the
 * GPU half is drivers/common/maligpu; this layer is what WDDM asks of a full
 * driver: allocations in one aperture segment mapped into the Mali MMU, one
 * engine node, submissions of job chains with fences.
 *
 * Shape and several WDDM details follow virtio-win's viogpu3d (BSD-3), the
 * one open WDDM driver with a Mesa d3d10umd user-mode side.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
#include "../common/maligpu.h"
#include "maliwddm_abi.h"
#include "rotate.h"
#include "hdmi_calc.h"
#include "thermal_calc.h"
#include "gpuclk_calc.h"
#include "perfdata_calc.h"
#include "scale.h"
#include "dual_calc.h"

/* The adapter's own power and ACPI target (video.h; dispmprt.h has it in newer WDKs). */
#ifndef DISPLAY_ADAPTER_HW_ID
#define DISPLAY_ADAPTER_HW_ID   0xFFFFFFFFu
#endif

#define MW_TAG                  'dwlM'
#define MW_SEGMENT_ID           1           /* the aperture segment */
#define MW_SEGMENT_SIZE         (1024ULL * 1024 * 1024)
#define MW_QUEUE_DEPTH          16          /* submissions the KMD holds */

/* 1: a render-only adapter (no VidPN sources or children; another device's
 * display-only driver keeps the screen). 0: a full adapter, display and
 * render on GPU0.
 *
 * Render-only is the WDDM pairing for a separate display device, but on
 * this ACPI GPU it never got past StartDevice: dxgkrnl reported the adapter
 * as a POST device (adapter type 0x9) and refused it with
 * STATUS_GRAPHICS_INVALID_DRIVER_MODEL whatever the DSDT order, resources,
 * INF class or caps (WinPE runs 5-11). A full adapter started (run 6 on the
 * installed Windows); it was black there only because DWM cannot render
 * through WARP, and WinPE has no DWM. */
#ifndef MW_RENDER_ONLY
#define MW_RENDER_ONLY          0
#endif
#define MW_W2(x)                L##x
#define MW_W(x)                 MW_W2(x)

/* ---- objects ---- */

typedef struct _MW_ALLOCATION {
    SIZE_T Size;
    ULONG Pages;
    ULONG Flags;                /* MW_ALLOC_* */
    ULONG64 GpuVa;              /* reserved at creation, fixed for life */
    BOOLEAN Mapped;             /* resident: pages mapped at GpuVa */
    /* Standard allocations (primary, shadow, staging) the runtime creates. */
    UINT Width, Height, Pitch;
    D3DDDIFORMAT Format;
    /* While mapped: the backing pages VidMm handed MAP_APERTURE_SEGMENT,
     * which is how a present reaches the pixels on the CPU. */
    PMDL Mdl;
    ULONG MdlPage;
    /* A primary the display controller can read: its IO address while it
     * is mapped (scanout.c), else 0. */
    ULONG ScanIova;
    /* memory.c's accounting: the per-process entry (index + 1, 0 none)
     * and whether the allocation is counted as resident. */
    ULONG MemSlot;
    BOOLEAN MemResident;
} MW_ALLOCATION;

/* One process's GPU memory (memory.c). Pid 0 is a free entry. */
typedef struct _MW_MEM_SLOT {
    volatile LONG64 Pid;
    volatile LONG64 Bytes, ResidentBytes, PeakBytes;
    volatile LONG Count, ResidentCount, Creates;
} MW_MEM_SLOT;

typedef struct _MW_ADAPTER MW_ADAPTER;

/* A DDI's CPU time (MW_ESCAPE_PERF): updated with interlocked operations,
 * since several DDIs, and the same one on several contexts, run at once. */
typedef struct _MW_PROF {
    volatile LONG64 Calls, Ticks, MaxTicks;
} MW_PROF;

/* HDMI hot-plug (hdmi.c, the hardware; hpd.c, what Windows sees). All of it
 * is behind the HdmiHpd setting, a DWORD in Device Parameters. Without the
 * value: 3 on an HDMI boot, 0 when the firmware handed over a DSI panel
 * (MwHpdLevelFor, hdmi_calc.h).
 *   0  nothing here runs and nothing here is allocated
 *   1  sense only: HPD is polled and the EDID read over DDC, the results
 *      go to the registry; Windows is told nothing new
 *   2  report: on an HDMI boot the one child becomes hot-plug aware and its
 *      EDID is the one just read; no mode is ever set (a DSI boot as 1)
 *   3  mode set: the driver programs VOP2 VP0, the HDMI controller and the
 *      HDPTX PHY itself, so HDMI works when plugged after a DSI boot, and a
 *      different monitor gets its own timing; HDMI audio follows the link */
#define MW_EDID_MAX 512
#define MW_HDMI_MODES       48
#define MW_WIRE_DSI         1
#define MW_WIRE_HDMI        2

/* HdmiExtend (docs/DUAL-DISPLAY.md): the second VidPN source. Source 0 is
 * the adapter's own state as it always was (A->Fb, ScanAlloc, the copy
 * thread...); source 1, which exists only with HdmiExtend on a DSI boot,
 * keeps here what a source needs: its target, its framebuffer (the
 * firmware's for the panel, the reserved one for HDMI), the window it is
 * shown through, and its flips. Its flips are scanned out like source 0's
 * (scanout.c MwScan2Flip), copied by the copy thread when they cannot be,
 * and completed by the same emulated vsync (command.c). */
typedef struct _MW_SRC2 {
    BOOLEAN Active;             /* a committed path */
    BOOLEAN Visible;
    BOOLEAN OnRes;              /* its framebuffer is the reserved one */
    ULONG Target;               /* MW_DUAL_PANEL or MW_DUAL_HDMI */
    UINT Width, Height;         /* the committed source mode */
    PHYSICAL_ADDRESS FbPa;
    PUCHAR FbVa;
    UINT FbW, FbH, FbPitch;
    SIZE_T FbSize;
    ULONG WinMst, WinCfg;       /* the window's YRGB_MST register and its port's CFG_DONE; 0 none */
    PHYSICAL_ADDRESS ScanAddr, ShownAddr;
    struct _MW_ALLOCATION *volatile ScanAlloc;
    volatile LONG FlipSeq, ShownSeq;
    volatile BOOLEAN Showing;   /* the window is on a primary, not on FbPa */
    volatile LONG CopyBusy;     /* the vsync DPC asked the copy thread for a copy */
    ULONG RefNum, RefDen;       /* the committed target mode's refresh rate */
    ULONG Flips, ScanFlips, Copies, CopySkips, NotMapped, Invisible, Vsyncs, ToFbs, Blts;
} MW_SRC2;

typedef struct _MW_HDMI {
    ULONG Level;                /* the HdmiHpd setting */
    ULONG Source;               /* HdmiHpdSource: 0 IOC LEVEL_INT, 1 IOC PORT_LEVEL, 2 GPIO4_PC1 pad */
    ULONG PollMs, DebounceMs;
    BOOLEAN Audio;              /* HdmiAudio, on by default at level 3 */
    BOOLEAN Dsi;                /* the firmware's screen is the DSI panel */
    BOOLEAN Expose;             /* the HDMI child is reported hot-plug aware */
    BOOLEAN Switch;             /* level 3: the driver sets modes */
    BOOLEAN Default60;          /* HdmiDefault60: the preferred mode is the largest size at 60 Hz */
    ULONG HdmiChild;            /* the HDMI child's id: 0 on an HDMI boot, 1 next to the panel */
    ULONG Children;

    /* Registers, mapped by hdmi.c; Mapped has a bit for each. */
    PUCHAR Ioc, Gpio4, Vo0Grf, PhyGrf, Phy, Ctl, Cru, Pmu1Cru, Pmu, Vop;
    ULONG Mapped;

    /* The poll thread, and the two locks it shares with the DDIs. HwLock:
     * the controller (DDC and mode sets). StateLock: the EDID and modes.
     * Mutexes, not fast mutexes: the registry trace needs PASSIVE_LEVEL. */
    PKTHREAD Thread;
    KEVENT Wake;
    volatile LONG Stop;
    KMUTEX HwLock, StateLock;
    BOOLEAN Started;

    /* HPD, the poll thread's. Connected is what Windows was last told. */
    BOOLEAN Connected, SeenHigh, Last;
    ULONG Polls, Changes, LastRaw, Edges;
    ULONG64 LastEdge;           /* interrupt time of the last change of the raw level */

    /* What DDC returned (StateLock). */
    UCHAR Edid[MW_EDID_MAX];
    ULONG EdidSize;
    MW_SINK Sink;
    UCHAR BootEdid[128];
    BOOLEAN HaveBootEdid;
    MW_TIMING Modes[MW_HDMI_MODES];
    ULONG ModeCount;

    /* The firmware's timing (Fw), and for a DSI boot the panel's real one. */
    MW_TIMING FwTiming, Panel;
    BOOLEAN HavePanel;

    /* What VP0 drives now. */
    ULONG WireKind;
    MW_TIMING Wire;
    ULONG ActiveTarget;
    volatile LONG Busy;         /* a mode set is writing VOP2: no flips */

    /* HdmiDiag (hpd.c): 1 the VOP2's own colour bars on VP0 after a mode
     * set; 2 a test pattern in our framebuffer, and flips kept off it
     * (Hold). After each mode set the poll thread logs what the flips did,
     * at +3 s and +10 s (Chk1_*, Chk2_*), against Base*. */
    ULONG Diag;
    BOOLEAN Exclusive;          /* HdmiExclusive: the panel reported gone while HDMI is there */
    volatile LONG Hold;
    ULONG64 MsetTick;
    ULONG CheckStage;
    /* After the last mode set the poll thread writes Per_* every 5 s for
     * 2 min, then every 30 s; the first event after it that can blank the
     * screen is stamped here at any IRQL and logged as Blk_* (hpd.c). */
    ULONG64 NextPer;
    ULONG PerN;
    volatile LONG64 BlackTick;
    volatile LONG BlackWhy;
    BOOLEAN BlackLogged;
    volatile LONG VisOn, VisOff, RecFunc, IsSupCalls, EnumCalls, StatusCalls, DescCalls;
    volatile LONG LastFlipWH, LastFlipWhy;
    /* IsSupportedVidPn answers by reason (0 supported, 1..5 hpd.c), the
     * last VidPN of two paths (source and target of each, a byte apiece),
     * the first four visibility-off times after a mode set (ms since
     * boot), the vsync interrupt switches, and the bases of the counters
     * the snapshots report as deltas. */
    volatile LONG SupBy[8];
    volatile LONG Sup2Path;
    ULONG VisOffMs[4];
    volatile LONG VisOffN;
    volatile LONG IntOn, IntOff;
    ULONG BaseVsyncs, BasePresents, BasePresentFlips, BaseCreates, BaseDestroys, BaseVisOff;
    /* The refresh rate of the committed target mode, which primaries report
     * in DescribeAllocation at level 3 (0/0: none committed yet). */
    ULONG RefNum, RefDen;
    /* Who moved the window onto A->Fb last (MwScanToFb's callers): 1
     * visibility off, 2 a blt present to the primary, 3 the copy thread, 4
     * a primary unmapped, 5 an IOMMU fault, 6 scan-out switched off, 7 the
     * bugcheck screen. */
    volatile LONG ToFbFrom;
    /* Primaries and presents, for the snapshots. */
    volatile LONG PrimCreates, LastPrimWH, LastPrimPitch, Describes, LastDescWH, LastDescRef;
    volatile LONG PresentBltsH, PresentFillsH, LastPresentFlags, LastPresentDstWH, LastPresentSrcWH;
    ULONG SharedPrimRefNum, SharedPrimRefDen;
    /* A->Fb/FbVa/FbSize change under the CPU writers that do not take
     * CopyLock (a blt present at submission, which can be above
     * PASSIVE_LEVEL, and the clear at visibility off): they count
     * themselves in FbUsers and stay out while FbSwitch is set, and UseFb
     * (hpd.c) waits for them to leave before it changes the three. */
    volatile LONG FbUsers, FbSwitch, FbSkips;
    ULONG BaseFlips, BaseScanned, BaseCopies, BaseNotMapped, BaseInvisible, BaseMismatch, BaseMaps, BaseScanFlips;
    ULONG LastSup, LastEnum, Commits;

    /* The firmware's framebuffer, and the one the driver scans out after a
     * mode set of its own (contiguous, below 4 GB, identity-mapped in the
     * VOP2 IOMMU like the firmware's; scanout.c). */
    DXGK_DISPLAY_INFORMATION FwFb;
    PUCHAR FwFbVa;
    SIZE_T FwFbSize;
    PUCHAR ResVa;
    PHYSICAL_ADDRESS ResPa;
    SIZE_T ResBytes;
    ULONG ResMaxPixels;
    BOOLEAN OnRes;

    ULONG ModeSets, ModeSetFails, Restores, RestoreOks;

    /* HdmiExtend (hpd.c, dual_calc.h). ExtendSetting is the setting as read
     * (0 by default); Extend says two sources were reported, which takes a
     * DSI boot at level 3 with the panel's timing known. PanelVp is the
     * port the panel is on (MW_PANEL_*): VP0 except while both outputs are
     * in use. With Extend off none of this is used. */
    BOOLEAN ExtendSetting, Extend;
    ULONG PanelVp;
    ULONG DualCommits, DualFails, LastDualSup;
    MW_SRC2 S2;
} MW_HDMI;

typedef struct _MW_DEVICE {
    MW_ADAPTER *Adapter;
} MW_DEVICE;

typedef struct _MW_CONTEXT {
    MW_DEVICE *Device;
    /* A submission of this context that was preempted part way (TimeSlice,
     * command.c): its commands, the one to resume at and where in it. The
     * scheduler resubmits it with a new fence; the first submission of the
     * context after that, if it carries the same commands, continues from
     * here instead of running again from the start. ResumeGen is the
     * adapter's ResetGen when this was saved: a reset makes it stale. */
    BOOLEAN ResumeValid;
    ULONG ResumeGen;
    ULONG ResumeCount, ResumeNext;
    ULONG64 ResumeTail;
    MW_COMMAND ResumeCommands[MW_MAX_COMMANDS];
} MW_CONTEXT;

/* What a DMA buffer carries, kept in its private data by Render and found
 * there again by SubmitCommand. */
/* A present the CPU carries out at submission (step d): a blt or a colour
 * fill into the primary (the POST framebuffer) or another allocation. */
#define MW_PRESENT_NONE         0
#define MW_PRESENT_BLT          1
#define MW_PRESENT_FILL         2
#define MW_MAX_RECTS            16

typedef struct _MW_PRESENT_OP {
    ULONG Kind;
    ULONG Color;
    ULONG Rotate;               /* degrees to rotate into the framebuffer (rotate.h), 0 none */
    MW_ALLOCATION *Src, *Dst;
    RECT SrcRect, DstRect;
    ULONG NumRects;             /* 0: DstRect alone */
    RECT Rects[MW_MAX_RECTS];   /* destination sub-rectangles */
} MW_PRESENT_OP;

typedef struct _MW_DMA_PRIVATE {
    ULONG Count;
    ULONG Reserved;
    MW_COMMAND Commands[MW_MAX_COMMANDS];
    MW_PRESENT_OP Present;
} MW_DMA_PRIVATE;

/* A submission the hardware works through, one job chain at a time. */
typedef struct _MW_SUBMISSION {
    ULONG FenceId;
    ULONG Count, Next;          /* commands, and the next one to start */
    MW_COMMAND Commands[MW_MAX_COMMANDS];
    /* TimeSlice (command.c): the context it came from (NULL for paging),
     * where the command Next is to start if it was stopped part way (0:
     * at its Jc), when it was submitted, whether a chain of it has started,
     * and whether it arrived while another context's chain was running. */
    struct _MW_CONTEXT *Ctx;
    ULONG64 ResumeTail;
    ULONG64 SubmitTick;
    BOOLEAN Started, BehindOther;
    ULONG Info;                 /* MW_SUB_*, for the TDR snapshot */
} MW_SUBMISSION;

/* One TDR as the job layer saw it (command.c, TdrSnap). Times in ms
 * before the reset; 0xFFFFFFFF: never. */
#define MW_TDR_SNAPS 4
typedef struct {
    ULONG Node;                 /* 0 3D, 1 copy, 0xFFFFFFFF adapter-wide */
    ULONG Running, Slot, RunMs; /* a chain on the GPU, its slot, for how long */
    ULONG JsStatus, JsHeadLo, JsTailLo, JobRaw, JobPending;
    ULONG QCount, HeadFence, HeadNext, HeadCount, HeadStarted, HeadAgeMs;
    ULONG LastSubmitted, LastCompleted;
    ULONG SinceDoneMs, SinceChainEndMs, SinceIsrMs, SincePollMs, SinceSubmitMs;
    ULONG PreemptPending, PreemptMs, StopAsked, StopMs;
    ULONG GpuClk, MilliC, JobsDone, ClkPending;
    /* Whether dxgkrnl was told (2026-11-25: TDRs with everything done).
     * NotifyPending: a DMA_COMPLETED notified and no DPC since; the last
     * one notified, on which node; the copy node's fence; the vsync and
     * the flips a vsync has not reported yet. */
    ULONG SinceDpcMs, NotifyPending, LastNotifiedFence, LastNotifiedNode;
    ULONG CopySubmits, CopyFence, VsyncOn, SinceVsyncMs;
    ULONG FlipsUnreported, SinceFlipMs, SourceVisible;
    /* Preemption (2026-11-25: dxgkrnl forces one when a context overstays
     * its switch deadline, whatever EnablePreemption says; command.c).
     * Node-0 requests seen, DMA_PREEMPTED sent for node 0, the last
     * request's age and fence, node-1 requests; node-0 submissions and
     * DMA_COMPLETED notifications. */
    ULONG PreemptCalls, PreemptAnswered, SincePreemptMs, LastPreemptFence, CopyPreempts;
    ULONG Node0Submits, Node0Notifies;
    /* The last MW_SUB_LOG node-0 submissions, newest first: fence, what it
     * was (MW_SUB_*), ms since it was submitted and since it was notified
     * complete (0xFFFFFFFF: not notified). */
    ULONG Sub[8][4];
} MW_TDR_SNAP;

#define MW_SUB_LOG 8
#define MW_SUB_CHAINS   0x000000FFu     /* job chains */
#define MW_SUB_PRESENT  0x00000100u     /* carried a present (done on the CPU) */
#define MW_SUB_PAGING   0x00000200u
#define MW_SUB_NOPRIV   0x00000400u     /* no private data of ours */
typedef struct {
    ULONG Fence, Info;
    ULONG64 SubmitTick, DoneTick;
} MW_SUB_REC;

struct _MW_ADAPTER {
    PDEVICE_OBJECT Pdo;
    DXGKRNL_INTERFACE Dxgk;
    ULONG64 Seen;               /* DDIs already traced (see MwRet) */
    ULONG64 Entered;            /* DDIs whose first entry is traced */
    ULONG CallOrder;

    /* Display: the firmware's framebuffer, as malidod. */
    DXGK_DISPLAY_INFORMATION Fb;
    PUCHAR FbVa;
    SIZE_T FbSize;
    BOOLEAN SourceVisible, PathActive;

    /* The monitor's EDID as the firmware read it (GPU0.EDID in the DSDT),
     * EdidSize 0 when the firmware has none (display.c). */
#define MW_EDID_MAX 512
    UCHAR Edid[MW_EDID_MAX];
    ULONG EdidSize;
    /* The firmware's timing, which the target mode reports: from the
     * EDID's detailed timing of the framebuffer's size if there is one,
     * else made up around the framebuffer's size. */
    ULONG TimingTotalW, TimingTotalH;
    ULONG TimingPixelRate;
    /* Source modes smaller than the firmware's, for the VOP2's Esmart0
     * window to scale up to the firmware's timing (scale.h, setting
     * ScaledModes, on by default; docs/DISPLAY-MODES.md). Modes[0] is the
     * firmware's size. SrcWidth and SrcHeight are the committed source
     * mode: the size primaries have. Equal to Fb.Width and Fb.Height unless
     * a smaller mode is committed.
     *
     * The window (scanout.c, MwScanScale): Scaled says Esmart0 is not the
     * firmware's window now; then ScrW x ScrH is what it reads, from the
     * top left of the framebuffer or from a primary of that size, and
     * ScrHow the scaling (MW_SCL_*). Everything that draws into the
     * framebuffer for the screen (the flip copy, presents, rotation, the
     * bugcheck screen) draws ScrW x ScrH there (MwScrW, MwScrH). WinVir is
     * the stride Esmart0 was last given, in 32-bit words. */
    BOOLEAN ScaledModes;
    UINT SrcWidth, SrcHeight;
    MW_SCL_SIZE Modes[MW_SCL_MAX_MODES];
    ULONG ModeCount;
    volatile BOOLEAN Scaled;
    volatile UINT ScrW, ScrH, ScrHow;
    ULONG WinVir;
    ULONG SclApplied, SclFlips;
    BOOLEAN SclResume;
    /* The committed path's content rotation in degrees, counter-clockwise:
     * 0, 90, 180 or 270 (display.c). The source mode and the primaries keep
     * the panel's own size whatever it is; DWM draws its desktop already
     * rotated into them, and the driver rotates only what Windows hands it
     * unrotated (MwFlipDegrees, a Rotate present, the bugcheck screen).
     * RotateFlips (setting, off): a flip at 180 degrees is to be rotated by
     * the driver too, for the case that DWM turns out not to do it. */
    volatile ULONG RotDeg;
    BOOLEAN RotateFlips;
    ULONG RotFlipCopies, RotPresents;

    /* The backlight of a MIPI DSI panel (backlight.c): its PWM channel,
     * mapped from GPU0's second memory resource, which the firmware adds
     * only when the screen it hands over is a lit DSI panel. BlVa NULL: no
     * such resource, and the output is reported as HDMI. Duties are in
     * cycles of the PWM clock; BlLevel is Windows' level in percent (the
     * firmware's until Windows sets one). BlLock guards the rest. */
    PUCHAR BlVa;
    ULONG BlPeriod, BlDuty, BlTarget, BlFrom, BlTick;
    UCHAR BlLevel;
    BOOLEAN BlSmooth, BlOff;
    KSPIN_LOCK BlLock;
    KTIMER BlTimer;
    KDPC BlDpc;
    ULONG BlSets, BlRamps;

    /* GPU. MmuLock serialises the address allocator and the page tables. */
    MK_GPU Gpu;
    FAST_MUTEX MmuLock;
    BOOLEAN GpuUp;

    /* Submissions, shared with the ISR (DxgkCbSynchronizeExecution). */
    MW_SUBMISSION Queue[MW_QUEUE_DEPTH];
    ULONG QHead;                /* ring; QHead is the one running */
    volatile ULONG QCount;
    BOOLEAN Running;            /* a job chain of Queue[QHead] is on the GPU */
    BOOLEAN NeedDpc;            /* a fence completed: queue the DPC */
    ULONG RunningSlot;
    ULONG64 JobStart;           /* interrupt time the running chain started */
    ULONG LastSubmittedFence, LastCompletedFence;
    ULONG JobsDone, JobsFailed, JobsTimedOut, ChainFlag;
    ULONG LastJsStatus, LastFaultStatus;
    ULONG64 LastFaultAddress;

    /* Polls the job status while work is in flight (command.c). */
    KTIMER PollTimer;
    KDPC PollDpc;
    volatile LONG PollArmed;
    volatile BOOLEAN Stopping;
    BOOLEAN Stopped;            /* MwAdapterStop has run since StartDevice (adapter.c) */

    /* VSync, emulated (command.c): the scanout is the firmware framebuffer,
     * which raises no interrupt, but DWM paces every frame on CRTC_VSYNC
     * and its flips complete only on one. ScanAlloc is the primary
     * SetVidPnSourceAddress last made current; ShownAlloc the one whose
     * pixels were last copied to the framebuffer. */
    KTIMER VsyncTimer;
    KDPC VsyncDpc;
    volatile LONG VsyncOn;
    PHYSICAL_ADDRESS ScanAddr;
    struct _MW_ALLOCATION *volatile ScanAlloc;
    struct _MW_ALLOCATION *ShownAlloc;
    /* Every flip, including one to the allocation already on screen: its
     * contents have changed, which is the point of flipping to it. */
    volatile LONG FlipSeq;
    volatile LONG ShownSeq;
    /* The copy runs on a thread of its own (command.c), not in the vsync
     * DPC: at 2560x1440 it took about 50 ms, and for that long the timer
     * missed its ticks and nothing else ran on that processor. The thread
     * holds CopyLock while it reads a primary; freeing or unmapping an
     * allocation takes it too. ShownAddr is the address of the flip whose
     * pixels are on the screen, which is what a vsync reports. With no
     * thread the DPC copies as before. */
    /* Scan-out by the VOP2 through its IOMMU (scanout.c). */
#define MW_SCAN_SLOTS 6
#define MW_SCAN_SLOTS_MAX 10    /* HdmiExtend: two sources' primaries */
    PUCHAR VopVa;
    PVOID ScanTables;
    PHYSICAL_ADDRESS ScanTablesPa;
    ULONG ScanTablesUsed, ScanTablesTotal, ScanSlotBytes;
    struct _MW_ALLOCATION *ScanSlot[MW_SCAN_SLOTS_MAX];
    ULONG ScanSlotCount;        /* MW_SCAN_SLOTS, or MW_SCAN_SLOTS_MAX with HdmiExtend */
    /* Source 0's window: its YRGB_MST register and its port's CFG_DONE.
     * Esmart0 on VP0 always, unless HdmiExtend has the panel on VP2 and
     * source 0 is the panel's (Esmart1). */
    ULONG Win0Mst, Win0Cfg;
    BOOLEAN ScanReady, ScanPaging;
    volatile BOOLEAN ScanShowing;
    volatile LONG ScanOn;
    ULONG ScanFlips, ScanMaps, ScanToFbs, ScanMismatch, ScanNoSlot, ScanFaults, ScanFaultAddr;
    /* The ScanOut setting as read at the start (on unless it is 0), scan-out
     * to be turned on again when the adapter is back in D0, IOMMU faults
     * that turned it off (MwScanCheck), and the last primary refused for
     * its size, to compare with the framebuffer's. */
    BOOLEAN ScanSetting, ScanResume;
    ULONG ScanAutoOff;
    ULONG ScanLastW, ScanLastH, ScanLastPitch;
    /* Which way each flip went (MwSetVidPnSourceAddress): scanned out there,
     * or left to the copy, and why (MwScanWhy). */
    ULONG FlipsScanned, WhyScanOff, WhyNotMapped, WhyRotated, WhyInvisible;
    ULONG LastPath;             /* the last flip shown: 1 scanned out, 2 copied, 3 copied and turned */
    ULONG64 CopyBytesAvoided;
    ULONG64 FlipCallTicks, FlipCallMaxTicks;
    ULONG64 FlipIntervalTicks, FlipIntervalMaxTicks;
    ULONG FlipIntervals;
    /* Flip to the vsync that reports it shown: ShownFlipTick is the flip
     * time of the flip now on the screen, ReportedSeq the last one a vsync
     * has reported. */
    volatile ULONG64 ShownFlipTick;
    LONG ReportedSeq;
    ULONG FlipShows;
    ULONG64 FlipShowTicks, FlipShowMaxTicks;
    ULONG64 PresentBltBytes;
    /* The user-mode drivers' reports (MW_ESCAPE_UMD, adapter.c): sums, and
     * by process, under UmdLock. */
    FAST_MUTEX UmdLock;
    ULONG UmdReports;
    MW_UMD_COUNT Umd[MW_UMD_KINDS];
    MW_UMD_PROC UmdProc[MW_UMD_PROCS];
    ULONG UmdProcNext;
    ULONG GpuClk;               /* (parent << 8) | divider last written here; 0 the firmware's */
    /* Clocks above 600 MHz (adapter.c, GpuClkWrite): the GpuClkHigh
     * setting, the supply as the firmware (GpuRail) and the GpuRailUv
     * setting give it, the chip's OTP offset, and the CRU page kept mapped
     * so a fault can drop the clock from the interrupt. */
    ULONG GpuClkHigh;
    ULONG RailUv;
    ULONG RailSource;           /* 1 firmware, 2 setting, 3 both (the lower) */
    ULONG RailOtpMv, RailOtpMinMhz, RailOtpMaxMhz;
    BOOLEAN RailNotPlain;       /* the firmware read a J, M or S variant */
    BOOLEAN GpuClkHighDefault;  /* GpuClkHigh not set: chosen from the firmware's rail */
    volatile BOOLEAN GpuClkIsHigh;
    PUCHAR CruClkVa;
    volatile ULONG GpuClkFallbacks;
    /* The temperature fallback (adapter.c, ThermDpc; thermal_calc.h): with
     * GpuClkHigh on, the TSADC read once a second; above 85 C the clock is
     * held at 594 MHz until below 75 C for GpuClkRestoreHoldSec.
     * GpuClkWanted is the clock above 600 MHz that was asked for (0 none);
     * a job fault no longer clears it (the fault policy does). The
     * clock writes of GpuClkWrite and the DPC are under ClkLock; the DPC's
     * write itself runs synchronised with the interrupt, where a job fault
     * drops the clock. */
    KSPIN_LOCK ClkLock;
    ULONG GpuClkWanted;
    PUCHAR TsadcVa;             /* mapped only once the CRU says the TSADC runs */
    volatile BOOLEAN ThermOn;   /* the timer runs */
    KTIMER ThermTimer;
    KDPC ThermDpc;
    MW_THERMAL Therm;
    volatile ULONG GpuClkThermalDrops, GpuClkThermalRestores;
    /* The clock switch (gpuclk_calc.h): a change asked for is written
     * between two job chains (MwGpuClkBetweenChains, from Kick) or at once
     * when none runs, never under a running chain. ClkPending is the
     * register's low byte | MW_CLK_PENDING, or 0. All at the interrupt's
     * level (synchronised). */
    volatile ULONG ClkPending;
    volatile ULONG ClkSwitches, ClkDeferred;
    /* The fault policy (thermal_calc.h): faults above 600 MHz, counted at
     * the interrupt, folded into Therm by ThermDpc. RestoreHoldMs is the
     * setting GpuClkRestoreHoldSec. The registry trace of drops, restores
     * and the fault lock is written by a work item (ClkLogItem). */
    /* What the job layer saw when dxgkrnl reset the engine or the adapter
     * (a TDR): taken in SyncReset before anything is reset, written to the
     * trace as Tdr<n>_* (n = count mod MW_TDR_SNAPS) by the work item.
     * docs/PERF.md, "TDRs with no job fault". */
    ULONG TdrNode;              /* the node ResetEngine named; 0xFFFFFFFF: ResetFromTimeout */
    MW_TDR_SNAP Tdr[MW_TDR_SNAPS];
    volatile LONG TdrCount;
    LONG TdrLogged;
    ULONG64 LastIsrTick, LastDoneTick, LastChainEndTick, LastPollTick, LastSubmitTick;
    volatile LONG HighFaultEvents;
    LONG HighFaultsSeen;
    ULONG64 RestoreHoldMs;
    PIO_WORKITEM ClkLogItem;
    volatile LONG ClkLogQueued;
    /* dxgkrnl's performance queries, QAI 24 to 27 (adapter.c,
     * perfdata_calc.h): PerfData the setting (default 1, read at start),
     * PerfSeen one bit per type answered once (24 and 25 are journaled and
     * traced only the first time: they come twice a second while Task
     * Manager is open), PerfAnswers how many of each in all. */
    ULONG PerfData;
    volatile LONG PerfSeen;
    volatile ULONG PerfAnswers[4];
    PKTHREAD CopyThread;
    KEVENT CopyEvent;
    FAST_MUTEX CopyLock;
    volatile LONG CopyBusy;
    volatile BOOLEAN CopyStop;
    PHYSICAL_ADDRESS ShownAddr;
    ULONG CopyDeferred;
    ULONG Vsyncs, Flips, FlipCopies, FlipCopySkips;
    /* Why a flip did or did not reach the framebuffer; logged by the stats
     * escape, which runs at passive level. */
    ULONG CommitsPinned, CommitsUnpinned, CommitsNoPath, CommitsNotOurs;
    ULONG PresentFlips;
    ULONG VisibleOn, VisibleOff, FlipsNullAlloc, FlipsInvisible, LastFlipFlags, LastFlipContexts;

    /* Counters for the trace. */
    ULONG Renders, Presents, Submits;
    ULONG PagingOps[16];
    ULONG Maps, MapFails, Unmaps, LastMapPages, Allocations;
    NTSTATUS LastMapStatus;
    ULONG64 LastMapVa, LastAllocVa;
    ULONG Patches, PatchAllocations, PatchPatchLocations, PatchSegment0;
    ULONG PresentBlts, PresentFills, PresentSkips;
    ULONG64 PatchAddress0;

    /* Where the time goes (MW_ESCAPE_STATS_DATA has the meanings): sums in
     * performance-counter ticks, and the moments they are measured from. */
    ULONG64 PerfHz;
    ULONG64 FlipCopyTicks, FlipWaitTicks, JobTicks, PresentBltTicks, VsyncTicks;
    ULONG64 FlipCopyMaxTicks, FlipWaitMaxTicks, JobMaxTicks, VsyncGapMaxTicks;
    ULONG64 JobStartTick, FlipTick, LastVsyncTick;
    ULONG FlipsWhileBusy;

    /* GPU memory, in bytes and by process (memory.c, MW_ESCAPE_MEMORY).
     * Updated with interlocked operations only: zero is the initial
     * state, and nothing has to be set up in AddDevice. */
    volatile LONG64 MemBytes, MemPeakBytes, MemResidentBytes;
    volatile LONG MemCount, MemResidentCount, MemCreates, MemDestroys, MemCreateFails, MemNoSlot;
    MW_MEM_SLOT MemProc[MW_MEM_PROCS];

    /* Preemption (command.c), setting TimeSlice: the least a chain runs, in
     * ms, before a preemption request soft-stops it; 0 is off and the
     * driver behaves as before it existed. Read once, at the first of
     * StartDevice and the DRIVERCAPS query. The rest is at interrupt level. */
    BOOLEAN SliceRead;
    ULONG Slice;
    BOOLEAN PreemptPending;     /* a request waits for the hardware to stop */
    BOOLEAN StopAsked;          /* a soft-stop was written for the running chain */
    BOOLEAN StopSlowSeen;
    BOOLEAN RunningFlag;        /* JOB_CHAIN_FLAG of the running chain */
    MW_CONTEXT *RunningCtx;
    ULONG PreemptFence;
    ULONG ResetGen;
    ULONG64 PreemptTick;        /* performance counter at the request */
    ULONG64 StopAskedAt;        /* interrupt time the soft-stop was written */
    /* Counters (MW_ESCAPE_STATS_DATA has the meanings). */
    ULONG PreemptRequests, PreemptIdle, PreemptOverlap, PreemptDropped;
    /* Setting PreemptAnswer (default 1): with TimeSlice 0, answer node-0
     * preemption requests at the end of the DMA buffers submitted before
     * them; 0 ignores them as before (dxgkrnl then resets node 0). */
    BOOLEAN PreemptAnswer;
    volatile LONG PreemptCalls;         /* node-0 PreemptCommand calls, any setting */
    ULONG PreemptAnswered;              /* node-0 DMA_PREEMPTED sent */
    volatile LONG64 LastPreemptCallTick;
    volatile ULONG LastPreemptCallFence;
    ULONG Node0Notifies;                /* node-0 DMA_COMPLETED sent */
    ULONG SubLogNext;
    MW_SUB_REC SubLog[MW_SUB_LOG];
    ULONG SoftStops, StopsTaken, StopsRacedDone, StopsAtBoundary, StopSlow;
    ULONG Resumes, ResumeMismatch, ResumeLost, StopBadTail, StopUnasked;
    ULONG64 PreemptTicks, PreemptMaxTicks;
    ULONG64 WaitTicks, WaitMaxTicks, WaitOtherMaxTicks;
    ULONG WaitCount, WaitOtherCount;

    /* m5test prof (MW_ESCAPE_PERF_DATA has the meanings). Prof, the
     * Render and map counts are interlocked; everything after them belongs
     * to the queue and changes only at the device's interrupt level. */
    MW_PROF Prof[MW_PERF_DDIS];
    volatile LONG64 RenderAllocs, RenderCmds, MapPages;
    volatile LONG RenderAllocsMax, RenderCmdsMax;
    ULONG PollMs;               /* setting PollMs: the poll timer's period, 1 (default) to 50 */
    ULONG64 SlotJobs[2], SlotTicks[2], SlotMaxTicks[2];
    ULONG SlotHist[2][MW_PERF_BUCKETS];
    ULONG64 FenceCount, FenceTicks, FenceMaxTicks;
    ULONG FenceHist[MW_PERF_BUCKETS];
    ULONG64 GapCount, GapTicks, GapMaxTicks;
    ULONG GapHist[MW_PERF_BUCKETS];
    ULONG64 LastJobEndTick;     /* when the last chain was collected; 0 once the next has started */
    ULONG ReapIsr, ReapPoll, PollRuns;
    volatile LONG64 NotifyTick; /* first DMA_COMPLETED not yet seen by a DPC */
    volatile LONG64 LastDpcTick;    /* the last MwDpcRoutine (DxgkCbNotifyDpc) */
    volatile ULONG LastNotifiedFence, LastNotifiedNode;     /* the last DMA_COMPLETED */
    ULONG64 DpcCount, DpcTicks, DpcMaxTicks;
    ULONG64 QueueDepthSum;
    ULONG QueueDepthMax, Submissions;
    ULONG RingNext;
    MW_PERF_JOB Ring[MW_PERF_RING];

    /* HDMI hot-plug, all of it behind HdmiHpd (hdmi.c, hpd.c). */
    MW_HDMI Hdmi;
};

/* The picture's size in the framebuffer: the firmware's, unless the window
 * is scaling a smaller source mode up (MwScanScale). Any IRQL. */
static __inline UINT MwScrW(const MW_ADAPTER *A)
{
    return A->Scaled ? A->ScrW : A->Fb.Width;
}

static __inline UINT MwScrH(const MW_ADAPTER *A)
{
    return A->Scaled ? A->ScrH : A->Fb.Height;
}

/* The end of a timed DDI call started at T0. Any IRQL. */
static __inline void MwProfEnd(MW_PROF *P, ULONG64 T0)
{
    LONG64 d = (LONG64)(KeQueryPerformanceCounter(NULL).QuadPart - (LONG64)T0), m;

    InterlockedIncrement64(&P->Calls);
    InterlockedAdd64(&P->Ticks, d);
    while (d > (m = P->MaxTicks) && InterlockedCompareExchange64(&P->MaxTicks, d, m) != m)
        ;
}

/* A running maximum shared between threads. */
static __inline void MwMaxLong(volatile LONG *Max, LONG V)
{
    LONG m;
    while (V > (m = *Max) && InterlockedCompareExchange(Max, V, m) != m)
        ;
}

/* The performance counter, at any IRQL. */
static __inline ULONG64 MwTick(void)
{
    return (ULONG64)KeQueryPerformanceCounter(NULL).QuadPart;
}

/* Add the time since Since to a sum and keep the longest. */
static __inline void MwTickAdd(ULONG64 *Sum, ULONG64 *Max, ULONG64 Since)
{
    ULONG64 d = MwTick() - Since;
    *Sum += d;
    if (d > *Max)
        *Max = d;
}

/* ---- trace: Seen_<ddi>, Fail_<ddi> in the device's registry key ---- */
void MwLog(MW_ADAPTER *A, PCWSTR Name, ULONG Value);
void MwLogHook(void *Owner, PCWSTR Name, ULONG Value);
NTSTATUS MwRet(MW_ADAPTER *A, PCWSTR Name, NTSTATUS St);
void MwEnter(MW_ADAPTER *A, PCWSTR Name);

/* adapter.c: StopDevice's work, from StopDevice and from
 * StopDeviceAndReleasePostDisplayOwnership; once per start. */
void MwAdapterStop(MW_ADAPTER *A);

/* display.c: VidPN and the POST framebuffer */
NTSTATUS MwDisplayStart(MW_ADAPTER *A);
void MwDisplayStop(MW_ADAPTER *A);
DXGKDDI_QUERY_CHILD_RELATIONS MwQueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS MwQueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR MwQueryDeviceDescriptor;
DXGKDDI_ISSUPPORTEDVIDPN MwIsSupportedVidPn;
DXGKDDI_RECOMMENDFUNCTIONALVIDPN MwRecommendFunctionalVidPn;
DXGKDDI_ENUMVIDPNCOFUNCMODALITY MwEnumVidPnCofuncModality;
DXGKDDI_SETVIDPNSOURCEVISIBILITY MwSetVidPnSourceVisibility;
DXGKDDI_COMMITVIDPN MwCommitVidPn;
DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH MwUpdateActiveVidPnPresentPath;
DXGKDDI_RECOMMENDMONITORMODES MwRecommendMonitorModes;
DXGKDDI_QUERYVIDPNHWCAPABILITY MwQueryVidPnHWCapability;
DXGKDDI_SETVIDPNSOURCEADDRESS MwSetVidPnSourceAddress;
DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP MwStopDeviceAndReleasePostDisplayOwnership;
DXGKDDI_SYSTEM_DISPLAY_ENABLE MwSystemDisplayEnable;
DXGKDDI_SYSTEM_DISPLAY_WRITE MwSystemDisplayWrite;

/* backlight.c: a MIPI DSI panel's backlight and the brightness interface */
void MwBacklightStart(MW_ADAPTER *A);
BOOLEAN MwBacklightPresent(MW_ADAPTER *A);
void MwBacklightStop(MW_ADAPTER *A);
void MwBacklightPower(MW_ADAPTER *A, BOOLEAN On);
NTSTATUS MwBacklightQueryInterface(MW_ADAPTER *A, PQUERY_INTERFACE Qi);

/* memory.c: allocations and the aperture segment */
DXGKDDI_CREATEALLOCATION MwCreateAllocation;
DXGKDDI_DESTROYALLOCATION MwDestroyAllocation;
DXGKDDI_DESCRIBEALLOCATION MwDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA MwGetStandardAllocationDriverData;
DXGKDDI_OPENALLOCATIONINFO MwOpenAllocation;
DXGKDDI_CLOSEALLOCATION MwCloseAllocation;
DXGKDDI_BUILDPAGINGBUFFER MwBuildPagingBuffer;
NTSTATUS MwQuerySegment(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info);
NTSTATUS MwEscapeAllocInfo(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_ALLOC_INFO_DATA *D);
void MwEscapeMemory(MW_ADAPTER *A, MW_ESCAPE_MEMORY_DATA *D);

/* command.c: devices, contexts, render, submission, fences */
DXGKDDI_CREATEDEVICE MwCreateDevice;
DXGKDDI_DESTROYDEVICE MwDestroyDevice;
DXGKDDI_CREATECONTEXT MwCreateContext;
DXGKDDI_DESTROYCONTEXT MwDestroyContext;
DXGKDDI_RENDER MwRender;
DXGKDDI_RENDERKM MwRenderKm;
DXGKDDI_PRESENT MwPresent;
DXGKDDI_PATCH MwPatch;
DXGKDDI_SUBMITCOMMAND MwSubmitCommand;
DXGKDDI_PREEMPTCOMMAND MwPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE MwQueryCurrentFence;
DXGKDDI_RESETFROMTIMEOUT MwResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT MwRestartFromTimeout;
DXGKDDI_COLLECTDBGINFO MwCollectDbgInfo;
DXGKDDI_GETNODEMETADATA MwGetNodeMetadata;
DXGKDDI_RESETENGINE MwResetEngine;
DXGKDDI_QUERYENGINESTATUS MwQueryEngineStatus;
DXGKDDI_QUERYDEPENDENTENGINEGROUP MwQueryDependentEngineGroup;
DXGKDDI_CANCELCOMMAND MwCancelCommand;
DXGKDDI_CONTROLINTERRUPT MwControlInterrupt;
DXGKDDI_GETSCANLINE MwGetScanLine;
BOOLEAN MwJobInterrupt(MW_ADAPTER *A);    /* from the ISR, at DIRQL */
ULONG MwSetting(MW_ADAPTER *A, PCWSTR Name, ULONG Default);
/* adapter.c: the GPU's temperature (hotter TSADC channel, 0 = no reading)
 * and clock (Hz, 0 = not known) now; pcw.c: the "RK3576 GPU" counters. */
int MwGpuTempMilliC(MW_ADAPTER *A, long *MilliC);
ULONG MwGpuClkHzNow(MW_ADAPTER *A);
void MwPcwInit(void);
void MwPcwExit(void);
void MwPcwAttach(MW_ADAPTER *A);
void MwPcwDetach(MW_ADAPTER *A);
void MwGpuClkFallback(MW_ADAPTER *A);
void MwGpuClkBetweenChains(MW_ADAPTER *A);
void MwGpuClkResetFault(MW_ADAPTER *A);
void MwTdrLogQueue(MW_ADAPTER *A);
#define MW_CLK_PENDING 0x80000000u
extern PDRIVER_OBJECT MwDriverObject;
void MwSetSetting(MW_ADAPTER *A, PCWSTR Name, ULONG Value);
void MwScanStart(MW_ADAPTER *A);
void MwScanStop(MW_ADAPTER *A);
void MwScanToFb(MW_ADAPTER *A);
BOOLEAN MwScanScale(MW_ADAPTER *A, UINT Width, UINT Height, UINT How);
void MwScanNativeWindow(MW_ADAPTER *A);
BOOLEAN MwScanUsable(MW_ADAPTER *A, struct _MW_ALLOCATION *P);
#define MW_SCAN_WHY_OFF         1   /* scan-out off, or the IOMMU not translating */
#define MW_SCAN_WHY_NOT_MAPPED  2   /* the primary has no VOP2 address */
#define MW_SCAN_WHY_ROTATED     3   /* the driver has to turn it */
#define MW_SCAN_WHY_SIZE        4   /* HdmiHpd 3: entered, but not the size VP0 shows now */
void MwHpdBlackEvent(MW_ADAPTER *A, LONG Why);
BOOLEAN MwHpdFbEnter(MW_ADAPTER *A);
void MwHpdFbLeave(MW_ADAPTER *A);
ULONG MwScanWhy(MW_ADAPTER *A, struct _MW_ALLOCATION *P);
void MwScanCheck(MW_ADAPTER *A);
void MwScanPower(MW_ADAPTER *A, BOOLEAN On);
ULONG MwScanMmuStatus(MW_ADAPTER *A, ULONG Which, ULONG *FaultAddr);
BOOLEAN MwScanFlip(MW_ADAPTER *A, struct _MW_ALLOCATION *P);
/* scanout.c, HdmiExtend's source 1 */
ULONG MwScan2Why(MW_ADAPTER *A, struct _MW_ALLOCATION *P);
BOOLEAN MwScan2Flip(MW_ADAPTER *A, struct _MW_ALLOCATION *P);
void MwScan2ToFb(MW_ADAPTER *A);
void MwScanWinToFb(MW_ADAPTER *A, ULONG Mst, ULONG Cfg, ULONG Pa);
UINT MwFlipDegrees(MW_ADAPTER *A, struct _MW_ALLOCATION *P);
void MwScanMap(MW_ADAPTER *A, struct _MW_ALLOCATION *P, PPFN_NUMBER Pfn, ULONG Pages);
void MwScanUnmap(MW_ADAPTER *A, struct _MW_ALLOCATION *P);
NTSTATUS MwScanSet(MW_ADAPTER *A, MW_ESCAPE_SCANOUT_DATA *D);
void MwCommandInit(MW_ADAPTER *A);
void MwCopyForget(MW_ADAPTER *A, struct _MW_ALLOCATION *Alloc, BOOLEAN Freed);
void MwCommandStop(MW_ADAPTER *A);
void MwArmPoll(MW_ADAPTER *A);
void MwCommandSettings(MW_ADAPTER *A);
NTSTATUS MwPerfSnapshot(MW_ADAPTER *A, MW_ESCAPE_PERF_DATA *D);   /* PASSIVE; the queue's part */

/* hdmi.c: the RK3576 HDMI hardware, ported from edk2-rk3576 */
NTSTATUS MwHdmiMap(MW_ADAPTER *A);
void MwHdmiUnmap(MW_ADAPTER *A);
BOOLEAN MwHdmiPowered(MW_ADAPTER *A);
BOOLEAN MwHdmiReadHpd(MW_ADAPTER *A, ULONG *Raw);
NTSTATUS MwHdmiReadEdid(MW_ADAPTER *A, UCHAR *Buf, ULONG Max, ULONG *Len);
NTSTATUS MwHdmiModeSet(MW_ADAPTER *A, const MW_TIMING *T, ULONG FbAddr, ULONG Pitch);
NTSTATUS MwHdmiDsiRestore(MW_ADAPTER *A, ULONG FbAddr, ULONG Pitch);
NTSTATUS MwHdmiPanelToVp2(MW_ADAPTER *A, ULONG FbAddr, ULONG Pitch, BOOLEAN FromVp0);
void MwHdmiVp2Park(MW_ADAPTER *A);
ULONG MwHdmiVpRawStatus(MW_ADAPTER *A);

/* hpd.c: hot-plug as Windows sees it */
void MwHpdSettings(MW_ADAPTER *A);
NTSTATUS MwHpdReserve(MW_ADAPTER *A);
void MwHpdStart(MW_ADAPTER *A);
void MwHpdStop(MW_ADAPTER *A);
void MwHpdDisplayStop(MW_ADAPTER *A);
NTSTATUS MwHpdQueryChildRelations(MW_ADAPTER *A, PDXGK_CHILD_DESCRIPTOR C, ULONG Size);
NTSTATUS MwHpdQueryChildStatus(MW_ADAPTER *A, PDXGK_CHILD_STATUS S);
NTSTATUS MwHpdQueryDeviceDescriptor(MW_ADAPTER *A, ULONG ChildUid, PDXGK_DEVICE_DESCRIPTOR D);
NTSTATUS MwHpdIsSupportedVidPn(MW_ADAPTER *A, DXGKARG_ISSUPPORTEDVIDPN *Arg);
NTSTATUS MwHpdEnumVidPnCofuncModality(MW_ADAPTER *A, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *Arg);
NTSTATUS MwHpdCommitVidPn(MW_ADAPTER *A, const DXGKARG_COMMITVIDPN *Arg);
NTSTATUS MwHpdRecommendMonitorModes(MW_ADAPTER *A, const DXGKARG_RECOMMENDMONITORMODES *Arg);
/* hpd.c, HdmiExtend's source 1 (display.c, command.c, memory.c) */
NTSTATUS MwDualSetAddress(MW_ADAPTER *A, const DXGKARG_SETVIDPNSOURCEADDRESS *Arg);
NTSTATUS MwDualVisibility(MW_ADAPTER *A, BOOLEAN Visible);
ULONG MwDualSourceOfAlloc(MW_ADAPTER *A, const struct _MW_ALLOCATION *P);
void MwDualCopy(MW_ADAPTER *A);
static __inline BOOLEAN MwDualOn(const MW_ADAPTER *A)
{
    return A->Hdmi.Extend;
}
/* display.c's, for hpd.c */
UINT MwPathDegrees(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION R);
void MwSetRotation(MW_ADAPTER *A, UINT Deg, PCWSTR Why);
