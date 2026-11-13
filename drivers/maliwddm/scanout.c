/* Scan-out of a primary by the display controller, in place of the copy
 * to the firmware framebuffer (docs/PERF.md, stage 2).
 *
 * The RK3576's VOP2 takes one address for the window it shows, and a
 * primary's pages are wherever VidMm found them. The VOP2 has an IOMMU of
 * its own (two instances, 0x27d07e00 and 0x27d07f00, "rockchip,iommu-v2"),
 * so a primary gets a fixed range of IO addresses and its pages are entered
 * there when VidMm maps it, from the same page list the Mali's MMU is
 * given. The firmware framebuffer is entered at its own physical address,
 * so the picture the VOP2 is already showing survives the moment
 * translation is switched on, and the bugcheck screen and a hand-back have
 * something to return to.
 *
 * A flip is then two writes: ESMART0's address, and the port's "config
 * done". The window's size, stride and format are the firmware's and are
 * the primary's too, or the primary is not scanned out and is copied.
 *
 * The VOP2's own registers are only ever written here. On this SoC reading
 * them back has cost the HDMI signal before (in the firmware). The IOMMU's
 * registers are read, as Linux reads them.
 *
 * Nothing here runs until MW_ESCAPE_SCANOUT turns it on, and the same
 * escape turns it off again. */

#include "maliwddm.h"

#define VOP_PA                  0x27D00000u
#define VOP_SIZE                0x8000u
#define VOP_CFG_DONE            0x0000u
#define VOP_ESMART0_YRGB_MST    0x1814u
#define VOP_MMU0                0x7E00u
#define VOP_MMU1                0x7F00u

#define MMU_DTE_ADDR            0x00
#define MMU_STATUS              0x04
#define MMU_COMMAND             0x08
#define MMU_INT_MASK            0x1C
#define MMU_AUTO_GATING         0x24
#define MMU_STATUS_PAGING       0x1u
#define MMU_STATUS_STALL        0x4u
#define MMU_CMD_ENABLE_PAGING   0
#define MMU_CMD_DISABLE_PAGING  1
#define MMU_CMD_ENABLE_STALL    2
#define MMU_CMD_DISABLE_STALL   3
#define MMU_CMD_ZAP_CACHE       4

/* IO addresses. DRAM starts at 0x40000000 on the RK3576, so the firmware
 * framebuffer, entered at its physical address, cannot be down here. */
#define SCAN_IOVA_BASE          0x10000000u
#define SCAN_SLOT_BYTES         0x01000000u     /* 16 MB: 2560x1440x4 is 14.7 */
#define SCAN_SLOT_PAGES         (SCAN_SLOT_BYTES >> PAGE_SHIFT)
#define SCAN_PT_SPAN            0x00400000u     /* one page table: 4 MB */
#define SCAN_FB_TABLES          8               /* up to 32 MB of framebuffer */
#define SCAN_TABLES             (1 + SCAN_FB_TABLES + MW_SCAN_SLOTS * (SCAN_SLOT_BYTES / SCAN_PT_SPAN))

static __inline void W(MW_ADAPTER *A, ULONG Off, ULONG V)
{
    WRITE_REGISTER_ULONG((PULONG)(A->VopVa + Off), V);
}

static __inline ULONG R(MW_ADAPTER *A, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(A->VopVa + Off));
}

/* The v2 entry: bits 31:12 of the address in place, 35:32 in 11:8, 39:36
 * in 7:4 (rk_mk_dte_v2 in Linux). */
static ULONG Entry(ULONG64 Pa, ULONG Flags)
{
    return (ULONG)(Pa & 0xFFFFF000u) | (ULONG)((Pa >> 32) & 0xF) << 8 | (ULONG)((Pa >> 36) & 0xF) << 4 | Flags;
}

static PULONG Table(MW_ADAPTER *A, ULONG Index)
{
    return (PULONG)((PUCHAR)A->ScanTables + (SIZE_T)Index * PAGE_SIZE);
}

