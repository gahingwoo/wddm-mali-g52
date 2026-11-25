/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The decisions behind maliwddm's WDDM 2.x answers, as plain C with no WDK
 * types, so that tests/wddm2-host can check them on the host (CI, Linux).
 * The driver (wddm2x.c, memory.c, version.c) fills the WDK structures from
 * these values and nothing else; a rule the host test checks is therefore
 * a rule the driver keeps.
 *
 * Where the rules come from (docs/WDDM2.md, sections 12 and 15):
 *   - Microsoft's DDI reference: DXGK_PHYSICALADAPTERCAPS (the handle is
 *     the one dxgkrnl passed as DXGKRNL_INTERFACE::DeviceHandle),
 *     DXGK_QUERYSEGMENTOUT4 (two calls, the first with NbSegment 0),
 *     DXGK_SEGMENTFLAGS, DXGK_QUERYADAPTERINFOTYPE;
 *   - the interface facts Jamie (claude-g615) and mr4g (codex-g610) found on
 *     hardware in github.com/awesomefroggy29/Panorama, used here with their
 *     permission (2026-11-22): the paging node is a COPY node of its own,
 *     never node 0 and never past NumExecutionNodes; the GpuMmu bit agrees
 *     everywhere it is reported; PageTableAlignmentInBytes is not 0.
 */
#pragma once

/* ---- what DriverEntry does with WddmProbe (version.c) ---- */

#define MW2_WDDM_1_3        0x1300u
#define MW2_WDDM_2_0        0x2000u

/* The version without a WddmProbe value, from 2026-11-24 (WDDM2.md 16.13):
 * 2.0, the run G configuration. WddmProbe 0x1300 forces 1.3. */
#define MW2_WDDM_DEFAULT    MW2_WDDM_2_0

#define MW2_RESULT_NONE         0   /* 1.3 is the version wanted (WddmProbe 0x1300, or a 1.3 default) */
#define MW2_RESULT_ACTIVE       1   /* a 2.x version in effect, WddmProbe's or the default */
#define MW2_RESULT_FELL_BACK    2   /* the last boot with it never confirmed: 1.3 */
#define MW2_RESULT_UNKNOWN      3   /* a value that is no WDDM version: 1.3 */
#define MW2_RESULT_NO_KEY       4   /* no service key path: 1.3 */
#define MW2_RESULT_ARM_FAILED   5   /* the arming could not be written and read back: 1.3 */
#define MW2_RESULT_STAYED_BACK  6   /* the default fell back earlier with this build: 1.3 until
                                     * the package changes or wddm-probe resets it */

typedef struct {
    unsigned Want;          /* Parameters\WddmProbe (0 = none: the default) */
    unsigned Default;       /* MW2_WDDM_DEFAULT */
    unsigned Known;         /* Mw2Wanted(Want, Default) names a version this driver knows */
    unsigned Armed;         /* Parameters\WddmProbeArmed */
    unsigned Keep;          /* Parameters\WddmProbeKeep */
    unsigned FellBack;      /* Parameters\WddmProbeFellBack */
    unsigned FellBackBuild; /* Parameters\WddmProbeFellBackBuild */
    unsigned Build;         /* MW_BUILD of this driver */
    /* How far the last armed boot got (Parameters\WddmProbeStage, MW2_STAGE_*)
     * and the unconfirmed boots in a row before it (WddmProbeHardMisses,
     * WddmProbeSoftMisses). Only read when Armed says it never confirmed. */
    unsigned Stage;
    unsigned HardMisses, SoftMisses;
} MW2_BOOT_IN;

typedef struct {
    unsigned Result;        /* MW2_RESULT_* before arming */
    unsigned Wanted;        /* the version wanted: WddmProbe, or the default */
    unsigned FallBack;      /* WddmProbeArmed = 0, WddmProbeFellBack = Wanted, WddmProbeFellBackBuild =
                             * Build; and delete WddmProbe if it was set */
    unsigned Arm;           /* write WddmProbeArmed = Wanted, flush, read it back */
    /* The last armed boot, when it never confirmed (MW2_CLASS_*), the miss
     * counts to write back (WddmProbeHardMisses, WddmProbeSoftMisses), and
     * with FallBack why (WddmProbeFellBackReason, MW2_REASON_*). */
    unsigned Class;
    unsigned HardMisses, SoftMisses;
    unsigned Reason;
} MW2_BOOT_OUT;

