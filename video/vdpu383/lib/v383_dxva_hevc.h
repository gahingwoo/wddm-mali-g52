/*
 * v383_dxva_hevc.h - the DXVA HEVC structures the VDPU383 register builder takes
 *
 * Byte-for-byte the layout of DXVA_PicEntry_HEVC, DXVA_PicParams_HEVC,
 * DXVA_Qmatrix_HEVC and DXVA_Slice_HEVC_Short in the Windows SDK's dxva.h
 * (packed to 1 byte, as dxva.h packs them), under V383_ names, like
 * v383_dxva.h for H.264. vdpu383test checks the layout against dxva.h at
 * compile time on Windows (dxva_check.c). sizeof: 232, 1000, 10.
 *
 * The three flag unions are plain integers here with accessors; the bit
 * positions are those of the dxva.h bitfields (first field in bit 0), which
 * is how MSVC lays them out.
 *
 * Written from the Microsoft "DXVA Specification for HEVC Video Decoding"
 * and dxva.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef V383_DXVA_HEVC_H
#define V383_DXVA_HEVC_H

#include "v383_dxva.h"      /* V383_PIC_INDEX() and friends work on this entry too */

#pragma pack(push, 1)

typedef struct V383_DXVA_PicEntry_HEVC {
    unsigned char bPicEntry;            /* Index7Bits | AssociatedFlag << 7; 0xFF = unused */
} V383_DXVA_PicEntry_HEVC;

typedef struct V383_DXVA_PicParams_HEVC {
    unsigned short PicWidthInMinCbsY;
    unsigned short PicHeightInMinCbsY;
    unsigned short wFormatAndSequenceInfoFlags;
    V383_DXVA_PicEntry_HEVC CurrPic;
    unsigned char  sps_max_dec_pic_buffering_minus1;
    unsigned char  log2_min_luma_coding_block_size_minus3;
    unsigned char  log2_diff_max_min_luma_coding_block_size;
    unsigned char  log2_min_transform_block_size_minus2;
    unsigned char  log2_diff_max_min_transform_block_size;
    unsigned char  max_transform_hierarchy_depth_inter;
    unsigned char  max_transform_hierarchy_depth_intra;
    unsigned char  num_short_term_ref_pic_sets;
    unsigned char  num_long_term_ref_pics_sps;
    unsigned char  num_ref_idx_l0_default_active_minus1;
    unsigned char  num_ref_idx_l1_default_active_minus1;
    signed char    init_qp_minus26;
    unsigned char  ucNumDeltaPocsOfRefRpsIdx;
    unsigned short wNumBitsForShortTermRPSInSlice;
    unsigned short ReservedBits2;
    unsigned int   dwCodingParamToolFlags;
    unsigned int   dwCodingSettingPicturePropertyFlags;
    signed char    pps_cb_qp_offset;
    signed char    pps_cr_qp_offset;
    unsigned char  num_tile_columns_minus1;
    unsigned char  num_tile_rows_minus1;
    unsigned short column_width_minus1[19];
    unsigned short row_height_minus1[21];
    unsigned char  diff_cu_qp_delta_depth;
    signed char    pps_beta_offset_div2;
    signed char    pps_tc_offset_div2;
    unsigned char  log2_parallel_merge_level_minus2;
    int            CurrPicOrderCntVal;
    V383_DXVA_PicEntry_HEVC RefPicList[15];     /* AssociatedFlag = long-term */
    unsigned char  ReservedBits5;
    int            PicOrderCntValList[15];
    unsigned char  RefPicSetStCurrBefore[8];    /* indices into RefPicList, 0xFF = none */
    unsigned char  RefPicSetStCurrAfter[8];
    unsigned char  RefPicSetLtCurr[8];
    unsigned short ReservedBits6;
    unsigned short ReservedBits7;
    unsigned int   StatusReportFeedbackNumber;
} V383_DXVA_PicParams_HEVC;

typedef struct V383_DXVA_Qmatrix_HEVC {
    unsigned char ucScalingLists0[6][16];       /* up-right diagonal scan order */
    unsigned char ucScalingLists1[6][64];
    unsigned char ucScalingLists2[6][64];
    unsigned char ucScalingLists3[2][64];
    unsigned char ucScalingListDCCoefSizeID2[6];
    unsigned char ucScalingListDCCoefSizeID3[2];
} V383_DXVA_Qmatrix_HEVC;

typedef struct V383_DXVA_Slice_HEVC_Short {
    unsigned int   BSNALunitDataLocation;
    unsigned int   SliceBytesInBuffer;
    unsigned short wBadSliceChopping;
} V383_DXVA_Slice_HEVC_Short;

#pragma pack(pop)

/* Accessors: field at bit `pos`, `bits` wide. */
#define V383_HB(v, pos, bits)   ((unsigned)(((v) >> (pos)) & ((1u << (bits)) - 1)))

