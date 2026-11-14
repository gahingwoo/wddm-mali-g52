/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

struct sim_stats {
    unsigned jobs[3], writes, faults, resets, as_updates, flushes;
    unsigned tiles, soft_stops, hard_stops, stops_ignored;
    /* Each of these should stay 0. */
    unsigned bad_config, flush_without_lock, started_while_locked, started_unpowered;
    unsigned started_while_active;
};

extern ULONG sim_regs[0x20000 / 4];
extern unsigned sim_delay_us;
/* Tiles a fragment job may run per start (0: all); sim_run gives more. */
extern ULONG sim_tile_budget;
void sim_run(unsigned slot, ULONG tiles);
extern struct sim_stats sim;
void sim_reset(void);
void *sim_translate(ULONG64 va, int write, ULONG64 *leaf);
