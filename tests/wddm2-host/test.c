// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/wddm2plan.h on the host: the rules maliwddm's WDDM 2.x
 * answers must keep, and the one-shot fallback over sequences of boots.
 *
 * The adapter rules are Microsoft's (DDI reference: DXGK_PHYSICALADAPTERCAPS,
 * DXGK_QUERYSEGMENTOUT4, DXGK_SEGMENTFLAGS) and those Jamie (claude-g615)
 * and mr4g (codex-g610) found on hardware in their WinMali drivers
 * (github.com/awesomefroggy29/Panorama), used with their permission. Each
 * CHECK names the rule. docs/WDDM2.md section 15 lists them.
 */
#include <stdio.h>
#include <string.h>
#include "../../drivers/maliwddm/wddm2plan.h"

static int fails, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fails++;                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* ---- the adapter ---- */

static void check_plan(unsigned wddm, unsigned shape, unsigned budget)
{
    MW2_PLAN p;

    Mw2Plan(wddm, shape, budget, &p);
    const char *w = wddm >= MW2_WDDM_2_0 ? "2.x" : "1.3";

    /* Every WDDMv2 driver: dxgkrnl checks SchedulingCaps.MultiEngineAware. */
    CHECK(p.MultiEngineAware, "%s shape %u: MultiEngineAware set", w, shape);
    CHECK(p.Nodes >= 1 && p.Nodes <= MW2_MAX_NODES, "%s: 1..%u nodes, got %u", w, MW2_MAX_NODES, p.Nodes);
    CHECK(p.Engine[MW2_NODE_3D] == MW2_ENGINE_3D, "%s: node 0 is the 3D node", w);
    /* The paging node names a node that exists (past the end is a phantom
     * node, 0xD1 in VidSch later). */
    CHECK(p.PagingNode < p.Nodes, "%s: paging node %u < nodes %u", w, p.PagingNode, p.Nodes);
    CHECK(p.NbSegment == 1, "one segment");
    CHECK(p.PagingBufferSegmentId >= 1 && p.PagingBufferSegmentId <= p.NbSegment,
          "PagingBufferSegmentId is 1-based and within NbSegment");
    CHECK(p.SegAperture && p.SegCpuVisible && !p.SegCacheCoherent,
          "the 1.3 aperture segment: aperture, CPU visible (shared primary), not coherent");
    CHECK(!(p.SegLocalBudget && p.SegNonLocalBudget), "at most one budget group");

    if (wddm < MW2_WDDM_2_0) {
        /* 1.3 is byte for byte what it was: one node, nothing new. */
        CHECK(p.Nodes == 1 && p.PagingNode == 0, "1.3: one node, paging on it");
        /* The 1.3 segment, whatever the budget knobs say: aperture and CPU
         * visible (0x5), no group, one segment, paging buffers in it. */
        CHECK(Mw2SegmentFlags(&p) == (MW2_SEGF_APERTURE | MW2_SEGF_CPU_VISIBLE),
              "1.3: segment flags 0x5, got 0x%x (budget %u)", Mw2SegmentFlags(&p), budget);
        CHECK(!p.SegLocalBudget, "1.3: no local budget flag");
        CHECK(p.NbSegment == 1 && p.PagingBufferSegmentId == 1, "1.3: one segment, paging buffers in it");
        CHECK(!p.GpuMmu && !p.VirtualAddressing && !p.PerEngineTdr && !p.CancelCommandAware,
              "1.3: no 2.x caps");
        CHECK(!p.SegApplicationTarget && !p.SegNonLocalBudget, "1.3: no budget flags");
        CHECK(!p.PowerDdis, "1.3: the DDI table as before, no CalibrateGpuClock/SetStablePowerState");
        CHECK(!p.RotationOffset0, "1.3: paths without Offset bits, as before");
        CHECK(!p.AcquireHandleData, "1.3: DxgkCbGetHandleData as before");
        return;
    }
    /* The paging node is a COPY node of its own: naming the 3D node made
     * dxgkrnl give up at StartDevice (Jamie, Win11 ARM64), and it is what the
     * first 2.0 probe did before hanging at the boot spinner. */
    CHECK(p.Nodes == 2, "2.x: two nodes");
    CHECK(p.PagingNode != MW2_NODE_3D, "2.x: paging node is not the 3D node");
    CHECK(p.Engine[p.PagingNode] == MW2_ENGINE_COPY, "2.x: paging node is a COPY engine");
    CHECK(p.VprPagingNode < p.Nodes, "2.x: VPRPagingNode %u names a real node", p.VprPagingNode);
    CHECK(p.PerEngineTdr, "2.x: SupportPerEngineTDR (WDDM 1.2 table, both WinMali drivers)");
    /* DXGK_PHYSICALADAPTERCAPS: "Handle, which is passed to the kernel mode
     * driver as DXGKRNL_INTERFACE::DeviceHandle in DxgkDdiStartDevice". */
    CHECK(p.HandleIsDxgkrnls, "2.x: DxgkPhysicalAdapterHandle is dxgkrnl's DeviceHandle");
    /* dxgkrnl 22621.2428 DXGADAPTER::Initialize: interface >= 0x5008 without
     * both DDIs fails the start (the 2026-11-23 0x139 boots). */
    CHECK(p.PowerDdis, "2.x: CalibrateGpuClock and SetStablePowerState registered");
    /* DMMVIDPNPRESENTPATH::SetRotationSupport: 0x113/0x1C (build 325). */
    CHECK(p.RotationOffset0, "2.x: Offset0 on every path (path-independent rotation)");
    /* DxgGetHandleDataCB: NULL for a WDDMv2 driver (build 330, black panel). */
    CHECK(p.AcquireHandleData, "2.x: allocations resolved with DxgkCbAcquireHandleData");
    CHECK(p.SegApplicationTarget == (budget != MW2_BUDGET_NONE), "2.x: application target with any budget group");
    CHECK(p.SegNonLocalBudget == (budget == MW2_BUDGET_NONLOCAL), "2.x: non-local group only for NONLOCAL");
    CHECK(p.SegLocalBudget == (budget == MW2_BUDGET_LOCAL), "2.x: local group for LOCAL");
    /* dxgmms2 22621.2428 LogMemoryState: usage * 100 / the local group's
     * budget, unguarded (run F, 0x7E / 0xC0000094). The default must give
     * the local group bytes; the one-shot knobs may reproduce run F. */
    if (budget == MW2_BUDGET_LOCAL) {
        CHECK(Mw2GroupBytes(&p, MW2_GROUP_LOCAL, 1ull << 30) != 0, "2.x: the local budget group is not empty");
        CHECK(Mw2SegmentFlags(&p) == 0x88005u, "2.x: segment flags 0x88005, got 0x%x", Mw2SegmentFlags(&p));
    }
    if (budget == MW2_BUDGET_NONLOCAL) {
        CHECK(Mw2GroupBytes(&p, MW2_GROUP_LOCAL, 1ull << 30) == 0, "run F's segment: local budget 0");
        CHECK(Mw2SegmentFlags(&p) == 0x108005u, "run F's segment flags 0x108005, got 0x%x", Mw2SegmentFlags(&p));
    }
    /* GpuMmu: one bit, the same in DRIVERCAPS, PHYSICALADAPTERCAPS and
     * every node's metadata (all three come from p.GpuMmu). */
    CHECK(p.GpuMmu == (shape == MW2_SHAPE_GPUMMU), "2.x shape %u: GpuMmu %u", shape, p.GpuMmu);
    CHECK(p.VirtualAddressing == p.GpuMmu, "VirtualAddressingSupported goes with GpuMmu");
    if (!p.GpuMmu) {
        CHECK(p.PtLevels == 0 && p.VaBits == 0, "physical shape: no page-table description");
        return;
    }
    CHECK(p.PtAlign != 0, "PageTableAlignmentInBytes is not 0 (dxgmms2 divides by it)");
    CHECK(p.PtSize == MW2_PAGE && p.PtAlign == MW2_PAGE, "4 KB tables, 4 KB aligned");
    CHECK(p.VaBits == p.PtLevels * p.PtIndexBits + 12, "VA bits = levels x index bits + 12");
    CHECK(p.PtLevels == 4 && p.PtIndexBits == 9, "the Mali legacy LPAE walk: four levels of 9 bits");
    CHECK(p.InternalVaStart < p.InternalVaEnd, "internal VA range ordered");
    CHECK(p.InternalVaEnd < (1ULL << MW2_HW_VA_BITS), "internal VA below the G52's 35 bits");
    CHECK(p.InternalVaStart >= 0x100000000ULL, "internal VA above Panfrost's 32 MB..4 GB");
}

