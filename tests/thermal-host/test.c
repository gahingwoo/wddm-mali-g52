// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/thermal_calc.h: the TSADC code table, the hotter of the
 * two GPU candidates, and the hold/release band (drop at 85 C, back only
 * below 75 C, three unreadable samples in a row count as hot).
 */
#include <stdio.h>
#include "../../drivers/maliwddm/thermal_calc.h"

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

static void table(void)
{
    long t = 0;

    CHECK(!MwTsadcToMilliC(0, &t), "code 0 is not a temperature");
    CHECK(!MwTsadcToMilliC(214, &t), "code 214 is below the table");
    CHECK(!MwTsadcToMilliC(0x200, &t), "a code above 9 bits is not a temperature");
    CHECK(MwTsadcToMilliC(215, &t) && t == -40000, "215 is -40 C, got %ld", t);
    CHECK(MwTsadcToMilliC(285, &t) && t == 25000, "285 is 25 C, got %ld", t);
    CHECK(MwTsadcToMilliC(350, &t) && t == 85000, "350 is 85 C, got %ld", t);
    CHECK(MwTsadcToMilliC(395, &t) && t == 125000, "395 is 125 C, got %ld", t);
    CHECK(MwTsadcToMilliC(0x1FF, &t) && t == 125000, "511 clamps to 125 C, got %ld", t);
    /* Monotonic over the whole table. */
    {
        long prev = -1000000;
        for (unsigned long c = 215; c <= 0x1FF; c++) {
            CHECK(MwTsadcToMilliC(c, &t) && t >= prev, "code %lu: %ld after %ld", c, t, prev);
            prev = t;
        }
    }
}

static void hotter(void)
{
    long t = 0;

    CHECK(!MwThermalHotter(0, 0x3FF, &t), "neither channel reads");
    CHECK(MwThermalHotter(285, 350, &t) && t == 85000, "channel 5 hotter, got %ld", t);
    CHECK(MwThermalHotter(350, 285, &t) && t == 85000, "channel 3 hotter, got %ld", t);
    CHECK(MwThermalHotter(0, 285, &t) && t == 25000, "only channel 5 reads, got %ld", t);
    CHECK(MwThermalHotter(285, 0, &t) && t == 25000, "only channel 3 reads, got %ld", t);
}

static void band(void)
{
    MW_THERMAL s = {0};

    CHECK(MwThermalStep(&s, 1, 50000) == MW_THERMAL_NONE && !s.Held, "50 C: nothing");
    CHECK(MwThermalStep(&s, 1, 84999) == MW_THERMAL_NONE && !s.Held, "84.999 C: nothing");
    CHECK(MwThermalStep(&s, 1, 85000) == MW_THERMAL_DROP && s.Held && s.Drops == 1, "85 C: drop");
    CHECK(MwThermalStep(&s, 1, 90000) == MW_THERMAL_NONE && s.Held && s.Drops == 1, "90 C held: no second drop");
    CHECK(MwThermalStep(&s, 1, 80000) == MW_THERMAL_NONE && s.Held, "80 C: still held");
    CHECK(MwThermalStep(&s, 1, 75000) == MW_THERMAL_NONE && s.Held, "75 C exactly: still held");
    CHECK(MwThermalStep(&s, 1, 74999) == MW_THERMAL_RESTORE && !s.Held, "74.999 C: back");
    CHECK(MwThermalStep(&s, 1, 84000) == MW_THERMAL_NONE && !s.Held, "84 C after release: nothing");
    CHECK(MwThermalStep(&s, 1, 86000) == MW_THERMAL_DROP && s.Drops == 2, "86 C: second drop");
    CHECK(s.LastMilliC == 86000, "last reading kept, got %ld", s.LastMilliC);
}

static void unreadable(void)
{
    MW_THERMAL s = {0};

    CHECK(MwThermalStep(&s, 0, 0) == MW_THERMAL_NONE && !s.Held, "1 unreadable: nothing");
    CHECK(MwThermalStep(&s, 0, 0) == MW_THERMAL_NONE && !s.Held, "2 unreadable: nothing");
    CHECK(MwThermalStep(&s, 1, 60000) == MW_THERMAL_NONE && s.Invalid == 0, "a good reading resets the count");
    CHECK(MwThermalStep(&s, 0, 0) == MW_THERMAL_NONE, "1 again");
    CHECK(MwThermalStep(&s, 0, 0) == MW_THERMAL_NONE, "2 again");
    CHECK(MwThermalStep(&s, 0, 0) == MW_THERMAL_DROP && s.Held, "3 unreadable in a row: drop");
    CHECK(MwThermalStep(&s, 0, 0) == MW_THERMAL_NONE && s.Held && s.Drops == 1, "still unreadable: held, one drop");
    CHECK(MwThermalStep(&s, 1, 80000) == MW_THERMAL_NONE && s.Held, "readable again at 80 C: held");
    CHECK(MwThermalStep(&s, 1, 60000) == MW_THERMAL_RESTORE && !s.Held, "60 C: back");
}

int main(void)
{
    table();
    hotter();
    band();
    unreadable();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