/* How far an armed 2.x boot got (version.c writes WddmProbeStage, flushed,
 * at each step). A power cut or a reset leaves the last one written. */
#define MW2_STAGE_ARMED         1u  /* DriverEntry armed it */
#define MW2_STAGE_STARTED       2u  /* the adapter started (the watchdog's first second) */
#define MW2_STAGE_PRESENTED     3u  /* DWM presented: MW2_STAGE_PRESENTS flips or presents */
#define MW2_STAGE_TDR_STORM     15u /* MW2_TDR_STORM TDRs before the confirm */
#define MW2_STAGE_PRESENTS      10u
#define MW2_TDR_STORM           3u

/* The last armed boot that never confirmed, from its stage. */
#define MW2_CLASS_NONE          0u  /* it confirmed, or nothing was armed */
#define MW2_CLASS_NO_PRESENT    1u  /* never showed a frame: a hang, a bugcheck, a black screen, or a cut that early */
#define MW2_CLASS_TDR_STORM     2u  /* frames, then TDR after TDR */
#define MW2_CLASS_UNCONFIRMED   3u  /* frames, and then nothing: a power cut, a reset, a later crash */

/* Why the default fell back (WddmProbeFellBackReason; m5test wddm, the
 * "WDDM fallback reason" counter). */
#define MW2_REASON_NONE         0u
#define MW2_REASON_NO_PRESENT   1u  /* MW2_HARD_LIMIT boots in a row without a frame */
#define MW2_REASON_TDR_STORM    2u  /* the same, the last of them a TDR storm */
#define MW2_REASON_UNCONFIRMED  3u  /* MW2_MISS_LIMIT unconfirmed boots in a row */
#define MW2_REASON_PROBE        4u  /* an explicit WddmProbe that never confirmed (one-shot) */
#define MW2_REASON_FORCED       5u  /* not a fallback: WddmProbe 0x1300 asks for 1.3 */

/* The default falls back after MW2_HARD_LIMIT failed boots in a row (no
 * frame, or a TDR storm), or MW2_MISS_LIMIT unconfirmed boots of any kind
 * in a row: a boot that showed frames and was cut counts, but alone
 * never falls back. */
#define MW2_HARD_LIMIT          2u
#define MW2_MISS_LIMIT          3u

static __inline unsigned Mw2ClassOf(unsigned Stage)
{
    if (Stage == MW2_STAGE_TDR_STORM)
        return MW2_CLASS_TDR_STORM;
    if (Stage >= MW2_STAGE_PRESENTED)
        return MW2_CLASS_UNCONFIRMED;
    return MW2_CLASS_NO_PRESENT;
}

static __inline unsigned Mw2Wanted(unsigned Want, unsigned Default)
{
    return Want != 0 ? Want : Default;
}

/* The first half: what the registry says. A 2.x version is armed before it
 * is used, every boot. A boot that never confirms leaves WddmProbeArmed set
 * and its stage (how far it got) behind.
 *
 * An explicit WddmProbe stays one-shot: one unconfirmed boot falls back,
 * whatever its stage (MW2_REASON_PROBE).
 *
 * The default (no WddmProbe) tells a failure from a cut (2026-11-24: one
 * 2.0 boot cut by the power switch before the confirm sent a working board
 * to 1.3 for good): a boot that never showed a frame or ended in a TDR storm
 * is a failure, one that showed frames and then stopped is only
 * unconfirmed. It falls back after MW2_HARD_LIMIT failures in a row or
 * MW2_MISS_LIMIT unconfirmed boots of any kind in a row; a confirmed boot
 * clears both counts. The fallback is sticky per build as before: a new
 * package (another MW_BUILD) tries it once more; `wddm-probe set` and
 * `wddm-probe default` clear it. */