/* ---- QUERYSEGMENT4's two calls ---- */

static void check_segment_pass(void)
{
    /* The first call has NbSegment 0; nothing else may be touched, even
     * with a pointer present. */
    CHECK(Mw2SegmentPass(0, 0, 0, 64, 1) == MW2_SEG_COUNT, "count pass, no pointer");
    CHECK(Mw2SegmentPass(0, 1, 96, 64, 1) == MW2_SEG_COUNT, "count pass with a pointer");
    CHECK(Mw2SegmentPass(1, 1, 96, 64, 1) == MW2_SEG_FILL, "fill pass");
    CHECK(Mw2SegmentPass(2, 1, 96, 64, 1) == MW2_SEG_FILL, "fill pass, room for more");
    CHECK(Mw2SegmentPass(1, 0, 96, 64, 1) == MW2_SEG_TOO_SMALL, "count but no array");
    CHECK(Mw2SegmentPass(1, 1, 32, 64, 1) == MW2_SEG_TOO_SMALL, "stride too small");
    CHECK(Mw2SegmentPass(1, 1, 96, 64, 2) == MW2_SEG_TOO_SMALL, "fewer than ours");
}

/* ---- QAI 10: one entry per node ---- */

static void check_per_node(void)
{
    for (unsigned w = 0; w < 2; w++) {
        MW2_PLAN p;
        Mw2Plan(w ? MW2_WDDM_2_0 : MW2_WDDM_1_3, 0, 1, &p);
        /* dxgkrnl sizes the buffer as 4 bytes x nodes (22631 asked 8 at 2.0). */
        CHECK(Mw2PerNodeEntries(4ull * p.Nodes, 4) == p.Nodes,
              "QAI 10: every node of %u gets an answer", p.Nodes);
    }
    CHECK(Mw2PerNodeEntries(3, 4) == 0, "QAI 10: too small is no entry");
    CHECK(Mw2PerNodeEntries(8, 0) == 0, "QAI 10: no divide by zero");
}

