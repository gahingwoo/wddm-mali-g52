/*
 * hevcparse.h - an HEVC Annex B parser for vdpu383test
 *
 * Main profile (8 bit 4:2:0): SPS, PPS, slice segment headers, picture
 * order count (8.3.1), reference picture sets and marking (8.3.2), output
 * order by the bumping process (C.5.2). Each picture comes out as what a
 * D3D11 decoder would be handed (DXVA_PicParams_HEVC, DXVA_Qmatrix_HEVC,
 * the slice NAL units behind start codes) plus what only a parser knows
 * (V383_HevcExtra: the SPS's reference picture sets and the ids).
 *
 * Written from ITU-T H.265 (2016) and the Microsoft DXVA HEVC
 * specification; FFmpeg's dxva2_hevc.c shows how players fill the DXVA
 * structures, and this follows it where the specification leaves a choice
 * (the order of RefPicList, IntraPicFlag).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef HEVCPARSE_H
#define HEVCPARSE_H

#include <stddef.h>
#include <stdint.h>

#include "../../../video/vdpu383/lib/vdpu383_hevc.h"

#define HEVC_MAX_SLOTS  20      /* surfaces: 16 DPB + current + spare */
#define HEVC_DPB_SIZE   17

typedef struct HevcSps {
    int      valid;
    unsigned sps_id, vps_id, max_sub_layers;
    unsigned chroma_format_idc, separate_colour_plane_flag;
    unsigned width, height;                     /* pic_width/height_in_luma_samples */
    unsigned crop_left, crop_right, crop_top, crop_bottom;     /* luma samples */
    unsigned bit_depth_luma_minus8, bit_depth_chroma_minus8;
    unsigned log2_max_poc_lsb;
    unsigned max_dec_pic_buffering[8], max_num_reorder[8], max_latency_plus1[8];
    unsigned log2_min_cb, log2_diff_cb, log2_min_tb, log2_diff_tb;
    unsigned max_th_depth_inter, max_th_depth_intra;
    unsigned scaling_list_enabled, scaling_list_data_present;
    uint8_t  sl[4][6][64], sl_dc[2][6];         /* raster order */
    unsigned amp, sao, pcm, pcm_bd_luma, pcm_bd_chroma, log2_min_pcm, log2_diff_pcm, pcm_loop_filter_disabled;
    unsigned num_st;
    V383_HevcStRps st[65];
    unsigned lt_present, num_lt_sps;
    unsigned lt_lsb[32], lt_used[32];
    unsigned temporal_mvp, strong_intra;
} HevcSps;

typedef struct HevcPps {
    int      valid;
    unsigned pps_id, sps_id;
    unsigned dependent_slices, output_flag_present, num_extra_bits;
    unsigned sign_data_hiding, cabac_init_present;
    unsigned num_ref_idx_l0_m1, num_ref_idx_l1_m1;
    int      init_qp_m26;
    unsigned constrained_intra, transform_skip, cu_qp_delta, diff_cu_qp_delta_depth;
    int      cb_qp_offset, cr_qp_offset;
    unsigned slice_chroma_qp_offsets, weighted_pred, weighted_bipred, transquant_bypass;
    unsigned tiles, entropy_sync, num_tile_cols_m1, num_tile_rows_m1, uniform;
    unsigned col_w_m1[20], row_h_m1[22];
    unsigned lf_across_tiles, lf_across_slices;
    unsigned deblocking_override, deblocking_disabled;
    int      beta_div2, tc_div2;
    unsigned scaling_list_data_present;
    uint8_t  sl[4][6][64], sl_dc[2][6];
    unsigned lists_modification, log2_parallel_merge_m2, sh_ext_present;
} HevcPps;

typedef struct HevcFrame {
    int      in_use, slot;
    int      ref;               /* 0 none, 1 short-term, 2 long-term */
    int      needed_for_output;
    int      poc;
    unsigned latency, decode_index;
} HevcFrame;

/* One picture, ready for the register builder. */
typedef struct HevcPicture {
    V383_DXVA_PicParams_HEVC pp;
    V383_DXVA_Qmatrix_HEVC   qm;
    V383_HevcExtra           extra;
    const uint8_t *stream;      /* slice segment NAL units, each behind 00 00 01 */
    size_t   stream_len;
    unsigned num_slices, slice_types;   /* bit 0 B, 1 P, 2 I */
    unsigned decode_index, nal_type;
    int      poc;
    unsigned width, height;             /* coded, luma samples */
    unsigned crop_left, crop_right, crop_top, crop_bottom;
} HevcPicture;

typedef int  (*HevcDecodeFn)(void *ctx, const HevcPicture *pic);
typedef void (*HevcOutputFn)(void *ctx, int slot, unsigned decode_index, int poc);

typedef struct HevcParser {
    HevcSps   sps[16];
    HevcPps   pps[64];
    HevcFrame dpb[HEVC_DPB_SIZE];
    int       slot_busy[HEVC_MAX_SLOTS];

    int       prev_tid0_poc;
    int       first_picture, after_eos, skip_rasl;

    /* the picture being collected */
    HevcPicture pic;
    uint8_t  *au;
    size_t    au_len, au_cap;
    int       open, skipping;
    HevcFrame *cur;
    const HevcSps *cur_sps;
    unsigned  max_reorder, max_latency, max_dec;

    unsigned  decode_index, max_pictures, skipped;
    HevcOutputFn out_fn;
    void     *out_ctx;
    HevcDecodeFn dec_fn;
    void     *dec_ctx;
    char      err[200];
} HevcParser;

void hevc_init(HevcParser *p, HevcOutputFn out, void *ctx);
/* 0 on success; -1 with p->err set. */
int  hevc_run(HevcParser *p, const uint8_t *data, size_t len, unsigned max_pictures,
              HevcDecodeFn dec, void *ctx);
void hevc_free(HevcParser *p);

/* An Annex B stream whose first NAL unit header looks like HEVC's. */
int  hevc_probe(const uint8_t *data, size_t len);

#endif /* HEVCPARSE_H */
