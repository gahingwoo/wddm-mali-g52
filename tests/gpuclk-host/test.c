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

/* The default above 600 MHz (2026-11-25): GpuClkHigh on by itself only
 * on the firmware's reading of at least 775 mV on a plain RK3576, a value
 * that is set always wins, and a stored GpuClk always wins at start. */
static void defaults(void)
{
    static const unsigned long rails[] = { 0, 550000, 700000, 750000, 768750, 774999, 775000, 781250, 850000, 900000 };
    int fromDefault;

    for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++)
        for (int notPlain = 0; notPlain < 2; notPlain++) {
            unsigned long want = (rails[i] >= 775000ul && !notPlain) ? 1ul : 0ul;
            unsigned long h = MwClkHighDefault(MW_CK_SETTING_ABSENT, rails[i], notPlain, &fromDefault);
            CHECK(h == want && fromDefault == 1, "absent, rail %lu, notPlain %d: got %lu", rails[i], notPlain, h);
            for (unsigned long set = 0; set < 6; set++) {
                h = MwClkHighDefault(set, rails[i], notPlain, &fromDefault);
                CHECK(h == (set <= 2 ? set : 0) && fromDefault == 0, "set %lu, rail %lu: got %lu", set, rails[i], h);
            }
        }
    /* The boards: TaishanPi 3M with 537ea09 (775 mV read back) on; the
     * CM5-IO and the ROCK 4D at the PMIC's 750 mV, or with no report, off. */
    CHECK(MwClkHighDefault(MW_CK_SETTING_ABSENT, 775000, 0, &fromDefault) == 1, "TSPI 775 mV: on");
    CHECK(MwClkHighDefault(MW_CK_SETTING_ABSENT, 750000, 0, &fromDefault) == 0, "750 mV: off");
    CHECK(MwClkHighDefault(MW_CK_SETTING_ABSENT, 0, 0, &fromDefault) == 0, "no report: off");
    CHECK(MwClkHighDefault(0, 775000, 0, &fromDefault) == 0 && !fromDefault, "GpuClkHigh 0 turns it off");

    /* The clock at start. */
    CHECK(MwClkStartWant(0, 1, 1) == ((3ul << 8) | 1ul), "on, allowed, nothing stored: SPLL / 1");
    CHECK(MwClkStartWant(0, 2, 1) == MW_CK_SPLL_702, "vendor table, allowed: 702");
    CHECK(MwClkStartWant(0, 1, 0) == MW_CK_GPLL_594, "on but not allowed (rail, no guard): 594");
    CHECK(MwClkStartWant(0, 0, 1) == MW_CK_GPLL_594, "off (CM5-IO, ROCK 4D at 750 mV): 594, not 198");
    CHECK(MwClkStartWant(0, 0, 0) == MW_CK_GPLL_594, "off, nothing known: 594");
    for (unsigned long h = 0; h < 3; h++)
        for (int ok = 0; ok < 2; ok++)
            CHECK(MwClkStartWant(0, h, ok) != 0, "never the firmware's 198 (High %lu, ok %d)", h, ok);
    CHECK(MwClkRate(MwClkVal(MW_CK_GPLL_594 >> 8, MW_CK_GPLL_594 & 0xFF), pll) == 594000000ul, "GPLL / 2 is 594 MHz");
    CHECK(MwClkStartWant(0x0002, 1, 1) == 0x0002, "stored 594 wins over the default");
    CHECK(MwClkStartWant(0x0301, 0, 0) == 0x0301, "stored 702 is still asked for (GpuClkWrite refuses it)");
    CHECK(MwClkRate(MwClkVal(MW_CK_SPLL_702 >> 8, MW_CK_SPLL_702 & 0xFF), pll) == 702000000ul, "SPLL / 1 is 702 MHz");
}

int main(void)
{
    all_pairs();
    board();
    defaults();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