static __inline void Mw2BootDecide(const MW2_BOOT_IN *In, MW2_BOOT_OUT *Out)
{
    unsigned want = Mw2Wanted(In->Want, In->Default);

    Out->Result = MW2_RESULT_NONE;
    Out->Wanted = want;
    Out->FallBack = 0;
    Out->Arm = 0;
    Out->Class = MW2_CLASS_NONE;
    Out->HardMisses = In->HardMisses;
    Out->SoftMisses = In->SoftMisses;
    Out->Reason = MW2_REASON_NONE;
    if (want == 0 || want == MW2_WDDM_1_3)
        return;
    if (!In->Known) {
        Out->Result = MW2_RESULT_UNKNOWN;
        return;
    }
    if (In->Want == 0 && In->FellBack == want && In->FellBackBuild == In->Build && !In->Keep) {
        Out->Result = MW2_RESULT_STAYED_BACK;
        return;
    }
    if (In->Armed == want && !In->Keep) {
        Out->Class = Mw2ClassOf(In->Stage);
        if (In->Want != 0) {
            Out->Result = MW2_RESULT_FELL_BACK;
            Out->FallBack = 1;
            Out->Reason = MW2_REASON_PROBE;
            Out->HardMisses = Out->SoftMisses = 0;
            return;
        }
        if (Out->Class == MW2_CLASS_UNCONFIRMED) {
            Out->SoftMisses = In->SoftMisses + 1;
        } else {
            Out->HardMisses = In->HardMisses + 1;
        }
        if (Out->HardMisses >= MW2_HARD_LIMIT || Out->HardMisses + Out->SoftMisses >= MW2_MISS_LIMIT) {
            Out->Result = MW2_RESULT_FELL_BACK;
            Out->FallBack = 1;
            Out->Reason = Out->HardMisses >= MW2_HARD_LIMIT ?
                          (Out->Class == MW2_CLASS_TDR_STORM ? MW2_REASON_TDR_STORM : MW2_REASON_NO_PRESENT) :
                          MW2_REASON_UNCONFIRMED;
            Out->HardMisses = Out->SoftMisses = 0;
            return;
        }
        /* Not enough to fall back: 2.x once more, armed again. */
    }
    Out->Result = MW2_RESULT_ACTIVE;
    Out->Arm = !In->Keep;
}

/* The second half: with Arm set, the version goes ahead only if the arming
 * reached the registry (read back equal). An unarmed 2.x boot that hangs
 * would hang again on every boot after it. */
static __inline unsigned Mw2BootArmed(unsigned Result, unsigned Arm, unsigned ReadBack, unsigned Want)
{
    if (Result == MW2_RESULT_ACTIVE && Arm && ReadBack != Want)
        return MW2_RESULT_ARM_FAILED;
    return Result;
}

/* The boot journal's size (probe.c): events per boot. A probe the user
 * asked for keeps ProbeJournalMax (default 1500); the default 2.x boot of
 * a daily driver keeps 400, and stops once the boot is confirmed. With
 * eight boots kept, the registry holds at most 8 x 1500 events. */
#define MW2_JOURNAL_PROBE       1500u
#define MW2_JOURNAL_DEFAULT     400u
#define MW2_JOURNAL_BOOTS       8u

static __inline unsigned Mw2JournalMax(unsigned Want, unsigned Configured)
{
    unsigned max = Configured != 0xFFFFFFFFu ? Configured : (Want != 0 ? MW2_JOURNAL_PROBE : MW2_JOURNAL_DEFAULT);
    return max > MW2_JOURNAL_PROBE ? MW2_JOURNAL_PROBE : max;
}

/* When a 2.x boot counts as good (the watchdog, probe.c, at PASSIVE_LEVEL):
 * DWM has drawn and presented, the adapter has been up for a while, no
 * DDI is stuck and no TDR storm. Until 2026-11-24 it took 300 renders and
 * 120 s on the desktop, and a power cut in those two minutes looked like a
 * hang; now it is seconds after the first frames, LogonUI included (a
 * static LogonUI presents rarely, hence few presents asked for). */
#define MW2_CONFIRM_RENDERS     60u
#define MW2_CONFIRM_PRESENTS    MW2_STAGE_PRESENTS
#define MW2_CONFIRM_SECONDS     20u