/* wFormatAndSequenceInfoFlags */
#define V383_HEVC_CHROMA_FORMAT_IDC(pp)         V383_HB((pp)->wFormatAndSequenceInfoFlags, 0, 2)
#define V383_HEVC_SEPARATE_COLOUR_PLANE(pp)     V383_HB((pp)->wFormatAndSequenceInfoFlags, 2, 1)
#define V383_HEVC_BIT_DEPTH_LUMA_MINUS8(pp)     V383_HB((pp)->wFormatAndSequenceInfoFlags, 3, 3)
#define V383_HEVC_BIT_DEPTH_CHROMA_MINUS8(pp)   V383_HB((pp)->wFormatAndSequenceInfoFlags, 6, 3)
#define V383_HEVC_LOG2_MAX_POC_LSB_MINUS4(pp)   V383_HB((pp)->wFormatAndSequenceInfoFlags, 9, 4)
#define V383_HEVC_NO_PIC_REORDERING(pp)         V383_HB((pp)->wFormatAndSequenceInfoFlags, 13, 1)
#define V383_HEVC_NO_BI_PRED(pp)                V383_HB((pp)->wFormatAndSequenceInfoFlags, 14, 1)

/* dwCodingParamToolFlags */
#define V383_HEVC_T(pp, pos, bits)              V383_HB((pp)->dwCodingParamToolFlags, pos, bits)
#define V383_HEVC_SCALING_LIST_ENABLED(pp)      V383_HEVC_T(pp, 0, 1)
#define V383_HEVC_AMP_ENABLED(pp)               V383_HEVC_T(pp, 1, 1)
#define V383_HEVC_SAO_ENABLED(pp)               V383_HEVC_T(pp, 2, 1)
#define V383_HEVC_PCM_ENABLED(pp)               V383_HEVC_T(pp, 3, 1)
#define V383_HEVC_PCM_BD_LUMA_MINUS1(pp)        V383_HEVC_T(pp, 4, 4)
#define V383_HEVC_PCM_BD_CHROMA_MINUS1(pp)      V383_HEVC_T(pp, 8, 4)
#define V383_HEVC_LOG2_MIN_PCM_CB_MINUS3(pp)    V383_HEVC_T(pp, 12, 2)
#define V383_HEVC_LOG2_DIFF_MAX_MIN_PCM_CB(pp)  V383_HEVC_T(pp, 14, 2)
#define V383_HEVC_PCM_LOOP_FILTER_DISABLED(pp)  V383_HEVC_T(pp, 16, 1)
#define V383_HEVC_LONG_TERM_REFS_PRESENT(pp)    V383_HEVC_T(pp, 17, 1)
#define V383_HEVC_SPS_TEMPORAL_MVP(pp)          V383_HEVC_T(pp, 18, 1)
#define V383_HEVC_STRONG_INTRA_SMOOTHING(pp)    V383_HEVC_T(pp, 19, 1)
#define V383_HEVC_DEPENDENT_SLICES(pp)          V383_HEVC_T(pp, 20, 1)
#define V383_HEVC_OUTPUT_FLAG_PRESENT(pp)       V383_HEVC_T(pp, 21, 1)
#define V383_HEVC_NUM_EXTRA_SH_BITS(pp)         V383_HEVC_T(pp, 22, 3)
#define V383_HEVC_SIGN_DATA_HIDING(pp)          V383_HEVC_T(pp, 25, 1)
#define V383_HEVC_CABAC_INIT_PRESENT(pp)        V383_HEVC_T(pp, 26, 1)

/* dwCodingSettingPicturePropertyFlags */
#define V383_HEVC_S(pp, pos)                    V383_HB((pp)->dwCodingSettingPicturePropertyFlags, pos, 1)
#define V383_HEVC_CONSTRAINED_INTRA_PRED(pp)    V383_HEVC_S(pp, 0)
#define V383_HEVC_TRANSFORM_SKIP(pp)            V383_HEVC_S(pp, 1)
#define V383_HEVC_CU_QP_DELTA(pp)               V383_HEVC_S(pp, 2)
#define V383_HEVC_SLICE_CHROMA_QP_OFFSETS(pp)   V383_HEVC_S(pp, 3)
#define V383_HEVC_WEIGHTED_PRED(pp)             V383_HEVC_S(pp, 4)
#define V383_HEVC_WEIGHTED_BIPRED(pp)           V383_HEVC_S(pp, 5)
#define V383_HEVC_TRANSQUANT_BYPASS(pp)         V383_HEVC_S(pp, 6)
#define V383_HEVC_TILES_ENABLED(pp)             V383_HEVC_S(pp, 7)
#define V383_HEVC_ENTROPY_CODING_SYNC(pp)       V383_HEVC_S(pp, 8)
#define V383_HEVC_UNIFORM_SPACING(pp)           V383_HEVC_S(pp, 9)
#define V383_HEVC_LF_ACROSS_TILES(pp)           V383_HEVC_S(pp, 10)
#define V383_HEVC_LF_ACROSS_SLICES(pp)          V383_HEVC_S(pp, 11)
#define V383_HEVC_DEBLOCKING_OVERRIDE(pp)       V383_HEVC_S(pp, 12)
#define V383_HEVC_DEBLOCKING_DISABLED(pp)       V383_HEVC_S(pp, 13)
#define V383_HEVC_LISTS_MODIFICATION(pp)        V383_HEVC_S(pp, 14)
#define V383_HEVC_SH_EXTENSION_PRESENT(pp)      V383_HEVC_S(pp, 15)
#define V383_HEVC_IRAP(pp)                      V383_HEVC_S(pp, 16)
#define V383_HEVC_IDR(pp)                       V383_HEVC_S(pp, 17)
#define V383_HEVC_INTRA(pp)                     V383_HEVC_S(pp, 18)

#endif /* V383_DXVA_HEVC_H */
