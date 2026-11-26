/* SPDX-License-Identifier: GPL-2.0 */
/* major/minor of a dev_t, for panvk's VK_EXT_physical_device_drm answers.
 * There are no device numbers on Windows; panvk leaves them 0 there. */
#pragma once
#include <sys/types.h>
#ifndef major
#define major(dev) ((unsigned)(((dev) >> 8) & 0xfff))
#define minor(dev) ((unsigned)(((dev) & 0xff) | (((dev) >> 12) & 0xfff00)))
#define makedev(ma, mi) ((dev_t)((((ma) & 0xfff) << 8) | ((mi) & 0xff) | (((mi) & 0xfff00) << 12)))
#endif
