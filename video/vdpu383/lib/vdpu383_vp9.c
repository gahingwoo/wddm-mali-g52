/*
 * vdpu383_vp9.c - DXVA VP9 picture -> VDPU383 register file
 *
 * The register values and the buffer handling are Rockchip MPP's
 * (Apache-2.0 OR MIT, used here under MIT), commit 9b56348:
 *
 *   mpp/hal/rkdec/vp9d/hal_vp9d_vdpu383.c     hal_vp9d_vdpu383_gen_regs(),
 *                                             vdpu383_vp9d_rcb_calc()
 *   mpp/hal/rkdec/common/vdpu383_com.c        vdpu383_init_ctrl_regs(VP9)
 *
 *   Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 *   SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * The bit layouts of the global record and of the probability tables are
 * those that MPP's mpp/hal/rkdec/vp9d/hal_vp9d_com.c writes
 * (vdpu38x_vp9d_uncomp_hdr(), hal_vp9d_prob_default(), hal_vp9d_prob_kf()).
 * That file is Apache-2.0 only; nothing of it is copied here. The packers
 * below were written from the list of fields, widths and alignments, and
 * the tables come from mainline Linux (vdpu383_vp9_probs.c).
 *
 * Differences from MPP, all deliberate:
 *
 * - DXVA's frame_refs[] are surface indices (MPP's parser puts reference
 *   slot numbers there); a reference's coded size is found through the
 *   ref_frame_map[] entry holding that surface.
 * - MPP rewrites the default table for every intra frame from its parser's
 *   probabilities, with tx, skip and coefficient probabilities forced to the
 *   defaults, and resets all four contexts on every intra frame. Here the
 *   default table is written once and the contexts follow the
 *   specification's setup_past_independence(): a key frame, an error
 *   resilient frame or reset_frame_context 3 resets all four, 2 resets the
 *   one frame_context_idx names, and the frame then starts from (and
 *   refreshes) context 0; an intra-only frame with reset 0 or 1 therefore
 *   starts from context 0 as a previous frame left it, and the other
 *   contexts survive it. V383_VP9_OPT_MPP_INTRA_RESET restores MPP's
 *   behaviour. Hosts that hand on frame_context_idx as 0 for intra frames
 *   (FFmpeg, Chromium) make reset 2 reset context 0.
 * - MPP points register 185 (where the decoder writes the adapted context)
 *   at context frame_context_idx whether or not the frame keeps it
 *   (refresh_frame_context 0). Here such a frame writes to a scratch table,
 *   so that a context is only ever changed by a frame that refreshes it;
 *   if the decoder honours the flag in the global record the two are the
 *   same. V383_VP9_OPT_MPP_UPDATE_BUF restores MPP's.
 * - The co-located motion vectors: two buffers used in turn, each frame
 *   reading the one the previous frame wrote (MPP: one per output slot,
 *   reading the previous frame's). The same buffers whatever surfaces the
 *   caller decodes into.
 * - A reference that is not a surface is the current picture (MPP's
 *   fallback); on an intra frame the references' sizes in the global record
 *   are 0 (MPP's are whatever its parser had last).
 * - Only 4:2:0, 8 or 10 bit, raster output, no scale-down output.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "vdpu383_vp9.h"

#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#define DIV_UP(x, d)    (((x) + (d) - 1) / (d))

static void v_zero(void *p, unsigned int n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--)
        *b++ = 0;
}

/* LSB-first bit writer (MPP's mpp_put_bits() order on a little-endian
 * CPU), with alignment to a multiple of bits from the buffer start. Writes
 * past the end are dropped, as MPP's are. */
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
            continue;
        if (i < 32 && ((value >> i) & 1))
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

static unsigned int is_intra(const V383_DXVA_PicParams_VP9 *pp)
{
    return !V383_VP9_FRAME_TYPE(pp) || V383_VP9_INTRA_ONLY(pp);
}

/* ------------------------------------------------------------- sizes */

unsigned int v383_vp9_stream_read_len(unsigned int stream_len)
{
    return ALIGN_UP(stream_len, 16u) + 0x80;
}

