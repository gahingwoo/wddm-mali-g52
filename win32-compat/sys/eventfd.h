/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <errno.h>
#include <stdint.h>
#define EFD_CLOEXEC   0x80000
#define EFD_NONBLOCK  0x800
#define EFD_SEMAPHORE 0x1
typedef uint64_t eventfd_t;
static inline int eventfd(unsigned int initval, int flags)
{
   (void)initval; (void)flags;
   errno = ENOSYS;
   return -1;
}
static inline int eventfd_read(int fd, eventfd_t *value) { (void)fd; (void)value; errno = ENOSYS; return -1; }
static inline int eventfd_write(int fd, eventfd_t value) { (void)fd; (void)value; errno = ENOSYS; return -1; }
