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
 *     channel reads below MW_THERMAL_RESTORE_MC (75 C);
 *   - MW_THERMAL_INVALID_LIMIT readings in a row without a valid
 *     temperature count as hot: a sensor that cannot be read is no reason
 *     to keep the higher clock.
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
#define MW_THERMAL_CH_A             3   /* the GPU by the TRM */
#define MW_THERMAL_CH_B             5   /* the GPU by the OTP trim and the vendor kernel */

enum {
    MW_THERMAL_NONE = 0,
    MW_THERMAL_DROP = 1,        /* write 594 MHz now */
    MW_THERMAL_RESTORE = 2,     /* the clock that was asked for may come back */
};

typedef struct {
    int Held;                   /* the clock is held at 594 MHz for heat */
    unsigned Invalid;           /* readings in a row with no valid channel */
    unsigned Drops;             /* MW_THERMAL_DROP decisions so far */
    long LastMilliC;            /* the hotter channel at the last valid reading */
} MW_THERMAL;

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

/* One reading. Returns what the caller has to do to the clock. */
MW_TH_INLINE int MwThermalStep(MW_THERMAL *T, int Valid, long MilliC)
{
    if (!Valid) {
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
    if (!T->Held && MilliC >= MW_THERMAL_DROP_MC) {
        T->Held = 1;
        T->Drops++;
        return MW_THERMAL_DROP;
    }
    if (T->Held && MilliC < MW_THERMAL_RESTORE_MC) {
        T->Held = 0;
        return MW_THERMAL_RESTORE;
    }
    return MW_THERMAL_NONE;
}