unsigned int v383_vp9_colmv_size(unsigned int width, unsigned int height)
{
    return ALIGN_UP(width * height / 2, 4096u);
}

/*
 * The RCB buffers: MPP vdpu383_vp9d_rcb_calc() with the picture as one tile
 * (in_tl_row = on_tl_row = width and on_tl_col = height, both aligned to
 * 64, as vdpu38x_vp9d_rcb_setup() sets them), 4:2:0 (intra_uv 2,
 * filter_row_uv 1, filter_col_uv 1) and the bit depth fixed at 10 as MPP
 * fixes it. Values are bits; rcb_bytes() is MPP_RCB_BYTES(). The float
 * products are exact for widths and heights that are multiples of 64:
 *   fltd row = w * (1.6 * 10 + 0.5) * (17.5 + 8) = w * 1683 / 4
 *   fltd col = h * 16.5 * (17.75 + 8)            = h * 3399 / 8
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

static void rcb_sizes(unsigned int width, unsigned int height, unsigned int size[RCB_COUNT])
{
    unsigned long long w = ALIGN_UP((unsigned long long)width, 64ull);
    unsigned long long h = ALIGN_UP((unsigned long long)height, 64ull);
    unsigned long long append = (w > 4096) ? 256 * 16 * 8 : 864 * 16 * 8;
    unsigned long long fltd = w * 1683 / 4;

    size[RCB_STRMD_IN_ROW] = 0;
    size[RCB_STRMD_ON_ROW] = (w > 4096) ? rcb_bytes(DIV_UP(w, 64ull) * 250) : 0;
    size[RCB_INTER_IN_ROW] = rcb_bytes(DIV_UP(w, 64ull) * 2368);
    size[RCB_INTER_ON_ROW] = size[RCB_INTER_IN_ROW];
    size[RCB_INTRA_IN_ROW] = rcb_bytes(ALIGN_UP(w * 24, 512ull));
    size[RCB_INTRA_ON_ROW] = size[RCB_INTRA_IN_ROW];
    size[RCB_FLTD_IN_ROW] = rcb_bytes(fltd / 2 + append);
    size[RCB_FLTD_PROT_IN_ROW] = size[RCB_FLTD_IN_ROW];
    size[RCB_FLTD_ON_ROW] = rcb_bytes(fltd);
    size[RCB_FLTD_ON_COL] = rcb_bytes(h * 3399 / 8);
    size[RCB_FLTD_UPSC_ON_COL] = 0;
}

unsigned int v383_vp9_rcb_size(unsigned int width, unsigned int height)
{
    unsigned int size[RCB_COUNT];
    unsigned int i, total = 0;

    rcb_sizes(width, height, size);
    for (i = 0; i < RCB_COUNT; i++)
        total += size[i];
    return total;
}

/* ----------------------------------------------------- probability tables */

/*
 * One probability table (default, frame context), 4864 bytes, in 128-bit
 * lines:
 *   partition, segment ids (0: the global record has them), skip, tx,
 *     is_inter                                          align 128
 *   y_mode, comp_mode, comp_ref, single_ref, inter_mode, interp_filter
 *                                                       align 128
 *   coefficients, intra then inter: per tx size and plane type the
 *     6 x 6 x 3 bytes in pieces of 27, each piece on a line of its own
 *   uv_mode by y mode in groups of 3, each group on a line; one zero line
 *   motion vectors: joint, sign, classes, class0, bits, class0_fr, fr,
 *     class0_hp, hp, both components of each in turn;   align, one zero line
 */
static void put_coef(BitPut *bp, const V383_Vp9Probs *p, unsigned int ref)
{
    unsigned int tx, pl, b, c, n, count;

    for (tx = 0; tx < 4; tx++)
        for (pl = 0; pl < 2; pl++) {
            count = 0;
            for (b = 0; b < 6; b++)
                for (c = 0; c < 6; c++)
                    for (n = 0; n < 3; n++) {
                        bp_put(bp, p->coef[tx][pl][ref][b][c][n], 8);
                        if (++count == 27) {
                            bp_align(bp, 128);
                            count = 0;
                        }
                    }
            bp_align(bp, 128);
        }
}

