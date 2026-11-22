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

#define MW2_RESULT_NONE         0   /* no WddmProbe: WDDM 1.3 */
#define MW2_RESULT_ACTIVE       1   /* WddmProbe in effect */
#define MW2_RESULT_FELL_BACK    2   /* the last boot with it never confirmed: 1.3 */
#define MW2_RESULT_UNKNOWN      3   /* a value that is no WDDM version: 1.3 */
#define MW2_RESULT_NO_KEY       4   /* no service key path: 1.3 */
#define MW2_RESULT_ARM_FAILED   5   /* the arming could not be written and read back: 1.3 */

typedef struct {
    unsigned Want;          /* Parameters\WddmProbe (0 = none) */
    unsigned Known;         /* Want names a version this driver knows */
    unsigned Armed;         /* Parameters\WddmProbeArmed */
    unsigned Keep;          /* Parameters\WddmProbeKeep */
} MW2_BOOT_IN;

typedef struct {
    unsigned Result;        /* MW2_RESULT_* before arming */
    unsigned FallBack;      /* delete WddmProbe, WddmProbeArmed = 0, WddmProbeFellBack = Want */
    unsigned Arm;           /* write WddmProbeArmed = Want, flush, read it back */
} MW2_BOOT_OUT;

/* The first half: what the registry says. A probe is armed before it is
 * used, every boot, so any boot that never confirms costs one boot. */
static __inline void Mw2BootDecide(const MW2_BOOT_IN *In, MW2_BOOT_OUT *Out)
{
    Out->Result = MW2_RESULT_NONE;
    Out->FallBack = 0;
    Out->Arm = 0;
    if (In->Want == 0 || In->Want == MW2_WDDM_1_3)
        return;
    if (!In->Known) {
        Out->Result = MW2_RESULT_UNKNOWN;
        return;
    }
    if (In->Armed == In->Want && !In->Keep) {
        Out->Result = MW2_RESULT_FELL_BACK;
        Out->FallBack = 1;
        return;
    }
    Out->Result = MW2_RESULT_ACTIVE;
    Out->Arm = !In->Keep;
}

/* The second half: with Arm set, the probe goes ahead only if the arming
 * reached the registry (read back equal). An unarmed 2.x boot that hangs
 * would hang again on every boot after it. */
static __inline unsigned Mw2BootArmed(unsigned Result, unsigned Arm, unsigned ReadBack, unsigned Want)
{
    if (Result == MW2_RESULT_ACTIVE && Arm && ReadBack != Want)
        return MW2_RESULT_ARM_FAILED;
    return Result;
}

/* When a 2.x boot counts as good (the watchdog, probe.c, at PASSIVE_LEVEL):
 * DWM has drawn for a while, the boot has been up for two minutes, and no
 * DDI is stuck. The time matters: 300 renders can all happen at LogonUI. */
#define MW2_CONFIRM_RENDERS     300u
#define MW2_CONFIRM_SECONDS     120u

static __inline unsigned Mw2Confirm(unsigned Renders, unsigned Seconds, unsigned Stuck)
{
    return Renders >= MW2_CONFIRM_RENDERS && Seconds >= MW2_CONFIRM_SECONDS && !Stuck;
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

    /* GpuMmu only */
    unsigned PtLevels, PtIndexBits, PtSize, PtAlign, VaBits;
    unsigned long long InternalVaStart, InternalVaEnd;

    /* QUERYSEGMENT4: the one aperture segment, as QUERYSEGMENT3 */
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

/* Budget = 1 (the default, Parameters\Wddm2Budget): the segment counts
 * against the non-local budget and is an application target, as the
 * aperture segment of Jamie's driver that composited at 2.0. 0 leaves both
 * clear, as the 1.3 segment. */
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
    if (Budget) {
        P->SegApplicationTarget = 1;
        P->SegNonLocalBudget = 1;
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
