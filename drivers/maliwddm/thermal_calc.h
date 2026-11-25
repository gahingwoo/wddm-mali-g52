/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The GPU clock's temperature fallback, the decisions only: no register is
 * touched here (adapter.c reads the TSADC and writes the CRU).
 *
 * Above 600 MHz (GpuClkHigh) the GPU runs on a raised clock and, on the
 * TaishanPi 3M, a raised rail. With the fan on, FurMark at 702 MHz peaked
 * at 49.9 C (2026-11-24); with the fan off the board idles at 67 to 68 C
 * already at 594 MHz. So once a second the driver reads the two TSADC
 * channels that may be the GPU's and, through this state machine:
 *
 *   - at or above MW_THERMAL_DROP_MC (85 C, mainline's gpu-alert trip) the
 *     clock goes to GPLL / 2 = 594 MHz and is held there;
 *   - it goes back to the clock that was asked for only once the hotter
 *     channel has read below MW_THERMAL_RESTORE_MC (75 C) at every
 *     reading for the hold time (GpuClkRestoreHoldSec, default 90 s);
 *     one reading at or above 75 C starts the hold over;
 *   - MW_THERMAL_INVALID_LIMIT readings in a row without a valid
 *     temperature count as hot: a sensor that cannot be read is no reason
 *     to keep the higher clock.
 *
 * And the fault policy (2026-11-24 board run: until then any job fault
 * above 600 MHz cleared the clock asked for, so one TDR, even one the
 * driver's own clock switch caused, lost 702 MHz until the next start):
 *
 *   - a job fault, a job timeout or a TDR reset while the GPU runs above
 *     600 MHz drops the clock to 594 MHz at once, as before, but the clock
 *     asked for is kept, and comes back at the next reading (1 s) unless
 *     something else holds it;
 *   - MW_THERMAL_FAULT_LIMIT (2) such faults within MW_THERMAL_FAULT_WINDOW_MS
 *     (10 minutes) lock the clock at 594 MHz until the next start
 *     (reason MW_THROTTLE_FAULTS). Faults at 594 MHz or below do not count:
 *     they are not the high clock's.
 *
 * MwThrottleReason says why the clock is held: 0 none, 1 thermal, 2 faults.
 *
 * Which channel is the GPU is not settled: the TRM (Table 19-1) and
 * mainline's thermal zones say 3, mainline's and the vendor's OTP trim
 * cells and the vendor's gpu-thermal zone say 5. The hotter of the two
 * counts. No OTP trim is applied (a few degrees), which the 10 C band
 * covers.
 *
 * Plain C with no WDK header, so that tests/thermal-host can build it.
 */
#pragma once

#ifdef _MSC_VER
#define MW_TH_INLINE static __inline
#else
#define MW_TH_INLINE static inline
#endif

#define MW_THERMAL_DROP_MC          85000
#define MW_THERMAL_RESTORE_MC       75000
#define MW_THERMAL_INVALID_LIMIT    3
#define MW_THERMAL_PERIOD_MS        1000
#define MW_THERMAL_HOLD_DEFAULT_S   90u     /* GpuClkRestoreHoldSec */
#define MW_THERMAL_HOLD_MAX_S       3600u
#define MW_THERMAL_FAULT_LIMIT      2u
#define MW_THERMAL_FAULT_WINDOW_MS  600000ull
#define MW_THERMAL_CH_A             3   /* the GPU by the TRM */
#define MW_THERMAL_CH_B             5   /* the GPU by the OTP trim and the vendor kernel */

enum {
    MW_THERMAL_NONE = 0,
    MW_THERMAL_DROP = 1,        /* write 594 MHz (between two job chains) */
    MW_THERMAL_RESTORE = 2,     /* the clock that was asked for may come back */
};

/* "Throttled reason" (pcw.c, m5test clk). */
#define MW_THROTTLE_NONE        0u
#define MW_THROTTLE_THERMAL     1u
#define MW_THROTTLE_FAULTS      2u

typedef struct {
    int Held;                   /* the clock is held at 594 MHz for heat */
    unsigned Invalid;           /* readings in a row with no valid channel */
    unsigned Drops;             /* MW_THERMAL_DROP decisions so far */
    long LastMilliC;            /* the hotter channel at the last valid reading */
    unsigned long long CoolSinceMs;  /* below 75 C at every reading since (0: not cool) */
    int FaultDropped;           /* a fault dropped the clock; it may come back */
    int FaultLocked;            /* MW_THERMAL_FAULT_LIMIT faults in the window: 594 MHz until restart */
    unsigned HighFaults;        /* faults above 600 MHz so far */
    unsigned long long LastFaultMs;  /* the last of them */
} MW_THERMAL;

/* Clamp of the setting GpuClkRestoreHoldSec (0 or absent: the default). */
MW_TH_INLINE unsigned long long MwThermalHoldMs(unsigned long Setting)
{
    unsigned long s = Setting == 0 ? MW_THERMAL_HOLD_DEFAULT_S : Setting;
    if (s > MW_THERMAL_HOLD_MAX_S)
        s = MW_THERMAL_HOLD_MAX_S;
    return (unsigned long long)s * 1000ull;
}

MW_TH_INLINE unsigned MwThrottleReason(const MW_THERMAL *T)
{
    if (T->FaultLocked)
        return MW_THROTTLE_FAULTS;
    if (T->Held)
        return MW_THROTTLE_THERMAL;
    return MW_THROTTLE_NONE;
}

/* A job fault, timeout or TDR at NowMs. AtHigh: the GPU ran above 600 MHz.
 * Returns 1 when this fault locks the clock. The caller has already
 * dropped the clock (it does that at once, from the interrupt). */
MW_TH_INLINE int MwThermalFault(MW_THERMAL *T, unsigned long long NowMs, int AtHigh)
{
    int lock = 0;

    if (!AtHigh)
        return 0;
    T->HighFaults++;
    if (!T->FaultLocked && T->HighFaults >= MW_THERMAL_FAULT_LIMIT && T->LastFaultMs != 0 &&
        NowMs - T->LastFaultMs <= MW_THERMAL_FAULT_WINDOW_MS) {
        T->FaultLocked = 1;
        lock = 1;
    }
    T->LastFaultMs = NowMs != 0 ? NowMs : 1;
    T->FaultDropped = 1;
    return lock;
}

/* One reading at NowMs (milliseconds, any monotonic clock above 0).
 * Returns what the caller has to do to the clock. */
MW_TH_INLINE int MwThermalStep(MW_THERMAL *T, int Valid, long MilliC, unsigned long long NowMs,
                               unsigned long long HoldMs)
{
    if (!Valid) {
        T->CoolSinceMs = 0;
        if (T->Invalid < MW_THERMAL_INVALID_LIMIT)
            T->Invalid++;
        if (!T->Held && T->Invalid >= MW_THERMAL_INVALID_LIMIT) {
            T->Held = 1;
            T->Drops++;
            return MW_THERMAL_DROP;
        }
        return MW_THERMAL_NONE;
    }
    T->Invalid = 0;
    T->LastMilliC = MilliC;
    if (MilliC >= MW_THERMAL_DROP_MC) {
        T->CoolSinceMs = 0;
        if (!T->Held) {
            T->Held = 1;
            T->Drops++;
            return MW_THERMAL_DROP;
        }
        return MW_THERMAL_NONE;
    }
    if (T->Held) {
        if (MilliC >= MW_THERMAL_RESTORE_MC) {
            T->CoolSinceMs = 0;
            return MW_THERMAL_NONE;
        }
        if (T->CoolSinceMs == 0) {
            T->CoolSinceMs = NowMs != 0 ? NowMs : 1;
            if (HoldMs != 0)
                return MW_THERMAL_NONE;
        }
        if (NowMs - T->CoolSinceMs < HoldMs)
            return MW_THERMAL_NONE;
        T->Held = 0;
        T->CoolSinceMs = 0;
        T->FaultDropped = 0;
        return T->FaultLocked ? MW_THERMAL_NONE : MW_THERMAL_RESTORE;
    }
    if (T->FaultDropped) {
        T->FaultDropped = 0;
        return T->FaultLocked ? MW_THERMAL_NONE : MW_THERMAL_RESTORE;
    }
    return MW_THERMAL_NONE;
}

/* mainline's rk3588_code_table, which the RK3576 uses, linear between
 * rows; no OTP trim. 0 when the code is not a temperature. */
MW_TH_INLINE int MwTsadcToMilliC(unsigned long Code, long *MilliC)
{
    if (Code > 0x1FF || Code < 215)
        return 0;
    if (Code <= 285)
        *MilliC = -40000 + (long)(65000 * (Code - 215) / 70);
    else if (Code <= 350)
        *MilliC = 25000 + (long)(60000 * (Code - 285) / 65);
    else if (Code <= 395)
        *MilliC = 85000 + (long)(40000 * (Code - 350) / 45);
    else
        *MilliC = 125000;
    return 1;
}

/* The hotter of the two GPU candidates; 0 when neither reads. */
MW_TH_INLINE int MwThermalHotter(unsigned long CodeA, unsigned long CodeB, long *MilliC)
{
    long a = 0, b = 0;
    int va = MwTsadcToMilliC(CodeA, &a), vb = MwTsadcToMilliC(CodeB, &b);

    if (!va && !vb)
        return 0;
    if (va && vb)
        *MilliC = a > b ? a : b;
    else
        *MilliC = va ? a : b;
    return 1;
}
