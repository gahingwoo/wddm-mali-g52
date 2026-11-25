/*
 * v383_dxva_av1.h - the DXVA AV1 structures the VDPU383 register builder takes
 *
 * Byte-for-byte the layout of DXVA_PicEntry_AV1, DXVA_PicParams_AV1 and
 * DXVA_Tile_AV1 in the Windows SDK's dxva.h (packed to 1 byte), under V383_
 * names, like v383_dxva.h for H.264. vdpu383test checks the layout against
 * dxva.h at compile time on Windows (dxva_check.c). sizeof: 36, 912, 16.
 *
 * Flag words are plain integers here with accessors; the bit positions are
 * those of the dxva.h bitfields (first field in bit 0).
 *
 * Conventions, from the Microsoft "DirectX Video Acceleration Specification
 * for AV1" and what FFmpeg's dxva2_av1.c and Mesa's d3d12 driver write:
 *
 * - CurrPicTextureIndex and RefFrameMapTextureIndex[] are surface indices,
 *   0xFF none; RefFrameMapTextureIndex[j] is reference slot j. frame_refs[i]
 *   is reference LAST + i, and its Index is the slot it refers to
 *   (ref_frame_idx[i], 0 to 7), not a surface: what FFmpeg's dxva2_av1.c
 *   and Mesa's d3d12 write. (Until 2026-11-25 this said surface, and FFmpeg
 *   decoded wrong references from the third picture of a stream on.)
 * - width and height are the upscaled frame size; with superres the coded
 *   width is (width * 8 + superres_denom / 2) / superres_denom.
 * - frame_refs[i].wmmat[] are the global motion parameters in force
 *   (gm_params), wmtype their type, wminvalid 1 for identity.
 * - qm_y, qm_u, qm_v are 0xFF when using_qmatrix is 0.
 * - frame_restoration_type[] is FrameRestorationType (0 none, 1 Wiener,
 *   2 self-guided, 3 switchable); log2_restoration_unit_size[] is the log2
 *   of the restoration unit size (6 + lr_unit_shift, chroma minus
 *   lr_uv_shift; 8 when no plane uses it), as FFmpeg's dxva2_av1.c writes
 *   it.
 * - The bitstream buffer holds the tile data; DXVA_Tile_AV1 gives each
 *   tile's offset and size in it. Nothing of the OBU headers, the sequence
 *   header or the frame header is in it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef V383_DXVA_AV1_H
#define V383_DXVA_AV1_H

#pragma pack(push, 1)

typedef struct V383_DXVA_PicEntry_AV1 {
    unsigned int   width;
    unsigned int   height;
    int            wmmat[6];
    unsigned char  GlobalMotionFlags;   /* wminvalid 1, wmtype 2 */
    unsigned char  Index;
    unsigned short Reserved16Bits;
} V383_DXVA_PicEntry_AV1;

typedef struct V383_DXVA_PicParams_AV1 {
    unsigned int   width;
    unsigned int   height;
    unsigned int   max_width;
    unsigned int   max_height;
    unsigned char  CurrPicTextureIndex;
    unsigned char  superres_denom;
    unsigned char  bitdepth;
    unsigned char  seq_profile;
    struct {
        unsigned char  cols;
        unsigned char  rows;
        unsigned short context_update_id;
        unsigned short widths[64];      /* superblocks */
        unsigned short heights[64];
    } tiles;
    unsigned int   CodingParamToolFlags;
    unsigned char  FormatAndPictureInfoFlags;
    unsigned char  primary_ref_frame;
    unsigned char  order_hint;
    unsigned char  order_hint_bits;
    V383_DXVA_PicEntry_AV1 frame_refs[7];
    unsigned char  RefFrameMapTextureIndex[8];
    struct {
        unsigned char  filter_level[2];
        unsigned char  filter_level_u;
        unsigned char  filter_level_v;
        unsigned char  sharpness_level;
        unsigned char  ControlFlags;    /* mode_ref_delta_enabled, _update, delta_lf_multi, delta_lf_present */
        signed char    ref_deltas[8];
        signed char    mode_deltas[2];
        unsigned char  delta_lf_res;
        unsigned char  frame_restoration_type[3];
        unsigned short log2_restoration_unit_size[3];
        unsigned short Reserved16Bits;
    } loop_filter;
    struct {
        unsigned char  ControlFlags;    /* delta_q_present 1, delta_q_res 2 */
        unsigned char  base_qindex;
        signed char    y_dc_delta_q;
        signed char    u_dc_delta_q;
        signed char    v_dc_delta_q;
        signed char    u_ac_delta_q;
        signed char    v_ac_delta_q;
        unsigned char  qm_y;
        unsigned char  qm_u;
        unsigned char  qm_v;
        unsigned short Reserved16Bits;
    } quantization;
    struct {
        unsigned char  ControlFlags;    /* damping (minus 3) 2, bits 2 */
        unsigned char  y_strengths[8];  /* primary 6, secondary 2 */
        unsigned char  uv_strengths[8];
    } cdef;
    unsigned char  interp_filter;
    struct {
        unsigned char  ControlFlags;    /* enabled, update_map, update_data, temporal_update */
        unsigned char  Reserved24Bits[3];
        unsigned char  feature_mask[8];
        short          feature_data[8][8];
    } segmentation;
    struct {
        unsigned short ControlFlags;    /* see V383_AV1_FG_* */
        unsigned short grain_seed;
        unsigned char  scaling_points_y[14][2];
        unsigned char  num_y_points;
        unsigned char  scaling_points_cb[10][2];
        unsigned char  num_cb_points;
        unsigned char  scaling_points_cr[10][2];
        unsigned char  num_cr_points;
        unsigned char  ar_coeffs_y[24];
        unsigned char  ar_coeffs_cb[25];
        unsigned char  ar_coeffs_cr[25];
        unsigned char  cb_mult;
        unsigned char  cb_luma_mult;
        unsigned char  cr_mult;
        unsigned char  cr_luma_mult;
        unsigned char  Reserved8Bits;
        short          cb_offset;
        short          cr_offset;
    } film_grain;
    unsigned int   Reserved32Bits;
    unsigned int   StatusReportFeedbackNumber;
} V383_DXVA_PicParams_AV1;

