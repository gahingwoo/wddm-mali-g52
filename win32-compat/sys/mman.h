/* SPDX-License-Identifier: GPL-2.0 */
/* Buffer mapping goes through the Mali driver in M3; it fails until then. */
#pragma once
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)
static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
   (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
   errno = ENOSYS;
   return MAP_FAILED;
}
static inline int munmap(void *addr, size_t len) { (void)addr; (void)len; errno = ENOSYS; return -1; }
