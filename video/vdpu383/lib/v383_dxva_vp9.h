/*
 * v383_dxva_vp9.h - the DXVA VP9 structures the VDPU383 register builder takes
 *
 * Byte-for-byte the layout of DXVA_PicEntry_VPx, DXVA_segmentation_VP9,
 * DXVA_PicParams_VP9 and DXVA_Slice_VPx_Short in the Windows SDK's dxva.h
 * (packed to 1 byte, as dxva.h packs them), under V383_ names, like
 * v383_dxva.h for H.264. vdpu383test checks the layout against dxva.h at
 * compile time on Windows (dxva_check.c). sizeof: 1, 83, 208, 10.
 *
 * The flag unions are plain integers here with accessors; the bit positions
 * are those of the dxva.h bitfields (first field in bit 0), which is how
 * MSVC lays them out.
 *
 * Conventions, from the Microsoft "DirectX Video Acceleration Specification
 * for VP9" and what the D3D11 VP9 hosts (Chromium, Mesa's d3d12 driver)
 * write:
 *
 * - CurrPic, ref_frame_map[] and frame_refs[] hold surface indices
 *   (Index7Bits); 0x7F or 0xFF is no surface. ref_frame_coded_width[i] and
 *   ref_frame_coded_height[i] are those of ref_frame_map[i].
 * - frame_type is the bitstream's (0 key frame, 1 not), show_frame and the
 *   other flags as coded.
 * - interp_filter is the filter type, not the coded literal: 0 EIGHTTAP
 *   (regular), 1 EIGHTTAP_SMOOTH, 2 EIGHTTAP_SHARP, 3 BILINEAR,
 *   4 SWITCHABLE.
 * - ref_frame_sign_bias[1..3] are LAST, GOLDEN, ALTREF.
 * - ref_deltas, mode_deltas and the segmentation features are the values in
 *   force for this frame (after any update in its header).
 * - The bitstream buffer holds the whole frame, uncompressed header first;
 *   uncompressed_header_size_byte_aligned and first_partition_size are the
 *   sizes of the two headers.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef V383_DXVA_VP9_H
#define V383_DXVA_VP9_H

#include "v383_dxva.h"      /* V383_PIC_INDEX() and friends work on these entries too */

#pragma pack(push, 1)

typedef struct V383_DXVA_PicEntry_VPx {
    unsigned char bPicEntry;            /* Index7Bits | AssociatedFlag << 7 */
} V383_DXVA_PicEntry_VPx;

typedef struct V383_DXVA_segmentation_VP9 {
    unsigned char  wSegmentInfoFlags;   /* enabled, update_map, temporal_update, abs_delta */
    unsigned char  tree_probs[7];
    unsigned char  pred_probs[3];
    short          feature_data[8][4];  /* [Q, LF, REF, SKIP] */
    unsigned char  feature_mask[8];     /* bit n: feature n enabled */
} V383_DXVA_segmentation_VP9;

typedef struct V383_DXVA_PicParams_VP9 {
    V383_DXVA_PicEntry_VPx CurrPic;
    unsigned char  profile;
    unsigned short wFormatAndPictureInfoFlags;
    unsigned int   width;
    unsigned int   height;
    unsigned char  BitDepthMinus8Luma;
    unsigned char  BitDepthMinus8Chroma;
    unsigned char  interp_filter;
    unsigned char  Reserved8Bits;
    V383_DXVA_PicEntry_VPx ref_frame_map[8];
    unsigned int   ref_frame_coded_width[8];
    unsigned int   ref_frame_coded_height[8];
    V383_DXVA_PicEntry_VPx frame_refs[3];
    signed char    ref_frame_sign_bias[4];
    signed char    filter_level;
    signed char    sharpness_level;
    unsigned char  wControlInfoFlags;
    signed char    ref_deltas[4];
    signed char    mode_deltas[2];
    short          base_qindex;
    signed char    y_dc_delta_q;
    signed char    uv_dc_delta_q;
    signed char    uv_ac_delta_q;
    V383_DXVA_segmentation_VP9 stVP9Segments;
    unsigned char  log2_tile_cols;
    unsigned char  log2_tile_rows;
    unsigned short uncompressed_header_size_byte_aligned;
    unsigned short first_partition_size;
    unsigned short Reserved16Bits;
    unsigned int   Reserved32Bits;
    unsigned int   StatusReportFeedbackNumber;
} V383_DXVA_PicParams_VP9;

