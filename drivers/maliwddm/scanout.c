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
 * It is on from the start unless the ScanOut setting (a DWORD in Device
 * Parameters) is 0; MW_ESCAPE_SCANOUT turns it on and off while running.
 * Until 2026-11-21 it was off unless the setting was 1, so every flip on
 * a board nobody had run "m5test scan 5" on was a CPU copy: about 25 ms a
 * frame on the TaishanPi 3M's 800x1280 panel.
 *
 * Rotation does not stop it. The source mode, and so every shared
 * primary, keeps the panel's own size at every rotation and DWM draws the
 * desktop already turned into it (display.c), so a 90 or 270 degree
 * desktop is scanned out like an unturned one. The VOP2 itself cannot
 * turn a picture by 90 or 270 degrees: on the RK3576 the Esmart windows
 * can only mirror vertically and the Cluster windows only mirror
 * (rk3576_vop_win_data in mainline rockchip_vop2_reg.c). Only a primary of
 * the turned size would need that, and it is copied (MwFlipDegrees).
 *
 * If the IOMMU reports a fault while a primary is shown, MwScanCheck puts
 * the window back on the framebuffer and flips are copied from then on. */

#include "maliwddm.h"

#define VOP_PA                  0x27D00000u
#define VOP_SIZE                0x8000u
#define VOP_CFG_DONE            0x0000u
#define VOP_ESMART0_YRGB_MST    0x1814u
#define VOP_MMU0                0x7E00u
#define VOP_MMU1                0x7F00u

#define VMMU_DTE_ADDR            0x00
#define VMMU_STATUS              0x04
#define VMMU_COMMAND             0x08
#define VMMU_INT_MASK            0x1C
#define VMMU_AUTO_GATING         0x24
#define VMMU_STATUS_PAGING       0x1u
#define VMMU_STATUS_STALL        0x4u
#define VMMU_CMD_ENABLE_PAGING   0
#define VMMU_CMD_DISABLE_PAGING  1
#define VMMU_CMD_ENABLE_STALL    2
#define VMMU_CMD_DISABLE_STALL   3
#define VMMU_CMD_ZAP_CACHE       4
#define VMMU_CMD_PAGE_FAULT_DONE 5
#define VMMU_PAGE_FAULT_ADDR    0x0C
#define VMMU_STATUS_FAULT        0x2u

/* IO addresses. DRAM starts at 0x40000000 on the RK3576, so the firmware
 * framebuffer, entered at its physical address, cannot be down here. */
#define SCAN_IOVA_BASE          0x10000000u
#define SCAN_PT_SPAN            0x00400000u     /* one page table: 4 MB */
/* A slot is the framebuffer's size rounded up to whole page tables, so the
 * mode the firmware set decides it: 8 MB at 1920x1080, 16 at 2560x1440,
 * 36 at 3840x2160. Six of the largest end below 0x40000000. */
#define SCAN_SLOT_BYTES         (A->ScanSlotBytes)
#define SCAN_SLOT_PAGES         (SCAN_SLOT_BYTES >> PAGE_SHIFT)
#define SCAN_TABLES             (A->ScanTablesTotal)
#define SCAN_SLOT_MAX           0x04000000u     /* 64 MB */

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
        W(A, VOP_MMU0 + VMMU_COMMAND, VMMU_CMD_ZAP_CACHE);
        W(A, VOP_MMU1 + VMMU_COMMAND, VMMU_CMD_ZAP_CACHE);
    }
}

