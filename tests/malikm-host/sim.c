// SPDX-License-Identifier: GPL-2.0
/*
 * A Mali-G52 small enough to check malikm against: the registers the driver
 * touches, an MMU that walks the driver's Mali LPAE tables for AS0 exactly
 * as the hardware would (and faults where it would), and a job manager that
 * runs WRITE_VALUE jobs. Physical addresses are host addresses.
 */
#include <stdio.h>
#include <unistd.h>
#include "mock/ntddk.h"
#include "../../drivers/malikm/malikm.h"
#include "sim.h"

ULONG sim_regs[0x20000 / 4];
unsigned sim_delay_us;
struct sim_stats sim;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static ULONG job_raw, mmu_raw, gpu_raw;
static int as_locked;

#define R(o) sim_regs[(o) / 4]

void sim_reset(void)
{
    memset(sim_regs, 0, sizeof(sim_regs));
    memset(&sim, 0, sizeof(sim));
    job_raw = mmu_raw = gpu_raw = 0;
    R(GPU_ID) = 0x74021000;
    R(GPU_L2_FEATURES) = 0x07120206;
    R(GPU_CORE_FEATURES) = 0x2;
    R(GPU_TILER_FEATURES) = 0x209;
    R(GPU_MEM_FEATURES) = 0x1;
    R(GPU_MMU_FEATURES) = 0x2823;
    R(GPU_AS_PRESENT) = 0xff;
    R(GPU_JS_PRESENT) = 0x7;
    R(GPU_SHADER_PRESENT_LO) = 0x7;
    R(GPU_TILER_PRESENT_LO) = 0x1;
    R(GPU_L2_PRESENT_LO) = 0x1;
}

/* Walk AS0. Returns the host address, or NULL with the fault recorded. */
void *sim_translate(ULONG64 va, int write, ULONG64 *leaf)
{
    ULONG64 t = ((ULONG64)R(AS0 + AS_TRANSTAB_HI) << 32) | R(AS0 + AS_TRANSTAB_LO);
    if ((t & 3) != 3)
        return NULL;                           /* not in table mode */
    ULONG64 *table = (ULONG64 *)(uintptr_t)(t & 0xFFFFFFFFF000ull);
    for (int lvl = 0; lvl < 4; lvl++) {
        ULONG64 e = table[(va >> (39 - 9 * lvl)) & 511];
        if (lvl < 3) {
            if ((e & 3) != 3)
                return NULL;
            table = (ULONG64 *)(uintptr_t)(e & 0xFFFFFFFFF000ull);
            continue;
        }
        /* Mali LPAE leaf: type 1 at level 3, read 0x40, write 0x80. */
        if ((e & 3) != 1 || !(e & 0x40) || (write && !(e & 0x80)))
            return NULL;
        if (leaf)
            *leaf = e;
        return (char *)(uintptr_t)(e & 0xFFFFFFFFF000ull) + (va & 0xFFF);
    }
    return NULL;
}

static void mmu_fault(ULONG64 va)
{
    mmu_raw |= 1;
    R(AS0 + AS_FAULTSTATUS) = 0xC1;            /* translation fault */
    R(AS0 + AS_FAULTADDRESS_LO) = (ULONG)va;
    R(AS0 + AS_FAULTADDRESS_HI) = (ULONG)(va >> 32);
    sim.faults++;
}

/* Run the chain at Jc. Returns 0 when it completed, -1 on an MMU fault
 * (the job then stalls, as on hardware, until the driver resets). */
static int run_chain(ULONG64 jc)
{
    for (int n = 0; jc && n < 64; n++) {
        volatile ULONG *h = sim_translate(jc, 1, NULL);
        if (!h || !sim_translate(jc + 63, 1, NULL)) {
            mmu_fault(jc);
            return -1;
        }
        ULONG type = (h[4] >> 1) & 0x7f;
        if (type == 2) {                       /* WRITE_VALUE */
            ULONG64 target = ((ULONG64)h[9] << 32) | h[8];
            ULONG *dst = sim_translate(target, 1, NULL);
            if (!dst) {
                mmu_fault(target);
                return -1;
            }
            if (h[10] != 6)                    /* only immediate 32 */
                return -1;
            if (sim_delay_us)
                usleep(sim_delay_us);
            *dst = h[12];
            sim.writes++;
        }
        h[0] = 1;                              /* exception status: DONE */
        jc = ((ULONG64)h[7] << 32) | h[6];
    }
    return 0;
}

