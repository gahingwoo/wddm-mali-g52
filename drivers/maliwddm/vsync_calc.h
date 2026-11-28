/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The emulated vsync's period, the arithmetic only (command.c arms the
 * timer). The VOP2 raises no interrupt Windows gets, so a timer stands in
 * for the vertical sync, and DWM paces every frame on it.
 *
 * Until 2026-11-28 that timer was KeSetTimerEx with a 16 ms period: 62.5
 * ticks a second against a monitor at 59.95 or 60 Hz, and at the default
 * 15.625 ms clock tick most ticks were 15.6 ms apart with a 31.3 ms gap
 * among them. DWM measured its refresh at 52 to 68 Hz, and a window
 * drag at 2560x1440 had 22 to 26 frame gaps over 1.5 periods in 14 s
 * (TaishanPi 3M, 2026-11-28); with this timer 0 to 4.
 *
 * Now the period is the committed mode's own refresh, in 100 ns units, on
 * a high-resolution timer, each tick aimed at a multiple of the period
 * (command.c). MwVsyncPeriod gives 0 for a rate
 * it does not believe (outside 20 to 240 Hz), and MwVsyncChoose then falls
 * back to the next source and at last to 60 Hz.
 */
#ifndef MW_VSYNC_CALC_H
#define MW_VSYNC_CALC_H

typedef unsigned long long mwv_u64;

#define MW_VSYNC_DEFAULT_100NS 166667ull        /* 60 Hz */
#define MW_VSYNC_MIN_100NS 41667ull             /* 240 Hz */
#define MW_VSYNC_MAX_100NS 500000ull            /* 20 Hz */

/* Num / Den Hz as a period in 100 ns, rounded; 0 if not plausible. */
static inline mwv_u64 MwVsyncPeriod(mwv_u64 Num, mwv_u64 Den)
{
    mwv_u64 p;

    if (Num == 0 || Den == 0 || Den > (~0ull - Num / 2) / 10000000ull)
        return 0;
    p = (Den * 10000000ull + Num / 2) / Num;
    return p < MW_VSYNC_MIN_100NS || p > MW_VSYNC_MAX_100NS ? 0 : p;
}

/* The period to keep: the HDMI mode the driver set (HdmiHpd 3) when there
 * is one, else the firmware's timing (pixel rate over the total size),
 * else 60 Hz. */
static inline mwv_u64 MwVsyncChoose(int HdmiSwitch, mwv_u64 RefNum, mwv_u64 RefDen,
                                    mwv_u64 PixelRate, mwv_u64 TotalW, mwv_u64 TotalH)
{
    mwv_u64 p = HdmiSwitch ? MwVsyncPeriod(RefNum, RefDen) : 0;

    if (p == 0)
        p = MwVsyncPeriod(PixelRate, TotalW * TotalH);
    return p != 0 ? p : MW_VSYNC_DEFAULT_100NS;
}

/* Whether a period differs enough from the one armed to arm again: more
 * than 1 in 4096 (about 0.004 Hz at 60 Hz). */
static inline int MwVsyncRearm(mwv_u64 Armed, mwv_u64 Want)
{
    mwv_u64 d = Armed > Want ? Armed - Want : Want - Armed;
    return Armed == 0 || d * 4096ull > Want;
}

#endif