static __inline unsigned Mw2Confirm(unsigned Renders, unsigned Presents, unsigned Seconds, unsigned Stuck,
                                    unsigned Tdrs)
{
    return Renders >= MW2_CONFIRM_RENDERS && Presents >= MW2_CONFIRM_PRESENTS &&
           Seconds >= MW2_CONFIRM_SECONDS && !Stuck && Tdrs < MW2_TDR_STORM;
}

/* The stage to record now for an armed boot (0: nothing new). */
static __inline unsigned Mw2StageNow(unsigned Recorded, unsigned Presents, unsigned Tdrs)
{
    if (Recorded == MW2_STAGE_TDR_STORM)
        return 0;
    if (Tdrs >= MW2_TDR_STORM)
        return MW2_STAGE_TDR_STORM;
    if (Presents >= MW2_STAGE_PRESENTS && Recorded < MW2_STAGE_PRESENTED)
        return MW2_STAGE_PRESENTED;
    if (Recorded < MW2_STAGE_STARTED)
        return MW2_STAGE_STARTED;
    return 0;
}

/* ---- the adapter the 2.x driver describes ---- */

#define MW2_SHAPE_PHYSICAL  0u  /* physical mode, a copy node as the paging node */
#define MW2_SHAPE_GPUMMU    1u  /* the same and GpuMmu */

#define MW2_ENGINE_3D       1u
#define MW2_ENGINE_COPY     2u

#define MW2_NODE_3D         0u
#define MW2_NODE_COPY       1u
#define MW2_MAX_NODES       2u

typedef struct {
    unsigned Nodes;                 /* DRIVERCAPS.GpuEngineTopology, PHYSICALADAPTERCAPS.NumExecutionNodes */
    unsigned PagingNode;            /* DRIVERCAPS.MemoryManagementCaps.PagingNode, PHYSICALADAPTERCAPS.PagingNodeIndex */
    unsigned VprPagingNode;         /* PHYSICALADAPTERCAPS.VPRPagingNode */
    unsigned Engine[MW2_MAX_NODES]; /* GetNodeMetadata.EngineType, MW2_ENGINE_* */
    unsigned GpuMmu;                /* DRIVERCAPS, PHYSICALADAPTERCAPS.Flags, every node's metadata */
    unsigned VirtualAddressing;     /* DRIVERCAPS.MemoryManagementCaps.VirtualAddressingSupported */
    unsigned PerEngineTdr;          /* DRIVERCAPS.SupportPerEngineTDR */
    unsigned MultiEngineAware;      /* DRIVERCAPS.SchedulingCaps: checked for every WDDMv2 driver */
    unsigned CancelCommandAware;
    unsigned HandleIsDxgkrnls;      /* PHYSICALADAPTERCAPS.DxgkPhysicalAdapterHandle = DeviceHandle */
    unsigned PowerDdis;             /* DxgkDdiCalibrateGpuClock and DxgkDdiSetStablePowerState registered */
    unsigned RotationOffset0;       /* every path's RotationSupport has Offset0 (path-independent rotation) */
    unsigned AcquireHandleData;     /* DxgkCbAcquireHandleData, not the WDDM 1.x DxgkCbGetHandleData */

    /* GpuMmu only */
    unsigned PtLevels, PtIndexBits, PtSize, PtAlign, VaBits;
    unsigned long long InternalVaStart, InternalVaEnd;

    /* QUERYSEGMENT4: the one aperture segment, as QUERYSEGMENT3, plus a budget group */
    unsigned NbSegment;
    unsigned PagingBufferSegmentId;
    unsigned SegAperture, SegCpuVisible, SegCacheCoherent;
    unsigned SegApplicationTarget, SegNonLocalBudget, SegLocalBudget;
} MW2_PLAN;

/* The Mali-G52's MMU translates 35 bits of VA (MMU_FEATURES 0x23); the
 * legacy LPAE walk takes four levels of 9 bits over 4 KB tables. */
#define MW2_PT_LEVELS       4u
#define MW2_PT_INDEX_BITS   9u
#define MW2_PAGE            4096u
#define MW2_HW_VA_BITS      35u