/* ---- the fallback over boots ---- */

/* How a 2.x boot ends. */
#define B_OK        0   /* confirms */
#define B_NOFRAME   1   /* hangs, bugchecks or is cut before any frame */
#define B_TDRS      2   /* frames, then a TDR storm */
#define B_CUT       3   /* frames, then a power cut or reset before the confirm */

typedef struct {
    unsigned Want, Armed, Keep, FellBack, FellBackBuild, Confirmed;
    unsigned Stage, Hard, Soft, Reason, LastMiss;
    unsigned RegistryBroken;    /* writes do not stick */
    unsigned Build;             /* MW_BUILD of the installed package */
    unsigned Default;           /* MW2_WDDM_DEFAULT */
    unsigned LastFallbackReason;
} REG;

static unsigned known(unsigned v)
{
    return v == MW2_WDDM_2_0 || v == 0x2100u;
}

/* The watchdog over one boot, second by second (probe.c and version.c):
 * the stage written as it goes, the confirm when it comes. */
static void run(REG *r, int how)
{
    unsigned renders = 0, presents = 0, tdrs = 0, stage = MW2_STAGE_ARMED, confirmed = 0;

    for (unsigned t = 1; t <= 300 && !confirmed; t++) {
        unsigned next;
        if (how == B_NOFRAME)
            break;                              /* no tick ever: hung in StartDevice */
        if (t >= 3) {
            renders += 30;
            presents += 5;
        }
        if (how == B_TDRS && t >= 5)
            tdrs = t - 4;
        if (how == B_CUT && t == 12)
            break;                              /* the power switch */
        next = Mw2StageNow(stage, presents, tdrs);
        if (next != 0) {
            stage = next;
            if (!r->RegistryBroken)
                r->Stage = stage;
        }
        if (Mw2Confirm(renders, presents, t, 0, tdrs)) {
            CHECK(how == B_OK, "only a good boot confirms (how %d)", how);
            CHECK(t >= MW2_CONFIRM_SECONDS && t <= MW2_CONFIRM_SECONDS + 5, "confirm at %u s", t);
            confirmed = 1;
            if (!r->RegistryBroken) {
                r->Armed = 0;
                r->Confirmed = Mw2Wanted(r->Want, r->Default);
                r->Hard = r->Soft = 0;
            }
        }
    }
    if (how == B_OK)
        CHECK(confirmed, "a good boot confirms");
}

