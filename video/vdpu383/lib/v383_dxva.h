/*
 * v383_dxva.h - the DXVA H.264 structures the VDPU383 register builder takes
 *
 * Byte-for-byte the layout of DXVA_PicEntry_H264, DXVA_PicParams_H264 and
 * DXVA_Qmatrix_H264 in the Windows SDK's dxva.h (packed to 1 byte, as dxva.h
 * packs them), under V383_ names so that this header can sit beside dxva.h
 * in a user-mode driver and needs nothing from it in a kernel-mode one.
 * vdpu383test checks the layout against dxva.h at compile time on Windows
 * (dxva_check.c).
 *
 * wBitFields is a plain USHORT here with accessors, not C bitfields: the
 * bit order is the one documented for the structure (field_pic_flag in bit
 * 0 up to IntraPicFlag in bit 15), which is also how MSVC lays out the
 * dxva.h bitfields.
 *
 * Written from the Microsoft DXVA H.264 specification ("DirectX Video
 * Acceleration Specification for H.264/AVC Decoding") and dxva.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef V383_DXVA_H
#define V383_DXVA_H

#pragma pack(push, 1)

typedef struct V383_DXVA_PicEntry_H264 {
    unsigned char bPicEntry;            /* Index7Bits | AssociatedFlag << 7; 0xFF = unused */
} V383_DXVA_PicEntry_H264;

#define V383_PIC_INDEX(e)       ((unsigned)((e).bPicEntry & 0x7F))
#define V383_PIC_FLAG(e)        ((unsigned)(((e).bPicEntry >> 7) & 1))
#define V383_PIC_UNUSED(e)      ((e).bPicEntry == 0xFF)
#define V383_PIC_ENTRY(i, f)    ((unsigned char)(((i) & 0x7F) | (((f) & 1) << 7)))

typedef struct V383_DXVA_PicParams_H264 {
    unsigned short wFrameWidthInMbsMinus1;
    unsigned short wFrameHeightInMbsMinus1;
    V383_DXVA_PicEntry_H264 CurrPic;    /* AssociatedFlag = bottom field */
    unsigned char  num_ref_frames;
    unsigned short wBitFields;
    unsigned char  bit_depth_luma_minus8;
    unsigned char  bit_depth_chroma_minus8;
    unsigned short Reserved16Bits;
    unsigned int   StatusReportFeedbackNumber;
    V383_DXVA_PicEntry_H264 RefFrameList[16];   /* AssociatedFlag = long-term */
    int            CurrFieldOrderCnt[2];
    int            FieldOrderCntList[16][2];
    signed char    pic_init_qs_minus26;
    signed char    chroma_qp_index_offset;
    signed char    second_chroma_qp_index_offset;
    unsigned char  ContinuationFlag;
    signed char    pic_init_qp_minus26;
    unsigned char  num_ref_idx_l0_active_minus1;
    unsigned char  num_ref_idx_l1_active_minus1;
    unsigned char  Reserved8BitsA;
    unsigned short FrameNumList[16];    /* frame_num, or LongTermFrameIdx for a long-term entry */
    unsigned int   UsedForReferenceFlags;   /* 2 bits per entry: top (even), bottom (odd) */
    unsigned short NonExistingFrameFlags;
    unsigned short frame_num;
    unsigned char  log2_max_frame_num_minus4;
    unsigned char  pic_order_cnt_type;
    unsigned char  log2_max_pic_order_cnt_lsb_minus4;
    unsigned char  delta_pic_order_always_zero_flag;
    unsigned char  direct_8x8_inference_flag;
    unsigned char  entropy_coding_mode_flag;
    unsigned char  pic_order_present_flag;
    unsigned char  num_slice_groups_minus1;
    unsigned char  slice_group_map_type;
    unsigned char  deblocking_filter_control_present_flag;
    unsigned char  redundant_pic_cnt_present_flag;
    unsigned char  Reserved8BitsB;
    unsigned short slice_group_change_rate_minus1;
    unsigned char  SliceGroupMap[810];
} V383_DXVA_PicParams_H264;

typedef struct V383_DXVA_Qmatrix_H264 {
    unsigned char bScalingLists4x4[6][16];
    unsigned char bScalingLists8x8[2][64];
} V383_DXVA_Qmatrix_H264;

#pragma pack(pop)

/* wBitFields */
#define V383_PP_FIELD_PIC_FLAG          0
#define V383_PP_MBAFF_FRAME_FLAG        1
#define V383_PP_RESIDUAL_COLOUR         2
#define V383_PP_SP_FOR_SWITCH           3
#define V383_PP_CHROMA_FORMAT_IDC       4   /* 2 bits */
#define V383_PP_REF_PIC_FLAG            6
#define V383_PP_CONSTRAINED_INTRA_PRED  7
#define V383_PP_WEIGHTED_PRED           8
#define V383_PP_WEIGHTED_BIPRED_IDC     9   /* 2 bits */
#define V383_PP_MBS_CONSECUTIVE         11
#define V383_PP_FRAME_MBS_ONLY          12
#define V383_PP_TRANSFORM_8X8           13
#define V383_PP_MIN_LUMA_BIPRED_8X8     14
#define V383_PP_INTRA_PIC               15

#define V383_PP_GET(pp, bit, width) \
    (((unsigned)(pp)->wBitFields >> (bit)) & ((1u << (width)) - 1))
#define V383_PP_SET(pp, bit, width, v) \
    ((pp)->wBitFields = (unsigned short)(((pp)->wBitFields & ~(((1u << (width)) - 1) << (bit))) | \
                                         ((((unsigned)(v)) & ((1u << (width)) - 1)) << (bit))))

#endif /* V383_DXVA_H */
