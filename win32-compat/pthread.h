/* SPDX-License-Identifier: GPL-2.0 */
/* Panfrost only uses plain mutexes; map them onto Mesa's C11 threads. */
#pragma once
#include "c11/threads.h"
typedef mtx_t pthread_mutex_t;
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
   (void)attr;
   return mtx_init(m, mtx_plain) == thrd_success ? 0 : -1;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) { return mtx_lock(m) == thrd_success ? 0 : -1; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { return mtx_unlock(m) == thrd_success ? 0 : -1; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { mtx_destroy(m); return 0; }

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
