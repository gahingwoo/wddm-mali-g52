/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static inline void errx(int code, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
   exit(code);
}
#define err errx