/* The page table for an IO address, entered in the directory on first use. */
static PULONG TableFor(MW_ADAPTER *A, ULONG Iova)
{
    PULONG dt = Table(A, 0);
    ULONG d = Iova >> 22;

    if ((dt[d] & 1) == 0) {
        if (A->ScanTablesUsed >= SCAN_TABLES)
            return NULL;
        dt[d] = Entry((ULONG64)A->ScanTablesPa.QuadPart + (ULONG64)A->ScanTablesUsed * PAGE_SIZE, 1);
        A->ScanTablesUsed++;
    }
    return (PULONG)((PUCHAR)A->ScanTables +
                    (SIZE_T)(((ULONG64)(dt[d] & 0xFFFFF000u) - (ULONG64)A->ScanTablesPa.QuadPart)));
}

static BOOLEAN Enter(MW_ADAPTER *A, ULONG Iova, ULONG64 Pa)
{
    PULONG pt = TableFor(A, Iova);

    if (pt == NULL)
        return FALSE;
    pt[(Iova >> 12) & 0x3FF] = Entry(Pa, 0x2 | 0x1);    /* readable, valid */
    return TRUE;
}

static void Zap(MW_ADAPTER *A)
{
    KeMemoryBarrier();
    if (A->ScanPaging) {
        W(A, VOP_MMU0 + MMU_COMMAND, MMU_CMD_ZAP_CACHE);
        W(A, VOP_MMU1 + MMU_COMMAND, MMU_CMD_ZAP_CACHE);
    }
}

static BOOLEAN WaitStatus(MW_ADAPTER *A, ULONG Mask, ULONG Want)
{
    for (int i = 0; i < 200; i++) {
        if ((R(A, VOP_MMU0 + MMU_STATUS) & Mask) == Want && (R(A, VOP_MMU1 + MMU_STATUS) & Mask) == Want)
            return TRUE;
        KeStallExecutionProcessor(50);
    }
    return FALSE;
}

/* Tables only; no register is touched. PASSIVE_LEVEL, after the
 * framebuffer is known. */