/* ---- the segment's budget group (Plan's Budget argument) ----
 * MW2_BUDGET_NONE: no budget flag, as the 1.3 segment (wddm-probe nobudget).
 * MW2_BUDGET_NONLOCAL: non-local budget group and application target, as
 *   the aperture segment of Jamie's driver; what run F (build 345) had.
 * MW2_BUDGET_LOCAL (the default at 2.x): local budget group and application
 *   target.
 *
 * Why local: dxgmms2 10.0.22621.2428, VIDMM_COMMIT_TELEMETRY::LogMemoryState
 * (RVA 0xE6160, the trap at +0x160), loops over the physical adapters and
 * for each takes GetSegmentGroupState(adapter, D3DKMT_MEMORY_SEGMENT_GROUP_
 * LOCAL) and computes usage (+0x78) * 100 / budget (+0x10), with no test of
 * the budget: `cbnz x6` over `brk #0xf004`, then `udiv x9, x8, x6`. The
 * non-local group is divided only on an adapter it names "Discrete"; on an
 * "Integrated" one, as ours, Local is the only group it looks at, and its
 * budget configuration has an L_ (local only) set next to the LNL_ one. A
 * budget comes from the segments in the group; with our one segment in the
 * non-local group the local budget was 0, and run F bugchecked 0x7E
 * (0xC0000094, integer divide by zero) there. On a UMA GPU all of its
 * memory is the system's, so the one aperture segment is the local group.
 * Parameters\Wddm2Budget 0 gives NONE; Wddm2LocalBudget 0 (wddm-probe
 * nonlocalbudget) gives NONLOCAL, run F's segment again. */
#define MW2_BUDGET_NONE         0u
#define MW2_BUDGET_NONLOCAL     1u
#define MW2_BUDGET_LOCAL        2u

static __inline unsigned Mw2BudgetMode(unsigned Wddm2Budget, unsigned Wddm2LocalBudget)
{
    if (Wddm2Budget == 0)
        return MW2_BUDGET_NONE;
    return Wddm2LocalBudget ? MW2_BUDGET_LOCAL : MW2_BUDGET_NONLOCAL;
}

/* DXGK_SEGMENTFLAGS.Value of the plan's segment, bit for bit as d3dkmddi.h
 * (Aperture 0, CpuVisible 2, CacheCoherent 4, ApplicationTarget 15,
 * LocalBudgetGroup 19, NonLocalBudgetGroup 20): what the journal prints and
 * dxgmms2 tests (VerifyBudgetGroups masks 0x80000 and 0x100000). */
#define MW2_SEGF_APERTURE           0x00000001u
#define MW2_SEGF_CPU_VISIBLE        0x00000004u
#define MW2_SEGF_CACHE_COHERENT     0x00000010u
#define MW2_SEGF_APPLICATION_TARGET 0x00008000u
#define MW2_SEGF_LOCAL_BUDGET       0x00080000u
#define MW2_SEGF_NONLOCAL_BUDGET    0x00100000u

/* Which D3DKMT_MEMORY_SEGMENT_GROUP a segment counts against. */
#define MW2_GROUP_LOCAL     0u
#define MW2_GROUP_NONLOCAL  1u

static __inline unsigned Mw2SegmentFlags(const MW2_PLAN *P)
{
    return (P->SegAperture ? MW2_SEGF_APERTURE : 0) |
           (P->SegCpuVisible ? MW2_SEGF_CPU_VISIBLE : 0) |
           (P->SegCacheCoherent ? MW2_SEGF_CACHE_COHERENT : 0) |
           (P->SegApplicationTarget ? MW2_SEGF_APPLICATION_TARGET : 0) |
           (P->SegLocalBudget ? MW2_SEGF_LOCAL_BUDGET : 0) |
           (P->SegNonLocalBudget ? MW2_SEGF_NONLOCAL_BUDGET : 0);
}

/* The bytes the plan puts in a budget group: the one segment's size if it
 * names the group, else 0. The local group's is the divisor of
 * LogMemoryState above and must not be 0 at 2.x. (VidMm may scale the
 * budget it derives; it is 0 when this is.) */
