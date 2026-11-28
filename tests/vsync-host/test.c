// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/vsync_calc.h: the emulated vsync's period from a mode's
 * refresh rate, the fallbacks, and when a new period is armed.
 */
#include <stdio.h>
#include "../../drivers/maliwddm/vsync_calc.h"

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

int main(void)
{
    mwv_u64 p;

    /* 2560x1440@60 CVT-RB: 241.5 MHz over 2720 x 1481 = 59.951 Hz. */
    p = MwVsyncPeriod(241500000ull, 2720ull * 1481);
    CHECK(p == 166804, "1440p60 is 166804, got %llu", p);
    /* 1920x1080@60 CEA: exactly 60 Hz. */
    p = MwVsyncPeriod(148500000ull, 2200ull * 1125);
    CHECK(p == 166667, "1080p60 is 166667, got %llu", p);
    /* 3840x2160@30: 297 MHz over 4400 x 2250. */
    p = MwVsyncPeriod(297000000ull, 4400ull * 2250);
    CHECK(p == 333333, "2160p30 is 333333, got %llu", p);
    /* 2560x1440@100 (397.98 MHz, the AOC panel's DTD): 2720 x 1463. */
    p = MwVsyncPeriod(397980000ull, 2720ull * 1463);
    CHECK(p == 99989 || p == 99990, "1440p100 is about 99990, got %llu", p);
    /* 59.94 as 60000/1001. */
    p = MwVsyncPeriod(60000, 1001);
    CHECK(p == 166833, "59.94 Hz is 166833, got %llu", p);

    CHECK(MwVsyncPeriod(0, 1) == 0, "no numerator");
    CHECK(MwVsyncPeriod(60, 0) == 0, "no denominator");
    CHECK(MwVsyncPeriod(300, 1) == 0, "300 Hz is not believed");
    CHECK(MwVsyncPeriod(240, 1) == 41667, "240 Hz is the fastest");
    CHECK(MwVsyncPeriod(10, 1) == 0, "10 Hz is not believed");
    CHECK(MwVsyncPeriod(20, 1) == 500000, "20 Hz is the slowest");
    CHECK(MwVsyncPeriod(1, ~0ull) == 0, "no overflow");

    /* The HDMI mode first, the firmware's timing next, 60 Hz last. */
    p = MwVsyncChoose(1, 241500000ull, 2720ull * 1481, 148500000ull, 2200, 1125);
    CHECK(p == 166804, "HdmiHpd 3: the HDMI mode, got %llu", p);
    p = MwVsyncChoose(0, 241500000ull, 2720ull * 1481, 148500000ull, 2200, 1125);
    CHECK(p == 166667, "no switch: the firmware's timing, got %llu", p);
    p = MwVsyncChoose(1, 0, 0, 148500000ull, 2200, 1125);
    CHECK(p == 166667, "switch without a mode yet: the firmware's, got %llu", p);
    p = MwVsyncChoose(1, 0, 0, 0, 0, 0);
    CHECK(p == MW_VSYNC_DEFAULT_100NS, "nothing known: 60 Hz, got %llu", p);
    /* A DSI panel's firmware timing, 71 MHz over 908 x 1317 (hpd.c). */
    p = MwVsyncChoose(0, 0, 0, 71000000ull, 908, 1317);
    CHECK(p == 168428, "TSPI panel about 59.37 Hz, got %llu", p);

    CHECK(MwVsyncRearm(0, 166667), "nothing armed");
    CHECK(!MwVsyncRearm(166667, 166667), "same period");
    CHECK(!MwVsyncRearm(166667, 166700), "40 ns off is the same");
    CHECK(MwVsyncRearm(166667, 166804), "60 to 59.95 Hz arms again");
    CHECK(MwVsyncRearm(166804, 99990), "60 to 100 Hz arms again");

    printf("%d check(s), %d failed\n", checks, fails);
    return fails != 0;
}