typedef struct V383_DXVA_Slice_VPx_Short {
    unsigned int   BSNALunitDataLocation;
    unsigned int   SliceBytesInBuffer;
    unsigned short wBadSliceChopping;
} V383_DXVA_Slice_VPx_Short;

#pragma pack(pop)

#define V383_VP9_B(v, pos, bits)        ((((unsigned)(v)) >> (pos)) & ((1u << (bits)) - 1))

/* wFormatAndPictureInfoFlags */
#define V383_VP9_F(pp, pos, bits)       V383_VP9_B((pp)->wFormatAndPictureInfoFlags, pos, bits)
#define V383_VP9_FRAME_TYPE(pp)         V383_VP9_F(pp, 0, 1)    /* 0: key frame */
#define V383_VP9_SHOW_FRAME(pp)         V383_VP9_F(pp, 1, 1)
#define V383_VP9_ERROR_RESILIENT(pp)    V383_VP9_F(pp, 2, 1)
#define V383_VP9_SUBSAMPLING_X(pp)      V383_VP9_F(pp, 3, 1)
#define V383_VP9_SUBSAMPLING_Y(pp)      V383_VP9_F(pp, 4, 1)
#define V383_VP9_EXTRA_PLANE(pp)        V383_VP9_F(pp, 5, 1)
#define V383_VP9_REFRESH_CTX(pp)        V383_VP9_F(pp, 6, 1)
#define V383_VP9_PARALLEL_MODE(pp)      V383_VP9_F(pp, 7, 1)
#define V383_VP9_INTRA_ONLY(pp)         V383_VP9_F(pp, 8, 1)
#define V383_VP9_FRAME_CTX_IDX(pp)      V383_VP9_F(pp, 9, 2)
#define V383_VP9_RESET_CTX(pp)          V383_VP9_F(pp, 11, 2)
#define V383_VP9_ALLOW_HP_MV(pp)        V383_VP9_F(pp, 13, 1)

#define V383_VP9_FLAG_FRAME_TYPE        0
#define V383_VP9_FLAG_SHOW_FRAME        1
#define V383_VP9_FLAG_ERROR_RESILIENT   2
#define V383_VP9_FLAG_SUBSAMPLING_X     3
#define V383_VP9_FLAG_SUBSAMPLING_Y     4
#define V383_VP9_FLAG_REFRESH_CTX       6
#define V383_VP9_FLAG_PARALLEL_MODE     7
#define V383_VP9_FLAG_INTRA_ONLY        8
#define V383_VP9_FLAG_FRAME_CTX_IDX     9   /* 2 bits */
#define V383_VP9_FLAG_RESET_CTX         11  /* 2 bits */
#define V383_VP9_FLAG_ALLOW_HP_MV       13

/* wControlInfoFlags */
#define V383_VP9_LF_DELTA_ENABLED(pp)   V383_VP9_B((pp)->wControlInfoFlags, 0, 1)
#define V383_VP9_LF_DELTA_UPDATE(pp)    V383_VP9_B((pp)->wControlInfoFlags, 1, 1)
#define V383_VP9_USE_PREV_MVS(pp)       V383_VP9_B((pp)->wControlInfoFlags, 2, 1)

/* stVP9Segments.wSegmentInfoFlags */
#define V383_VP9_SEG(pp, pos)           V383_VP9_B((pp)->stVP9Segments.wSegmentInfoFlags, pos, 1)
#define V383_VP9_SEG_ENABLED(pp)        V383_VP9_SEG(pp, 0)
#define V383_VP9_SEG_UPDATE_MAP(pp)     V383_VP9_SEG(pp, 1)
#define V383_VP9_SEG_TEMPORAL(pp)       V383_VP9_SEG(pp, 2)
#define V383_VP9_SEG_ABS_DELTA(pp)      V383_VP9_SEG(pp, 3)

/* interp_filter */
#define V383_VP9_EIGHTTAP               0
#define V383_VP9_EIGHTTAP_SMOOTH        1
#define V383_VP9_EIGHTTAP_SHARP         2
#define V383_VP9_BILINEAR               3
#define V383_VP9_SWITCHABLE             4

#define V383_VP9_NO_SURFACE(e)          (((e).bPicEntry & 0x7F) == 0x7F)

#endif /* V383_DXVA_VP9_H */