static __inline unsigned long long Mw2GroupBytes(const MW2_PLAN *P, unsigned Group,
                                                 unsigned long long SegmentSize)
{
    unsigned in = Group == MW2_GROUP_LOCAL ? P->SegLocalBudget : P->SegNonLocalBudget;
    return in ? SegmentSize : 0;
}

static __inline void Mw2Plan(unsigned Wddm, unsigned Shape, unsigned Budget, MW2_PLAN *P)
{
    unsigned i;
    unsigned char *b = (unsigned char *)P;

    for (i = 0; i < sizeof(*P); i++)
        b[i] = 0;
    P->MultiEngineAware = 1;
    P->NbSegment = 1;
    P->PagingBufferSegmentId = 1;
    P->SegAperture = 1;
    P->SegCpuVisible = 1;       /* the shared primary (memory.c) */
    P->SegCacheCoherent = 0;    /* the Mali does not snoop the CPU caches */
    P->Engine[MW2_NODE_3D] = MW2_ENGINE_3D;
    if (Wddm < MW2_WDDM_2_0) {  /* WDDM 1.3: exactly as before */
        P->Nodes = 1;
        P->PagingNode = MW2_NODE_3D;
        return;
    }
    P->Nodes = 2;
    P->Engine[MW2_NODE_COPY] = MW2_ENGINE_COPY;
    P->PagingNode = MW2_NODE_COPY;
    P->VprPagingNode = MW2_NODE_COPY;   /* no VPR; a real node, not a phantom one */
    P->PerEngineTdr = 1;
    P->CancelCommandAware = 1;
    P->HandleIsDxgkrnls = 1;
    P->PowerDdis = 1;           /* Mw2NeedsPowerDdis: every 2.x interface */
    P->RotationOffset0 = 1;     /* Mw2PathIndependentRotation: every 2.x interface */
    P->AcquireHandleData = 1;   /* GetHandleData returns NULL to a WDDMv2 driver (memory.c) */
    if (Budget != MW2_BUDGET_NONE) {
        P->SegApplicationTarget = 1;
        if (Budget == MW2_BUDGET_NONLOCAL)
            P->SegNonLocalBudget = 1;
        else
            P->SegLocalBudget = 1;
    }
    if (Shape != MW2_SHAPE_GPUMMU)
        return;
    P->GpuMmu = 1;
    P->VirtualAddressing = 1;
    P->PtLevels = MW2_PT_LEVELS;
    P->PtIndexBits = MW2_PT_INDEX_BITS;
    P->PtSize = MW2_PAGE;
    P->PtAlign = MW2_PAGE;
    P->VaBits = MW2_PT_LEVELS * MW2_PT_INDEX_BITS + 12;
    P->InternalVaStart = 0x100000000ULL;                        /* above Panfrost's 32 MB..4 GB */
    P->InternalVaEnd = (1ULL << MW2_HW_VA_BITS) - 1;
}

/* ---- the DDIs dxgkrnl insists on by interface version ----
 * dxgkrnl 10.0.22621.2428 (the 22631 binary), DXGADAPTER::Initialize, right
 * after ADAPTER_DISPLAY::Initialize has asked QAI 16, on a dxgmms2 adapter
 * with a render core:
 *   "Driver is compiled against DXGKDDI_INTERFACE_VERSION_WDDM2_0_M2_2_1 or
 *    greater, but does not fill in the pfnCalibrateGpuClock or
 *    pfnSetStablePowerState DDI."
 * The test is DRIVER_INITIALIZATION_DATA.Version >= 0x5008 and either
 * pointer NULL, and the adapter start fails. Every 2.x interface this
 * driver can claim (0x5023 for 2.0 and up) is past it; 1.3's 0x4002 is not.
 * Read from the public symbols of the amd64 build of the same version
 * (docs/WDDM2.md 16.6, tools/re). */
#define MW2_DDI_WDDM2_0_M2_2_1  0x5008u

static __inline unsigned Mw2NeedsPowerDdis(unsigned DdiInterface)
{
    return DdiInterface >= MW2_DDI_WDDM2_0_M2_2_1;
}