static void start_slot(unsigned slot)
{
    ULONG base = JS_SLOT(slot);
    ULONG64 jc = ((ULONG64)R(base + JS_HEAD_NEXT_HI) << 32) | R(base + JS_HEAD_NEXT_LO);
    ULONG cfg = R(base + JS_CONFIG_NEXT);

    sim.jobs[slot]++;
    if ((cfg & 0xf) != 0 || !(cfg & (3u << 8)) || !(cfg & (3u << 12)))
        sim.bad_config++;                      /* AS0 and both cache flushes */
    if (R(base + JS_AFFINITY_NEXT_LO) != R(GPU_SHADER_PRESENT_LO))
        sim.bad_config++;
    if (as_locked)
        sim.started_while_locked++;
    if (run_chain(jc) == 0)
        job_raw |= 1u << slot;
}

ULONG READ_REGISTER_ULONG(volatile ULONG *p)
{
    uintptr_t off = (uintptr_t)p - (uintptr_t)sim_regs;
    if (off >= sizeof(sim_regs))
        return *p;
    pthread_mutex_lock(&lock);
    ULONG v;
    switch (off) {
    case GPU_INT_RAWSTAT: v = gpu_raw; break;
    case GPU_INT_STAT:    v = gpu_raw & R(GPU_INT_MASK); break;
    case JOB_INT_RAWSTAT: v = job_raw; break;
    case JOB_INT_STAT:    v = job_raw & R(JOB_INT_MASK); break;
    case MMU_INT_RAWSTAT: v = mmu_raw; break;
    case MMU_INT_STAT:    v = mmu_raw & R(MMU_INT_MASK); break;
    case AS0 + AS_STATUS: v = 0; break;
    default:              v = sim_regs[off / 4]; break;
    }
    pthread_mutex_unlock(&lock);
    return v;
}

void WRITE_REGISTER_ULONG(volatile ULONG *p, ULONG v)
{
    uintptr_t off = (uintptr_t)p - (uintptr_t)sim_regs;
    if (off >= sizeof(sim_regs)) {
        *p = v;
        return;
    }
    pthread_mutex_lock(&lock);
    switch (off) {
    case GPU_CMD:
        if (v == GPU_CMD_SOFT_RESET || v == GPU_CMD_HARD_RESET) {
            gpu_raw |= GPU_IRQ_RESET_COMPLETED;
            job_raw = mmu_raw = 0;
            R(L2_READY_LO) = R(SHADER_READY_LO) = R(TILER_READY_LO) = 0;
            sim.resets++;
        }
        break;
    case GPU_INT_CLEAR: gpu_raw &= ~v; break;
    case JOB_INT_CLEAR: job_raw &= ~v; break;
    case MMU_INT_CLEAR: mmu_raw &= ~v; break;
    case L2_PWRON_LO:     R(L2_READY_LO) |= v; break;
    case SHADER_PWRON_LO: R(SHADER_READY_LO) |= v; break;
    case TILER_PWRON_LO:  R(TILER_READY_LO) |= v; break;
    case AS0 + AS_COMMAND:
        if (v == AS_COMMAND_UPDATE)
            sim.as_updates++;
        else if (v == AS_COMMAND_LOCK)
            as_locked = 1;
        else if (v == AS_COMMAND_FLUSH_PT) {
            if (!as_locked)
                sim.flush_without_lock++;
            as_locked = 0;                     /* a flush unlocks */
            sim.flushes++;
        }
        break;
    default:
        sim_regs[off / 4] = v;
        for (unsigned s = 0; s < 3; s++)
            if (off == JS_SLOT(s) + JS_COMMAND_NEXT && v == JS_COMMAND_START) {
                if (!R(L2_READY_LO) || !R(SHADER_READY_LO))
                    sim.started_unpowered++;
                start_slot(s);
            }
        break;
    }
    pthread_mutex_unlock(&lock);
}
