/*
 * vdec.h - the user-mode driver's video decoder backend: DXVA H.264
 * pictures decoded by the RK3576 VDPU383.
 *
 * Milestone V3a (docs/VIDEO-DECODE.md): the decoder is driven through
 * vdpu383.sys (woa-rk3576, drivers/video/vdpu383) from the process that
 * decodes, with the bring-up interface vdpu383test proved on the board:
 * contiguous buffers from IOCTL_VDPU383_ALLOC, the register file built here
 * by the same library (vdpu383/lib), one IOCTL_VDPU383_RUN per picture.
 * Each DXVA surface index has a decoder surface of its own (references are
 * read from there); the frame is then copied by the caller into the
 * application's NV12 texture.
 *
 * With D3D10UMD_VDEC_FAKE=1 (CI, no decoder) the buffers are ordinary
 * memory, the register file is still built and checked, and the "decoded"
 * picture is a pattern vdectest knows (vdec_fake_pixel).
 *
 * Plain C, no Mesa headers, so that a host test can build it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDEC_H
#define VDEC_H

#include <stddef.h>
#include <stdint.h>

#include "vdpu383/lib/v383_dxva.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vdec_h264 vdec_h264;

typedef struct vdec_picture {
    const uint8_t *data;        /* luma rows, then chroma rows (NV12) */
    unsigned stride;            /* bytes per row, both planes */
    unsigned luma_rows;         /* rows of luma before the chroma starts */
    unsigned width, height;     /* coded size (macroblocks * 16) */
} vdec_picture;

typedef struct vdec_stats {
    unsigned pictures;          /* decoded by this process */
    unsigned errors;            /* run failed, timed out or reported an error */
    unsigned hw_us_total;       /* time inside IOCTL_VDPU383_RUN */
    unsigned driver_runs;       /* vdpu383.sys's own counter (all processes), 0 if unknown */
    unsigned fake;              /* 1: D3D10UMD_VDEC_FAKE */
} vdec_stats;

/* 1 when a decoder can be opened in this process (the device or the fake);
 * cached after the first call. */
int vdec_available(void);

/* A decoder for pictures up to width x height (luma pixels). NULL and a
 * reason in err when the device or the memory is not there. */
vdec_h264 *vdec_h264_create(unsigned width, unsigned height, char *err, size_t errlen);
void vdec_h264_destroy(vdec_h264 *d);

/* Decode one picture. pp and the bitstream are what DXVA hands a driver
 * (the short slice format: each slice NAL unit behind a 00 00 01 start
 * code); qm may be NULL (flat). On success, *out describes the decoded
 * picture, valid until the next call. Returns 0, or -1 with err filled. */
int vdec_h264_decode(vdec_h264 *d, const V383_DXVA_PicParams_H264 *pp,
                     const V383_DXVA_Qmatrix_H264 *qm, const uint8_t *bitstream,
                     unsigned bitstream_len, vdec_picture *out, char *err, size_t errlen);

void vdec_get_stats(vdec_stats *s);

/* The pattern the fake decoder writes: every luma byte of a picture is
 * (StatusReportFeedbackNumber & 0xFF), every chroma byte 0x80 ^ the
 * picture's surface index. */
static inline uint8_t vdec_fake_luma(unsigned status_report) { return (uint8_t)status_report; }
static inline uint8_t vdec_fake_chroma(unsigned index) { return (uint8_t)(0x80 ^ index); }

#ifdef __cplusplus
}
#endif

#endif /* VDEC_H */
