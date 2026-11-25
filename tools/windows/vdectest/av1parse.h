/*
 * av1parse.h - AV1 in IVF for vdpu383test: OBUs, the sequence header, the
 * frame header, tile groups and the reference slots, enough to hand the
 * register builder what a D3D11 AV1 host hands a decoder
 * (DXVA_PicParams_AV1 and the tiles), plus what a parser knows besides
 * (V383_Av1Extra) and the frame's OBUs.
 *
 * Nothing here decodes symbols: the decoder reads the tiles and keeps the
 * CDFs itself.
 *
 * Written from the AV1 Bitstream & Decoding Process Specification
 * (sections 5 and 7) and the IVF layout. Main profile (4:2:0, 8 and 10
 * bit); operating point 0; large-scale tile decoding is not supported.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef AV1PARSE_H
#define AV1PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "../../../video/vdpu383/lib/vdpu383_av1.h"

#define AV1_MAX_SLOTS   10          /* surfaces: 8 reference slots + current + spare */
#define AV1_MAX_TILES   512

typedef struct Av1Seq {
    int      valid;
    unsigned profile, still_picture, reduced_still;
    unsigned timing_info, decoder_model_info, equal_picture_interval;
    unsigned buffer_removal_time_length, frame_presentation_time_length;
    unsigned op_count, op_idc[32], decoder_model_present[32];
    unsigned frame_width_bits, frame_height_bits, max_width, max_height;
    unsigned frame_id_numbers, delta_frame_id_length, frame_id_length;
    unsigned sb128, filter_intra, intra_edge, interintra, masked_compound, warped_motion, dual_filter;
    unsigned order_hint, jnt_comp, ref_frame_mvs, order_hint_bits;
    unsigned force_screen_content_tools, force_integer_mv;
    unsigned superres, cdef, restoration;
    unsigned bit_depth, mono, ss_x, ss_y, matrix_coefficients, separate_uv_delta_q;
    unsigned film_grain_present;
} Av1Seq;

typedef struct Av1FilmGrain {
    unsigned apply, seed, update, num_y, num_cb, num_cr, csfl, scaling_m8, lag, ar_shift_m6, scale_shift;
    unsigned overlap, clip, cb_mult, cb_luma_mult, cb_offset, cr_mult, cr_luma_mult, cr_offset;
    unsigned char y_pts[14][2], cb_pts[10][2], cr_pts[10][2];
    unsigned char ar_y[24], ar_cb[25], ar_cr[25];
} Av1FilmGrain;

/* What a reference slot remembers (spec 7.20). */
typedef struct Av1Slot {
    int      surface;               /* -1: empty */
    unsigned valid, frame_id, upscaled_width, width, height, render_width, render_height;
    unsigned frame_type, order_hint, saved_hints[8], bit_depth;
    int      gm[8][6];
    signed char lf_ref[8], lf_mode[2];
    unsigned char feat_en[8][8];
    short    feat[8][8];
    Av1FilmGrain fg;
    unsigned showable;
} Av1Slot;

/* One frame for the decoder. */
typedef struct Av1Picture {
    V383_DXVA_PicParams_AV1 pp;
    V383_Av1Extra extra;
    V383_DXVA_Tile_AV1 tiles[AV1_MAX_TILES];
    unsigned num_tiles;
    const uint8_t *tile_data;       /* DataOffset is from here */
    const uint8_t *obu;             /* the frame's OBUs */
    size_t   obu_len;
    unsigned decode_index, packet, width, height, bit_depth;
    unsigned refresh_frame_flags, frame_header_bits;
} Av1Picture;

typedef int  (*Av1DecodeFn)(void *ctx, const Av1Picture *pic);
typedef void (*Av1OutputFn)(void *ctx, int slot, unsigned width, unsigned height, unsigned decode_index,
                            int existing);

typedef struct Av1Parser {
    Av1Seq   seq;
    Av1Slot  ref[8];
    int      surface_busy[AV1_MAX_SLOTS];
    unsigned decode_index, max_pictures;
    unsigned packets, frames_hidden, shown, existing, keyframes, intra_only, switch_frames, tiles_max;
    unsigned resized, superres, scaled_refs, dropped_layers, grain_frames, max_width, max_height, max_bit_depth;
    unsigned short_signaling, sb128_frames, lr_frames;
    Av1OutputFn out_fn;
    void    *out_ctx;
    int      dxva_only;
    /* the frame being collected */
    Av1Picture *pic;
    uint8_t  *tile_buf;
    size_t    tile_buf_cap, tile_len;
    char      err[200];
} Av1Parser;

void av1_init(Av1Parser *p, Av1OutputFn out, void *ctx);
/* 0 on success; -1 with p->err set. dec NULL: parse only. */
int  av1_run(Av1Parser *p, const uint8_t *data, size_t len, unsigned max_pictures, Av1DecodeFn dec, void *ctx);
void av1_free(Av1Parser *p);

/* An IVF file with an AV1 fourcc. */
int  av1_probe(const uint8_t *data, size_t len);

#endif /* AV1PARSE_H */