/* One boot as DriverEntry (version.c) sees it, then the boot itself.
 * Returns the version it ran. */
static unsigned boot(REG *r, int how)
{
    MW2_BOOT_IN in = { r->Want, r->Default, known(Mw2Wanted(r->Want, r->Default)), r->Armed, r->Keep,
                       r->FellBack, r->FellBackBuild, r->Build, r->Stage, r->Hard, r->Soft };
    MW2_BOOT_OUT out;
    unsigned result;

    Mw2BootDecide(&in, &out);
    CHECK(out.Wanted == Mw2Wanted(r->Want, r->Default), "Wanted is WddmProbe or the default");
    if (!r->RegistryBroken) {
        if (out.Class != MW2_CLASS_NONE)
            r->LastMiss = out.Class;
        r->Hard = out.HardMisses;
        r->Soft = out.SoftMisses;
    }
    if (out.FallBack && !r->RegistryBroken) {
        CHECK(out.Reason != MW2_REASON_NONE, "a fallback has a reason");
        r->FellBack = out.Wanted;
        r->FellBackBuild = r->Build;
        r->Reason = out.Reason;
        r->LastFallbackReason = out.Reason;
        if (r->Want != 0)
            r->Want = 0;
        r->Armed = 0;
    }
    if (out.Arm && !r->RegistryBroken) {
        r->Stage = MW2_STAGE_ARMED;
        r->Armed = out.Wanted;
    }
    result = Mw2BootArmed(out.Result, out.Arm, r->Armed, out.Wanted);
    if (result != MW2_RESULT_ACTIVE)
        return MW2_WDDM_1_3;
    run(r, how);
    return out.Wanted;
}

static void reg_init(REG *r, unsigned want)
{
    memset(r, 0, sizeof(*r));
    r->Want = want;
    r->Default = MW2_WDDM_DEFAULT;
    r->Build = 371;
    r->FellBackBuild = 0xFFFFFFFFu;     /* version.c's default when the value is absent */
}

/* `wddm-probe set` and `wddm-probe default` delete these. */
static void reg_reset(REG *r)
{
    r->Armed = 0;
    r->FellBack = 0;
    r->FellBackBuild = 0xFFFFFFFFu;
    r->Confirmed = 0;
    r->Keep = 0;
    r->Stage = r->Hard = r->Soft = r->Reason = r->LastMiss = 0;
}

