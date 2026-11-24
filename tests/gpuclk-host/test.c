// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/gpuclk_calc.h: the CRU_CLKSEL_CON165 writes that take
 * the GPU clock from one mux/divider to another. For every pair of the
 * PLLs the driver takes and every divider: the writes end at the target,
 * each changes one field under its own mask, bit 8 (PVTPLL) is never
 * written, and no state on the way is faster than the faster end (the
 * 2026-11-24 drop wrote both fields at once, which may pass through
 * GPLL / 1 = 1188 MHz). Plus the board's two transitions, by value.
 */
#include <stdio.h>
#include "../../drivers/maliwddm/gpuclk_calc.h"

static int fails, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fails++;                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static const unsigned long pll[8] = { 1188000000ul, 1000000000ul, 0, 702000000ul, 0, 0, 0, 0 };
static const unsigned muxes[] = { 0, 1, 3 };

static void all_pairs(void)
{
    for (unsigned a = 0; a < 3; a++)
        for (unsigned da = 1; da <= 32; da++)
            for (unsigned b = 0; b < 3; b++)
                for (unsigned db = 1; db <= 32; db++) {
                    unsigned long cur = MwClkVal(muxes[a], da), next = MwClkVal(muxes[b], db), w[2];
                    unsigned long reg = cur;
                    unsigned long hi = MwClkRate(cur, pll) > MwClkRate(next, pll) ? MwClkRate(cur, pll)
                                                                                  : MwClkRate(next, pll);
                    unsigned n = MwClkSteps(cur, next, pll, w);
                    int expect = (cur & 0xE0) != (next & 0xE0) ? 1 : 0;
                    expect += (cur & 0x1F) != (next & 0x1F) ? 1 : 0;

                    CHECK(n == (unsigned)expect, "%lx->%lx: %u writes, expected %d", cur, next, n, expect);
                    for (unsigned i = 0; i < n; i++) {
                        unsigned long mask = w[i] >> 16;
                        CHECK(mask == 0xE0 || mask == 0x1F, "%lx->%lx: write %u mask %lx", cur, next, i, mask);
                        CHECK((w[i] & 0xFFFF & ~mask) == 0, "%lx->%lx: bits outside the mask", cur, next);
                        CHECK((mask & 0x100) == 0, "%lx->%lx: PVTPLL bit written", cur, next);
                        reg = MwClkApplyWrite(reg, w[i]);
                        CHECK(MwClkRate(reg, pll) <= hi, "%lx->%lx: %lu Hz on the way, ends %lu and %lu", cur,
                              next, MwClkRate(reg, pll), MwClkRate(cur, pll), MwClkRate(next, pll));
                    }
                    CHECK((reg & 0xFF) == next, "%lx->%lx: ends at %lx", cur, next, reg & 0xFF);
                }
}

static void board(void)
{
    unsigned long w[2], reg;
    unsigned n;

    /* The drop: SPLL / 1 (702) to GPLL / 2 (594). Divider first: 351 MHz
     * on the way, never GPLL / 1. As TF-A's clk_gpu_set_rate. */
    n = MwClkSteps(MwClkVal(3, 1), MwClkVal(0, 2), pll, w);
    CHECK(n == 2 && w[0] == ((0x1Ful << 16) | 1u) && w[1] == ((0xE0ul << 16) | 0u), "702 -> 594: div, then mux");
    reg = MwClkApplyWrite(MwClkVal(3, 1), w[0]);
    CHECK(MwClkRate(reg, pll) == 351000000ul, "halfway 351 MHz, got %lu", MwClkRate(reg, pll));
    /* The restore: GPLL / 2 to SPLL / 1. Mux first: SPLL / 2 = 351. */
    n = MwClkSteps(MwClkVal(0, 2), MwClkVal(3, 1), pll, w);
    CHECK(n == 2 && (w[0] >> 16) == 0xE0 && (w[1] >> 16) == 0x1F, "594 -> 702: mux, then div");
    reg = MwClkApplyWrite(MwClkVal(0, 2), w[0]);
    CHECK(MwClkRate(reg, pll) == 351000000ul, "halfway 351 MHz, got %lu", MwClkRate(reg, pll));
    /* The start: the firmware's GPLL / 6 (198) to 594, the divider alone. */
    n = MwClkSteps(MwClkVal(0, 6), MwClkVal(0, 2), pll, w);
    CHECK(n == 1 && w[0] == ((0x1Ful << 16) | 1u), "198 -> 594: one divider write");
    /* GPLL / 6 to SPLL / 1 (GpuClk 0x301 at start). */
    n = MwClkSteps(MwClkVal(0, 6), MwClkVal(3, 1), pll, w);
    reg = MwClkApplyWrite(MwClkVal(0, 6), w[0]);
    CHECK(n == 2 && MwClkRate(reg, pll) <= 702000000ul, "198 -> 702 halfway %lu", MwClkRate(reg, pll));
    CHECK(MwClkSteps(MwClkVal(0, 2), MwClkVal(0, 2), pll, w) == 0, "no change, no write");
}

int main(void)
{
    all_pairs();
    board();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