typedef struct V383_DXVA_Tile_AV1 {
    unsigned int   DataOffset;
    unsigned int   DataSize;
    unsigned short row;
    unsigned short column;
    unsigned short Reserved16Bits;
    unsigned char  anchor_frame;
    unsigned char  Reserved8Bits;
} V383_DXVA_Tile_AV1;

#pragma pack(pop)

#define V383_AV1_B(v, pos, bits)        ((((unsigned)(v)) >> (pos)) & ((1u << (bits)) - 1))

/* CodingParamToolFlags */
#define V383_AV1_C(pp, pos, bits)       V383_AV1_B((pp)->CodingParamToolFlags, pos, bits)
#define V383_AV1_SB128(pp)              V383_AV1_C(pp, 0, 1)
#define V383_AV1_INTRA_EDGE(pp)         V383_AV1_C(pp, 1, 1)
#define V383_AV1_INTERINTRA(pp)         V383_AV1_C(pp, 2, 1)
#define V383_AV1_MASKED_COMPOUND(pp)    V383_AV1_C(pp, 3, 1)
#define V383_AV1_WARPED_MOTION(pp)      V383_AV1_C(pp, 4, 1)
#define V383_AV1_DUAL_FILTER(pp)        V383_AV1_C(pp, 5, 1)
#define V383_AV1_JNT_COMP(pp)           V383_AV1_C(pp, 6, 1)
#define V383_AV1_SCREEN_CONTENT(pp)     V383_AV1_C(pp, 7, 1)
#define V383_AV1_INTEGER_MV(pp)         V383_AV1_C(pp, 8, 1)
#define V383_AV1_CDEF(pp)               V383_AV1_C(pp, 9, 1)
#define V383_AV1_RESTORATION(pp)        V383_AV1_C(pp, 10, 1)
#define V383_AV1_FILM_GRAIN(pp)         V383_AV1_C(pp, 11, 1)
#define V383_AV1_INTRABC(pp)            V383_AV1_C(pp, 12, 1)
#define V383_AV1_HIGH_PRECISION_MV(pp)  V383_AV1_C(pp, 13, 1)
#define V383_AV1_SWITCHABLE_MOTION(pp)  V383_AV1_C(pp, 14, 1)
#define V383_AV1_FILTER_INTRA(pp)       V383_AV1_C(pp, 15, 1)
#define V383_AV1_DISABLE_END_CDF(pp)    V383_AV1_C(pp, 16, 1)
#define V383_AV1_DISABLE_CDF_UPDATE(pp) V383_AV1_C(pp, 17, 1)
#define V383_AV1_REFERENCE_MODE(pp)     V383_AV1_C(pp, 18, 1)
#define V383_AV1_SKIP_MODE(pp)          V383_AV1_C(pp, 19, 1)
#define V383_AV1_REDUCED_TX_SET(pp)     V383_AV1_C(pp, 20, 1)
#define V383_AV1_SUPERRES(pp)           V383_AV1_C(pp, 21, 1)
#define V383_AV1_TX_MODE(pp)            V383_AV1_C(pp, 22, 2)
#define V383_AV1_USE_REF_FRAME_MVS(pp)  V383_AV1_C(pp, 24, 1)
#define V383_AV1_ENABLE_REF_FRAME_MVS(pp) V383_AV1_C(pp, 25, 1)
#define V383_AV1_REF_FRAME_UPDATE(pp)   V383_AV1_C(pp, 26, 1)

