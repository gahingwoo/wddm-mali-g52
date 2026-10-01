/* SPDX-License-Identifier: GPL-2.0 */
/* clock_gettime for Panfrost's timeouts and BO cache ages. The UCRT has only
 * timespec_get(TIME_UTC); it stands in for CLOCK_MONOTONIC, which is fine for
 * the relative timeouts these callers compute. */
#pragma once
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
static inline int clock_gettime(int clk, struct timespec *ts)
{
   (void)clk;
   return timespec_get(ts, TIME_UTC) ? 0 : -1;
}
#endif
