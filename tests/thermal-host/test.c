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
    unsigned long long now = 0;

    CHECK(MwThermalStep(&s, 1, 50000, ++now, 0) == MW_THERMAL_NONE && !s.Held, "50 C: nothing");
    CHECK(MwThermalStep(&s, 1, 84999, ++now, 0) == MW_THERMAL_NONE && !s.Held, "84.999 C: nothing");
    CHECK(MwThermalStep(&s, 1, 85000, ++now, 0) == MW_THERMAL_DROP && s.Held && s.Drops == 1, "85 C: drop");
    CHECK(MwThermalStep(&s, 1, 90000, ++now, 0) == MW_THERMAL_NONE && s.Held && s.Drops == 1, "90 C held: no second drop");
    CHECK(MwThermalStep(&s, 1, 80000, ++now, 0) == MW_THERMAL_NONE && s.Held, "80 C: still held");
    CHECK(MwThermalStep(&s, 1, 75000, ++now, 0) == MW_THERMAL_NONE && s.Held, "75 C exactly: still held");
    CHECK(MwThermalStep(&s, 1, 74999, ++now, 0) == MW_THERMAL_RESTORE && !s.Held, "74.999 C: back");
    CHECK(MwThermalStep(&s, 1, 84000, ++now, 0) == MW_THERMAL_NONE && !s.Held, "84 C after release: nothing");
    CHECK(MwThermalStep(&s, 1, 86000, ++now, 0) == MW_THERMAL_DROP && s.Drops == 2, "86 C: second drop");
    CHECK(s.LastMilliC == 86000, "last reading kept, got %ld", s.LastMilliC);
}

static void unreadable(void)
{
    MW_THERMAL s = {0};
    unsigned long long now = 0;

    CHECK(MwThermalStep(&s, 0, 0, ++now, 0) == MW_THERMAL_NONE && !s.Held, "1 unreadable: nothing");
    CHECK(MwThermalStep(&s, 0, 0, ++now, 0) == MW_THERMAL_NONE && !s.Held, "2 unreadable: nothing");
    CHECK(MwThermalStep(&s, 1, 60000, ++now, 0) == MW_THERMAL_NONE && s.Invalid == 0, "a good reading resets the count");
    CHECK(MwThermalStep(&s, 0, 0, ++now, 0) == MW_THERMAL_NONE, "1 again");
    CHECK(MwThermalStep(&s, 0, 0, ++now, 0) == MW_THERMAL_NONE, "2 again");
    CHECK(MwThermalStep(&s, 0, 0, ++now, 0) == MW_THERMAL_DROP && s.Held, "3 unreadable in a row: drop");
    CHECK(MwThermalStep(&s, 0, 0, ++now, 0) == MW_THERMAL_NONE && s.Held && s.Drops == 1, "still unreadable: held, one drop");
    CHECK(MwThermalStep(&s, 1, 80000, ++now, 0) == MW_THERMAL_NONE && s.Held, "readable again at 80 C: held");
    CHECK(MwThermalStep(&s, 1, 60000, ++now, 0) == MW_THERMAL_RESTORE && !s.Held, "60 C: back");
}

/* Back to the clock asked for only after HoldMs below 75 C at every
 * reading; one warm reading starts the hold over; the drop stays at once. */
static void hold(void)
{
    MW_THERMAL s = {0};
    unsigned long long h = MwThermalHoldMs(0), t = 1000;

    CHECK(h == 90000, "default hold 90 s, got %llu", h);
    CHECK(MwThermalHoldMs(30) == 30000 && MwThermalHoldMs(100000) == 3600000, "setting and its ceiling");
    CHECK(MwThermalStep(&s, 1, 85000, t, h) == MW_THERMAL_DROP, "85 C: drop at once");
    CHECK(MwThrottleReason(&s) == MW_THROTTLE_THERMAL, "reason thermal");
    for (int i = 1; i <= 89; i++)
        CHECK(MwThermalStep(&s, 1, 70000, t + i * 1000ull, h) == MW_THERMAL_NONE && s.Held,
              "70 C for %d s: still held", i);
    CHECK(MwThermalStep(&s, 1, 75000, t + 90000, h) == MW_THERMAL_NONE && s.Held && s.CoolSinceMs == 0,
          "75 C after 89 s: the hold starts over");
    for (int i = 1; i <= 90; i++)
        CHECK(MwThermalStep(&s, 1, 60000, t + 90000 + i * 1000ull, h) == MW_THERMAL_NONE && s.Held,
              "60 C reading %d after the restart: held", i);
    CHECK(MwThermalStep(&s, 1, 60000, t + 90000 + 91000, h) == MW_THERMAL_RESTORE && !s.Held,
          "90 s below 75 C: back");
    CHECK(MwThrottleReason(&s) == MW_THROTTLE_NONE, "reason none");
    CHECK(MwThermalStep(&s, 1, 84000, t + 200000, h) == MW_THERMAL_NONE, "84 C: nothing");
    CHECK(MwThermalStep(&s, 1, 85000, t + 201000, h) == MW_THERMAL_DROP, "85 C again: at once");
    /* An unreadable reading is not a cool one. */
    CHECK(MwThermalStep(&s, 1, 60000, t + 202000, h) == MW_THERMAL_NONE && s.CoolSinceMs != 0, "cool starts");
    CHECK(MwThermalStep(&s, 0, 0, t + 203000, h) == MW_THERMAL_NONE && s.CoolSinceMs == 0, "unreadable: over");
}