void v383_vp9_write_probs(const V383_Vp9Probs *p, unsigned char *out)
{
    BitPut bp;
    unsigned int i, j, k, c;

    bp_init(&bp, out, V383_VP9_PROB_BYTES);

    for (i = 0; i < 16; i++)
        for (j = 0; j < 3; j++)
            bp_put(&bp, p->partition[i][j], 8);
    bp_put(&bp, 0, 3 * 8);              /* segment id prediction */
    bp_put(&bp, 0, 7 * 8);              /* segment id tree */
    for (i = 0; i < 3; i++)
        bp_put(&bp, p->skip[i], 8);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 3; j++)
            bp_put(&bp, p->tx32[i][j], 8);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            bp_put(&bp, p->tx16[i][j], 8);
    for (i = 0; i < 2; i++)
        bp_put(&bp, p->tx8[i][0], 8);
    for (i = 0; i < 4; i++)
        bp_put(&bp, p->is_inter[i], 8);
    bp_align(&bp, 128);

    for (i = 0; i < 4; i++)
        for (j = 0; j < 9; j++)
            bp_put(&bp, p->y_mode[i][j], 8);
    for (i = 0; i < 5; i++)
        bp_put(&bp, p->comp_mode[i], 8);
    for (i = 0; i < 5; i++)
        bp_put(&bp, p->comp_ref[i], 8);
    for (i = 0; i < 5; i++)
        for (j = 0; j < 2; j++)
            bp_put(&bp, p->single_ref[i][j], 8);
    for (i = 0; i < 7; i++)
        for (j = 0; j < 3; j++)
            bp_put(&bp, p->inter_mode[i][j], 8);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 2; j++)
            bp_put(&bp, p->interp_filter[i][j], 8);
    bp_align(&bp, 128);

    put_coef(&bp, p, 0);
    put_coef(&bp, p, 1);

    for (i = 0; i < 10; i++) {
        for (j = 0; j < 9; j++)
            bp_put(&bp, p->uv_mode[i][j], 8);
        if (i == 2 || i == 5 || i == 8 || i == 9)
            bp_align(&bp, 128);
    }
    bp_put(&bp, 0, 8);
    bp_align(&bp, 128);

    for (i = 0; i < 3; i++)
        bp_put(&bp, p->mv.joint[i], 8);
    for (c = 0; c < 2; c++)
        bp_put(&bp, p->mv.sign[c], 8);
    for (c = 0; c < 2; c++)
        for (j = 0; j < 10; j++)
            bp_put(&bp, p->mv.classes[c][j], 8);
    for (c = 0; c < 2; c++)
        bp_put(&bp, p->mv.class0_bit[c], 8);
    for (c = 0; c < 2; c++)
        for (j = 0; j < 10; j++)
            bp_put(&bp, p->mv.bits[c][j], 8);
    for (c = 0; c < 2; c++)
        for (j = 0; j < 2; j++)
            for (k = 0; k < 3; k++)
                bp_put(&bp, p->mv.class0_fr[c][j][k], 8);
    for (c = 0; c < 2; c++)
        for (j = 0; j < 3; j++)
            bp_put(&bp, p->mv.fr[c][j], 8);
    for (c = 0; c < 2; c++)
        bp_put(&bp, p->mv.class0_hp[c], 8);
    for (c = 0; c < 2; c++)
        bp_put(&bp, p->mv.hp[c], 8);
    bp_align(&bp, 128);
    bp_put(&bp, 0, 8);
    bp_align(&bp, 128);
}

/*
 * The key-frame mode tables, 1312 bytes: the 48 partition probabilities,
 * then per above mode the 10 x 9 y mode probabilities in pieces of 27 (each
 * piece but the last on a line of its own) followed by 23 bytes of the uv
 * mode table (the 90 bytes of v383_vp9_kf_uv_mode_prob, flat, over the
 * first four above modes; zero after), and a line boundary. The last
 * line falls outside the 1312 bytes and holds only zeros.
 */
