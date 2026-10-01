/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif
static inline long sysconf(int name) { return name == _SC_PAGESIZE ? 4096 : -1; }
