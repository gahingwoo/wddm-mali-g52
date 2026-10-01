/* SPDX-License-Identifier: GPL-2.0 */
/* Waiting on fences goes through the Mali driver in M3; nothing to poll yet. */
#pragma once
#include <errno.h>
#include "posix_time.h"
#if !defined(_WINSOCK2API_) && !defined(_WINSOCKAPI_)
struct pollfd { int fd; short events; short revents; };
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
#endif
static inline int poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
   (void)fds; (void)nfds; (void)timeout;
   errno = ENOSYS;
   return -1;
}
