/*
 * vdpu383_av1.c - DXVA AV1 picture -> VDPU383 register file
 *
 * Ported from Rockchip MPP (Apache-2.0 OR MIT, used here under MIT),
 * commit 9b56348:
 *
 *   mpp/hal/rkdec/av1d/hal_av1d_vdpu383.c     vdpu383_av1d_gen_regs(),
 *                                             vdpu383_av1d_rcb_calc()
 *   mpp/hal/rkdec/av1d/hal_av1d_com.c         vdpu38x_av1d_uncomp_hdr(),
 *                                             vdpu38x_av1d_set_cdf_segid(),
 *                                             vdpu38x_av1d_cdf_setup(),
 *                                             vdpu38x_av1d_colmv_setup()
 *   mpp/codec/dec/av1/av1d_parser.c,
 *   av1d_codec.c                              the fields MPP's parser adds
 *                                             to DXVA_PicParams_AV1 (skip
 *                                             mode references, lossless
 *                                             flags, coded global motion
 *                                             parameters, slot state)
 *
 *   Copyright (c) 2024-2026 Rockchip Electronics Co., Ltd.
 *   SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * The global record keeps MPP's choices where they look accidental, since
 * those are what the decoder has been seen to decode with: the yuv format
 * test reads subsampling_y twice, use_prev_segmentation_ids compares the
 * frame with slot 0 (not LAST) and takes any primary_ref_frame other than 0,
 * the reference scale factors are indexed by slot, superres_denom is the
 * coded 3-bit value, delta_q_res and delta_lf_res are written as MPP writes
 * them.
 *
 * Differences from MPP, deliberate:
 *
 * - DXVA gives surfaces, not slots: a reference's slot is the first slot of
 *   RefFrameMapTextureIndex holding its surface (slots holding the same
 *   surface hold the same frame, so any of them gives the same record).
 * - What MPP keeps per slot (CDF valid, coefficient class) is kept per
 *   surface, the CDF buffer being the surface's. A frame with
 *   disable_frame_end_update_cdf leaves its surface marked as holding the
 *   default coefficient class it read (MPP leaves the slot's earlier valid
 *   flag, which then points at a buffer nothing adapted).
 * - matrix_coefficients is not in DXVA: from V383_Av1Extra, else 0 for
 *   matrix_coeff_is_identity and 2 (unspecified) otherwise.
 * - Film grain is handed to the decoder as DXVA states it (MPP enables
 *   grain on the decoder's output since 9b56348); not checked on the board.
 * - Only 4:2:0, 8 or 10 bit, raster output, no scale-down output.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "vdpu383_av1.h"

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

/* LSB-first bit writer (mpp_put_bits() order on a little-endian CPU). */
typedef struct BitPut {
    unsigned char *buf;
    unsigned int   size;
    unsigned int   pos;
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

static unsigned int frame_is_intra(const V383_DXVA_PicParams_AV1 *pp)
{
    return V383_AV1_FRAME_TYPE(pp) == V383_AV1_KEY_FRAME || V383_AV1_FRAME_TYPE(pp) == V383_AV1_INTRA_ONLY_FRAME;
}

/* ------------------------------------------------------------- sizes */

static unsigned int coded_width(const V383_DXVA_PicParams_AV1 *pp)
{
    unsigned int d = pp->superres_denom;

    if (!V383_AV1_SUPERRES(pp) || d <= 8)
        return pp->width;
    return (pp->width * 8 + d / 2) / d;
}

static unsigned int segid_size(unsigned int w, unsigned int h)
{
    return DIV_UP(w, 128u) * DIV_UP(h, 128u) * 32 * 16;
}

unsigned int v383_av1_cdf_size(unsigned int width, unsigned int height)
{
    return ALIGN_UP(V383_AV1_ALL_CDF_BYTES + segid_size(width, height), 4096u);
}

unsigned int v383_av1_aux_size(unsigned int width, unsigned int height)
{
    return ALIGN_UP(V383_AV1_AUX_DEFCDF_OFF + V383_AV1_ALL_CDF_BYTES + segid_size(width, height), 4096u);
}

unsigned int v383_av1_colmv_size(unsigned int width, unsigned int height)
{
    return ALIGN_UP(DIV_UP(width, 64u) * DIV_UP(height, 64u) * 1024, 4096u);
}

unsigned int v383_av1_stream_read_len(unsigned int stream_len)
{
    return ALIGN_UP(stream_len + 15, 128u);
}

/*
 * The RCB buffers: MPP vdpu383_av1d_rcb_calc() with the frame as one tile
 * (width and height of the frame as vdpu38x_av1d_rcb_setup() gives them),
 * 4:2:0, bit depth fixed at 10 as MPP fixes it. The float products, exact
 * for 64-aligned sizes:
 *   fltd row = w * 16.5 * (12.5 + 6 + 1.5 lr)                 = w * 33 * (37 + 3 lr) / 4
 *   fltd col = h * 16.5 * (14 + 7 + 26.5 lr + (upsc ? 15.5 : 6))
 *                                              = h * 33 * (42 + 53 lr + (upsc ? 31 : 12)) / 4
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

static unsigned int uses_lr(const V383_DXVA_PicParams_AV1 *pp)
{
    unsigned int i, u = 0;

    for (i = 0; i < (V383_AV1_MONO_CHROME(pp) ? 1u : 3u); i++)
        u |= pp->loop_filter.frame_restoration_type[i] != 0;
    return u;
}

static void rcb_sizes(const V383_DXVA_PicParams_AV1 *pp, unsigned int size[RCB_COUNT])
{
    unsigned long long w = pp->width, h = pp->height;
    unsigned long long w64 = ALIGN_UP(w, 64ull), h64 = ALIGN_UP(h, 64ull);
    unsigned long long append = (w > 4096) ? 256 * 16 * 8 : 864 * 16 * 8;
    unsigned long long lr = uses_lr(pp), up = V383_AV1_SUPERRES(pp) ? 1 : 0;
    unsigned long long fltd = w64 * 33 * (37 + 3 * lr) / 4;

    size[RCB_STRMD_IN_ROW] = rcb_bytes(DIV_UP(w, 8ull) * 100);
    size[RCB_STRMD_ON_ROW] = 0;
    size[RCB_INTER_IN_ROW] = rcb_bytes(DIV_UP(w, 64ull) * 2752);
    size[RCB_INTER_ON_ROW] = size[RCB_INTER_IN_ROW];
    size[RCB_INTRA_IN_ROW] = rcb_bytes(ALIGN_UP(w * 24, 512ull));
    size[RCB_INTRA_ON_ROW] = size[RCB_INTRA_IN_ROW];
    size[RCB_FLTD_IN_ROW] = rcb_bytes(fltd / 2 + append);
    size[RCB_FLTD_PROT_IN_ROW] = size[RCB_FLTD_IN_ROW];
    size[RCB_FLTD_ON_ROW] = rcb_bytes(fltd);
    size[RCB_FLTD_ON_COL] = rcb_bytes(h64 * 33 * (42 + 53 * lr + (up ? 31 : 12)) / 4);
    size[RCB_FLTD_UPSC_ON_COL] = rcb_bytes(DIV_UP(h, 64ull) * 10 * 22);
}

unsigned int v383_av1_rcb_size(const V383_DXVA_PicParams_AV1 *pp)
{
    unsigned int size[RCB_COUNT], i, total = 0;

    rcb_sizes(pp, size);
    for (i = 0; i < RCB_COUNT; i++)
        total += size[i];
    return total;
}

/* ------------------------------------------------------------- derive */

static int relative_dist(unsigned int bits, int a, int b)
{
    int diff, m;

    if (!bits)
        return 0;
    diff = a - b;
    m = 1 << (bits - 1);
    return (diff & (m - 1)) - (diff & m);
}

static unsigned int ceil_log2(unsigned int v)
{
    unsigned int k = 0;

    while ((1u << k) < v)
        k++;
    return k;
}

/* The coded parameter of global motion parameter idx (MPP wmmat_val). */
static int wm_val(int wmmat, unsigned int type, unsigned int idx, unsigned int hp)
{
    unsigned int prec = 15, diff;
    int round = (idx % 3 == 2) ? (1 << 16) : 0;

    if (idx < 2)
        prec = (type == 1) ? 3 - !hp : 6;
    diff = 16 - prec;
    return (wmmat - round) >> diff;     /* exact: wmmat = (val << diff) + round */
}

void v383_av1_derive(const V383_DXVA_PicParams_AV1 *pp, const V383_Av1Extra *ex, const V383_Av1State *s,
                     V383_Av1Hdr *h)
{
    unsigned int i, j, bits = pp->order_hint_bits, intra = frame_is_intra(pp);
    int seg_lossless_all = 1;

    v_zero(h, sizeof(*h));
    h->pp = pp;
    h->upscaled_width = pp->width;
    h->frame_width = coded_width(pp);
    h->coded_denom = (V383_AV1_SUPERRES(pp) && pp->superres_denom > 8) ? pp->superres_denom - 9u : 8u;

    for (i = 0; i < 8; i++) {
        unsigned int sf = pp->RefFrameMapTextureIndex[i];

        h->slot_surface[i] = 0xFF;
        if (sf < V383_AV1_MAX_SURFACES && s->surf[sf].valid) {
            const V383_Av1SurfState *st = &s->surf[sf];

            h->slot_surface[i] = sf;
            h->slot_order_hint[i] = st->order_hint;
            for (j = 0; j < 7; j++)
                h->slot_ref_hints[i][j] = st->ref_hints[j];
            h->slot_width[i] = st->width;
            h->slot_upscaled_width[i] = st->upscaled_width;
            h->slot_height[i] = st->height;
            h->slot_frame_type[i] = st->frame_type;
        }
    }

    h->ref_frame_valued = !intra;
    for (i = 0; i < 7; i++) {
        h->ref_frame_idx[i] = 0;
        if (ex && ex->valid) {
            h->ref_frame_idx[i] = ex->ref_frame_idx[i] & 7;
        } else if (!intra) {
            for (j = 0; j < 8; j++)
                if (pp->frame_refs[i].Index != V383_AV1_NO_SURFACE &&
                    pp->RefFrameMapTextureIndex[j] == pp->frame_refs[i].Index) {
                    h->ref_frame_idx[i] = j;
                    break;
                }
        }
    }

    if (ex && ex->valid) {
        h->operating_point_idc = ex->operating_point_idc;
        h->matrix_coefficients = ex->matrix_coefficients;
        h->frame_header_bits = ex->frame_header_bits;
        h->tile_size_bytes = ex->tile_size_bytes ? ex->tile_size_bytes : 4;
        h->tile_cols_log2 = ex->tile_cols_log2;
        h->tile_rows_log2 = ex->tile_rows_log2;
    } else {
        h->matrix_coefficients = V383_AV1_FG_MATRIX_IDENTITY(pp) ? 0 : 2;
        h->tile_size_bytes = 4;
        h->tile_cols_log2 = ceil_log2(pp->tiles.cols);
        h->tile_rows_log2 = ceil_log2(pp->tiles.rows);
    }

    /* skip mode references (MPP read_skip_mode_params(), spec 5.9.22) */
    if (!intra && V383_AV1_REFERENCE_MODE(pp) && bits) {
        int fwd = -1, bwd = -1, fwd_hint = -1, bwd_hint = -1, cur = pp->order_hint;

        for (i = 0; i < 7; i++) {
            int rh = (int)h->slot_order_hint[h->ref_frame_idx[i]];
            int d = relative_dist(bits, rh, cur);

            if (d < 0) {
                if (fwd < 0 || relative_dist(bits, rh, fwd_hint) > 0) {
                    fwd = (int)i;
                    fwd_hint = rh;
                }
            } else if (d > 0) {
                if (bwd < 0 || relative_dist(bits, rh, bwd_hint) < 0) {
                    bwd = (int)i;
                    bwd_hint = rh;
                }
            }
        }
        if (fwd >= 0 && bwd >= 0) {
            h->skip_ref0 = (unsigned int)((fwd < bwd ? fwd : bwd) + 1);
            h->skip_ref1 = (unsigned int)((fwd > bwd ? fwd : bwd) + 1);
        } else if (fwd >= 0) {
            int fwd2 = -1, fwd2_hint = -1;

            for (i = 0; i < 7; i++) {
                int rh = (int)h->slot_order_hint[h->ref_frame_idx[i]];
                if (relative_dist(bits, rh, fwd_hint) < 0 &&
                    (fwd2 < 0 || relative_dist(bits, rh, fwd2_hint) > 0)) {
                    fwd2 = (int)i;
                    fwd2_hint = rh;
                }
            }
            if (fwd2 >= 0) {
                h->skip_ref0 = (unsigned int)((fwd < fwd2 ? fwd : fwd2) + 1);
                h->skip_ref1 = (unsigned int)((fwd > fwd2 ? fwd : fwd2) + 1);
            }
        }
    }

    /* lossless (spec 5.9.2 / MPP set_current_coded_lossless_param) */
    for (i = 0; i < 8; i++) {
        int q = pp->quantization.base_qindex;
        int lossless;

        if (V383_AV1_SEG_ENABLED(pp) && (pp->segmentation.feature_mask[i] & 1))
            q += pp->segmentation.feature_data[i][0];
        if (q < 0)
            q = 0;
        if (q > 255)
            q = 255;
        lossless = !q && !pp->quantization.y_dc_delta_q && !pp->quantization.u_dc_delta_q &&
                   !pp->quantization.v_dc_delta_q && !pp->quantization.u_ac_delta_q &&
                   !pp->quantization.v_ac_delta_q;
        if (!lossless)
            seg_lossless_all = 0;
        if (!V383_AV1_SEG_ENABLED(pp))
            break;
    }
    h->coded_lossless = (unsigned int)seg_lossless_all;
    h->all_lossless = h->coded_lossless && h->frame_width == h->upscaled_width;

    /* SegIdPreSkip, LastActiveSegId */
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            if (pp->segmentation.feature_mask[i] & (1u << j)) {
                h->last_active = i;
                if (j >= 5)
                    h->preskip = 1;
            }

    h->using_qmatrix = pp->quantization.qm_y != 0xFF;

    for (i = 0; i < 7; i++) {
        unsigned int type = V383_AV1_WM_TYPE(pp->frame_refs[i]);

        for (j = 0; j < 6; j++) {
            unsigned int coded = (type >= 1 && j < 2) || (type >= 2 && (j == 2 || j == 3)) ||
                                 (type == 3 && j >= 4);
            h->wmmat_val[i][j] = coded ? wm_val(pp->frame_refs[i].wmmat[j], type, j,
                                                V383_AV1_HIGH_PRECISION_MV(pp)) : 0;
        }
    }
}

/* ------------------------------------------------------ global record */

void v383_av1_write_gbl(const V383_Av1Hdr *h, unsigned char *out)
{
    const V383_DXVA_PicParams_AV1 *pp = h->pp;
    BitPut bp;
    unsigned int i, j, intra = frame_is_intra(pp), fw = h->frame_width;

    bp_init(&bp, out, V383_AV1_GBL_BYTES);

    /* sequence header */
    bp_put(&bp, h->operating_point_idc, 12);
    bp_put(&bp, V383_AV1_SB128(pp), 1);
    bp_put(&bp, V383_AV1_FILTER_INTRA(pp), 1);
    bp_put(&bp, V383_AV1_INTRA_EDGE(pp), 1);
    bp_put(&bp, V383_AV1_INTERINTRA(pp), 1);
    bp_put(&bp, V383_AV1_MASKED_COMPOUND(pp), 1);
    bp_put(&bp, V383_AV1_DUAL_FILTER(pp), 1);
    bp_put(&bp, pp->order_hint_bits != 0, 1);
    bp_put(&bp, V383_AV1_JNT_COMP(pp), 1);
    bp_put(&bp, V383_AV1_ENABLE_REF_FRAME_MVS(pp), 1);
    bp_put(&bp, pp->order_hint_bits ? pp->order_hint_bits - 1u : 0, 3);
    {
        unsigned int y = V383_AV1_CDEF_PRI(pp->cdef.y_strengths[0]) || V383_AV1_CDEF_SEC(pp->cdef.y_strengths[0]);
        unsigned int uv = V383_AV1_CDEF_PRI(pp->cdef.uv_strengths[0]) || V383_AV1_CDEF_SEC(pp->cdef.uv_strengths[0]);
        bp_put(&bp, !h->coded_lossless && (V383_AV1_CDEF_BITS(pp) || y || uv), 1);
    }
    bp_put(&bp, pp->bitdepth > 8 ? pp->bitdepth - 8u : 0, 3);
    {
        unsigned int fmt;

        if (V383_AV1_MONO_CHROME(pp))
            fmt = 0;
        else if (V383_AV1_SUBSAMPLING_Y(pp) == 1)   /* MPP tests subsampling_y twice */
            fmt = 1;
        else if (V383_AV1_SUBSAMPLING_X(pp) == 1)
            fmt = 2;
        else
            fmt = 3;
        bp_put(&bp, fmt, 2);
    }
    bp_put(&bp, h->matrix_coefficients, 8);
    bp_put(&bp, V383_AV1_FILM_GRAIN(pp), 1);

    /* frame header */
    bp_put(&bp, intra, 1);
    bp_put(&bp, V383_AV1_DISABLE_CDF_UPDATE(pp), 1);
    bp_put(&bp, V383_AV1_SCREEN_CONTENT(pp), 1);
    bp_put(&bp, V383_AV1_INTEGER_MV(pp) || intra, 1);
    bp_put(&bp, pp->order_hint, 8);
    bp_put(&bp, fw, 16);
    bp_put(&bp, pp->height, 16);
    bp_put(&bp, V383_AV1_SUPERRES(pp), 1);
    bp_put(&bp, h->coded_denom, 5);
    bp_put(&bp, h->upscaled_width, 16);
    bp_put(&bp, V383_AV1_HIGH_PRECISION_MV(pp), 1);
    bp_put(&bp, V383_AV1_INTRABC(pp), 1);

    for (i = 0; i < 7; i++)
        bp_put(&bp, h->ref_frame_valued ? h->ref_frame_idx[i] : 7u, 3);
    bp_put(&bp, pp->interp_filter, 3);
    bp_put(&bp, V383_AV1_SWITCHABLE_MOTION(pp), 1);
    bp_put(&bp, V383_AV1_USE_REF_FRAME_MVS(pp), 1);

    for (i = 0; i < 8; i++)
        bp_put(&bp, h->slot_order_hint[i], 8);
    for (i = 0; i < 7; i++)
        bp_put(&bp, h->slot_order_hint[h->ref_frame_idx[i]], 8);

    for (i = 0; i < 7; i++) {
        unsigned int bias = 0;

        /* MPP tests slot i for a surface and takes the order hint of
         * reference i's slot */
        if (pp->order_hint_bits && h->slot_surface[i] != 0xFF)
            bias = relative_dist(pp->order_hint_bits, (int)h->slot_order_hint[h->ref_frame_idx[i]],
                                 pp->order_hint) > 0;
        bp_put(&bp, bias, 1);
    }
    bp_put(&bp, V383_AV1_DISABLE_END_CDF(pp), 1);

    /* quantization */
    bp_put(&bp, pp->quantization.base_qindex, 8);
    bp_put(&bp, (unsigned int)pp->quantization.y_dc_delta_q, 7);
    bp_put(&bp, (unsigned int)pp->quantization.u_dc_delta_q, 7);
    bp_put(&bp, (unsigned int)pp->quantization.u_ac_delta_q, 7);
    bp_put(&bp, (unsigned int)pp->quantization.v_dc_delta_q, 7);
    bp_put(&bp, (unsigned int)pp->quantization.v_ac_delta_q, 7);
    bp_put(&bp, h->using_qmatrix, 1);

    /* segmentation */
    bp_put(&bp, V383_AV1_SEG_ENABLED(pp), 1);
    bp_put(&bp, V383_AV1_SEG_UPDATE_MAP(pp), 1);
    bp_put(&bp, V383_AV1_SEG_TEMPORAL(pp), 1);
    {
        unsigned int mi_w = ALIGN_UP(fw, 8u) >> 2, mi_h = ALIGN_UP(pp->height, 8u) >> 2;
        unsigned int pw = ALIGN_UP(h->slot_upscaled_width[0], 8u) >> 2, ph = ALIGN_UP(h->slot_height[0], 8u) >> 2;

        bp_put(&bp, V383_AV1_SEG_ENABLED(pp) && pp->primary_ref_frame && mi_w == pw && mi_h == ph, 1);
    }
    for (i = 0; i < 8; i++)
        bp_put(&bp, pp->segmentation.feature_mask[i], 8);
    for (i = 0; i < 8; i++) {
        bp_put(&bp, (unsigned int)pp->segmentation.feature_data[i][0], 9);
        bp_put(&bp, (unsigned int)pp->segmentation.feature_data[i][1], 7);
        bp_put(&bp, (unsigned int)pp->segmentation.feature_data[i][2], 7);
        bp_put(&bp, (unsigned int)pp->segmentation.feature_data[i][3], 7);
        bp_put(&bp, (unsigned int)pp->segmentation.feature_data[i][4], 7);
        bp_put(&bp, (unsigned int)pp->segmentation.feature_data[i][5], 3);
    }
    bp_put(&bp, h->last_active, 3);
    bp_put(&bp, h->preskip, 1);
    bp_put(&bp, V383_AV1_DELTA_Q_PRESENT(pp), 1);
    bp_put(&bp, V383_AV1_DELTA_Q_PRESENT(pp) ? V383_AV1_DELTA_Q_RES(pp) : (1u << V383_AV1_DELTA_Q_RES(pp)), 2);

    /* delta lf */
    bp_put(&bp, V383_AV1_DELTA_LF_PRESENT(pp), 1);
    bp_put(&bp, 1u << (pp->loop_filter.delta_lf_res & 3), 2);
    bp_put(&bp, V383_AV1_DELTA_LF_MULTI(pp), 1);
    bp_put(&bp, h->coded_lossless, 1);
    for (i = 0; i < 8; i++) {
        int q = pp->quantization.base_qindex;

        if (pp->segmentation.feature_mask[i] & 1)
            q += pp->segmentation.feature_data[i][0];
        if (q < 0)
            q = 0;
        if (q > 255)
            q = 255;
        bp_put(&bp, !q && !pp->quantization.y_dc_delta_q && !pp->quantization.u_dc_delta_q &&
                    !pp->quantization.v_dc_delta_q && !pp->quantization.u_ac_delta_q &&
                    !pp->quantization.v_ac_delta_q, 1);
    }
    bp_put(&bp, h->all_lossless, 1);

    bp_put(&bp, pp->quantization.qm_y, 4);
    bp_put(&bp, pp->quantization.qm_u, 4);
    bp_put(&bp, pp->quantization.qm_v, 4);
    bp_put(&bp, pp->loop_filter.filter_level[0], 6);
    bp_put(&bp, pp->loop_filter.filter_level[1], 6);
    bp_put(&bp, pp->loop_filter.filter_level_u, 6);
    bp_put(&bp, pp->loop_filter.filter_level_v, 6);
    bp_put(&bp, pp->loop_filter.sharpness_level, 3);
    bp_put(&bp, V383_AV1_LF_DELTA_ENABLED(pp), 1);
    for (i = 0; i < 8; i++)
        bp_put(&bp, (unsigned int)pp->loop_filter.ref_deltas[i], 7);
    for (i = 0; i < 2; i++)
        bp_put(&bp, (unsigned int)pp->loop_filter.mode_deltas[i], 7);

    /* cdef */
    bp_put(&bp, V383_AV1_CDEF_DAMPING_M3(pp) + 3, 3);
    bp_put(&bp, V383_AV1_CDEF_BITS(pp), 2);
    for (i = 0; i < 8; i++)
        bp_put(&bp, V383_AV1_CDEF_PRI(pp->cdef.y_strengths[i]), 4);
    for (i = 0; i < 8; i++)
        bp_put(&bp, V383_AV1_CDEF_PRI(pp->cdef.uv_strengths[i]), 4);
    for (i = 0; i < 8; i++)
        bp_put(&bp, V383_AV1_CDEF_SEC(pp->cdef.y_strengths[i]), 2);
    for (i = 0; i < 8; i++)
        bp_put(&bp, V383_AV1_CDEF_SEC(pp->cdef.uv_strengths[i]), 2);

    /* loop restoration */
    bp_put(&bp, uses_lr(pp), 1);
    for (i = 0; i < 3; i++)
        bp_put(&bp, pp->loop_filter.frame_restoration_type[i], 2);
    for (i = 0; i < 2; i++) {                   /* 0: 32x32 ... 3: 256x256 */
        unsigned int l = pp->loop_filter.log2_restoration_unit_size[i];
        bp_put(&bp, l >= 5 ? l - 5 : 0, 2);
    }

    bp_put(&bp, V383_AV1_TX_MODE(pp), 2);
    bp_put(&bp, V383_AV1_REFERENCE_MODE(pp), 1);
    bp_put(&bp, h->skip_ref0, 3);
    bp_put(&bp, h->skip_ref1, 3);
    bp_put(&bp, V383_AV1_SKIP_MODE(pp), 1);
    bp_put(&bp, V383_AV1_WARPED_MOTION(pp), 1);
    bp_put(&bp, V383_AV1_REDUCED_TX_SET(pp), 1);

    /* global motion */
    for (i = 0; i < 7; i++)
        bp_put(&bp, V383_AV1_WM_TYPE(pp->frame_refs[i]), 2);
    for (i = 0; i < 7; i++)
        for (j = 0; j < 6; j++)
            bp_put(&bp, (unsigned int)h->wmmat_val[i][j], 17);

    /* film grain */
    bp_put(&bp, V383_AV1_FG_APPLY(pp), 1);
    bp_put(&bp, pp->film_grain.grain_seed, 16);
    bp_put(&bp, V383_AV1_FG_APPLY(pp), 1);          /* update_grain: DXVA hands the values in force */
    bp_put(&bp, pp->film_grain.num_y_points, 4);
    for (i = 0; i < 14; i++)
        bp_put(&bp, pp->film_grain.scaling_points_y[i][0], 8);
    for (i = 0; i < 14; i++)
        bp_put(&bp, pp->film_grain.scaling_points_y[i][1], 8);
    bp_put(&bp, V383_AV1_FG_CHROMA_FROM_LUMA(pp), 1);
    bp_put(&bp, pp->film_grain.num_cb_points, 4);
    for (i = 0; i < 10; i++)
        bp_put(&bp, pp->film_grain.scaling_points_cb[i][0], 8);
    for (i = 0; i < 10; i++)
        bp_put(&bp, pp->film_grain.scaling_points_cb[i][1], 8);
    bp_put(&bp, pp->film_grain.num_cr_points, 4);
    for (i = 0; i < 10; i++)
        bp_put(&bp, pp->film_grain.scaling_points_cr[i][0], 8);
    for (i = 0; i < 10; i++)
        bp_put(&bp, pp->film_grain.scaling_points_cr[i][1], 8);
    bp_put(&bp, V383_AV1_FG_SCALING_SHIFT_M8(pp), 2);
    bp_put(&bp, V383_AV1_FG_AR_COEFF_LAG(pp), 2);
    for (i = 0; i < 24; i++)
        bp_put(&bp, pp->film_grain.ar_coeffs_y[i], 8);
    for (i = 0; i < 25; i++)
        bp_put(&bp, pp->film_grain.ar_coeffs_cb[i], 8);
    for (i = 0; i < 25; i++)
        bp_put(&bp, pp->film_grain.ar_coeffs_cr[i], 8);
    bp_put(&bp, V383_AV1_FG_AR_SHIFT_M6(pp), 2);
    bp_put(&bp, V383_AV1_FG_GRAIN_SCALE_SHIFT(pp), 2);
    bp_put(&bp, pp->film_grain.cb_mult, 8);
    bp_put(&bp, pp->film_grain.cb_luma_mult, 8);
    bp_put(&bp, (unsigned int)pp->film_grain.cb_offset, 9);
    bp_put(&bp, pp->film_grain.cr_mult, 8);
    bp_put(&bp, pp->film_grain.cr_luma_mult, 8);
    bp_put(&bp, (unsigned int)pp->film_grain.cr_offset, 9);
    bp_put(&bp, V383_AV1_FG_OVERLAP(pp), 1);
    bp_put(&bp, V383_AV1_FG_CLIP_RESTRICTED(pp), 1);

    /* the reference slots */
    for (i = 0; i < 8; i++)
        bp_put(&bp, h->slot_upscaled_width[i], 16);
    for (i = 0; i < 8; i++)
        bp_put(&bp, h->slot_height[i], 16);
    for (i = 0; i < 8; i++)
        bp_put(&bp, h->slot_width[i], 16);
    for (i = 0; i < 8; i++)
        bp_put(&bp, h->slot_frame_type[i], 2);
    for (i = 0; i < 8; i++)
        for (j = 0; j < 7; j++)
            bp_put(&bp, h->slot_ref_hints[i][j], 8);
    {
        unsigned int mw[8] = { 0 }, mh[8] = { 0 };

        for (i = 0; i < 7; i++) {
            mw[h->ref_frame_idx[i]] = h->slot_width[h->ref_frame_idx[i]];
            mh[h->ref_frame_idx[i]] = h->slot_height[h->ref_frame_idx[i]];
        }
        for (i = 0; i < 8; i++) {           /* MPP: indexed by slot, 8 entries */
            unsigned int hs = V383_AV1_INTRABC(pp) ? fw : mw[i];
            unsigned int vs = V383_AV1_INTRABC(pp) ? pp->height : mh[i];

            bp_put(&bp, ((hs << 14) + fw / 2) / fw, 16);
            bp_put(&bp, ((vs << 14) + pp->height / 2) / pp->height, 16);
        }
    }

    bp_put(&bp, (h->frame_header_bits + 7) >> 3, 10);
    bp_put(&bp, pp->tiles.cols, 7);
    bp_put(&bp, pp->tiles.rows, 7);
    bp_put(&bp, pp->tiles.context_update_id, 12);
    bp_put(&bp, h->tile_size_bytes, 3);
    bp_put(&bp, (unsigned int)pp->tiles.cols * pp->tiles.rows, 13);
    bp_put(&bp, h->tile_cols_log2 + h->tile_rows_log2, 12);
    for (i = 0; i < 64; i++)
        bp_put(&bp, i < pp->tiles.cols ? pp->tiles.widths[i] : 0u, 7);
    for (i = 0; i < 64; i++)
        bp_put(&bp, i < pp->tiles.rows ? pp->tiles.heights[i] : 0u, 10);
}

/* ------------------------------------------------------------- stream */

static unsigned int put_leb128(unsigned char *p, unsigned int v)
{
    unsigned int n = 0;

    do {
        unsigned char b = (unsigned char)(v & 0x7F);
        v >>= 7;
        p[n++] = (unsigned char)(b | (v ? 0x80 : 0));
    } while (v);
    return n;
}

unsigned int v383_av1_synth_stream(const V383_DXVA_PicParams_AV1 *pp, const V383_DXVA_Tile_AV1 *tiles,
                                   unsigned int num_tiles, const unsigned char *data, unsigned int mode,
                                   unsigned int frame_header_bytes, unsigned char *out, unsigned int out_cap)
{
    unsigned int i, tg = 0, need, pos = 0, hdr = mode ? frame_header_bytes : 0;

    (void)pp;
    if (!num_tiles)
        return 0;
    tg = (num_tiles > 1) ? 1 : 0;                   /* tile_start_and_end_present_flag 0, aligned */
    for (i = 0; i < num_tiles; i++)
        tg += tiles[i].DataSize + ((i + 1 < num_tiles) ? 4 : 0);
    need = 2 + 5 + hdr + tg;
    if (need > out_cap)
        return 0;
    out[pos++] = (unsigned char)(((mode ? 6u : 4u) << 3) | 0x02);   /* OBU_FRAME / OBU_TILE_GROUP, has_size */
    pos += put_leb128(out + pos, hdr + tg);
    for (i = 0; i < hdr; i++)
        out[pos++] = 0;
    if (num_tiles > 1)
        out[pos++] = 0;
    for (i = 0; i < num_tiles; i++) {
        if (i + 1 < num_tiles) {
            unsigned int s = tiles[i].DataSize - 1;
            out[pos++] = (unsigned char)s;
            out[pos++] = (unsigned char)(s >> 8);
            out[pos++] = (unsigned char)(s >> 16);
            out[pos++] = (unsigned char)(s >> 24);
        }
        v_copy(out + pos, data + tiles[i].DataOffset, tiles[i].DataSize);
        pos += tiles[i].DataSize;
    }
    return pos;
}

/* ------------------------------------------------------------- state */

void v383_av1_reset(V383_Av1State *s)
{
    v_zero(s, sizeof(*s));
}

/* ------------------------------------------------------------- build */

int v383_av1_build(const V383_Av1Input *in, V383_RegFile *rf)
{
    const V383_DXVA_PicParams_AV1 *pp = in->pp;
    V383_Av1State *s = in->state;
    V383_Av1Hdr h;
    unsigned int rcb[RCB_COUNT];
    unsigned int cur, i, off, y_size, coeff_idx, rd, rd_idx, intra;
    V383_Av1SurfState *cs;

    if (!pp || !s || !in->aux_cpu || !in->surfaces || !rf)
        return V383_E_INVALID;
    if (pp->seq_profile != 0 || V383_AV1_MONO_CHROME(pp) || !V383_AV1_SUBSAMPLING_X(pp) ||
        !V383_AV1_SUBSAMPLING_Y(pp) || (pp->bitdepth != 8 && pp->bitdepth != 10))
        return V383_E_UNSUPPORTED;
    if (!pp->width || !pp->height || pp->width > 8192 || pp->height > 8192 ||
        !pp->tiles.cols || !pp->tiles.rows || pp->tiles.cols > 64 || pp->tiles.rows > 64)
        return V383_E_INVALID;
    cur = pp->CurrPicTextureIndex;
    if (cur >= in->num_surfaces || cur >= V383_AV1_MAX_SURFACES)
        return V383_E_INVALID;
    if ((in->hor_stride & 15) || (in->ver_stride & 7) || !in->stream_len)
        return V383_E_INVALID;
    intra = frame_is_intra(pp);

    if (in->extra && in->extra->valid) {
        v383_av1_derive(pp, in->extra, s, &h);
    } else {
        v383_av1_derive(pp, (const V383_Av1Extra *)0, s, &h);
        h.frame_header_bits = in->frame_header_bits;
        h.tile_size_bytes = in->tile_size_bytes ? in->tile_size_bytes : 4;
    }

    /* ---- auxiliary buffer */
    if (!s->frames)
        v_copy(in->aux_cpu + V383_AV1_AUX_DEFCDF_OFF, v383_av1_default_cdf, V383_AV1_ALL_CDF_BYTES);
    v383_av1_write_gbl(&h, in->aux_cpu + V383_AV1_AUX_GBL_OFF);

    /* ---- registers */
    v_zero(rf, sizeof(*rf));

    /* vdpu383_init_ctrl_regs(MPP_VIDEO_CodingAV1) */
    reg_set(rf, 8, 4);
    reg_set(rf, 9, 0);
    reg_set(rf, 10, 0x3FF);
    reg_set(rf, 13, 0xFFFFFF);
    reg_set(rf, 16, 1);
    reg_set(rf, 20, 0xFFFFFFDF);
    reg_set(rf, 21, 0x3FFFFFFF);
    /* vdpu383_setup_statistic() */
    reg_set(rf, 28, 1u | (1u << 3) | (11u << 4));
    reg_set(rf, 29, 1u | (1u << 3) | (17u << 4));
    reg_set(rf, 30, 0);
    rf->core_timeout = rf->regs[13];

    reg_set(rf, 65, 0);
    reg_set(rf, 66, v383_av1_stream_read_len(in->stream_len));
    reg_set(rf, 67, V383_AV1_GBL_LEN);
    reg_set(rf, 128, in->stream_dma);
    reg_set(rf, 131, in->aux_dma + V383_AV1_AUX_GBL_OFF);
    reg_set(rf, 169, in->stream_dma);           /* MPP: the error reference is the stream buffer */

    rcb_sizes(pp, rcb);
    off = 0;
    for (i = 0; i < RCB_COUNT; i++) {
        reg_set(rf, 140 + 2 * i, in->rcb_dma + off);
        reg_set(rf, 141 + 2 * i, rcb[i]);
        off += rcb[i];
    }

    y_size = in->hor_stride * in->ver_stride;
    reg_set(rf, 68, in->hor_stride >> 4);
    reg_set(rf, 69, in->hor_stride >> 4);
    reg_set(rf, 70, y_size >> 4);
    reg_set(rf, 80, rf->regs[68]);
    reg_set(rf, 81, rf->regs[69]);
    reg_set(rf, 82, rf->regs[70]);

    reg_set(rf, 168, in->surfaces[cur].dma);
    reg_set(rf, 192, in->surfaces[cur].dma);
    for (i = 0; i < 7; i++) {
        unsigned int slot = h.ref_frame_idx[i], sf = h.slot_surface[slot];

        if (sf == 0xFF || sf >= in->num_surfaces)
            continue;
        reg_set(rf, 83 + 3 * slot, in->hor_stride >> 4);
        reg_set(rf, 84 + 3 * slot, in->hor_stride >> 4);
        reg_set(rf, 85 + 3 * slot, y_size >> 4);
        reg_set(rf, 170 + slot, in->surfaces[sf].dma);
        reg_set(rf, 195 + slot, in->surfaces[sf].dma);
    }
    reg_set(rf, 216, in->surfaces[cur].colmv_dma);
    for (i = 0; i < 8; i++) {
        unsigned int sf = h.slot_surface[i];
        if (sf != 0xFF && sf < in->num_surfaces)
            reg_set(rf, 217 + i, in->surfaces[sf].colmv_dma);
    }

    /* CDFs (vdpu38x_av1d_set_cdf_segid) */
    if (V383_AV1_FRAME_TYPE(pp) == V383_AV1_KEY_FRAME)
        for (i = 0; i < V383_AV1_MAX_SURFACES; i++)
            s->surf[i].cdf_valid = 0;
    coeff_idx = pp->quantization.base_qindex <= 20 ? 0 : pp->quantization.base_qindex <= 60 ? 1 :
                pp->quantization.base_qindex <= 120 ? 2 : 3;
    if (V383_AV1_FRAME_TYPE(pp) == V383_AV1_KEY_FRAME || pp->primary_ref_frame == V383_AV1_PRIMARY_REF_NONE) {
        rd = in->aux_dma + V383_AV1_AUX_DEFCDF_OFF;
        rd_idx = coeff_idx;
        reg_set(rf, 184, rd);
        reg_set(rf, 178, rd + V383_AV1_NON_COEF_CDF_BYTES + V383_AV1_COEF_CDF_BYTES * rd_idx);
    } else {
        unsigned int sf = h.slot_surface[h.ref_frame_idx[pp->primary_ref_frame & 7]];

        rd = in->aux_dma + V383_AV1_AUX_DEFCDF_OFF;
        rd_idx = coeff_idx;
        if (sf != 0xFF && sf < in->num_surfaces) {
            rd_idx = s->surf[sf].coeff_idx;
            if (!V383_AV1_DISABLE_END_CDF(pp) && s->surf[sf].cdf_valid)
                rd = in->surfaces[sf].cdf_dma;
        }
        reg_set(rf, 184, rd);
        reg_set(rf, 178, rd + V383_AV1_NON_COEF_CDF_BYTES + V383_AV1_COEF_CDF_BYTES * rd_idx);
        reg_set(rf, 181, rd + V383_AV1_ALL_CDF_BYTES);
    }
    reg_set(rf, 185, in->surfaces[cur].cdf_dma);
    reg_set(rf, 179, in->surfaces[cur].cdf_dma + V383_AV1_NON_COEF_CDF_BYTES);
    reg_set(rf, 182, in->surfaces[cur].cdf_dma + V383_AV1_ALL_CDF_BYTES);

    mask_range(rf, 8, 30);
    mask_range(rf, 64, 106);
    mask_range(rf, 128, 232);

    /* ---- what later frames need to know of this surface */
    cs = &s->surf[cur];
    cs->valid = 1;
    cs->frame_type = (unsigned char)V383_AV1_FRAME_TYPE(pp);
    cs->order_hint = pp->order_hint;
    for (i = 0; i < 7; i++)
        cs->ref_hints[i] = (unsigned char)h.slot_order_hint[h.ref_frame_idx[i]];
    cs->width = (unsigned short)h.frame_width;
    cs->upscaled_width = (unsigned short)h.upscaled_width;
    cs->height = (unsigned short)pp->height;
    if (!V383_AV1_DISABLE_END_CDF(pp)) {
        cs->cdf_valid = 1;
        cs->coeff_idx = 0;
    } else {
        cs->cdf_valid = 0;
        cs->coeff_idx = (unsigned char)rd_idx;
    }
    (void)intra;
    s->frames++;
    return V383_OK;
}
