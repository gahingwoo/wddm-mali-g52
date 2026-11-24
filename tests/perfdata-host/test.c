// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/perfdata_calc.h: which QAI types the driver answers at
 * which WDDM version, the temperature as Task Manager reads it (from the
 * TSADC code through thermal_calc.h to deci-Celsius to the whole degrees
 * the GPU page prints), the clock from CRU_CLKSEL_CON165, and the highest
 * reachable clock under a limit.
 */
#include <stdio.h>
#include "../../drivers/maliwddm/thermal_calc.h"
#include "../../drivers/maliwddm/perfdata_calc.h"

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

/* adapter.c's MwGpuPll: GPLL, CPLL, (AUPLL never), SPLL, (LPLL never). */
static const unsigned long pll[8] = { 1188000000ul, 1000000000ul, 0, 702000000ul, 0, 0, 0, 0 };

static void answers(void)
{
    for (unsigned t = 0; t < 64; t++) {
        int perf = t >= 24 && t <= 27;
        /* 1.3 (MwWddmProbe 0): never, whatever PerfData says. */
        CHECK(!MwPerfAnswers(t, 0, 1), "type %u answered at 1.3", t);
        CHECK(!MwPerfAnswers(t, 0x2000, 0), "type %u answered with PerfData 0", t);
        CHECK(MwPerfAnswers(t, 0x2000, 1) == perf, "type %u at 2.0", t);
        CHECK(MwPerfAnswers(t, 0x2400, 1) == perf, "type %u at 2.4", t);
        CHECK(MwPerfPolled(t) == (t == 24 || t == 25), "type %u polled", t);
    }
}

static void node_input(void)
{
    CHECK(MwPerfNodeOk(0x00000, 2), "node 0 of 2");
    CHECK(MwPerfNodeOk(0x00001, 2), "node 1 of 2");
    CHECK(!MwPerfNodeOk(0x00002, 2), "node 2 of 2");
    CHECK(!MwPerfNodeOk(0x10000, 2), "physical adapter 1");
    CHECK(MwPerfNodeOk(0x00000, 1) && !MwPerfNodeOk(0x00001, 1), "one node at 1.3");
}

static void temperature(void)
{
    long mc = 0;

    CHECK(MwPerfDeciC(0, 50000) == 0, "no reading is 0 (Task Manager hides it)");
    CHECK(MwPerfDeciC(1, 67800) == 678, "67.8 C, got %lu", MwPerfDeciC(1, 67800));
    CHECK(MwPerfDeciC(1, 67849) == 678 && MwPerfDeciC(1, 67850) == 679, "rounding to 0.1 C");
    CHECK(MwPerfDeciC(1, 0) == 10 && MwPerfDeciC(1, -40000) == 10, "below 1 C is 1 C, not 'no sensor'");
    CHECK(MwPerfDeciC(1, 125000) == 1250, "125 C");

    CHECK(MwTaskmgrShows(0) == -1, "0 is hidden");
    CHECK(MwTaskmgrShows(9) == -1, "0.9 C is hidden too (whole degrees)");
    CHECK(MwTaskmgrShows(10) == 1, "1.0 C shows 1");
    CHECK(MwTaskmgrShows(679) == 67, "67.9 C shows 67 (truncated), got %ld", MwTaskmgrShows(679));

    /* Every valid TSADC code gives a temperature Task Manager shows. */
    for (unsigned long a = 0; a <= 0x200; a++) {
        unsigned long b = 0x1FF - (a & 0xFF);
        int v = MwThermalHotter(a, b, &mc);
        unsigned long dc = MwPerfDeciC(v, mc);
        CHECK(!v || MwTaskmgrShows(dc) >= 1, "codes %lu/%lu: valid but hidden (%lu)", a, b, dc);
        CHECK(v || dc == 0, "codes %lu/%lu: invalid but %lu", a, b, dc);
        CHECK(dc <= 1250, "codes %lu/%lu: %lu above the table", a, b, dc);
    }
    /* The board's idle without the fan, 2026-11-24: 67 to 68 C. TSADC code
     * for 67.x C sits between rows 285 (25 C) and 350 (85 C). */
    CHECK(MwThermalHotter(331, 0, &mc) && MwTaskmgrShows(MwPerfDeciC(1, mc)) == 67,
          "code 331 shows 67, got %ld (%ld mC)", MwTaskmgrShows(MwPerfDeciC(1, mc)), mc);
    /* The hotter channel counts, as for the fallback. */
    CHECK(MwThermalHotter(300, 331, &mc) && MwPerfDeciC(1, mc) >= 670, "hotter channel");
    CHECK(MW_PD_TEMP_WARNING_DC * 100 == MW_THERMAL_DROP_MC, "the warning is the drop point");
    CHECK(MW_PD_TEMP_MAX_DC > MW_PD_TEMP_WARNING_DC, "max above warning");
}

