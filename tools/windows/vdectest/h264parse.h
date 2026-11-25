/*
 * h264parse.h - a small H.264 Annex B parser that produces, per picture,
 * what a DXVA decoder is given: DXVA_PicParams_H264, DXVA_Qmatrix_H264 and
 * the picture's slice NAL units. It also keeps the DPB (reference marking,
 * frame_num gaps) and the output order (bumping).
 *
 * Frame pictures only (progressive and MBAFF), 4:2:0 8-bit, no FMO/ASO,
 * no MVC/SVC. Written from ITU-T H.264 (08/2021); no code from any other
 * decoder.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef H264PARSE_H
#define H264PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "../../../video/vdpu383/lib/v383_dxva.h"
#include "../../../video/vdpu383/lib/vdpu383_h264.h"

#define H264_MAX_SLOTS  20      /* surfaces: 16 DPB + current + spare */

typedef struct H264Sps {
    int valid;
    unsigned profile_idc, constraint_flags, level_idc, sps_id;
    unsigned chroma_format_idc, separate_colour_plane_flag;
    unsigned bit_depth_luma_minus8, bit_depth_chroma_minus8;
    unsigned qpprime_y_zero_transform_bypass_flag;
    unsigned seq_scaling_matrix_present_flag;
    uint8_t  sl4[6][16], sl8[6][64];        /* raster order, after fall-back */
    unsigned log2_max_frame_num_minus4;
    unsigned pic_order_cnt_type, log2_max_poc_lsb_minus4;
    unsigned delta_pic_order_always_zero_flag;
    int      offset_for_non_ref_pic, offset_for_top_to_bottom_field;
    unsigned num_ref_frames_in_poc_cycle;
    int      offset_for_ref_frame[256];
    unsigned max_num_ref_frames, gaps_in_frame_num_allowed_flag;
    unsigned pic_width_in_mbs_minus1, pic_height_in_map_units_minus1;
    unsigned frame_mbs_only_flag, mb_adaptive_frame_field_flag, direct_8x8_inference_flag;
    unsigned crop_left, crop_right, crop_top, crop_bottom;     /* in luma pixels */
    int      max_dec_frame_buffering;       /* -1 if not signalled */
} H264Sps;

typedef struct H264Pps {
    int valid;
    unsigned pps_id, sps_id;
    unsigned entropy_coding_mode_flag, bottom_field_pic_order_in_frame_present_flag;
    unsigned num_slice_groups_minus1;
    unsigned num_ref_idx_l0_default_active_minus1, num_ref_idx_l1_default_active_minus1;
    unsigned weighted_pred_flag, weighted_bipred_idc;
    int      pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset;
    unsigned deblocking_filter_control_present_flag, constrained_intra_pred_flag;
    unsigned redundant_pic_cnt_present_flag, transform_8x8_mode_flag;
    unsigned pic_scaling_matrix_present_flag;
    uint8_t  sl4[6][16], sl8[6][64];        /* raster order, after fall-back (PPS or SPS) */
    int      second_chroma_qp_index_offset;
    int      sl_resolved_for_sps;           /* sps_id the lists were resolved against, -1 none */
    /* raw PPS scaling syntax, kept so the lists can be resolved later */
    unsigned sl_present[12], sl_default[12];
    uint8_t  sl_raw4[6][16], sl_raw8[6][64];
} H264Pps;

/* One reference / output frame in the DPB. */
typedef struct H264Frame {
    int      in_use;            /* holds a decoded or non-existing frame */
    int      slot;              /* surface index (DXVA Index7Bits) */
    int      is_ref;            /* 0 none, 1 short-term, 2 long-term */
    int      needed_for_output;
    int      non_existing;
    unsigned frame_num;
    int      frame_num_wrap;
    unsigned long_term_frame_idx;
    int      top_poc, bottom_poc, poc;
    unsigned decode_index;      /* order of decoding, for reports */
} H264Frame;

/* One picture, ready for the register builder. */
typedef struct H264Picture {
    V383_DXVA_PicParams_H264 pp;
    V383_DXVA_Qmatrix_H264   qm;
    V383_H264Extra           extra;
    unsigned scaling_matrix_present;
    unsigned pps_id;
    unsigned slice_types;       /* bit 0 P, 1 B, 2 I, 3 SP, 4 SI seen */
    unsigned num_slices;
    unsigned idr;
    const uint8_t *stream;      /* VCL NAL units, each behind 00 00 01 */
    size_t   stream_len;
    unsigned decode_index;
    int      poc;
    unsigned width, height;     /* coded, luma pixels */
    unsigned crop_left, crop_right, crop_top, crop_bottom;
} H264Picture;

typedef void (*H264OutputFn)(void *ctx, int slot, unsigned decode_index, int poc);

typedef struct H264Parser {
    H264Sps  sps[32];
    H264Pps  pps[256];

    /* DPB */
    H264Frame dpb[17];
    unsigned  dpb_size;
    int       max_long_term_frame_idx;      /* -1: no long-term frame indices */
    int       slot_busy[H264_MAX_SLOTS];    /* held by a DPB frame */

    /* POC / frame_num state */
    unsigned prev_ref_frame_num;
    int      prev_poc_msb, prev_poc_lsb;
    unsigned prev_frame_num, prev_frame_num_offset;
    int      prev_has_mmco5;
    int      prev_ref_has_mmco5, prev_ref_top_poc_after_mmco5, prev_ref_is_bottom;
    int      have_prev;

    /* current access unit being collected */
    uint8_t *au;
    size_t   au_len, au_cap;
    int      au_open;
    void    *cur_hdr;           /* first slice header of the open picture */
    unsigned cur_slice_types, cur_num_slices;

    /* decoded but held until the picture is "decoded" */
    unsigned decode_count;

    H264OutputFn out_fn;
    void *out_ctx;

    char err[160];
} H264Parser;

/* The decode callback: build registers from pic and run the hardware. The
 * slot to decode into is pic->pp.CurrPic. Return 0 on success. */
typedef int (*H264DecodeFn)(void *ctx, const H264Picture *pic);

void h264_init(H264Parser *p, H264OutputFn out, void *out_ctx);
void h264_free(H264Parser *p);

/* Feed a whole Annex B file. Calls decode for each picture and out for each
 * frame in output order. Returns 0, or -1 with p->err set. */
int  h264_run(H264Parser *p, const uint8_t *data, size_t len, unsigned max_pictures,
              H264DecodeFn decode, void *decode_ctx);

#endif