/* ---- path-independent rotation: the Offset bits of a path ----
 * From interface 0x4003 (DXGKDDI_INTERFACE_VERSION_WDDM1_3_PATH_INDEPENDENT_
 * ROTATION) on, a present path's RotationSupport also carries Offset0..270,
 * the rotation the path adds to its clone group's. dxgkrnl 10.0.22621.2428,
 * DMMVIDPNPRESENTPATH::SetRotationSupport (pfnUpdatePathSupportInfo), with
 * test signing on and the driver past that interface, bugchecks 0x113
 * VIDEO_DXGKRNL_FATAL_ERROR, P1 0x1C, P2 = the RotationSupport bits:
 *   - the primary clone path (a path alone is one) without Offset0: P3 1;
 *   - the primary clone path with Offset90, 180 or 270: P3 2;
 *   - any other path with no Offset bit at all: P3 0.
 * The 2026-11-23 build 325 boot B: P2 0xF (Identity and Rotate90..270, no
 * Offset), P3 1, inside our EnumVidPnCofuncModality. Offset0 alone satisfies
 * all three: this driver never rotates one clone against another. 1.3's
 * 0x4002 is below the interface, and its paths stay without Offset bits. */
#define MW2_DDI_WDDM1_3_PATH_INDEPENDENT_ROTATION  0x4003u
#define MW2_ROTATION_OFFSET0                       0x10u   /* bit 4 of D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT */

static __inline unsigned Mw2PathIndependentRotation(unsigned DdiInterface)
{
    return DdiInterface >= MW2_DDI_WDDM1_3_PATH_INDEPENDENT_ROTATION;
}

/* The rule above, on the bits as dxgkrnl tests them. 0 = accepted. */
static __inline unsigned Mw2RotationSupportFault(unsigned Bits, unsigned PrimaryClonePath)
{
    if (PrimaryClonePath)
        return !(Bits & 0x10u) ? 1u : (Bits & 0xE0u) ? 2u : 0u;
    return (Bits & 0xF0u) ? 0u : 3u;    /* dxgkrnl's P3 0, shifted so 0 means none */
}

/* ---- DXGKQAITYPE_QUERYSEGMENT4: which of the two calls this is ----
 * Microsoft: "First time, NbSegment will be set to 0. The driver should
 * return STATUS_SUCCESS and set NbSegment ... without accessing any other
 * member. Second time, the driver will be called with the reported number
 * of segments and the driver should fill all required information." */
#define MW2_SEG_COUNT       0   /* write NbSegment only */
#define MW2_SEG_FILL        1   /* write the descriptors and the paging-buffer fields */
#define MW2_SEG_TOO_SMALL   2   /* STATUS_BUFFER_TOO_SMALL */

static __inline int Mw2SegmentPass(unsigned NbSegmentIn, int HaveDescriptors,
                                   unsigned long long Stride, unsigned long long MinStride,
                                   unsigned Ours)
{
    if (NbSegmentIn == 0)
        return MW2_SEG_COUNT;
    if (!HaveDescriptors || NbSegmentIn < Ours || Stride < MinStride)
        return MW2_SEG_TOO_SMALL;
    return MW2_SEG_FILL;
}

/* ---- DXGKQAITYPE_HISTORYBUFFERPRECISION (10): one answer per node ----
 * Microsoft: OutputDataSize "is sizeof(DXGKARG_HISTORYBUFFERPRECISION) *
 * m_DriverCaps.GpuEngineTopology.NbAsymetricProcessingNodes". With two
 * nodes dxgkrnl 22631 asked with 8 bytes, and build 310 filled only the
 * first: node 1's precision was whatever the buffer held, and 0 means
 * "call DxgkDdiFormatHistoryBuffer", which this driver does not have.
 * Every whole entry gets the answer; 0 entries is BUFFER_TOO_SMALL. */
static __inline unsigned Mw2PerNodeEntries(unsigned long long OutputSize, unsigned long long EntrySize)
{
    return EntrySize == 0 ? 0u : (unsigned)(OutputSize / EntrySize);
}