void MwScanStart(MW_ADAPTER *A)
{
    PHYSICAL_ADDRESS lo = {0}, hi, vop;
    ULONG64 fb = (ULONG64)A->Fb.PhysicAddress.QuadPart;
    SIZE_T n;

    A->ScanReady = FALSE;
    if (A->FbVa == NULL || fb < 0x40000000ull || fb + A->FbSize > 0x100000000ull ||
        A->FbSize > (SIZE_T)SCAN_FB_TABLES * SCAN_PT_SPAN - SCAN_PT_SPAN)
        return;
    hi.QuadPart = 0xFFFFFFFFll;
    n = (SIZE_T)SCAN_TABLES * PAGE_SIZE;
    A->ScanTables = MmAllocateContiguousMemorySpecifyCache(n, lo, hi, lo, MmNonCached);
    if (A->ScanTables == NULL)
        return;
    RtlZeroMemory(A->ScanTables, n);
    A->ScanTablesPa = MmGetPhysicalAddress(A->ScanTables);
    A->ScanTablesUsed = 1;                      /* the directory */
    for (ULONG64 p = fb & ~0xFFFull; p < fb + A->FbSize; p += PAGE_SIZE)
        if (!Enter(A, (ULONG)p, p))
            goto fail;
    vop.QuadPart = VOP_PA;
    A->VopVa = (PUCHAR)MmMapIoSpaceEx(vop, VOP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (A->VopVa == NULL)
        goto fail;
    for (int i = 0; i < MW_SCAN_SLOTS; i++)
        A->ScanSlot[i] = NULL;
    A->ScanReady = TRUE;
    return;
fail:
    MmFreeContiguousMemory(A->ScanTables);
    A->ScanTables = NULL;
}

/* Back to the firmware framebuffer. Any IRQL. */
void MwScanToFb(MW_ADAPTER *A)
{
    if (!A->ScanReady || !A->ScanShowing)
        return;
    W(A, VOP_ESMART0_YRGB_MST, A->Fb.PhysicAddress.LowPart);
    W(A, VOP_CFG_DONE, 0x00018001);
    A->ScanShowing = FALSE;
    A->ScanToFbs++;
}

/* Would a flip to this allocation be scanned out? */
BOOLEAN MwScanUsable(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    return A->ScanOn && A->ScanPaging && P != NULL && P->ScanIova != 0 && P->Mapped;
}

/* A flip: point the window at the primary. Called at the level
 * SetVidPnSourceAddress is. */
BOOLEAN MwScanFlip(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    if (!MwScanUsable(A, P))
        return FALSE;
    W(A, VOP_ESMART0_YRGB_MST, P->ScanIova);
    W(A, VOP_CFG_DONE, 0x00018001);
    A->ScanShowing = TRUE;
    A->ScanFlips++;
    return TRUE;
}

/* VidMm has given a primary its pages. Under MmuLock. */
void MwScanMap(MW_ADAPTER *A, MW_ALLOCATION *P, PPFN_NUMBER Pfn, ULONG Pages)
{
    int s;
    ULONG iova;

    if (!A->ScanReady || !(P->Flags & MW_ALLOC_PRIMARY) || Pages > SCAN_SLOT_PAGES)
        return;
    /* Only what the window is already set up for. */
    if (P->Width != A->Fb.Width || P->Height != A->Fb.Height || P->Pitch != A->Fb.Pitch) {
        A->ScanMismatch++;
        return;
    }
    for (s = 0; s < MW_SCAN_SLOTS; s++)
        if (A->ScanSlot[s] == P || A->ScanSlot[s] == NULL)
            break;
    if (s == MW_SCAN_SLOTS) {
        A->ScanNoSlot++;
        return;
    }
    iova = SCAN_IOVA_BASE + (ULONG)s * SCAN_SLOT_BYTES;
    for (ULONG i = 0; i < Pages; i++)
        if (!Enter(A, iova + i * PAGE_SIZE, (ULONG64)Pfn[i] << PAGE_SHIFT)) {
            A->ScanNoSlot++;
            return;
        }
    A->ScanSlot[s] = P;
    Zap(A);
    P->ScanIova = iova;
    A->ScanMaps++;
}

/* The primary is losing its pages or going away. Under MmuLock. */
void MwScanUnmap(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    ULONG iova = P->ScanIova;

    if (!A->ScanReady || iova == 0)
        return;
    P->ScanIova = 0;
    if (A->ScanShowing)
        MwScanToFb(A);          /* whichever primary it was: be safe */
    for (ULONG i = 0; i < SCAN_SLOT_PAGES; i++) {
        PULONG pt = TableFor(A, iova + i * PAGE_SIZE);
        if (pt != NULL)
            pt[((iova >> 12) + i) & 0x3FF] = 0;
    }
    Zap(A);
    for (int s = 0; s < MW_SCAN_SLOTS; s++)
        if (A->ScanSlot[s] == P)
            A->ScanSlot[s] = NULL;
}

/* MW_ESCAPE_SCANOUT. PASSIVE_LEVEL.
 *   1: translation on if it is not, and flips scan out from now on
 *   0: flips are copied again; the window goes back to the framebuffer
 *   2: as 0, and translation off as well
 *   anything else: only report */
NTSTATUS MwScanSet(MW_ADAPTER *A, MW_ESCAPE_SCANOUT_DATA *D)
{
    if (!A->ScanReady) {
        D->Status = 0xFFFFFFFFu;
        return STATUS_SUCCESS;
    }
    ExAcquireFastMutex(&A->MmuLock);
    if (D->Mode == 1) {
        if (!A->ScanPaging) {
            ULONG dte = Entry((ULONG64)A->ScanTablesPa.QuadPart, 1);
            for (ULONG m = VOP_MMU0; m <= VOP_MMU1; m += VOP_MMU1 - VOP_MMU0) {
                W(A, m + MMU_DTE_ADDR, dte);
                W(A, m + MMU_COMMAND, MMU_CMD_ZAP_CACHE);
                W(A, m + MMU_INT_MASK, 0);
                /* "Workaround for iommu blocked" in Linux: bit 31. */
                W(A, m + MMU_AUTO_GATING, R(A, m + MMU_AUTO_GATING) | 0x80000000u);
            }
            W(A, VOP_MMU0 + MMU_COMMAND, MMU_CMD_ENABLE_PAGING);
            W(A, VOP_MMU1 + MMU_COMMAND, MMU_CMD_ENABLE_PAGING);
            A->ScanPaging = WaitStatus(A, MMU_STATUS_PAGING, MMU_STATUS_PAGING);
        }
        InterlockedExchange(&A->ScanOn, A->ScanPaging ? 1 : 0);
    } else if (D->Mode == 0 || D->Mode == 2) {
        InterlockedExchange(&A->ScanOn, 0);
        MwScanToFb(A);
        if (D->Mode == 2 && A->ScanPaging) {
            LARGE_INTEGER frames;
            frames.QuadPart = -500000;          /* 50 ms: the window has moved */
            KeDelayExecutionThread(KernelMode, FALSE, &frames);
            W(A, VOP_MMU0 + MMU_COMMAND, MMU_CMD_ENABLE_STALL);
            W(A, VOP_MMU1 + MMU_COMMAND, MMU_CMD_ENABLE_STALL);
            WaitStatus(A, MMU_STATUS_STALL, MMU_STATUS_STALL);
            W(A, VOP_MMU0 + MMU_COMMAND, MMU_CMD_DISABLE_PAGING);
            W(A, VOP_MMU1 + MMU_COMMAND, MMU_CMD_DISABLE_PAGING);
            A->ScanPaging = !WaitStatus(A, MMU_STATUS_PAGING, 0);
            W(A, VOP_MMU0 + MMU_COMMAND, MMU_CMD_DISABLE_STALL);
            W(A, VOP_MMU1 + MMU_COMMAND, MMU_CMD_DISABLE_STALL);
        }
    }
    D->Status = (A->ScanOn ? 1u : 0) | (A->ScanPaging ? 2u : 0) | (A->ScanShowing ? 4u : 0);
    D->Mmu0Status = R(A, VOP_MMU0 + MMU_STATUS);
    D->Mmu1Status = R(A, VOP_MMU1 + MMU_STATUS);
    D->Flips = A->ScanFlips;
    D->Maps = A->ScanMaps;
    D->ToFbs = A->ScanToFbs;
    D->Mismatch = A->ScanMismatch;
    D->NoSlot = A->ScanNoSlot;
    D->FbPa = A->Fb.PhysicAddress.LowPart;
    ExReleaseFastMutex(&A->MmuLock);
    return STATUS_SUCCESS;
}

/* The device is stopping or handing the display back: the window on the
 * framebuffer, translation off, and only then the tables freed. */
void MwScanStop(MW_ADAPTER *A)
{
    MW_ESCAPE_SCANOUT_DATA d = {0};

    if (!A->ScanReady)
        return;
    d.Mode = 2;
    MwScanSet(A, &d);
    A->ScanReady = FALSE;
    if (A->VopVa != NULL && !A->ScanPaging) {
        MmUnmapIoSpace(A->VopVa, VOP_SIZE);
        A->VopVa = NULL;
        MmFreeContiguousMemory(A->ScanTables);
        A->ScanTables = NULL;
    }
    /* If translation would not go off the tables stay: the VOP2 still
     * reads the framebuffer through them. */
}