void v383_vp9_write_kf_probs(unsigned char *out)
{
    const unsigned char *uv = &v383_vp9_kf_uv_mode_prob[0][0];
    BitPut bp;
    unsigned int i, j, k, m, count;

    bp_init(&bp, out, V383_VP9_KFPROB_BYTES);
    for (i = 0; i < 16; i++)
        for (j = 0; j < 3; j++)
            bp_put(&bp, v383_vp9_kf_partition_probs[i][j], 8);
    for (i = 0; i < 10; i++) {
        count = 0;
        for (j = 0; j < 10; j++)
            for (k = 0; k < 9; k++) {
                bp_put(&bp, v383_vp9_kf_y_mode_prob[i][j][k], 8);
                if (++count == 27) {
                    count = 0;
                    bp_align(&bp, 128);
                }
            }
        for (m = 0; m < 23; m++)
            bp_put(&bp, (i < 4 && i * 23 + m < 90) ? uv[i * 23 + m] : 0, 8);
        bp_align(&bp, 128);
    }
}

/* ------------------------------------------------------- global record */

/* ref_frame_map[] slot holding frame_refs[i]'s surface, or -1. */
static int ref_slot(const V383_DXVA_PicParams_VP9 *pp, unsigned int i)
{
    unsigned int s;

    if (V383_VP9_NO_SURFACE(pp->frame_refs[i]))
        return -1;
    for (s = 0; s < 8; s++)
        if (!V383_VP9_NO_SURFACE(pp->ref_frame_map[s]) &&
            V383_PIC_INDEX(pp->ref_frame_map[s]) == V383_PIC_INDEX(pp->frame_refs[i]))
            return (int)s;
    return -1;
}

static void ref_size(const V383_DXVA_PicParams_VP9 *pp, unsigned int i, unsigned int *w, unsigned int *h)
{
    int s = ref_slot(pp, i);

    if (is_intra(pp)) {
        *w = *h = 0;
    } else if (s < 0 || !pp->ref_frame_coded_width[s] || !pp->ref_frame_coded_height[s]) {
        *w = pp->width;
        *h = pp->height;
    } else {
        *w = pp->ref_frame_coded_width[s];
        *h = pp->ref_frame_coded_height[s];
    }
}

/* MPP's parser's use_last_frame_mvs, from the decoder's own history. */
static unsigned int use_prev_mvs(const V383_DXVA_PicParams_VP9 *pp, const V383_Vp9State *s)
{
    return !V383_VP9_ERROR_RESILIENT(pp) && pp->width == s->last_width && pp->height == s->last_height &&
           !s->last_intra_only && s->last_show_frame;
}

/*
 * The 352-byte global record (register 131), fields in order, LSB first:
 *   frame_type 1, error_resilient 1, bit depth - 8 3, chroma format 2 (1),
 *   width 16, height 16, intra 1, sign bias LAST/GOLDEN/ALTREF 1+1+1,
 *   allow_high_precision_mv 1, interp_filter 3 (0 on intra frames),
 *   frame_parallel 1, refresh_frame_context 1,
 *   filter_level 6, sharpness 3, delta enabled 1, delta update 1,
 *   ref_deltas 4 x 7, mode_deltas 2 x 7,
 *   base_q 8, y_dc 5, uv_dc 5, uv_ac 5, lossless 1,
 *   segment pred probs 3 x 8, tree probs 7 x 8, enabled, update_map,
 *   temporal_update, abs_delta 1 each, use_prev_frame_mvs 1,
 *   feature enables 8 x 4 x 1, feature data 8 x (Q 9, LF 7, REF 2),
 *   first_partition_size 16, reference sizes 3 x (16, 16),
 *   previous frame: mode_deltas 2 x 7, ref_deltas 4 x 7, segmentation 1,
 *   show_frame 1; intra_only 1, same size as previous 1, previous colour
 *   space 3 (0, as MPP), previous was a key frame 1, 0, 1, 1, 1,
 *   motion vector scale 3 x (x 16, y 16) (Q14; 16 on intra frames),
 *   tile columns 7, tile rows 3, tile column widths 64 x 10 and row
 *   heights 4 x 10 (superblocks), zero to a 64-bit boundary.
 */