static void check_stage_confirm(void)
{
    CHECK(Mw2StageNow(MW2_STAGE_ARMED, 0, 0) == MW2_STAGE_STARTED, "first tick: started");
    CHECK(Mw2StageNow(MW2_STAGE_STARTED, 9, 0) == 0, "9 presents: nothing new");
    CHECK(Mw2StageNow(MW2_STAGE_STARTED, 10, 0) == MW2_STAGE_PRESENTED, "10 presents: presented");
    CHECK(Mw2StageNow(MW2_STAGE_PRESENTED, 500, 2) == 0, "two TDRs: nothing new");
    CHECK(Mw2StageNow(MW2_STAGE_PRESENTED, 500, 3) == MW2_STAGE_TDR_STORM, "three: a storm");
    CHECK(Mw2StageNow(MW2_STAGE_TDR_STORM, 500, 0) == 0, "a storm stays");
    CHECK(!Mw2Confirm(1000, 100, 19, 0, 0), "not before 20 s");
    CHECK(!Mw2Confirm(59, 100, 60, 0, 0), "not before 60 renders");
    CHECK(!Mw2Confirm(1000, 9, 60, 0, 0), "not before 10 presents");
    CHECK(!Mw2Confirm(1000, 100, 60, 1, 0), "not while stuck");
    CHECK(!Mw2Confirm(1000, 100, 60, 0, 3), "not after a TDR storm");
    CHECK(Mw2Confirm(60, 10, 20, 0, 2), "60 renders, 10 presents, 20 s, two TDRs: good");
    CHECK(Mw2ClassOf(MW2_STAGE_ARMED) == MW2_CLASS_NO_PRESENT && Mw2ClassOf(MW2_STAGE_STARTED) == MW2_CLASS_NO_PRESENT,
          "armed or started only: no frame");
    CHECK(Mw2ClassOf(MW2_STAGE_PRESENTED) == MW2_CLASS_UNCONFIRMED, "presented: unconfirmed");
    CHECK(Mw2ClassOf(MW2_STAGE_TDR_STORM) == MW2_CLASS_TDR_STORM, "storm");
}

