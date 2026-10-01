/* SPDX-License-Identifier: GPL-2.0 */
/* Panfrost only uses plain mutexes. They map onto Mesa's simple_mtx, which
 * (like a Linux pthread mutex, unlike a Windows CRITICAL_SECTION) is valid
 * when zeroed: Panfrost never initialises dev->bo_map_lock and relies on
 * that. A CRITICAL_SECTION here crashed the first panfrost_bo_unreference. */
#pragma once
#include "util/simple_mtx.h"
typedef simple_mtx_t pthread_mutex_t;
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
   (void)attr;
   simple_mtx_init(m, mtx_plain);
   return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) { simple_mtx_lock(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { simple_mtx_unlock(m); return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { simple_mtx_destroy(m); return 0; }

/* panfrost_kmod.c raises its event thread to SCHED_FIFO; on Windows that
 * thread is replaced in M3, so the request simply fails. */
#include <errno.h>
#ifndef SCHED_FIFO
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
struct sched_param { int sched_priority; };
static inline int sched_get_priority_max(int policy) { (void)policy; return 0; }
static inline int sched_get_priority_min(int policy) { (void)policy; return 0; }
static inline int sched_setscheduler(int pid, int policy, const struct sched_param *param)
{ (void)pid; (void)policy; (void)param; errno = ENOSYS; return -1; }
#endif
