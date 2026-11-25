/*
 * vdpu383_h264.c - DXVA H.264 picture -> VDPU383 register file
 *
 * The register and buffer layouts are Rockchip MPP's (Apache-2.0 OR MIT,
 * used here under MIT), commit 9b56348:
 *
 *   mpp/hal/rkdec/h264d/hal_h264d_vdpu383.c   set_registers(), prepare_framerps(),
 *                                             vdpu383_h264d_rcb_calc(), gen_regs()
 *   mpp/hal/rkdec/h264d/hal_h264d_com.c       vdpu38x_h264d_prepare_spspps(),
 *                                             vdpu38x_h264d_prepare_scanlist()
 *   mpp/hal/rkdec/common/vdpu383_com.c        vdpu383_init_ctrl_regs(),
 *                                             vdpu383_setup_statistic(),
 *                                             vdpu383_setup_cur_stride_info()
 *   mpp/hal/rkdec/common/vdpu38x_com.c        RCB bookkeeping
 *
 *   Copyright (c) 2024-2025 Rockchip Electronics Co., Ltd.
 *   SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * Differences from MPP, all deliberate:
 *
 * - MPP's parser hands its HAL the three default reference lists in a
 *   private DXVA_Slice_H264_Long extension. DXVA does not carry them, so
 *   they are built here from DXVA_PicParams_H264, the way the H.264 spec
 *   (8.2.4.2) and mainline's v4l2-h264.c build them. The hardware applies
 *   ref_pic_list_modification itself: mainline's rkvdec also hands it only
 *   the default lists.
 * - MPP writes the 176-byte SPS/PPS record at the start of its buffer;
 *   mainline writes it at pps_id * 176 in a 256-entry table and points the
 *   decoder at the table. It is written at both places here, which is right
 *   whichever the hardware does. pic_parameter_set_id is not in
 *   DXVA_PicParams_H264, so it is read from the first slice header.
 * - MPP's RCB sizes use float arithmetic; the same values are computed
 *   here with integers (the float results are exact for every size the
 *   decoder supports, so nothing changes).
 * - Only frame pictures, 4:2:0, 8 bit, raster NV12 output. Field
 *   pictures are refused (V383_E_UNSUPPORTED); MBAFF frames go through but
 *   have not been tested.
 * - MPP never sets NonExistingFrameFlags (h264d_fill.c), so its
 *   "frame_num_wrap = 0 for a non-existing frame" branch never runs; the
 *   flag is ignored here for the same effective behaviour.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "vdpu383_h264.h"

/* ----------------------------------------------------------- helpers */

#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#define DIV_UP(x, d)    (((x) + (d) - 1) / (d))

static void v_zero(void *p, unsigned int n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--)
        *b++ = 0;
}

static void v_copy(void *d, const void *s, unsigned int n)
{
    unsigned char *db = (unsigned char *)d;
    const unsigned char *sb = (const unsigned char *)s;
    while (n--)
        *db++ = *sb++;
}

/* LSB-first bit writer, the order of MPP's mpp_put_bits() and mainline's
 * rkvdec_set_bw_field() on a little-endian CPU. */
typedef struct BitPut {
    unsigned char *buf;
    unsigned int   size;    /* bytes */
    unsigned int   pos;     /* bits */
} BitPut;

static void bp_init(BitPut *bp, unsigned char *buf, unsigned int size)
{
    bp->buf = buf;
    bp->size = size;
    bp->pos = 0;
    v_zero(buf, size);
}

static void bp_put(BitPut *bp, unsigned int value, unsigned int bits)
{
    unsigned int i;

    for (i = 0; i < bits; i++, bp->pos++) {
        if ((bp->pos >> 3) >= bp->size)
            return;
        if ((value >> i) & 1)
            bp->buf[bp->pos >> 3] = (unsigned char)(bp->buf[bp->pos >> 3] | (1u << (bp->pos & 7)));
    }
}

static void bp_align(BitPut *bp, unsigned int bits)
{
    bp->pos = ALIGN_UP(bp->pos, bits);
}