static BOOLEAN WaitStatus(MW_ADAPTER *A, ULONG Mask, ULONG Want)
{
    for (int i = 0; i < 200; i++) {
        if ((R(A, VOP_MMU0 + VMMU_STATUS) & Mask) == Want && (R(A, VOP_MMU1 + VMMU_STATUS) & Mask) == Want)
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
    if (A->FbVa == NULL || A->FbSize == 0 || fb < 0x40000000ull || fb + A->FbSize > 0x100000000ull)
        return;
    A->ScanSlotBytes = (ULONG)((A->FbSize + SCAN_PT_SPAN - 1) & ~((SIZE_T)SCAN_PT_SPAN - 1));
    if (A->ScanSlotBytes > SCAN_SLOT_MAX)
        return;
    A->ScanTablesTotal = 1 + (ULONG)(((fb + A->FbSize - 1) >> 22) - (fb >> 22) + 1) +
                         MW_SCAN_SLOTS * (A->ScanSlotBytes / SCAN_PT_SPAN);
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
    MwLog(A, L"ScanSlotBytes", A->ScanSlotBytes);
    /* On from the start unless the setting says 0 (m5test scan 4, or
     * reg add ... /v ScanOut /d 0). */
    A->ScanSetting = MwSetting(A, L"ScanOut", 1) != 0;
    MwLog(A, L"ScanOutSetting", A->ScanSetting);
    if (A->ScanSetting) {
        MW_ESCAPE_SCANOUT_DATA d = {0};
        d.Mode = 1;
        MwScanSet(A, &d);
        MwLog(A, L"ScanOutAtStart", d.Status);
    }
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

/* Would a flip to this allocation be scanned out? Not if the driver has to
 * turn it (MwFlipDegrees): the window cannot rotate, and the copy does. */
ULONG MwScanWhy(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    if (!A->ScanOn || !A->ScanPaging)
        return MW_SCAN_WHY_OFF;
    if (P == NULL || P->ScanIova == 0 || !P->Mapped)
        return MW_SCAN_WHY_NOT_MAPPED;
    if (MwFlipDegrees(A, P) != 0)
        return MW_SCAN_WHY_ROTATED;
    return 0;
}

BOOLEAN MwScanUsable(MW_ADAPTER *A, MW_ALLOCATION *P)
{
    return MwScanWhy(A, P) == 0;
}

/* From the vsync DPC, every few vsyncs: an IOMMU fault while a primary is
 * shown means the window reads pages that are not entered, and the
 * display stalls on every one of them. The window goes back to the
 * framebuffer, which is always entered, the faults are acknowledged as
 * MwScanSet does, and flips are copied until scan-out is turned on again.
 * Only the IOMMU's registers are read. Any IRQL. */
void MwScanCheck(MW_ADAPTER *A)
{
    BOOLEAN fault = FALSE;
    ULONG addr = 0;

    if (!A->ScanReady || !A->ScanPaging || !A->ScanOn)
        return;
    for (ULONG m = VOP_MMU0; m <= VOP_MMU1; m += VOP_MMU1 - VOP_MMU0)
        if (R(A, m + VMMU_STATUS) & VMMU_STATUS_FAULT) {
            fault = TRUE;
            addr = R(A, m + VMMU_PAGE_FAULT_ADDR);
        }
    if (!fault)
        return;
    InterlockedExchange(&A->ScanOn, 0);
    MwScanToFb(A);
    /* The rest of the frame being read can fault again before the window
     * moves: acknowledge for a little while, about a millisecond. */
    for (int i = 0; i < 64; i++) {
        for (ULONG m = VOP_MMU0; m <= VOP_MMU1; m += VOP_MMU1 - VOP_MMU0)
            if (R(A, m + VMMU_STATUS) & VMMU_STATUS_FAULT) {
                W(A, m + VMMU_COMMAND, VMMU_CMD_ZAP_CACHE);
                W(A, m + VMMU_COMMAND, VMMU_CMD_PAGE_FAULT_DONE);
            }
        KeStallExecutionProcessor(20);
    }
    A->ScanFaults++;
    A->ScanFaultAddr = addr;
    A->ScanAutoOff++;
}

/* The adapter leaving D0 (shutdown, restart, sleep): the window on the
 * framebuffer and translation off, so that whatever runs next finds the
 * VOP2 reading physical addresses again; back on when it returns to D0 if
 * it was on. PASSIVE_LEVEL. */
void MwScanPower(MW_ADAPTER *A, BOOLEAN On)
{
    MW_ESCAPE_SCANOUT_DATA d = {0};

    if (!A->ScanReady)
        return;
    if (!On) {
        if (A->ScanOn || A->ScanPaging) {
            A->ScanResume = A->ScanOn != 0;
            d.Mode = 2;
            MwScanSet(A, &d);
        }
    } else if (A->ScanResume) {
        A->ScanResume = FALSE;
        d.Mode = 1;
        MwScanSet(A, &d);
    }
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
        A->ScanLastW = P->Width;
        A->ScanLastH = P->Height;
        A->ScanLastPitch = P->Pitch;
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
    /* The entries stay. The window moves at the next frame start, and
     * until then the VOP2 is still reading this primary: with the entries
     * cleared it faulted (IOMMU status 0x40b, seen when dwm.exe was ended
     * a dozen times in a row). It reads at most one frame from pages that
     * are no longer ours, and the next primary in the slot overwrites the
     * entries. */
    for (int s = 0; s < MW_SCAN_SLOTS; s++)
        if (A->ScanSlot[s] == P)
            A->ScanSlot[s] = NULL;
}

/* MW_ESCAPE_SCANOUT. PASSIVE_LEVEL.
 *   1: translation on if it is not, and flips scan out from now on
 *   0: flips are copied again; the window goes back to the framebuffer
 *   2: as 0, and translation off as well
 *   5: as 1, and at every start from now on; 4: as 0, and not at start
 *   anything else: only report */
NTSTATUS MwScanSet(MW_ADAPTER *A, MW_ESCAPE_SCANOUT_DATA *D)
{
    if (!A->ScanReady) {
        D->Status = 0xFFFFFFFFu;
        return STATUS_SUCCESS;
    }
    if (D->Mode == 4 || D->Mode == 5) {
        MwSetSetting(A, L"ScanOut", D->Mode == 5);
        A->ScanSetting = D->Mode == 5;
        D->Mode = D->Mode == 5 ? 1 : 0;
    }
    ExAcquireFastMutex(&A->MmuLock);
    if (D->Mode == 1) {
        if (!A->ScanPaging) {
            ULONG dte = Entry((ULONG64)A->ScanTablesPa.QuadPart, 1);
            for (ULONG m = VOP_MMU0; m <= VOP_MMU1; m += VOP_MMU1 - VOP_MMU0) {
                W(A, m + VMMU_DTE_ADDR, dte);
                W(A, m + VMMU_COMMAND, VMMU_CMD_ZAP_CACHE);
                W(A, m + VMMU_INT_MASK, 0);
                /* "Workaround for iommu blocked" in Linux: bit 31. */
                W(A, m + VMMU_AUTO_GATING, R(A, m + VMMU_AUTO_GATING) | 0x80000000u);
            }
            W(A, VOP_MMU0 + VMMU_COMMAND, VMMU_CMD_ENABLE_PAGING);
            W(A, VOP_MMU1 + VMMU_COMMAND, VMMU_CMD_ENABLE_PAGING);
            A->ScanPaging = WaitStatus(A, VMMU_STATUS_PAGING, VMMU_STATUS_PAGING);
        }
        InterlockedExchange(&A->ScanOn, A->ScanPaging ? 1 : 0);
    } else if (D->Mode == 0 || D->Mode == 2) {
        InterlockedExchange(&A->ScanOn, 0);
        MwScanToFb(A);
        if (D->Mode == 2 && A->ScanPaging) {
            LARGE_INTEGER frames;
            frames.QuadPart = -500000;          /* 50 ms: the window has moved */
            KeDelayExecutionThread(KernelMode, FALSE, &frames);
            W(A, VOP_MMU0 + VMMU_COMMAND, VMMU_CMD_ENABLE_STALL);
            W(A, VOP_MMU1 + VMMU_COMMAND, VMMU_CMD_ENABLE_STALL);
            WaitStatus(A, VMMU_STATUS_STALL, VMMU_STATUS_STALL);
            W(A, VOP_MMU0 + VMMU_COMMAND, VMMU_CMD_DISABLE_PAGING);
            W(A, VOP_MMU1 + VMMU_COMMAND, VMMU_CMD_DISABLE_PAGING);
            A->ScanPaging = !WaitStatus(A, VMMU_STATUS_PAGING, 0);
            W(A, VOP_MMU0 + VMMU_COMMAND, VMMU_CMD_DISABLE_STALL);
            W(A, VOP_MMU1 + VMMU_COMMAND, VMMU_CMD_DISABLE_STALL);
        }
    }
    D->Status = (A->ScanOn ? 1u : 0) | (A->ScanPaging ? 2u : 0) | (A->ScanShowing ? 4u : 0);
    D->Mmu0Status = R(A, VOP_MMU0 + VMMU_STATUS);
    D->Mmu1Status = R(A, VOP_MMU1 + VMMU_STATUS);
    /* A fault is reported once, with its address, and then cleared so
     * translation goes on. */
    for (ULONG m = VOP_MMU0; m <= VOP_MMU1; m += VOP_MMU1 - VOP_MMU0)
        if (R(A, m + VMMU_STATUS) & VMMU_STATUS_FAULT) {
            A->ScanFaults++;
            A->ScanFaultAddr = R(A, m + VMMU_PAGE_FAULT_ADDR);
            W(A, m + VMMU_COMMAND, VMMU_CMD_ZAP_CACHE);
            W(A, m + VMMU_COMMAND, VMMU_CMD_PAGE_FAULT_DONE);
        }
    D->Faults = A->ScanFaults;
    D->FaultAddr = A->ScanFaultAddr;
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
