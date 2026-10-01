/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <string.h>
/* POSIX basename(): may modify path; returns the last component. */
static inline char *basename(char *path)
{
   char *p = path, *last = path;
   if (!path || !*path)
      return (char *)".";
   for (; *p; p++)
      if ((*p == '/' || *p == '\\') && p[1])
         last = p + 1;
   size_t n = strlen(last);
   while (n > 1 && (last[n - 1] == '/' || last[n - 1] == '\\'))
      last[--n] = '\0';
   return last;
}
