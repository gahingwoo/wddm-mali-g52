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