/* FormatAndPictureInfoFlags */
#define V383_AV1_FRAME_TYPE(pp)         V383_AV1_B((pp)->FormatAndPictureInfoFlags, 0, 2)
#define V383_AV1_SHOW_FRAME(pp)         V383_AV1_B((pp)->FormatAndPictureInfoFlags, 2, 1)
#define V383_AV1_SHOWABLE_FRAME(pp)     V383_AV1_B((pp)->FormatAndPictureInfoFlags, 3, 1)
#define V383_AV1_SUBSAMPLING_X(pp)      V383_AV1_B((pp)->FormatAndPictureInfoFlags, 4, 1)
#define V383_AV1_SUBSAMPLING_Y(pp)      V383_AV1_B((pp)->FormatAndPictureInfoFlags, 5, 1)
#define V383_AV1_MONO_CHROME(pp)        V383_AV1_B((pp)->FormatAndPictureInfoFlags, 6, 1)

#define V383_AV1_KEY_FRAME              0
#define V383_AV1_INTER_FRAME            1
#define V383_AV1_INTRA_ONLY_FRAME       2
#define V383_AV1_SWITCH_FRAME           3
#define V383_AV1_PRIMARY_REF_NONE       7

/* frame_refs[].GlobalMotionFlags */
#define V383_AV1_WM_INVALID(e)          V383_AV1_B((e).GlobalMotionFlags, 0, 1)
#define V383_AV1_WM_TYPE(e)             V383_AV1_B((e).GlobalMotionFlags, 1, 2)

/* loop_filter.ControlFlags */
#define V383_AV1_LF_DELTA_ENABLED(pp)   V383_AV1_B((pp)->loop_filter.ControlFlags, 0, 1)
#define V383_AV1_LF_DELTA_UPDATE(pp)    V383_AV1_B((pp)->loop_filter.ControlFlags, 1, 1)
#define V383_AV1_DELTA_LF_MULTI(pp)     V383_AV1_B((pp)->loop_filter.ControlFlags, 2, 1)
#define V383_AV1_DELTA_LF_PRESENT(pp)   V383_AV1_B((pp)->loop_filter.ControlFlags, 3, 1)

/* quantization.ControlFlags */
#define V383_AV1_DELTA_Q_PRESENT(pp)    V383_AV1_B((pp)->quantization.ControlFlags, 0, 1)
#define V383_AV1_DELTA_Q_RES(pp)        V383_AV1_B((pp)->quantization.ControlFlags, 1, 2)

/* cdef */
#define V383_AV1_CDEF_DAMPING_M3(pp)    V383_AV1_B((pp)->cdef.ControlFlags, 0, 2)
#define V383_AV1_CDEF_BITS(pp)          V383_AV1_B((pp)->cdef.ControlFlags, 2, 2)
#define V383_AV1_CDEF_PRI(s)            ((unsigned)(s) & 0x3F)
#define V383_AV1_CDEF_SEC(s)            (((unsigned)(s) >> 6) & 3)

/* segmentation.ControlFlags */
#define V383_AV1_SEG_ENABLED(pp)        V383_AV1_B((pp)->segmentation.ControlFlags, 0, 1)
#define V383_AV1_SEG_UPDATE_MAP(pp)     V383_AV1_B((pp)->segmentation.ControlFlags, 1, 1)
#define V383_AV1_SEG_UPDATE_DATA(pp)    V383_AV1_B((pp)->segmentation.ControlFlags, 2, 1)
#define V383_AV1_SEG_TEMPORAL(pp)       V383_AV1_B((pp)->segmentation.ControlFlags, 3, 1)

/* film_grain.ControlFlags */
#define V383_AV1_FG(pp, pos, bits)      V383_AV1_B((pp)->film_grain.ControlFlags, pos, bits)
#define V383_AV1_FG_APPLY(pp)           V383_AV1_FG(pp, 0, 1)
#define V383_AV1_FG_SCALING_SHIFT_M8(pp) V383_AV1_FG(pp, 1, 2)
#define V383_AV1_FG_CHROMA_FROM_LUMA(pp) V383_AV1_FG(pp, 3, 1)
#define V383_AV1_FG_AR_COEFF_LAG(pp)    V383_AV1_FG(pp, 4, 2)
#define V383_AV1_FG_AR_SHIFT_M6(pp)     V383_AV1_FG(pp, 6, 2)
#define V383_AV1_FG_GRAIN_SCALE_SHIFT(pp) V383_AV1_FG(pp, 8, 2)
#define V383_AV1_FG_OVERLAP(pp)         V383_AV1_FG(pp, 10, 1)
#define V383_AV1_FG_CLIP_RESTRICTED(pp) V383_AV1_FG(pp, 11, 1)
#define V383_AV1_FG_MATRIX_IDENTITY(pp) V383_AV1_FG(pp, 12, 1)

#define V383_AV1_NO_SURFACE             0xFF

#endif /* V383_DXVA_AV1_H */
