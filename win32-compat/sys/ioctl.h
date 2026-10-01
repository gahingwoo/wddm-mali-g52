/* SPDX-License-Identifier: GPL-2.0 */
/* For util/libsync.h; sync files go through the Mali driver in M3. */
#pragma once
#include <errno.h>
#include "sys/ioccom.h"
static inline int ioctl(int fd, unsigned long request, ...)
{
   (void)fd; (void)request;
   errno = ENOSYS;
   return -1;
}
