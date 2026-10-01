/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <errno.h>
#include "posix_time.h"
#define TFD_CLOEXEC  0x80000
#define TFD_NONBLOCK 0x800
#define TFD_TIMER_ABSTIME 0x1
struct itimerspec { struct timespec it_interval; struct timespec it_value; };
static inline int timerfd_create(int clockid, int flags) { (void)clockid; (void)flags; errno = ENOSYS; return -1; }
static inline int timerfd_settime(int fd, int flags, const struct itimerspec *n, struct itimerspec *o)
{ (void)fd; (void)flags; (void)n; (void)o; errno = ENOSYS; return -1; }
