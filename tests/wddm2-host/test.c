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
        CHECK(!p.GpuMmu && !p.VirtualAddressing && !p.PerEngineTdr && !p.CancelCommandAware,
              "1.3: no 2.x caps");
        CHECK(!p.SegApplicationTarget && !p.SegNonLocalBudget, "1.3: no budget flags");
        CHECK(!p.PowerDdis, "1.3: the DDI table as before, no CalibrateGpuClock/SetStablePowerState");
        CHECK(!p.RotationOffset0, "1.3: paths without Offset bits, as before");
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
    CHECK(p.SegApplicationTarget == (budget != 0) && p.SegNonLocalBudget == (budget != 0),
          "2.x: budget flags follow Wddm2Budget %u", budget);
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

/* ---- the one-shot fallback over boots ---- */

typedef struct {
    unsigned Want, Armed, Keep, FellBack, Confirmed;
    unsigned RegistryBroken;    /* writes do not stick */
} REG;

/* One boot as DriverEntry and the watchdog see it. Returns the version
 * that boot ran. Hangs = the 2.x boot never confirms. */
static unsigned boot(REG *r, int hangs)
{
    MW2_BOOT_IN in = { r->Want, r->Want == MW2_WDDM_2_0 || r->Want == 0x2100u, r->Armed, r->Keep };
    MW2_BOOT_OUT out;
    unsigned result;

    Mw2BootDecide(&in, &out);
    if (out.FallBack && !r->RegistryBroken) {
        r->FellBack = r->Want;
        r->Want = 0;
        r->Armed = 0;
    }
    if (out.Arm && !r->RegistryBroken)
        r->Armed = in.Want;
    result = Mw2BootArmed(out.Result, out.Arm, r->Armed, in.Want);
    if (result != MW2_RESULT_ACTIVE)
        return MW2_WDDM_1_3;
    if (!hangs) {
        CHECK(!Mw2Confirm(MW2_CONFIRM_RENDERS, MW2_CONFIRM_SECONDS - 1, 0), "no confirm before 120 s");
        CHECK(!Mw2Confirm(MW2_CONFIRM_RENDERS - 1, 600, 0), "no confirm before 300 renders");
        CHECK(!Mw2Confirm(5000, 600, 1), "no confirm while a DDI is stuck");
        if (Mw2Confirm(5000, 600, 0) && !r->RegistryBroken) {
            r->Armed = 0;
            r->Confirmed = in.Want;
        }
    }
    return in.Want;
}

static void check_fallback(void)
{
    REG r;
    unsigned v;

    /* Set, hang, fall back, stay at 1.3. */
    memset(&r, 0, sizeof(r));
    r.Want = MW2_WDDM_2_0;
    CHECK(boot(&r, 1) == MW2_WDDM_2_0, "first boot runs 2.0");
    CHECK(boot(&r, 0) == MW2_WDDM_1_3, "the boot after a hang is 1.3");
    CHECK(r.FellBack == MW2_WDDM_2_0 && r.Want == 0, "WddmProbeFellBack written, WddmProbe deleted");
    for (int i = 0; i < 5; i++)
        CHECK(boot(&r, 1) == MW2_WDDM_1_3, "and every boot after it");

    /* A good boot confirms; the next boot re-arms; a later hang still
     * costs one boot. */
    memset(&r, 0, sizeof(r));
    r.Want = MW2_WDDM_2_0;
    CHECK(boot(&r, 0) == MW2_WDDM_2_0 && r.Confirmed == MW2_WDDM_2_0 && r.Armed == 0, "good boot confirms");
    CHECK(boot(&r, 1) == MW2_WDDM_2_0 && r.Armed == MW2_WDDM_2_0, "next boot is 2.0 again, armed");
    CHECK(boot(&r, 0) == MW2_WDDM_1_3, "a later hang still falls back");

    /* Over any pattern of hangs, two 2.x boots that both hang are never
     * back to back. */
    for (unsigned pat = 0; pat < 256; pat++) {
        unsigned prevHung = 0;
        memset(&r, 0, sizeof(r));
        r.Want = MW2_WDDM_2_0;
        for (int i = 0; i < 8; i++) {
            int hangs = (pat >> i) & 1;
            v = boot(&r, hangs);
            int hung2x = v == MW2_WDDM_2_0 && hangs;
            CHECK(!(prevHung && hung2x), "pattern 0x%02x boot %d: two hung 2.x boots in a row", pat, i);
            prevHung = hung2x;
        }
    }

    /* The arming did not reach the registry: 1.3, never an unarmed 2.x. */
    memset(&r, 0, sizeof(r));
    r.Want = MW2_WDDM_2_0;
    r.RegistryBroken = 1;
    CHECK(boot(&r, 1) == MW2_WDDM_1_3, "no arming, no probe");

    /* Values that are not a WDDM version, or 1.3 itself. */
    memset(&r, 0, sizeof(r));
    r.Want = 0x1234;
    CHECK(boot(&r, 1) == MW2_WDDM_1_3, "unknown value: 1.3");
    r.Want = MW2_WDDM_1_3;
    CHECK(boot(&r, 1) == MW2_WDDM_1_3, "0x1300: 1.3");

    /* Keep = 1 turns the arming off: documented as never for a first test. */
    memset(&r, 0, sizeof(r));
    r.Want = MW2_WDDM_2_0;
    r.Keep = 1;
    CHECK(boot(&r, 1) == MW2_WDDM_2_0 && boot(&r, 1) == MW2_WDDM_2_0, "Keep: no fallback (by design)");
}

int main(void)
{
    for (unsigned shape = 0; shape <= 1; shape++)
        for (unsigned budget = 0; budget <= 1; budget++) {
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
    check_segment_pass();
    check_per_node();
    check_fallback();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
