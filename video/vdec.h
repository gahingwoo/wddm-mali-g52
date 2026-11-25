/*
 * vdec.h - the user-mode driver's video decoder backend: DXVA pictures
 * (H.264, HEVC Main, VP9 profile 0, AV1 Main 8 bit) decoded by the RK3576
 * VDPU383.
 *
 * Milestone V3a (docs/VIDEO-DECODE.md): the decoder is driven through
 * vdpu383.sys (woa-rk3576, drivers/video/vdpu383) from the process that
 * decodes, with the bring-up interface vdpu383test proved on the board:
 * buffers from IOCTL_VDPU383_ALLOC (IO addresses through the decoder's
 * IOMMU, or physically contiguous with the driver's Iommu 0; the same to
 * this code), the register file built here by the same library
 * (vdpu383/lib), one IOCTL_VDPU383_RUN per picture. Each DXVA surface index
 * has a decoder surface of its own (references are read from there); the
 * frame is then copied by the caller into the application's NV12 texture.
 *
 * With D3D10UMD_VDEC_FAKE=1 (CI, no decoder) the buffers are ordinary
 * memory, the register file is still built and checked, and the "decoded"
 * picture is a pattern vdectest knows (vdec_fake_luma, vdec_fake_chroma).
 *
 * With D3D10UMD_VDEC_REGLOG=<file>, every register file built is appended
 * to that file with its addresses written as buffer and offset (vdec.c,
 * reglog), so that a host test and vdectest can compare what the UMD
 * programs with what vdpu383test --dxva-only programs for the same stream
 * (tests/vdec-host).
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
#include "vdpu383/lib/v383_dxva_hevc.h"
#include "vdpu383/lib/v383_dxva_vp9.h"
#include "vdpu383/lib/v383_dxva_av1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vdec_codec {
    VDEC_CODEC_H264 = 0,
    VDEC_CODEC_HEVC = 1,
    VDEC_CODEC_VP9 = 2,
    VDEC_CODEC_AV1 = 3,
    VDEC_CODEC_COUNT
} vdec_codec;

/* Choices only the board can settle (docs/VIDEO-DECODE.md). */
typedef struct vdec_options {
    /* AV1: how the tile data DXVA hands over is wrapped for the decoder,
     * which parses OBUs itself (v383_av1_synth_stream): 0 one
     * OBU_TILE_GROUP; 1 an OBU_FRAME with 16 zero bytes of frame header. */
    unsigned av1_synth;
    /* VP9: V383_VP9_OPT_* (MPP's frame context handling where it differs
     * from the specification); 0 the specification's. */
    unsigned vp9_options;
} vdec_options;

typedef struct vdec vdec;
typedef struct vdec vdec_h264;          /* the name of milestone V3a */

typedef struct vdec_picture {
    const uint8_t *data;        /* luma rows, then chroma rows (NV12) */
    unsigned stride;            /* bytes per row, both planes */
    unsigned luma_rows;         /* rows of luma before the chroma starts */
    unsigned width, height;     /* the picture (H.264: macroblocks * 16; HEVC, VP9, AV1: as coded) */
} vdec_picture;

typedef struct vdec_stats {
    unsigned pictures;          /* decoded by this process */
    unsigned errors;            /* run failed, timed out or reported an error */
    unsigned hw_us_total;       /* time inside IOCTL_VDPU383_RUN */
    unsigned driver_runs;       /* vdpu383.sys's own counter (all processes), 0 if unknown */
    unsigned fake;              /* 1: D3D10UMD_VDEC_FAKE */
    unsigned iommu;             /* 1: the driver maps buffers through the decoder's IOMMU */
} vdec_stats;

/* 1 when a decoder can be opened in this process (the device or the fake);
 * cached after the first call. */
int vdec_available(void);

/* A decoder for pictures up to width x height (luma pixels). For VP9 and
 * AV1 the surfaces are sized for that once (frames may change size within
 * it); H.264 and HEVC size them for each stream's pictures. opt may be
 * NULL. NULL and a reason in err when the device or the memory is not
 * there. */
vdec *vdec_create(vdec_codec codec, unsigned width, unsigned height, const vdec_options *opt,
                  char *err, size_t errlen);
void vdec_destroy(vdec *d);
vdec_codec vdec_get_codec(const vdec *d);

/* Decode one picture. On success, *out describes the decoded picture,
 * valid until the next call. Each returns 0, or -1 with err filled. */

/* H.264: the short slice format (each slice NAL unit behind a 00 00 01
 * start code); qm may be NULL (flat). */
int vdec_h264_decode(vdec *d, const V383_DXVA_PicParams_H264 *pp,
                     const V383_DXVA_Qmatrix_H264 *qm, const uint8_t *bitstream,
                     unsigned bitstream_len, vdec_picture *out, char *err, size_t errlen);

/* HEVC Main: the slice segment NAL units, each behind 00 00 01
 * (ConfigBitstreamRaw 1); qm when scaling_list_enabled_flag. */
int vdec_hevc_decode(vdec *d, const V383_DXVA_PicParams_HEVC *pp, const V383_DXVA_Qmatrix_HEVC *qm,
                     const uint8_t *bitstream, unsigned bitstream_len, vdec_picture *out,
                     char *err, size_t errlen);

/* VP9 profile 0: the whole frame, uncompressed header first. Call once per
 * frame the application submits, in decode order. */
int vdec_vp9_decode(vdec *d, const V383_DXVA_PicParams_VP9 *pp, const uint8_t *frame,
                    unsigned frame_len, vdec_picture *out, char *err, size_t errlen);

/* AV1 Main 8 bit: the tiles DXVA_Tile_AV1 describes, DataOffset from data
 * (data_len bytes there). */
int vdec_av1_decode(vdec *d, const V383_DXVA_PicParams_AV1 *pp, const V383_DXVA_Tile_AV1 *tiles,
                    unsigned num_tiles, const uint8_t *data, unsigned data_len, vdec_picture *out,
                    char *err, size_t errlen);

/* The old names. */
vdec *vdec_h264_create(unsigned width, unsigned height, char *err, size_t errlen);
void vdec_h264_destroy(vdec *d);

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