static void check_fallback(void)
{
    REG r;
    unsigned v;

    CHECK(MW2_WDDM_DEFAULT == MW2_WDDM_2_0, "the default is WDDM 2.0");

    /* The 2026-11-24 evening: power cuts during firmware tests. One cut
     * boot, and another: still 2.0. */
    reg_init(&r, 0);
    CHECK(boot(&r, B_OK) == MW2_WDDM_2_0, "default: 2.0");
    CHECK(boot(&r, B_CUT) == MW2_WDDM_2_0, "cut after the first frames");
    CHECK(boot(&r, B_CUT) == MW2_WDDM_2_0 && r.Soft == 1, "the next boot: 2.0, one cut counted");
    CHECK(boot(&r, B_OK) == MW2_WDDM_2_0, "two cuts in a row: still 2.0");
    CHECK(r.Soft == 0 && r.Hard == 0 && r.Armed == 0, "a good boot clears the counts");
    CHECK(r.FellBack == 0, "no fallback");
    /* Three cuts in a row: unconfirmed three times, 1.3. */
    CHECK(boot(&r, B_CUT) == MW2_WDDM_2_0 && boot(&r, B_CUT) == MW2_WDDM_2_0 &&
          boot(&r, B_CUT) == MW2_WDDM_2_0, "three cut boots, each 2.0");
    CHECK(boot(&r, B_OK) == MW2_WDDM_1_3 && r.Reason == MW2_REASON_UNCONFIRMED, "the fourth: 1.3, unconfirmed x3");

    /* No frame once: 2.0 again. Twice: 1.3. */
    reg_init(&r, 0);
    CHECK(boot(&r, B_NOFRAME) == MW2_WDDM_2_0, "no frame");
    CHECK(boot(&r, B_OK) == MW2_WDDM_2_0 && r.LastMiss == MW2_CLASS_NO_PRESENT, "retried at 2.0, and it works");
    CHECK(boot(&r, B_NOFRAME) == MW2_WDDM_2_0 && boot(&r, B_NOFRAME) == MW2_WDDM_2_0, "no frame twice");
    CHECK(boot(&r, B_OK) == MW2_WDDM_1_3 && r.Reason == MW2_REASON_NO_PRESENT, "then 1.3: no frame x2");
    for (int i = 0; i < 6; i++)
        CHECK(boot(&r, i & 1 ? B_OK : B_NOFRAME) == MW2_WDDM_1_3, "sticky for the build (boot %d)", i);
    r.Build++;
    CHECK(boot(&r, B_OK) == MW2_WDDM_2_0, "a new package tries 2.0 again");
    reg_reset(&r);
    CHECK(boot(&r, B_OK) == MW2_WDDM_2_0, "after wddm-probe default: 2.0");

    /* TDR storms. */
    reg_init(&r, 0);
    CHECK(boot(&r, B_TDRS) == MW2_WDDM_2_0 && boot(&r, B_TDRS) == MW2_WDDM_2_0, "two storms");
    CHECK(boot(&r, B_OK) == MW2_WDDM_1_3 && r.Reason == MW2_REASON_TDR_STORM, "1.3, TDR storm");
    /* Mixed: a cut and a no-frame boot, then a cut: three in a row. */
    reg_init(&r, 0);
    CHECK(boot(&r, B_CUT) == MW2_WDDM_2_0 && boot(&r, B_NOFRAME) == MW2_WDDM_2_0 &&
          boot(&r, B_CUT) == MW2_WDDM_2_0, "cut, no frame, cut");
    CHECK(boot(&r, B_OK) == MW2_WDDM_1_3 && r.Reason == MW2_REASON_UNCONFIRMED, "1.3 after three");

    /* An explicit probe stays one-shot. */
    reg_init(&r, MW2_WDDM_2_0);
    CHECK(boot(&r, B_CUT) == MW2_WDDM_2_0, "probe: cut");
    CHECK(boot(&r, B_OK) == MW2_WDDM_1_3 && r.Reason == MW2_REASON_PROBE && r.Want == 0,
          "probe: 1.3 at once, WddmProbe deleted");

    /* Every pattern of eight boots (4^8): no more than two failed 2.x boots
     * in a row, no more than three unconfirmed ones in a row, never 2.0
     * again after a fallback with the same build, and no fallback at all
     * unless a limit was reached. */
    for (unsigned pat = 0; pat < 65536; pat++) {
        unsigned hardRun = 0, missRun = 0, fell = 0, limit = 0;
        reg_init(&r, 0);
        for (int i = 0; i < 8; i++) {
            int how = (pat >> (2 * i)) & 3;
            v = boot(&r, how);
            if (v == MW2_WDDM_2_0) {
                CHECK(!fell, "pattern %04x boot %d: 2.0 after a fallback", pat, i);
                if (how == B_OK) {
                    hardRun = missRun = 0;
                } else {
                    missRun++;
                    if (how != B_CUT)
                        hardRun++;
                    if (hardRun >= MW2_HARD_LIMIT || missRun >= MW2_MISS_LIMIT)
                        limit = 1;
                }
                CHECK(hardRun <= MW2_HARD_LIMIT, "pattern %04x boot %d: %u failed in a row", pat, i, hardRun);
                CHECK(missRun <= MW2_MISS_LIMIT, "pattern %04x boot %d: %u unconfirmed in a row", pat, i, missRun);
            } else {
                CHECK(limit, "pattern %04x boot %d: 1.3 with no limit reached", pat, i);
                fell = 1;
            }
        }
    }

    /* The arming did not reach the registry: 1.3, never an unarmed 2.x. */
    reg_init(&r, 0);
    r.RegistryBroken = 1;
    CHECK(boot(&r, B_NOFRAME) == MW2_WDDM_1_3, "no arming, no 2.0, default too");
    reg_init(&r, MW2_WDDM_2_0);
    r.RegistryBroken = 1;
    CHECK(boot(&r, B_NOFRAME) == MW2_WDDM_1_3, "no arming, no probe");

    /* Forced 1.3: 1.3, nothing armed, nothing written, every boot. */
    reg_init(&r, MW2_WDDM_1_3);
    for (int i = 0; i < 3; i++)
        CHECK(boot(&r, B_NOFRAME) == MW2_WDDM_1_3 && r.Armed == 0 && r.FellBack == 0, "0x1300: 1.3, untouched");
    {
        MW2_BOOT_IN in = { MW2_WDDM_1_3, MW2_WDDM_DEFAULT, 1, 0, 0, 0, 0, 1, 0, 0, 0 };
        MW2_BOOT_OUT out;
        Mw2BootDecide(&in, &out);
        CHECK(out.Result == MW2_RESULT_NONE && !out.Arm && !out.FallBack, "0x1300: result NONE, no arming");
        in.Want = 0;
        in.FellBack = MW2_WDDM_2_0;
        in.FellBackBuild = 1;
        Mw2BootDecide(&in, &out);
        CHECK(out.Result == MW2_RESULT_STAYED_BACK && !out.Arm && !out.FallBack,
              "default fell back with this build: STAYED_BACK, nothing written");
    }

    /* A value that is not a WDDM version. */
    reg_init(&r, 0x1234);
    CHECK(boot(&r, B_NOFRAME) == MW2_WDDM_1_3, "unknown value: 1.3");

    /* Keep = 1 turns the arming off: documented as never for a first test. */
    reg_init(&r, MW2_WDDM_2_0);
    r.Keep = 1;
    CHECK(boot(&r, B_NOFRAME) == MW2_WDDM_2_0 && boot(&r, B_NOFRAME) == MW2_WDDM_2_0, "Keep: no fallback (by design)");

    /* The journal is bounded. */
    CHECK(Mw2JournalMax(0, 0xFFFFFFFFu) == MW2_JOURNAL_DEFAULT, "default boot: 400 events");
    CHECK(Mw2JournalMax(MW2_WDDM_2_0, 0xFFFFFFFFu) == MW2_JOURNAL_PROBE, "a probe: 1500 events");
    CHECK(Mw2JournalMax(0, 100000u) == MW2_JOURNAL_PROBE, "ProbeJournalMax cannot raise it past 1500");
    CHECK(Mw2JournalMax(0, 0) == 0, "ProbeJournalMax 0: no journal");
}

