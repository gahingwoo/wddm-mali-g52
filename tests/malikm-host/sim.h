/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

struct sim_stats {
    unsigned jobs[3], writes, faults, resets, as_updates, flushes;
    /* Each of these should stay 0. */
    unsigned bad_config, flush_without_lock, started_while_locked, started_unpowered;
};

extern ULONG sim_regs[0x20000 / 4];
extern unsigned sim_delay_us;
extern struct sim_stats sim;
void sim_reset(void);
void *sim_translate(ULONG64 va, int write, ULONG64 *leaf);
