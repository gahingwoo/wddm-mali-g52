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