void v383_vp9_write_gbl(const V383_DXVA_PicParams_VP9 *pp, const V383_Vp9State *s, unsigned char *out)
{
    BitPut bp;
    unsigned int i, j, intra = is_intra(pp);
    unsigned int rw[3], rh[3];
    unsigned int sb_cols = ALIGN_UP(pp->width, 64u) / 64, sb_rows = ALIGN_UP(pp->height, 64u) / 64;
    unsigned int log2c = pp->log2_tile_cols, log2r = pp->log2_tile_rows;

    for (i = 0; i < 3; i++)
        ref_size(pp, i, &rw[i], &rh[i]);

    bp_init(&bp, out, V383_VP9_GBL_BYTES);
    bp_put(&bp, V383_VP9_FRAME_TYPE(pp), 1);
    bp_put(&bp, V383_VP9_ERROR_RESILIENT(pp), 1);
    bp_put(&bp, pp->BitDepthMinus8Luma, 3);
    bp_put(&bp, 1, 2);                                  /* 4:2:0 */
    bp_put(&bp, pp->width, 16);
    bp_put(&bp, pp->height, 16);
    bp_put(&bp, intra, 1);
    bp_put(&bp, (unsigned int)pp->ref_frame_sign_bias[1], 1);
    bp_put(&bp, (unsigned int)pp->ref_frame_sign_bias[2], 1);
    bp_put(&bp, (unsigned int)pp->ref_frame_sign_bias[3], 1);
    bp_put(&bp, V383_VP9_ALLOW_HP_MV(pp), 1);
    bp_put(&bp, intra ? 0 : pp->interp_filter, 3);
    bp_put(&bp, V383_VP9_ERROR_RESILIENT(pp) ? 1 : V383_VP9_PARALLEL_MODE(pp), 1);
    bp_put(&bp, V383_VP9_ERROR_RESILIENT(pp) ? 0 : V383_VP9_REFRESH_CTX(pp), 1);

    bp_put(&bp, (unsigned int)pp->filter_level, 6);
    bp_put(&bp, (unsigned int)pp->sharpness_level, 3);
    bp_put(&bp, V383_VP9_LF_DELTA_ENABLED(pp), 1);
    bp_put(&bp, V383_VP9_LF_DELTA_UPDATE(pp), 1);
    for (i = 0; i < 4; i++)
        bp_put(&bp, (unsigned int)pp->ref_deltas[i], 7);
    for (i = 0; i < 2; i++)
        bp_put(&bp, (unsigned int)pp->mode_deltas[i], 7);

    bp_put(&bp, (unsigned int)pp->base_qindex, 8);
    bp_put(&bp, (unsigned int)pp->y_dc_delta_q, 5);
    bp_put(&bp, (unsigned int)pp->uv_dc_delta_q, 5);
    bp_put(&bp, (unsigned int)pp->uv_ac_delta_q, 5);
    bp_put(&bp, !pp->base_qindex && !pp->y_dc_delta_q && !pp->uv_dc_delta_q && !pp->uv_ac_delta_q, 1);

    for (i = 0; i < 3; i++)
        bp_put(&bp, pp->stVP9Segments.pred_probs[i], 8);
    for (i = 0; i < 7; i++)
        bp_put(&bp, pp->stVP9Segments.tree_probs[i], 8);
    bp_put(&bp, V383_VP9_SEG_ENABLED(pp), 1);
    bp_put(&bp, V383_VP9_SEG_UPDATE_MAP(pp), 1);
    bp_put(&bp, V383_VP9_SEG_TEMPORAL(pp), 1);
    bp_put(&bp, V383_VP9_SEG_ABS_DELTA(pp), 1);
    bp_put(&bp, use_prev_mvs(pp, s), 1);
    for (i = 0; i < 8; i++)
        for (j = 0; j < 4; j++)
            bp_put(&bp, (pp->stVP9Segments.feature_mask[i] >> j) & 1, 1);
    for (i = 0; i < 8; i++) {
        bp_put(&bp, (unsigned int)pp->stVP9Segments.feature_data[i][0], 9);
        bp_put(&bp, (unsigned int)pp->stVP9Segments.feature_data[i][1], 7);
        bp_put(&bp, (unsigned int)pp->stVP9Segments.feature_data[i][2], 2);
    }

    bp_put(&bp, pp->first_partition_size, 16);
    for (i = 0; i < 3; i++) {
        bp_put(&bp, rw[i], 16);
        bp_put(&bp, rh[i], 16);
    }

    bp_put(&bp, (unsigned int)s->last_mode_deltas[0], 7);
    bp_put(&bp, (unsigned int)s->last_mode_deltas[1], 7);
    for (i = 0; i < 4; i++)
        bp_put(&bp, (unsigned int)s->last_ref_deltas[i], 7);
    bp_put(&bp, s->last_seg_enabled, 1);
    bp_put(&bp, s->last_show_frame, 1);
    bp_put(&bp, V383_VP9_INTRA_ONLY(pp), 1);
    bp_put(&bp, pp->width == s->last_width && pp->height == s->last_height, 1);
    bp_put(&bp, 0, 3);                                  /* previous colour space */
    bp_put(&bp, !s->last_frame_type, 1);
    bp_put(&bp, 0, 1);
    bp_put(&bp, 1, 1);
    bp_put(&bp, 1, 1);
    bp_put(&bp, 1, 1);

    for (i = 0; i < 3; i++) {
        unsigned int sx = 16, sy = 16;

        if (!intra && pp->width && pp->height) {
            sx = (rw[i] << 14) / pp->width;
            sy = (rh[i] << 14) / pp->height;
        }
        bp_put(&bp, sx, 16);
        bp_put(&bp, sy, 16);
    }

    bp_put(&bp, 1u << log2c, 7);
    bp_put(&bp, 1u << log2r, 3);
    for (i = 0; i < 64; i++) {
        unsigned int w = 0;

        if (i < (1u << log2c)) {
            unsigned int start = (i * sb_cols) >> log2c, end = ((i + 1) * sb_cols) >> log2c;
            if (start > sb_cols)
                start = sb_cols;
            if (end > sb_cols)
                end = sb_cols;
            w = end - start;
        }
        bp_put(&bp, w, 10);
    }
    for (j = 0; j < 4; j++) {
        unsigned int h = 0;

        if (j < (1u << log2r)) {
            unsigned int start = (j * sb_rows) >> log2r, end = ((j + 1) * sb_rows) >> log2r;
            if (start > sb_rows)
                start = sb_rows;
            if (end > sb_rows)
                end = sb_rows;
            h = end - start;
        }
        bp_put(&bp, h, 10);
    }
    bp_align(&bp, 64);
}