static void clock(void)
{
    CHECK(MwPerfClkHz((0u << 5) | 1u, pll) == 594000000ul, "GPLL / 2");
    CHECK(MwPerfClkHz((0u << 5) | 5u, pll) == 198000000ul, "GPLL / 6, the firmware's");
    CHECK(MwPerfClkHz((3u << 5) | 0u, pll) == 702000000ul, "SPLL / 1");
    CHECK(MwPerfClkHz((1u << 5) | 1u, pll) == 500000000ul, "CPLL / 2");
    CHECK(MwPerfClkHz(0x00FF0000ul | (0u << 5) | 1u, pll) == 594000000ul, "write-mask bits ignored");
    CHECK(MwPerfClkHz((2u << 5) | 1u, pll) == 0, "AUPLL is not known");
    CHECK(MwPerfClkHz((4u << 5) | 1u, pll) == 0, "LPLL is not known");
    CHECK(MwPerfClkHz(0x100u | 1u, pll) == 0, "the PVTPLL path is not known");
    CHECK(MwPerfClkHz(0xFFFFFFFFul, pll) == 0, "a failed read");

    CHECK(MwPerfBestHz(600000000ul, pll) == 594000000ul, "default ceiling: 594 MHz");
    CHECK(MwPerfBestHz(800000000ul, pll) == 702000000ul, "GpuClkHigh to 800: 702 MHz");
    CHECK(MwPerfBestHz(950000000ul, pll) == 702000000ul, "GpuClkHigh to 950: still 702 MHz");
    CHECK(MwPerfBestHz(0, pll) == 0, "no limit, no rate");
    /* Every rate at or below the limit, and reachable. */
    for (unsigned long lim = 30000000ul; lim <= 1200000000ul; lim += 7000000ul) {
        unsigned long b = MwPerfBestHz(lim, pll);
        int reachable = 0;
        CHECK(b <= lim, "limit %lu: %lu", lim, b);
        for (unsigned p = 0; p < 8; p++)
            for (unsigned long d = 1; d <= 32; d++)
                if (pll[p] && pll[p] / d == b)
                    reachable = 1;
        CHECK(reachable, "limit %lu: %lu is no PLL / divider", lim, b);
    }
}

static void pcw(void)
{
    CHECK(MwPcwTempC(0, 50000) == 0, "no reading is 0");
    CHECK(MwPcwTempC(1, 67900) == 67, "67.9 C is 67, as Task Manager");
    for (long m = -40000; m <= 130000; m += 37)
        CHECK((long)MwPcwTempC(1, m) == MwTaskmgrShows(MwPerfDeciC(1, m)), "%ld mC: counter %lu, Task Manager %ld",
              m, MwPcwTempC(1, m), MwTaskmgrShows(MwPerfDeciC(1, m)));
    CHECK(MwPcwMhz(594000000ul) == 594 && MwPcwMhz(702000000ul) == 702 && MwPcwMhz(198000000ul) == 198, "MHz");
    CHECK(MwPcwMhz(0) == 0, "unknown clock is 0");
    CHECK(MwPcwTicksTo100ns(24000000ull, 24000000ull) == 10000000ull, "one second at 24 MHz");
    CHECK(MwPcwTicksTo100ns(12000000ull, 24000000ull) == 5000000ull, "half a second");
    CHECK(MwPcwTicksTo100ns(5, 0) == 0, "no frequency");
    /* A year at 1 GHz: no overflow, exact to 100 ns. */
    {
        unsigned long long hz = 1000000000ull, t = 365ull * 86400ull * hz + 123456789ull;
        CHECK(MwPcwTicksTo100ns(t, hz) == 365ull * 86400ull * 10000000ull + 1234567ull, "a year at 1 GHz");
    }
    /* Monotonic: PDH must never see the busy time go back. */
    {
        unsigned long long prev = 0;
        for (unsigned long long t = 0; t < 2000000ull; t += 977) {
            unsigned long long v = MwPcwTicksTo100ns(t, 19200000ull);
            CHECK(v >= prev, "tick %llu: %llu after %llu", t, v, prev);
            prev = v;
        }
    }
}

int main(void)
{
    pcw();
    answers();
    node_input();
    temperature();
    clock();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