/* The 2026-11-24 board run, replayed: 702 MHz, FurMark, fan off; the
 * readings of gpu-thermal-fallback-2026-11-24.log every 8 s. */
static void board_run(void)
{
    static const long temps[] = { 73, 77, 79, 83, 86, 80, 81, 82, 80, 75, 73, 70, 68, 66, 66, 66, 66, 66, 66, 66, 66, 66 };
    MW_THERMAL s = {0};
    unsigned long long h = MwThermalHoldMs(0), t = 0;
    int drops = 0, restores = 0;
    unsigned long long restoredAt = 0, coolFrom = 0;

    for (unsigned i = 0; i < sizeof(temps) / sizeof(temps[0]); i++) {
        for (int k = 0; k < 8; k++) {      /* one reading a second */
            int a;
            t += 1000;
            a = MwThermalStep(&s, 1, temps[i] * 1000, t, h);
            drops += a == MW_THERMAL_DROP;
            if (a == MW_THERMAL_RESTORE) {
                restores++;
                restoredAt = t;
            }
            if (temps[i] < 75 && coolFrom == 0 && s.Held)
                coolFrom = t;
        }
    }
    CHECK(drops == 1, "one drop, got %d", drops);
    CHECK(restores == 1, "one restore, got %d", restores);
    CHECK(restoredAt >= coolFrom + h && restoredAt <= coolFrom + h + 1000,
          "restore %llu ms after the first reading below 75 C", restoredAt - coolFrom);
}

/* A fault above 600 MHz keeps the clock asked for; two within 10 minutes
 * lock 594 MHz until restart; faults at 594 MHz do not count. */
static void faults(void)
{
    MW_THERMAL s = {0};
    unsigned long long h = MwThermalHoldMs(0), t = 1000;

    CHECK(!MwThermalFault(&s, t, 0), "a fault at 594 MHz");
    CHECK(s.HighFaults == 0 && !s.FaultDropped, "does not count");
    CHECK(!MwThermalFault(&s, t, 1) && s.HighFaults == 1 && !s.FaultLocked, "first fault at 702: no lock");
    CHECK(MwThrottleReason(&s) == MW_THROTTLE_NONE, "reason none after one");
    CHECK(MwThermalStep(&s, 1, 60000, t + 1000, h) == MW_THERMAL_RESTORE, "back at the next reading");
    CHECK(MwThermalStep(&s, 1, 60000, t + 2000, h) == MW_THERMAL_NONE, "once");
    /* Eleven minutes later: a new window. */
    t += 11 * 60000ull;
    CHECK(!MwThermalFault(&s, t, 1) && !s.FaultLocked, "second fault 11 min later: no lock");
    CHECK(MwThermalStep(&s, 1, 60000, t + 1000, h) == MW_THERMAL_RESTORE, "back again");
    /* Nine minutes later: two within ten. */
    t += 9 * 60000ull;
    CHECK(MwThermalFault(&s, t, 1) && s.FaultLocked, "third fault 9 min later: locked");
    CHECK(MwThrottleReason(&s) == MW_THROTTLE_FAULTS, "reason faults");
    for (int i = 1; i < 2000; i++)
        CHECK(MwThermalStep(&s, 1, 50000, t + i * 1000ull, h) == MW_THERMAL_NONE, "locked: never back (%d)", i);
    CHECK(MwThermalStep(&s, 1, 90000, t + 3000000, h) == MW_THERMAL_DROP, "heat still drops (nothing to write)");
    CHECK(MwThrottleReason(&s) == MW_THROTTLE_FAULTS, "faults outrank heat");
    for (int i = 1; i < 200; i++)
        CHECK(MwThermalStep(&s, 1, 50000, t + 3000000 + i * 1000ull, h) == MW_THERMAL_NONE, "locked after cooling (%d)", i);
    CHECK(MwThrottleReason(&s) == MW_THROTTLE_FAULTS && !s.Held, "still faults");

    /* A fault while held for heat: the clock is 594 already, the caller
     * passes AtHigh 0. A fault during the hold at 702 is impossible. */
    {
        MW_THERMAL u = {0};
        CHECK(MwThermalStep(&u, 1, 86000, 1000, h) == MW_THERMAL_DROP, "held");
        CHECK(!MwThermalFault(&u, 2000, 0) && u.HighFaults == 0, "fault while held does not count");
        CHECK(MwThermalStep(&u, 1, 60000, 3000, h) == MW_THERMAL_NONE && u.Held, "no early restore from a fault");
    }
    /* Two quick faults right after each other (fault, then dxgkrnl's TDR of
     * the same hang): the first drops to 594, so the second is at 594 and
     * does not count. The driver passes AtHigh from the clock it had. */
    {
        MW_THERMAL u = {0};
        CHECK(!MwThermalFault(&u, 5000, 1), "the fault");
        CHECK(!MwThermalFault(&u, 5100, 0) && u.HighFaults == 1 && !u.FaultLocked, "its TDR at 594: no lock");
    }
}

int main(void)
{
    hold();
    board_run();
    faults();
    table();
    hotter();
    band();
    unreadable();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