/* ------------------------------------------------------------- state */

void v383_vp9_reset(V383_Vp9State *s)
{
    v_zero(s, sizeof(*s));
    s->segid_flag = 1;              /* MPP hw_ctx->last_segid_flag = 1 */
}

/* ------------------------------------------------------------- build */

int v383_vp9_build(const V383_Vp9Input *in, V383_RegFile *rf)
{
    const V383_DXVA_PicParams_VP9 *pp = in->pp;
    V383_Vp9State *s = in->state;
    unsigned int rcb[RCB_COUNT];
    unsigned int cur, i, off, ctx, intra, uh, refresh, last_prob, ref[3];
    unsigned int y_size, colmv_cur;

    if (!pp || !s || !in->aux_cpu || !in->surfaces || !rf)
        return V383_E_INVALID;
    if (pp->profile & 1)                        /* 4:2:2, 4:4:0, 4:4:4 */
        return V383_E_UNSUPPORTED;
    if (pp->BitDepthMinus8Luma > 2 || pp->BitDepthMinus8Luma != pp->BitDepthMinus8Chroma ||
        (pp->BitDepthMinus8Luma & 1))
        return V383_E_UNSUPPORTED;
    if (!pp->width || !pp->height || pp->width > 8192 || pp->height > 8192)
        return V383_E_INVALID;
    if (pp->log2_tile_cols > 6 || pp->log2_tile_rows > 2)
        return V383_E_INVALID;
    cur = V383_PIC_INDEX(pp->CurrPic);
    if (cur >= in->num_surfaces)
        return V383_E_INVALID;
    if ((in->hor_stride & 15) || (in->ver_stride & 7))
        return V383_E_INVALID;
    uh = pp->uncompressed_header_size_byte_aligned;
    if (!uh || uh >= in->stream_len)
        return V383_E_STREAM;

    intra = is_intra(pp);
    ctx = V383_VP9_FRAME_CTX_IDX(pp);
    refresh = V383_VP9_ERROR_RESILIENT(pp) ? 0 : V383_VP9_REFRESH_CTX(pp);
    for (i = 0; i < 3; i++) {
        ref[i] = cur;
        if (!intra && !V383_VP9_NO_SURFACE(pp->frame_refs[i]) &&
            V383_PIC_INDEX(pp->frame_refs[i]) < in->num_surfaces)
            ref[i] = V383_PIC_INDEX(pp->frame_refs[i]);
    }

    /* ---- auxiliary buffer */
    if (!s->tables_written) {
        v383_vp9_write_kf_probs(in->aux_cpu + V383_VP9_AUX_KFPROB_OFF);
        v383_vp9_write_probs(&v383_vp9_default_probs, in->aux_cpu + V383_VP9_AUX_DEFPROB_OFF);
        s->tables_written = 1;
    }
    v383_vp9_write_gbl(pp, s, in->aux_cpu + V383_VP9_AUX_GBL_OFF);

    /* Which probabilities the frame starts from and where the adapted
     * ones go (setup_past_independence(): a reset by the coded index, then
     * context 0 for an intra or error resilient frame) */
    if (intra || V383_VP9_ERROR_RESILIENT(pp)) {
        unsigned int reset = V383_VP9_RESET_CTX(pp);

        if (!V383_VP9_FRAME_TYPE(pp) || V383_VP9_ERROR_RESILIENT(pp) || reset == 3 ||
            (in->options & V383_VP9_OPT_MPP_INTRA_RESET)) {
            for (i = 0; i < 4; i++)
                s->ctx_valid[i] = 0;
        } else if (reset == 2) {
            s->ctx_valid[ctx] = 0;
        }
        ctx = 0;
    }
    if (s->ctx_valid[ctx]) {
        last_prob = V383_VP9_AUX_CTX_OFF(ctx);
    } else {
        last_prob = V383_VP9_AUX_DEFPROB_OFF;
        if (refresh)
            s->ctx_valid[ctx] = 1;
    }

    /* ---- registers */
    v_zero(rf, sizeof(*rf));

    /* vdpu383_init_ctrl_regs(MPP_VIDEO_CodingVP9) */
    reg_set(rf, 8, 2);                          /* dec_mode: VP9 */
    reg_set(rf, 9, 0);                          /* raster, no FBC, no scale-down, buf_empty_en 0 */
    reg_set(rf, 10, 0x3FF);                     /* every block auto-gating enable */
    reg_set(rf, 13, 0xFFFFFF);                  /* core timeout */
    reg_set(rf, 16, 1);                         /* error_proc_disable */
    reg_set(rf, 20, 0xFFFFFFDF);                /* cabac error enable, low */
    reg_set(rf, 21, 0x3FFFFFFF);                /* cabac error enable, high */
    /* vdpu383_setup_statistic() */
    reg_set(rf, 28, 1u | (1u << 3) | (11u << 4));
    reg_set(rf, 29, 1u | (1u << 3) | (17u << 4));
    reg_set(rf, 30, 0);
    rf->core_timeout = rf->regs[13];

    /* The decoder starts at the compressed header: base at its 16-byte
     * boundary, the rest as a start bit. */
    reg_set(rf, 65, 8 * (uh & 15));
    reg_set(rf, 66, v383_vp9_stream_read_len(in->stream_len));
    reg_set(rf, 67, V383_VP9_GBL_LEN);

    /* vdpu383_setup_cur_stride_info(), raster */
    y_size = in->hor_stride * in->ver_stride;
    reg_set(rf, 68, in->hor_stride >> 4);
    reg_set(rf, 69, in->hor_stride >> 4);
    reg_set(rf, 70, y_size >> 4);
    reg_set(rf, 80, rf->regs[68]);
    reg_set(rf, 81, rf->regs[69]);
    reg_set(rf, 82, rf->regs[70]);
    for (i = 0; i < 3; i++) {                   /* the references' strides: the same surfaces */
        reg_set(rf, 83 + 3 * i, in->hor_stride >> 4);
        reg_set(rf, 84 + 3 * i, in->hor_stride >> 4);
        reg_set(rf, 85 + 3 * i, y_size >> 4);
    }

    reg_set(rf, 128, in->stream_dma + (uh & ~15u));
    reg_set(rf, 131, in->aux_dma + V383_VP9_AUX_GBL_OFF);

    rcb_sizes(pp->width, pp->height, rcb);
    off = 0;
    for (i = 0; i < RCB_COUNT; i++) {
        reg_set(rf, 140 + 2 * i, in->rcb_dma + off);
        reg_set(rf, 141 + 2 * i, rcb[i]);
        off += rcb[i];
    }

    reg_set(rf, 168, in->surfaces[cur].dma);    /* output */
    reg_set(rf, 169, in->surfaces[cur].dma);    /* error reference */
    for (i = 0; i < 3; i++) {
        reg_set(rf, 170 + i, in->surfaces[ref[i]].dma);
        reg_set(rf, 195 + i, in->surfaces[ref[i]].dma);
    }
    /* segment maps: the previous one and the one this frame writes */
    reg_set(rf, 181, in->aux_dma + V383_VP9_AUX_SEGID_OFF(s->segid_flag ? 0 : 1));
    reg_set(rf, 182, in->aux_dma + V383_VP9_AUX_SEGID_OFF(s->segid_flag ? 1 : 0));
    reg_set(rf, 183, in->aux_dma + V383_VP9_AUX_KFPROB_OFF);
    reg_set(rf, 184, in->aux_dma + last_prob);
    reg_set(rf, 185, in->aux_dma + ((refresh || (in->options & V383_VP9_OPT_MPP_UPDATE_BUF)) ?
                                    V383_VP9_AUX_CTX_OFF(ctx) : V383_VP9_AUX_SCRATCH_OFF));
    reg_set(rf, 192, in->surfaces[cur].dma);
    reg_set(rf, 194, in->surfaces[cur].dma);

    /* co-located motion vectors: write one buffer, read the other (the
     * previous frame's; the first frame reads its own, as MPP's does) */
    colmv_cur = s->frames ? !s->colmv_last : 0;
    reg_set(rf, 216, in->colmv_dma[colmv_cur]);
    reg_set(rf, 217, in->colmv_dma[s->frames ? s->colmv_last : colmv_cur]);

    mask_range(rf, 8, 30);
    mask_range(rf, 64, 106);
    mask_range(rf, 128, 232);

    /* ---- what the next frame needs to know of this one (MPP ls_info) */
    if ((V383_VP9_SEG_ENABLED(pp) && V383_VP9_SEG_UPDATE_MAP(pp)) ||
        pp->width != s->last_width || pp->height != s->last_height || intra || V383_VP9_ERROR_RESILIENT(pp))
        s->segid_flag = !s->segid_flag;
    s->colmv_last = (unsigned char)colmv_cur;
    for (i = 0; i < 4; i++)
        s->last_ref_deltas[i] = pp->ref_deltas[i];
    for (i = 0; i < 2; i++)
        s->last_mode_deltas[i] = pp->mode_deltas[i];
    if (!s->last_seg_enabled)
        s->last_seg_enabled = (unsigned char)V383_VP9_SEG_ENABLED(pp);
    s->last_show_frame = (unsigned char)V383_VP9_SHOW_FRAME(pp);
    s->last_width = pp->width;
    s->last_height = pp->height;
    s->last_frame_type = (unsigned char)V383_VP9_FRAME_TYPE(pp);
    s->last_intra_only = (unsigned char)intra;
    if (s->frames < 255)
        s->frames++;
    return V383_OK;
}
