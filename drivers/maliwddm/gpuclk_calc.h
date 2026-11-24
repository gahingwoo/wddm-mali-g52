/* SPDX-License-Identifier: GPL-2.0 */
/*
 * How the GPU clock is changed: CRU_CLKSEL_CON165, mux in [7:5] (0 GPLL
 * 1188, 1 CPLL 1000, 3 SPLL 702 MHz), divider minus one in [4:0], each
 * written with its own write-enable mask in [31:16]. The decisions only;
 * adapter.c does the writes. Plain C, so that tests/gpuclk-host builds it.
 *
 * The 2026-11-24 board run (FurMark at 702 MHz, fan off): the 85 C drop
 * to 594 MHz was one write, (0xFF << 16) | GPLL << 5 | (2 - 1), from
 * SPLL / 1, made from a DPC while a job chain ran. Three seconds later
 * Windows logged a TDR (event 4101): one job failed, one timed out, two
 * resets. Two things were wrong with that write, and both are fixed:
 *
 * 1. The order. One write changes the mux and the divider together, and
 *    nothing says the two take effect on the same GPU clock edge. From
 *    SPLL / 1 to GPLL / 2 the halfway state "new mux, old divider" is
 *    GPLL / 1 = 1188 MHz, at a rail set for 702. Mainline
 *    (clk_composite_set_rate_and_parent, drivers/clk/clk-composite.c,
 *    used by clk-rk3576.c's COMPOSITE CLK_GPU_SRC_PRE) writes them
 *    separately: the divider first when the old divider on the new parent
 *    would be faster than the target, else the mux first. TF-A's SCMI GPU
 *    clock (plat/rockchip/rk3576/scmi/rk3576_clk.c, clk_gpu_set_rate)
 *    writes the divider, then the mux. MwClkSteps does the same as
 *    mainline, and more exactly: of the two halfway states it takes the
 *    slower, which is never faster than the faster of the two end rates
 *    (their product is the product of the end rates).
 *
 * 2. The moment. Neither mainline nor TF-A waits for an idle GPU (devfreq
 *    sets the rate while jobs run), so the order alone may be enough. But
 *    the board showed a hang, and the cost of waiting is small: the
 *    driver runs one job chain at a time, and the switch is now made only
 *    between two chains, from Kick (command.c) before the next one
 *    starts, or at once when nothing runs. A chain is bounded by the
 *    driver's 2 s job timeout, so a switch waits at most that long, and
 *    no submission is held back.
 */
#pragma once

#ifdef _MSC_VER
#define MW_CK_INLINE static __inline
#else
#define MW_CK_INLINE static inline
#endif

#define MW_CK_MUX_SHIFT     5u
#define MW_CK_MUX_MASK      0xE0u
#define MW_CK_DIV_MASK      0x1Fu

/* The register's low byte for a mux and a divider (1..32). */
MW_CK_INLINE unsigned long MwClkVal(unsigned Mux, unsigned Div)
{
    return ((unsigned long)(Mux & 7u) << MW_CK_MUX_SHIFT) | ((Div - 1u) & MW_CK_DIV_MASK);
}

MW_CK_INLINE unsigned long MwClkRate(unsigned long Val, const unsigned long Pll[8])
{
    return Pll[(Val >> MW_CK_MUX_SHIFT) & 7u] / ((Val & MW_CK_DIV_MASK) + 1u);
}

/* The writes that take the clock from Cur to Next (low bytes as MwClkVal
 * makes them; bit 8, the PVTPLL path, is never written). Each entry of Out
 * is a whole register value with the write mask of the one field it
 * changes. Returns 0 to 2, the number of writes. */
