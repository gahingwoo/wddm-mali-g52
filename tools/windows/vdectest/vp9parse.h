/*
 * vp9parse.h - VP9 in IVF for vdpu383test: the container, superframes, the
 * uncompressed header and the reference slots, enough to hand the register
 * builder what a D3D11 VP9 host hands a decoder (DXVA_PicParams_VP9 and
 * the frame).
 *
 * The decoder reads the compressed header itself and adapts the
 * probabilities in hardware, so nothing here touches the arithmetic-coded
 * part of a frame.
 *
 * Written from the VP9 Bitstream & Decoding Process Specification v0.6
 * (sections 6.2 and 7.2) and the IVF layout.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VP9PARSE_H
#define VP9PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "../../../video/vdpu383/lib/vdpu383_vp9.h"

#define VP9_MAX_SLOTS   10      /* surfaces: 8 reference slots + current + spare */

/* One frame for the decoder. */
typedef struct Vp9Picture {
    V383_DXVA_PicParams_VP9 pp;
    const uint8_t *data;            /* the frame, uncompressed header first */
    size_t   len;
    unsigned decode_index;
    unsigned packet;                /* IVF frame it came from */
    unsigned width, height, bit_depth;
    unsigned refresh_frame_flags;
    unsigned coded_ctx_idx;         /* frame_context_idx as coded */
    unsigned color_space;
} Vp9Picture;

typedef int  (*Vp9DecodeFn)(void *ctx, const Vp9Picture *pic);
/* A frame to show: the surface, its size; existing: show_existing_frame. */
typedef void (*Vp9OutputFn)(void *ctx, int slot, unsigned width, unsigned height, unsigned decode_index,
                            int existing);

typedef struct Vp9Slot {
    int      surface;               /* -1: empty */
    unsigned width, height, bit_depth, subsampling_x, subsampling_y;
} Vp9Slot;

typedef struct Vp9Parser {
    Vp9Slot  ref[8];
    int      surface_busy[VP9_MAX_SLOTS];   /* referenced by a slot */
    unsigned bit_depth, subsampling_x, subsampling_y, color_space, color_range;
    unsigned profile;
    unsigned last_width, last_height, last_show_frame, last_intra_only;
    int      have_key;

    /* persistent header state */
    signed char ref_deltas[4], mode_deltas[2];
    unsigned seg_enabled, seg_update_map, seg_temporal, seg_abs_delta;
    unsigned char seg_tree_probs[7], seg_pred_probs[3];
    short    seg_data[8][4];
    unsigned char seg_mask[8];

    int      dxva_only;             /* frame_context_idx as hosts give it (0 on intra frames) */
    unsigned decode_index, max_pictures;
    unsigned packets, superframes, hidden, shown, existing, intra_only, keyframes, resized, scaled_refs, dropped;
    unsigned max_width, max_height, max_bit_depth;
    Vp9OutputFn out_fn;
    void    *out_ctx;
    char     err[200];
} Vp9Parser;

void vp9_init(Vp9Parser *p, Vp9OutputFn out, void *ctx);
/* 0 on success; -1 with p->err set. dec NULL: parse only (sizes, counts). */
int  vp9_run(Vp9Parser *p, const uint8_t *data, size_t len, unsigned max_pictures, Vp9DecodeFn dec, void *ctx);

/* An IVF file with a VP9 fourcc. */
int  vp9_probe(const uint8_t *data, size_t len);

#endif /* VP9PARSE_H */
