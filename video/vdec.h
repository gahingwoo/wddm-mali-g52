/*
 * vdec.h - the user-mode driver's video decoder backend: DXVA pictures
 * (H.264, HEVC Main, VP9 profile 0, AV1 Main 8 bit) decoded by the RK3576
 * VDPU383.
 *
 * Milestone V3a (docs/VIDEO-DECODE.md): the decoder is driven through
 * vdpu383.sys (woa-rk3576, drivers/video/vdpu383) for the process that
 * decodes, with the bring-up interface vdpu383test proved on the board.
 * Each call goes through maliwddm (vdec_kmd.h, docs/CHROMIUM-VIDEO.md C2:
 * an escape, made by the KMD in a session of vdpu383.sys of its own), or,
 * when that path is off, straight to \\.\VDPU383 (UserAccess). The calls:
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
 * Zero copy (milestone V3b, docs/VIDEO-DECODE.md): given the application's
 * texture (vdec_set_target before a decode: the token of maliwddm's export
 * of its allocation and where the picture goes in it), the decoder writes
 * the picture into the texture's pages, imported into the decoder's IOMMU
 * by vdpu383.sys (IOCTL_VDPU383_IMPORT, interface version 3), and the
 * caller copies nothing (vdec_picture.in_target). The layout of every
 * surface of a decoder is then the texture's; a frame whose texture does
 * not fit goes into a surface of the decoder's own, in that layout, and is
 * copied as before. Without a target, with a texture layout the decoder
 * cannot use, with vdec_options.no_zero_copy, D3D10UMD_VDEC_ZEROCOPY=0 or a
 * driver without imports, everything is as in V3a.
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
    /* 1: never decode into the application's texture (UmdVideoZeroCopy 0) */
    unsigned no_zero_copy;
} vdec_options;

typedef struct vdec vdec;
typedef struct vdec vdec_h264;          /* the name of milestone V3a */

typedef struct vdec_picture {
    unsigned in_target;         /* 1: decoded into the target (vdec_set_target); data is NULL */
    const uint8_t *data;        /* luma rows, then chroma rows (NV12) */
    unsigned stride;            /* bytes per row, both planes */
    unsigned luma_rows;         /* rows of luma before the chroma starts */
    unsigned width, height;     /* the picture (H.264: macroblocks * 16; HEVC, VP9, AV1: as coded) */
} vdec_picture;

/* How the decoder is reached (vdec_stats.path; vdec.c, "transport") */
#define VDEC_PATH_NONE      0
#define VDEC_PATH_DIRECT    1   /* \\.\VDPU383 opened by this process (UserAccess) */
#define VDEC_PATH_KMD       2   /* through maliwddm, MW_ESCAPE_VDEC_CALL */
#define VDEC_PATH_MEM       3   /* the fake in ordinary memory (CI) */

typedef struct vdec_stats {
    unsigned pictures;          /* decoded by this process */
    unsigned errors;            /* run failed, timed out or reported an error */
    unsigned hw_us_total;       /* time inside IOCTL_VDPU383_RUN */
    unsigned driver_runs;       /* vdpu383.sys's own counter (all processes), 0 if unknown */
    unsigned fake;              /* 1: D3D10UMD_VDEC_FAKE */
    unsigned iommu;             /* 1: the driver maps buffers through the decoder's IOMMU */
    unsigned zero_copy;         /* pictures decoded into the application's texture */
    unsigned imports;           /* textures imported into the decoder (IOCTL_VDPU383_IMPORT) */
    unsigned import_failures;
    unsigned refreshes;         /* imports refreshed after the display driver took the pages back */
    unsigned can_import;        /* 1: the driver imports (interface 3, IOMMU on) or the fake */
    unsigned import_retries;    /* refused imports asked again (every 64th use, D3D10UMD_VDEC_IMPORT_RETRY) */
    unsigned last_import_error; /* GetLastError() of the last refused import (1168 STATUS_NOT_FOUND, ...) */
    unsigned last_import_pages; /* the pages it asked for */
    unsigned path;              /* VDEC_PATH_* */
    unsigned kmd_calls;         /* escapes to maliwddm made by this process */
    unsigned session_runs;      /* vdpu383.sys's decodes through sessions (all processes), 0 if unknown */
} vdec_stats;

/*
 * The application's texture for the next decode (zero copy). refresh, given
 * refresh_ctx, returns a new export token of the same allocation (and its
 * pages) after the display driver took the old one back, 0 if there is
 * none; fake_map is the allocation's memory for the fake decoder.
 */
typedef uint64_t (*vdec_refresh_fn)(void *ctx, unsigned *pages);

typedef struct vdec_target {
    uint64_t token;             /* maliwddm MW_ESCAPE_VDEC_EXPORT */
    unsigned pages;             /* the allocation's size in 4 KiB pages */
    uint64_t offset;            /* the picture's first luma byte in the allocation */
    unsigned stride;            /* bytes per row, both planes */
    unsigned luma_rows;         /* rows of luma before the chroma rows */
    vdec_refresh_fn refresh;
    void *refresh_ctx;
    uint8_t *fake_map;
} vdec_target;

/* 1 when a decoder can be opened in this process (the device or the fake);
 * cached after the first call. */
int vdec_available(void);

/*
 * The escape to maliwddm for VDEC_PATH_KMD (vdec_kmd.h): data is a
 * VDEC_KMD_CALL of size bytes, with_device 0 for VDEC_KMD_PROBE on the
 * adapter alone; returns the escape's NTSTATUS. On Windows it is
 * D3DKMTEscape on a device of this process's own; a test sets its own
 * (before vdec_available), which then takes every call, with the fake
 * pattern too when D3D10UMD_VDEC_FAKE is 1.
 */
typedef long (*vdec_escape_fn)(void *ctx, int with_device, void *data, unsigned size);
void vdec_set_escape(vdec_escape_fn fn, void *ctx);

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

/* 1 when zero copy can be used in this process: the decoder opens, its
 * driver imports (or the fake), D3D10UMD_VDEC_ZEROCOPY is not 0. */
int vdec_zero_copy_possible(void);

/* The target of the next vdec_*_decode call only; NULL for none. */
void vdec_set_target(vdec *d, const vdec_target *t);

/* The allocation of this token is going away (its texture is destroyed):
 * the decoder lets go of its import. */
void vdec_forget(uint64_t token);

/* The old names. */
vdec *vdec_h264_create(unsigned width, unsigned height, char *err, size_t errlen);
void vdec_h264_destroy(vdec *d);

void vdec_get_stats(vdec_stats *s);

/* The pattern the fake decoder writes: every luma byte of a picture is
 * (StatusReportFeedbackNumber & 0xFF), every Cb byte 0x80 ^ the picture's
 * surface index, every Cr byte 0x40 ^ that index. Cb and Cr differ, so a
 * reader that swaps or drops one of them is caught. */
static inline uint8_t vdec_fake_luma(unsigned status_report) { return (uint8_t)status_report; }
static inline uint8_t vdec_fake_chroma(unsigned index) { return (uint8_t)(0x80 ^ index); }
static inline uint8_t vdec_fake_chroma_v(unsigned index) { return (uint8_t)(0x40 ^ index); }

#ifdef __cplusplus
}
#endif

#endif /* VDEC_H */
