/*
 * md5.c - MD5 (RFC 1321), for comparing decoded frames with the conformance
 * reference output. Written from the RFC.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include <stdio.h>
#include "md5.h"

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const uint8_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_block(Md5 *m, const uint8_t *p)
{
    uint32_t w[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3], f, t;
    unsigned i, g;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
               ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    for (i = 0; i < 64; i++) {
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        t = d;
        d = c;
        c = b;
        f = a + f + K[i] + w[g];
        b = b + ((f << S[i]) | (f >> (32 - S[i])));
        a = t;
    }
    m->h[0] += a;
    m->h[1] += b;
    m->h[2] += c;
    m->h[3] += d;
}

void md5_init(Md5 *m)
{
    m->h[0] = 0x67452301;
    m->h[1] = 0xefcdab89;
    m->h[2] = 0x98badcfe;
    m->h[3] = 0x10325476;
    m->len = 0;
    m->n = 0;
}

void md5_update(Md5 *m, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    m->len += len;
    while (len) {
        size_t take = 64 - m->n;
        if (take > len)
            take = len;
        memcpy(m->buf + m->n, p, take);
        m->n += (unsigned)take;
        p += take;
        len -= take;
        if (m->n == 64) {
            md5_block(m, m->buf);
            m->n = 0;
        }
    }
}

void md5_final(Md5 *m, char hex[33])
{
    uint8_t pad[72];
    uint64_t bits = m->len * 8;
    unsigned padlen = (m->n < 56) ? 56 - m->n : 120 - m->n, i;
    uint8_t out[16];

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    md5_update(m, pad, padlen);
    for (i = 0; i < 8; i++)
        pad[i] = (uint8_t)(bits >> (8 * i));
    md5_update(m, pad, 8);
    for (i = 0; i < 4; i++) {
        out[4 * i] = (uint8_t)m->h[i];
        out[4 * i + 1] = (uint8_t)(m->h[i] >> 8);
        out[4 * i + 2] = (uint8_t)(m->h[i] >> 16);
        out[4 * i + 3] = (uint8_t)(m->h[i] >> 24);
    }
    for (i = 0; i < 16; i++)
        sprintf(hex + 2 * i, "%02x", out[i]);
    hex[32] = 0;
}