MW_CK_INLINE unsigned MwClkSteps(unsigned long Cur, unsigned long Next, const unsigned long Pll[8],
                                 unsigned long Out[2])
{
    unsigned long mux = (Next & MW_CK_MUX_MASK) | (MW_CK_MUX_MASK << 16);
    unsigned long div = (Next & MW_CK_DIV_MASK) | (MW_CK_DIV_MASK << 16);
    int sameMux = (Cur & MW_CK_MUX_MASK) == (Next & MW_CK_MUX_MASK);
    int sameDiv = (Cur & MW_CK_DIV_MASK) == (Next & MW_CK_DIV_MASK);
    unsigned long viaMux, viaDiv;

    if (sameMux && sameDiv)
        return 0;
    if (sameMux) {
        Out[0] = div;
        return 1;
    }
    if (sameDiv) {
        Out[0] = mux;
        return 1;
    }
    /* Halfway after the mux write: new parent, old divider; after the
     * divider write: old parent, new divider. The slower goes first. */
    viaMux = MwClkRate((Next & MW_CK_MUX_MASK) | (Cur & MW_CK_DIV_MASK), Pll);
    viaDiv = MwClkRate((Cur & MW_CK_MUX_MASK) | (Next & MW_CK_DIV_MASK), Pll);
    if (viaDiv <= viaMux) {
        Out[0] = div;
        Out[1] = mux;
    } else {
        Out[0] = mux;
        Out[1] = div;
    }
    return 2;
}

/* What the register's low byte reads after Steps (for the host test). */
MW_CK_INLINE unsigned long MwClkApplyWrite(unsigned long Reg, unsigned long Write)
{
    unsigned long mask = (Write >> 16) & 0xFFFFu;
    return (Reg & ~mask) | (Write & mask);
}

/* ---- the default above 600 MHz (2026-11-25, the user's decision) ----
 *
 * With the temperature guard and the fault policy (thermal_calc.h) and the
 * switch between job chains above, 702 MHz is the default where the
 * firmware has put vdd_gpu_s0 at the voltage mainline asks for it: 775 mV,
 * mainline's 800 MHz operating point, which covers 702 (edk2-rk3576
 * 537ea09 does it on the TaishanPi 3M; GpuRail reports the rail it read
 * back). Everywhere else GpuClkHigh stays off: the CM5-IO and the ROCK 4D
 * leave BUCK5 at the PMIC's default (750 mV). There, with no GpuClk
 * stored, the GPU now starts at GPLL / 2 = 594 MHz instead of the
 * firmware's GPLL / 6 = 198 MHz (the user's decision, 2026-11-25): 594 is
 * at or below mainline's 600 MHz operating point, 700 mV, which every
 * supply above 600 MHz checks already assumes.
 *
 * Only the firmware's reading turns it on by default, never GpuRailUv:
 * that setting is somebody's claim, and it can still only lower the rail
 * the checks use (adapter.c takes the lower of the two).
 *
 * MwClkHighDefault: the GpuClkHigh this start. Setting is the DWORD in
 * Device Parameters, or MW_CK_SETTING_ABSENT. A value that is there is
 * taken as it is (0 off, 1 mainline, 2 vendor, anything else off), so
 * GpuClkHigh 0 still turns it off. Absent: 1 when the firmware reported a
 * valid rail of at least MW_CK_DEFAULT_RAIL_UV on a plain RK3576, else 0.
 * *FromDefault says which of the two it was. */
#define MW_CK_SETTING_ABSENT    0xFFFFFFFFul
#define MW_CK_DEFAULT_RAIL_UV   775000ul

MW_CK_INLINE unsigned long MwClkHighDefault(unsigned long Setting, unsigned long FwRailUv, int NotPlain,
                                            int *FromDefault)
{
    *FromDefault = Setting == MW_CK_SETTING_ABSENT;
    if (!*FromDefault)
        return Setting <= 2ul ? Setting : 0ul;
    return (FwRailUv >= MW_CK_DEFAULT_RAIL_UV && !NotPlain) ? 1ul : 0ul;
}

/* The clock to ask for at start, as (parent << 8) | divider. A stored
 * GpuClk (m5test clk ... keep) wins, whatever it is, so a board kept at
 * 594 MHz (0 2) stays there. With none stored and GpuClkHigh on, SPLL / 1
 * = 702 MHz, but only when the checks would allow it (Ok702: the supply
 * meets what 702 needs, the temperature guard runs); GpuClkWrite checks
 * again. Otherwise GPLL / 2 = 594 MHz, never the firmware's 198. */
#define MW_CK_SPLL_702          ((3ul << 8) | 1ul)
#define MW_CK_GPLL_594          ((0ul << 8) | 2ul)

MW_CK_INLINE unsigned long MwClkStartWant(unsigned long Stored, unsigned long High, int Ok702)
{
    if (Stored != 0)
        return Stored;
    return (High != 0 && Ok702) ? MW_CK_SPLL_702 : MW_CK_GPLL_594;
}
