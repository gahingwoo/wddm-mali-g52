/* SPDX-License-Identifier: GPL-2.0 */
/* mmap of a Mali buffer object, through the kernel driver (malikm_drm.c).
 * Any other fd fails with ENODEV; there is no anonymous or file mapping. */
#pragma once
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);

static inline int mprotect(void *addr, size_t len, int prot)
{
   (void)addr; (void)len; (void)prot;
   errno = ENOSYS;
   return -1;
}

#ifdef __cplusplus
}
#endif
