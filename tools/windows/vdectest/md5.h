/*
 * md5.h - see md5.c.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383TEST_MD5_H
#define VDPU383TEST_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct Md5 {
    uint32_t h[4];
    uint64_t len;
    uint8_t  buf[64];
    unsigned n;
} Md5;

void md5_init(Md5 *m);
void md5_update(Md5 *m, const void *data, size_t len);
void md5_final(Md5 *m, char hex[33]);

#endif