static void reg_set(V383_RegFile *rf, unsigned int r, unsigned int v)
{
    rf->regs[r] = v;
}

static void mask_range(V383_RegFile *rf, unsigned int first, unsigned int last)
{
    unsigned int r;

    for (r = first; r <= last; r++)
        rf->write_mask[r / 32] |= 1u << (r % 32);
}

/* ------------------------------------------------------ sizes and ids */

unsigned int v383_h264_colmv_size(unsigned int width, unsigned int height, unsigned int frame_mbs_only)
{
    /* MPP vdpu383_h264d_gen_regs(): both dimensions to 64, then w64 * h16. */
    unsigned int w = ALIGN_UP(width, 64);
    unsigned int h = ALIGN_UP(height, 64);
    unsigned int size = ALIGN_UP(w, 64) * ALIGN_UP(h, 16);

    if (!frame_mbs_only)
        size *= 2;
    return size;
}

/*
 * The RCB buffers, MPP vdpu383_h264d_rcb_calc() with one tile covering the
 * picture, 4:2:0, and the bit depth fixed at 10 as MPP fixes it for the
 * VDPU383. Values are bits; MPP_RCB_BYTES() turns them into bytes aligned to
 * 64. Order and register numbers are MPP's Vdpu38xRcbType.
 */
enum {
    RCB_STRMD_IN_ROW, RCB_STRMD_ON_ROW, RCB_INTER_IN_ROW, RCB_INTER_ON_ROW,
    RCB_INTRA_IN_ROW, RCB_INTRA_ON_ROW, RCB_FLTD_IN_ROW, RCB_FLTD_PROT_IN_ROW,
    RCB_FLTD_ON_ROW, RCB_FLTD_ON_COL, RCB_FLTD_UPSC_ON_COL, RCB_COUNT
};

static unsigned int rcb_bytes(unsigned long long bits)
{
    return (unsigned int)ALIGN_UP((bits + 7) / 8, 64ull);
}

static void rcb_sizes(unsigned int width, unsigned int height, unsigned int mbaff,
                      unsigned int size[RCB_COUNT])
{
    unsigned int w = ALIGN_UP(width, 64);   /* in_tl_row = on_tl_row = picture width */
    unsigned int m = mbaff ? 2 : 1;
    unsigned long long fltd;
    unsigned int append = (w > 4096) ? 256 * 16 * 8 : 864 * 16 * 8;

    (void)height;

    size[RCB_STRMD_IN_ROW] = 0;
    size[RCB_STRMD_ON_ROW] = (w > 4096) ? rcb_bytes((unsigned long long)DIV_UP(w, 16) * 154 * m) : 0;
    size[RCB_INTER_IN_ROW] = rcb_bytes((unsigned long long)DIV_UP(w, 4) * 92 * m);
    size[RCB_INTER_ON_ROW] = size[RCB_INTER_IN_ROW];
    /* ROUNDUP(512, w * (10 + 2) * m * intra_uv_coef(420) = 2) */
    size[RCB_INTRA_IN_ROW] = rcb_bytes(ALIGN_UP((unsigned long long)w * 12 * m * 2, 512ull));
    size[RCB_INTRA_ON_ROW] = size[RCB_INTRA_IN_ROW];
    /*
     * ALIGN(w, 16) * (1.6 * 10 + 0.5) * ((6 + 3) * m + 2 + 1.5)
     *   = ALIGN(w, 16) * 33 * (18 * m + 7) / 4      (filter_row_uv_coef(420) = 1)
     * The in-tile row buffer is half of that plus a fixed append; the
     * protect buffer is the same size.
     */
    fltd = (unsigned long long)ALIGN_UP(w, 16) * 33 * (18 * m + 7);
    size[RCB_FLTD_IN_ROW] = rcb_bytes(fltd / 8 + append);
    size[RCB_FLTD_PROT_IN_ROW] = size[RCB_FLTD_IN_ROW];
    size[RCB_FLTD_ON_ROW] = rcb_bytes(fltd / 4);
    size[RCB_FLTD_ON_COL] = 0;
    size[RCB_FLTD_UPSC_ON_COL] = 0;
}