/* ---- what each start-up gives the driver: the plan version.c builds ---- */

static void check_default_plans(void)
{
    MW2_PLAN p13, forced, def, g;

    /* version.c: Mw2Plan(1.3, 0, 0) first; replaced only for a 2.x result. */
    Mw2Plan(MW2_WDDM_1_3, 0, 0, &p13);
    /* Forced 1.3 (WddmProbe 0x1300) and every fallback keep that plan, byte
     * for byte, whatever the 2.x knobs say. */
    for (unsigned shape = 0; shape < 2; shape++)
        for (unsigned b = 0; b < 3; b++) {
            Mw2Plan(MW2_WDDM_1_3, shape, b, &forced);
            CHECK(memcmp(&p13, &forced, sizeof(p13)) == 0, "forced 1.3: the 1.3 plan, shape %u budget %u", shape, b);
        }
    CHECK(p13.Nodes == 1 && Mw2SegmentFlags(&p13) == 0x5u && !p13.PowerDdis && !p13.AcquireHandleData,
          "the 1.3 plan: one node, segment 0x5, the 1.3 DDI table and callbacks");
    /* The default: 2.0 with the registry's defaults (Wddm2Shape 0,
     * Wddm2Budget 1, Wddm2LocalBudget 1, the rest 1), which is run G. */
    Mw2Plan(MW2_WDDM_DEFAULT, MW2_SHAPE_PHYSICAL, Mw2BudgetMode(1, 1), &def);
    CHECK(def.Nodes == 2 && def.PagingNode == MW2_NODE_COPY && !def.GpuMmu, "default: 2.0 physical, copy paging node");
    CHECK(Mw2SegmentFlags(&def) == 0x88005u, "default: run G's segment, flags 0x88005");
    CHECK(def.PowerDdis && def.RotationOffset0 && def.AcquireHandleData, "default: run G's DDIs and callbacks");
    Mw2Plan(MW2_WDDM_2_0, MW2_SHAPE_PHYSICAL, MW2_BUDGET_LOCAL, &g);
    CHECK(memcmp(&def, &g, sizeof(g)) == 0, "default plan == WddmProbe 0x2000 plan (run G)");
}

/* ---- the knobs to the budget group ---- */