unsigned int v383_h264_rcb_size(unsigned int width, unsigned int height, unsigned int mbaff)
{
    unsigned int size[RCB_COUNT];
    unsigned int i, total = 0;

    rcb_sizes(width, height, mbaff, size);
    for (i = 0; i < RCB_COUNT; i++)
        total += size[i];
    return total;
}

/* RBSP bit reader over a few unescaped bytes. */
typedef struct BitGet {
    unsigned char b[48];
    unsigned int  n;
    unsigned int  pos;
} BitGet;

static int bg_bit(BitGet *g)
{
    int v;
    if (g->pos >= g->n * 8)
        return -1;
    v = (g->b[g->pos >> 3] >> (7 - (g->pos & 7))) & 1;
    g->pos++;
    return v;
}

static int bg_ue(BitGet *g, unsigned int *out)
{
    unsigned int zeros = 0, v = 0, i;
    int b;

    while ((b = bg_bit(g)) == 0)
        if (++zeros > 31)
            return -1;
    if (b < 0)
        return -1;
    for (i = 0; i < zeros; i++) {
        b = bg_bit(g);
        if (b < 0)
            return -1;
        v = (v << 1) | (unsigned int)b;
    }
    *out = (1u << zeros) - 1 + v;
    return 0;
}

int v383_h264_parse_pps_id(const unsigned char *s, unsigned int len)
{
    unsigned int i, zeros = 0;
    BitGet g;
    unsigned int first_mb, slice_type, pps_id;

    for (i = 0; i + 3 < len; i++) {
        unsigned int type;

        if (!(s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1))
            continue;
        type = s[i + 3] & 0x1F;
        if (type != 1 && type != 5)
            continue;

        /* Unescape what follows the NAL header (emulation prevention). */
        g.n = 0;
        g.pos = 0;
        zeros = 0;
        for (i = i + 4; i < len && g.n < sizeof(g.b); i++) {
            if (zeros >= 2 && s[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = (s[i] == 0) ? zeros + 1 : 0;
            g.b[g.n++] = s[i];
        }
        if (bg_ue(&g, &first_mb) || bg_ue(&g, &slice_type) || bg_ue(&g, &pps_id) || pps_id > 255)
            return -1;
        return (int)pps_id;
    }
    return -1;
}

unsigned int v383_h264_default_profile(const V383_DXVA_PicParams_H264 *pp, unsigned int scaling)
{
    if (V383_PP_GET(pp, V383_PP_TRANSFORM_8X8, 1) || scaling ||
        V383_PP_GET(pp, V383_PP_CHROMA_FORMAT_IDC, 2) != 1 ||
        pp->bit_depth_luma_minus8 || pp->bit_depth_chroma_minus8)
        return 100;                         /* High */
    if (pp->entropy_coding_mode_flag || !V383_PP_GET(pp, V383_PP_FRAME_MBS_ONLY, 1))
        return 77;                          /* Main */
    return 66;                              /* Baseline */
}

/* ------------------------------------------------------ reference lists */

typedef struct RefKey {
    unsigned char idx;
    int           key;
} RefKey;

static void sort_keys(RefKey *k, unsigned int n, int descending)
{
    unsigned int i, j;

    /* Insertion sort, stable: n <= 16. */
    for (i = 1; i < n; i++) {
        RefKey t = k[i];
        for (j = i; j > 0; j--) {
            int before = descending ? (k[j - 1].key < t.key) : (k[j - 1].key > t.key);
            if (!before)
                break;
            k[j] = k[j - 1];
        }
        k[j] = t;
    }
}

static int frame_poc(const V383_DXVA_PicParams_H264 *pp, unsigned int i)
{
    int t = pp->FieldOrderCntList[i][0];
    int b = pp->FieldOrderCntList[i][1];
    return t < b ? t : b;
}

void v383_h264_build_reflists(const V383_DXVA_PicParams_H264 *pp, V383_H264RefLists *out)
{
    RefKey st[16], lt[16], before[16], after[16];
    unsigned int nst = 0, nlt = 0, nbefore = 0, nafter = 0, i, n;
    unsigned int max_frame_num = 1u << (pp->log2_max_frame_num_minus4 + 4);
    int cur_poc;
    int same;

    v_zero(out, sizeof(*out));
    for (i = 0; i < 32; i++)
        out->p[i] = out->b0[i] = out->b1[i] = 0xFF;

    cur_poc = pp->CurrFieldOrderCnt[0] < pp->CurrFieldOrderCnt[1] ?
              pp->CurrFieldOrderCnt[0] : pp->CurrFieldOrderCnt[1];

    for (i = 0; i < 16; i++) {
        unsigned int used = (pp->UsedForReferenceFlags >> (2 * i)) & 3;

        if (V383_PIC_UNUSED(pp->RefFrameList[i]) || used != 3)
            continue;               /* frame decoding uses frames only */
        if (V383_PIC_FLAG(pp->RefFrameList[i])) {
            lt[nlt].idx = (unsigned char)i;
            lt[nlt].key = pp->FrameNumList[i];          /* LongTermPicNum = LongTermFrameIdx */
            nlt++;
        } else {
            int fn = pp->FrameNumList[i];
            st[nst].idx = (unsigned char)i;
            st[nst].key = (fn > pp->frame_num) ? fn - (int)max_frame_num : fn;  /* PicNum */
            nst++;

            if (frame_poc(pp, i) < cur_poc) {
                before[nbefore].idx = (unsigned char)i;
                before[nbefore].key = frame_poc(pp, i);
                nbefore++;
            } else {
                after[nafter].idx = (unsigned char)i;
                after[nafter].key = frame_poc(pp, i);
                nafter++;
            }
        }
    }

    sort_keys(st, nst, 1);
    sort_keys(lt, nlt, 0);
    sort_keys(before, nbefore, 1);
    sort_keys(after, nafter, 0);

    /* 8.2.4.2.1: P, short-term by PicNum descending, long-term ascending. */
    n = 0;
    for (i = 0; i < nst; i++) out->p[n++] = st[i].idx;
    for (i = 0; i < nlt; i++) out->p[n++] = lt[i].idx;
    out->num_p = n;

    /* 8.2.4.2.3: B. */
    n = 0;
    for (i = 0; i < nbefore; i++) out->b0[n++] = before[i].idx;
    for (i = 0; i < nafter; i++)  out->b0[n++] = after[i].idx;
    for (i = 0; i < nlt; i++)     out->b0[n++] = lt[i].idx;
    out->num_b0 = n;

    n = 0;
    for (i = 0; i < nafter; i++)  out->b1[n++] = after[i].idx;
    for (i = 0; i < nbefore; i++) out->b1[n++] = before[i].idx;
    for (i = 0; i < nlt; i++)     out->b1[n++] = lt[i].idx;
    out->num_b1 = n;

    /* RefPicList1 identical to RefPicList0 with more than one entry: swap
     * its first two. */
    if (out->num_b1 > 1) {
        same = 1;
        for (i = 0; i < out->num_b1; i++)
            if (out->b0[i] != out->b1[i])
                same = 0;
        if (same) {
            unsigned char t = out->b1[0];
            out->b1[0] = out->b1[1];
            out->b1[1] = t;
        }
    }
}

/* ------------------------------------------------------ buffer contents */

/* MPP vdpu38x_h264d_prepare_spspps(), the RK3576 branch (RPS separate). */
static void build_spspps(const V383_H264Input *in, unsigned int pps_id, unsigned int sps_id,
                         unsigned int profile_idc, unsigned int cs3, unsigned int scaling,
                         unsigned char out[V383_H264_SPSPPS_BYTES])
{
    const V383_DXVA_PicParams_H264 *pp = in->pp;
    BitPut bp;
    unsigned int pic_width, pic_height, tmp, i;
    unsigned int field_pic = V383_PP_GET(pp, V383_PP_FIELD_PIC_FLAG, 1);
    unsigned int frame_mbs_only = V383_PP_GET(pp, V383_PP_FRAME_MBS_ONLY, 1);

    bp_init(&bp, out, V383_H264_SPSPPS_BYTES);

    /*
     * MPP's structure carries pic_height_in_map_units_minus1 here and
     * multiplies by (2 - frame_mbs_only_flag). DXVA's wFrameHeightInMbsMinus1
     * is already FrameHeightInMbs - 1, so only the field halving is left.
     */
    pic_width   = 16 * (pp->wFrameWidthInMbsMinus1 + 1u);
    pic_height  = 16 * (pp->wFrameHeightInMbsMinus1 + 1u);
    pic_height /= (1 + field_pic);

    /* SPS */
    bp_put(&bp, sps_id, 4);
    bp_put(&bp, profile_idc, 8);
    bp_put(&bp, cs3, 1);
    bp_put(&bp, V383_PP_GET(pp, V383_PP_CHROMA_FORMAT_IDC, 2), 2);
    bp_put(&bp, pp->bit_depth_luma_minus8, 3);
    bp_put(&bp, pp->bit_depth_chroma_minus8, 3);
    bp_put(&bp, 0, 1);          /* MPP: "set 0" (qpprime_y_zero_transform_bypass) */
    bp_put(&bp, pp->log2_max_frame_num_minus4, 4);
    bp_put(&bp, pp->num_ref_frames, 5);
    bp_put(&bp, pp->pic_order_cnt_type, 2);
    bp_put(&bp, pp->log2_max_pic_order_cnt_lsb_minus4, 4);
    bp_put(&bp, pp->delta_pic_order_always_zero_flag, 1);
    bp_put(&bp, pic_width, 16);
    bp_put(&bp, pic_height, 16);
    bp_put(&bp, frame_mbs_only, 1);
    bp_put(&bp, V383_PP_GET(pp, V383_PP_MBAFF_FRAME_FLAG, 1), 1);
    bp_put(&bp, pp->direct_8x8_inference_flag, 1);
    bp_put(&bp, 0, 1);          /* mvc_extension_enable */
    bp_put(&bp, 0, 22);         /* num_views, view_id[2] */

    /* PPS */
    bp_put(&bp, pps_id, 8);
    bp_put(&bp, sps_id, 5);
    bp_put(&bp, pp->entropy_coding_mode_flag, 1);
    bp_put(&bp, pp->pic_order_present_flag, 1);
    bp_put(&bp, pp->num_ref_idx_l0_active_minus1, 5);
    bp_put(&bp, pp->num_ref_idx_l1_active_minus1, 5);
    bp_put(&bp, V383_PP_GET(pp, V383_PP_WEIGHTED_PRED, 1), 1);
    bp_put(&bp, V383_PP_GET(pp, V383_PP_WEIGHTED_BIPRED_IDC, 2), 2);
    bp_put(&bp, (unsigned int)(int)pp->pic_init_qp_minus26, 7);
    bp_put(&bp, (unsigned int)(int)pp->pic_init_qs_minus26, 6);
    bp_put(&bp, (unsigned int)(int)pp->chroma_qp_index_offset, 5);
    bp_put(&bp, pp->deblocking_filter_control_present_flag, 1);
    bp_put(&bp, V383_PP_GET(pp, V383_PP_CONSTRAINED_INTRA_PRED, 1), 1);
    bp_put(&bp, pp->redundant_pic_cnt_present_flag, 1);
    bp_put(&bp, V383_PP_GET(pp, V383_PP_TRANSFORM_8X8, 1), 1);
    bp_put(&bp, (unsigned int)(int)pp->second_chroma_qp_index_offset, 5);
    bp_put(&bp, scaling, 1);

    /* DPB: long-term flags, then view index (0) */
    tmp = 0;
    for (i = 0; i < 16; i++)
        if (!V383_PIC_UNUSED(pp->RefFrameList[i]) && V383_PIC_FLAG(pp->RefFrameList[i]))
            tmp |= 1u << i;
    bp_put(&bp, tmp, 32);

    /* Current picture */
    bp_put(&bp, field_pic, 1);
    bp_put(&bp, field_pic && V383_PIC_FLAG(pp->CurrPic), 1);
    bp_put(&bp, (unsigned int)pp->CurrFieldOrderCnt[0], 32);
    bp_put(&bp, (unsigned int)pp->CurrFieldOrderCnt[1], 32);

    /* Reference POCs */
    for (i = 0; i < 16; i++) {
        bp_put(&bp, (unsigned int)pp->FieldOrderCntList[i][0], 32);
        bp_put(&bp, (unsigned int)pp->FieldOrderCntList[i][1], 32);
    }

    /*
     * RefPicFiledFlags (MPP: the reference was coded as fields; 0 for the
     * frame-only decoding done here), top-field used flags, bottom-field
     * used flags, RefPicColmvUsedFlags (MPP: every reference picture that
     * was decoded rather than cloned; mainline: every active entry).
     */
    tmp = 0;
    for (i = 0; i < 16; i++)
        tmp |= ((pp->UsedForReferenceFlags >> (2 * i)) & 1u) << (i + 16);
    bp_put(&bp, tmp, 32);
    tmp = 0;
    for (i = 0; i < 16; i++) {
        tmp |= ((pp->UsedForReferenceFlags >> (2 * i + 1)) & 1u) << i;
        if (!V383_PIC_UNUSED(pp->RefFrameList[i]))
            tmp |= 1u << (i + 16);
    }
    bp_put(&bp, tmp, 32);
    bp_align(&bp, 64);
}

/* MPP prepare_framerps(). */
static void build_rps(const V383_DXVA_PicParams_H264 *pp, const V383_H264RefLists *l,
                      unsigned char *out, unsigned int size)
{
    BitPut bp;
    unsigned int i, j;
    unsigned int max_frame_num = 1u << (pp->log2_max_frame_num_minus4 + 4);
    const unsigned char *lists[3];
    unsigned int counts[3];

    bp_init(&bp, out, size);

    for (i = 0; i < 16; i++) {
        unsigned int wrap = 0;

        if (!V383_PIC_UNUSED(pp->RefFrameList[i]) && V383_PIC_FLAG(pp->RefFrameList[i]))
            wrap = pp->FrameNumList[i];
        else
            wrap = (pp->FrameNumList[i] > pp->frame_num) ?
                   (unsigned int)(pp->FrameNumList[i] - max_frame_num) : pp->FrameNumList[i];
        bp_put(&bp, wrap & 0xFFFF, 16);
    }

    bp_put(&bp, 0, 32);         /* RefPicLayerIdList << 16: no MVC */

    lists[0] = l->p;  counts[0] = l->num_p;
    lists[1] = l->b0; counts[1] = l->num_b0;
    lists[2] = l->b1; counts[2] = l->num_b1;
    for (j = 0; j < 3; j++) {
        for (i = 0; i < 32; i++) {
            unsigned int v = 0;

            if (i < counts[j] && lists[j][i] != 0xFF)
                v = (lists[j][i] & 0x0F) | (1u << 4);    /* dpb index, valid; bottom 0, view 0 */
            bp_put(&bp, v, 7);
        }
    }
    bp_align(&bp, 128);
}

/* MPP vdpu38x_h264d_prepare_scanlist(). DXVA lists are in zig-zag (bitstream)
 * order; the decoder wants raster order, 8x8 lists as four 4x4 blocks. */
static const unsigned char zz4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const unsigned char zz8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static void build_scanlist(const V383_DXVA_Qmatrix_H264 *qm, unsigned char *out)
{
    unsigned char r4[16], r8[64];
    unsigned int i, j, n = 0, bx, by;

    for (i = 0; i < 6; i++) {
        for (j = 0; j < 16; j++)
            r4[zz4[j]] = qm->bScalingLists4x4[i][j];
        for (j = 0; j < 16; j++)
            out[n++] = r4[j];
    }
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 64; j++)
            r8[zz8[j]] = qm->bScalingLists8x8[i][j];
        for (by = 0; by < 8; by += 4)
            for (bx = 0; bx < 8; bx += 4)
                for (j = 0; j < 4; j++) {
                    unsigned int pos = by * 8 + bx + j * 8;
                    out[n++] = r8[pos + 0];
                    out[n++] = r8[pos + 1];
                    out[n++] = r8[pos + 2];
                    out[n++] = r8[pos + 3];
                }
    }
}