static void check_budget_knobs(void)
{
    MW2_PLAN p;

    /* The defaults (Wddm2Budget 1, Wddm2LocalBudget 1) at 2.0: local. */
    CHECK(Mw2BudgetMode(1, 1) == MW2_BUDGET_LOCAL, "defaults: the local group");
    CHECK(Mw2BudgetMode(1, 0) == MW2_BUDGET_NONLOCAL, "nonlocalbudget: run F's group");
    CHECK(Mw2BudgetMode(0, 1) == MW2_BUDGET_NONE && Mw2BudgetMode(0, 0) == MW2_BUDGET_NONE,
          "nobudget: no group, whatever Wddm2LocalBudget says");
    Mw2Plan(MW2_WDDM_2_0, MW2_SHAPE_PHYSICAL, Mw2BudgetMode(1, 1), &p);
    CHECK(Mw2GroupBytes(&p, MW2_GROUP_LOCAL, 1ull << 30) == (1ull << 30),
          "2.0 defaults: the whole 1 GB segment counts as local");
    CHECK(Mw2GroupBytes(&p, MW2_GROUP_NONLOCAL, 1ull << 30) == 0, "2.0 defaults: nothing non-local");
    /* What version.c does before any probe: the 1.3 plan. */
    Mw2Plan(MW2_WDDM_1_3, 0, 0, &p);
    CHECK(Mw2SegmentFlags(&p) == 0x5u, "the 1.3 plan's segment flags 0x5");
    for (unsigned b = 0; b < 2; b++)
        for (unsigned l = 0; l < 2; l++) {
            MW2_PLAN q;
            Mw2Plan(MW2_WDDM_1_3, MW2_SHAPE_PHYSICAL, Mw2BudgetMode(b, l), &q);
            CHECK(memcmp(&p, &q, sizeof(p)) == 0, "1.3: the plan ignores Wddm2Budget %u Wddm2LocalBudget %u", b, l);
        }
}

int main(void)
{
    check_stage_confirm();
    for (unsigned shape = 0; shape <= 1; shape++)
        for (unsigned budget = MW2_BUDGET_NONE; budget <= MW2_BUDGET_LOCAL; budget++) {
            check_plan(MW2_WDDM_1_3, shape, budget);
            check_plan(MW2_WDDM_2_0, shape, budget);
            check_plan(0x2100u, shape, budget);
        }
    /* The interface versions version.c can claim (DXGKDDI_INTERFACE_VERSION_*):
     * 1.3 is below dxgkrnl's 0x5008 test, every 2.x and 3.x one above. */
    CHECK(!Mw2NeedsPowerDdis(0x4002u), "1.3 interface 0x4002 needs neither DDI");
    CHECK(Mw2NeedsPowerDdis(0x5023u), "2.0 interface 0x5023 needs CalibrateGpuClock and SetStablePowerState");
    CHECK(Mw2NeedsPowerDdis(0x5008u), "the 0x5008 boundary itself");
    CHECK(Mw2NeedsPowerDdis(0xB004u), "3.x interfaces too");
    /* Path-independent rotation (dxgkrnl's 0x113/0x1C rule). */
    CHECK(!Mw2PathIndependentRotation(0x4002u), "1.3 interface 0x4002 is before path-independent rotation");
    CHECK(Mw2PathIndependentRotation(0x5023u), "2.0 interface 0x5023 is past it");
    CHECK(Mw2RotationSupportFault(0x0Fu, 1) == 1, "build 325's 0xF on the primary path: dxgkrnl's P3 1");
    CHECK(Mw2RotationSupportFault(0x1Fu, 1) == 0, "0x1F (Offset0) on the primary path is accepted");
    CHECK(Mw2RotationSupportFault(0x1Fu, 0) == 0, "0x1F on a secondary clone path is accepted");
    CHECK(Mw2RotationSupportFault(0x3Fu, 1) == 2, "Offset90 on the primary path: dxgkrnl's P3 2");
    CHECK(Mw2RotationSupportFault(0x0Fu, 0) == 3, "no Offset bit on a secondary path is refused too");
    CHECK(Mw2RotationSupportFault(0x0Fu | MW2_ROTATION_OFFSET0, 1) == 0, "what Mw2xRotationSupport makes of 0xF");
    check_budget_knobs();
    check_default_plans();
    check_segment_pass();
    check_per_node();
    check_fallback();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