/* ------------------------------------------------------------- build */

int v383_h264_build(const V383_H264Input *in, V383_RegFile *rf)
{
    const V383_DXVA_PicParams_H264 *pp = in->pp;
    V383_H264RefLists lists;
    unsigned char spspps[V383_H264_SPSPPS_BYTES];
    unsigned int rcb[RCB_COUNT];
    unsigned int width, height, i, off;
    unsigned int pps_id, sps_id, profile_idc, cs3, scaling;
    unsigned int cur, near_valid = 0, near_index = 0, min_frame_num = 0;
    const V383_Surface *s;

    if (!pp || !in->aux_cpu || !in->surfaces || !rf)
        return V383_E_INVALID;
    if (V383_PP_GET(pp, V383_PP_FIELD_PIC_FLAG, 1))
        return V383_E_UNSUPPORTED;
    if (V383_PP_GET(pp, V383_PP_CHROMA_FORMAT_IDC, 2) != 1 ||
        pp->bit_depth_luma_minus8 || pp->bit_depth_chroma_minus8)
        return V383_E_UNSUPPORTED;
    if (pp->num_slice_groups_minus1)
        return V383_E_UNSUPPORTED;
    cur = V383_PIC_INDEX(pp->CurrPic);
    if (cur >= in->num_surfaces)
        return V383_E_INVALID;
    for (i = 0; i < 16; i++)
        if (!V383_PIC_UNUSED(pp->RefFrameList[i]) &&
            V383_PIC_INDEX(pp->RefFrameList[i]) >= in->num_surfaces)
            return V383_E_INVALID;
    if ((in->hor_stride & 15) || (in->ver_stride & 15))
        return V383_E_INVALID;

    width  = 16 * (pp->wFrameWidthInMbsMinus1 + 1u);
    height = 16 * (pp->wFrameHeightInMbsMinus1 + 1u);     /* FrameHeightInMbs, DXVA */

    pps_id = in->pps_id;
    if (pps_id == V383_PPS_ID_FROM_STREAM) {
        int id = in->stream_cpu ? v383_h264_parse_pps_id(in->stream_cpu, in->stream_len) : -1;
        if (id < 0)
            return V383_E_STREAM;
        pps_id = (unsigned int)id;
    }
    if (pps_id > 255)
        return V383_E_INVALID;

    scaling = (in->qm && in->scaling_matrix_present) ? 1 : 0;
    if (in->extra && in->extra->valid) {
        profile_idc = in->extra->profile_idc;
        sps_id = in->extra->seq_parameter_set_id;
        cs3 = in->extra->constraint_set3_flag;
    } else {
        profile_idc = v383_h264_default_profile(pp, scaling);
        sps_id = 0;
        cs3 = 0;
    }

    /* ---- auxiliary buffer */
    v383_h264_build_reflists(pp, &lists);
    build_spspps(in, pps_id, sps_id, profile_idc, cs3, scaling, spspps);

    v_copy(in->aux_cpu + V383_H264_AUX_CABAC_OFF, v383_h264_cabac_table, sizeof(v383_h264_cabac_table));
    build_rps(pp, &lists, in->aux_cpu + V383_H264_AUX_RPS_OFF, 384);
    if (scaling)
        build_scanlist(in->qm, in->aux_cpu + V383_H264_AUX_SCALING_OFF);
    v_copy(in->aux_cpu + V383_H264_AUX_GBL_OFF, spspps, V383_H264_SPSPPS_BYTES);
    if (pps_id)
        v_copy(in->aux_cpu + V383_H264_AUX_GBL_OFF + pps_id * V383_H264_SPSPPS_BYTES,
               spspps, V383_H264_SPSPPS_BYTES);

    /* ---- registers */
    v_zero(rf, sizeof(*rf));

    /* vdpu383_init_ctrl_regs(MPP_VIDEO_CodingAVC) */
    reg_set(rf, 8, 1);                          /* dec_mode: H.264 */
    reg_set(rf, 9, 0);                          /* raster, no FBC, no scale-down, buf_empty_en 0 */
    reg_set(rf, 10, 0x3FF);                     /* every block auto-gating enable */
    reg_set(rf, 13, 0xFFFFFF);                  /* core timeout */
    reg_set(rf, 16, 1);                         /* error_proc_disable */
    reg_set(rf, 20, 0xFFFEDFFF);                /* cabac error enable, low */
    reg_set(rf, 21, 0x0FFBF9FF);                /* cabac error enable, high */
    /* vdpu383_setup_statistic() */
    reg_set(rf, 28, 1u | (1u << 3) | (11u << 4));       /* axi_perf_work_e, axi_cnt_type, rd_latency_id 11 */
    reg_set(rf, 29, 1u | (1u << 3) | (17u << 4));       /* addr_align_type 1, aw_cnt_id_type 1, ar_count_id 17 */
    reg_set(rf, 30, 0);                                 /* QoS 0 ("hurry") */
    rf->core_timeout = rf->regs[13];

    /* Stream */
    reg_set(rf, 66, in->stream_len);
    reg_set(rf, 67, V383_H264_SPSPPS_BYTES / 16);       /* 128-bit units */

    /* vdpu383_setup_cur_stride_info(), raster */
    reg_set(rf, 68, in->hor_stride >> 4);
    reg_set(rf, 69, in->hor_stride >> 4);
    reg_set(rf, 70, (in->hor_stride * in->ver_stride) >> 4);
    reg_set(rf, 80, rf->regs[68]);
    reg_set(rf, 81, rf->regs[69]);
    reg_set(rf, 82, rf->regs[70]);

    reg_set(rf, 128, in->stream_dma);
    reg_set(rf, 129, in->aux_dma + V383_H264_AUX_RPS_OFF);
    reg_set(rf, 130, in->aux_dma + V383_H264_AUX_CABAC_OFF);
    reg_set(rf, 131, in->aux_dma + V383_H264_AUX_GBL_OFF);
    reg_set(rf, 132, scaling ? in->aux_dma + V383_H264_AUX_SCALING_OFF : 0);

    /* RCB: offset/length pairs from register 140, MPP vdpu38x_setup_rcb() */
    rcb_sizes(width, height, V383_PP_GET(pp, V383_PP_MBAFF_FRAME_FLAG, 1), rcb);
    off = 0;
    for (i = 0; i < RCB_COUNT; i++) {
        reg_set(rf, 140 + 2 * i, in->rcb_dma + off);
        reg_set(rf, 141 + 2 * i, rcb[i]);
        off += rcb[i];
    }

    /* MPP set_registers(): current picture */
    s = &in->surfaces[cur];
    reg_set(rf, 168, s->dma);                   /* decout */
    reg_set(rf, 192, s->dma);                   /* payload_st_cur */
    reg_set(rf, 216, s->colmv_dma);             /* colmv_cur */
    reg_set(rf, 169, s->dma);                   /* error_ref, refined below */

    /* References: an unused entry repeats the nearest used one before it,
     * or the current picture. */
    for (i = 0; i < 16; i++) {
        unsigned int ref;

        if (!V383_PIC_UNUSED(pp->RefFrameList[i])) {
            ref = V383_PIC_INDEX(pp->RefFrameList[i]);
            near_index = ref;
            near_valid = 1;
        } else {
            ref = near_valid ? near_index : cur;
        }

        if (i < 15 && pp->FrameNumList[i] < pp->frame_num && pp->FrameNumList[i] > min_frame_num) {
            min_frame_num = pp->FrameNumList[i];
            reg_set(rf, 169, in->surfaces[ref].dma);
        }

        reg_set(rf, 170 + i, in->surfaces[ref].dma);
        reg_set(rf, 195 + i, in->surfaces[ref].dma);
        reg_set(rf, 217 + i, in->surfaces[ref].colmv_dma);
    }

    /* What MPP writes: ctrl_regs, comm_paras and comm_addrs, whole. */
    mask_range(rf, 8, 30);
    mask_range(rf, 64, 106);
    mask_range(rf, 128, 232);

    return V383_OK;
}
