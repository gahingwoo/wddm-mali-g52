/*
 * vdpu383_hevc.c - DXVA HEVC picture -> VDPU383 register file
 *
 * The register and buffer layouts are Rockchip MPP's (Apache-2.0 OR MIT,
 * used here under MIT), commit 9b56348:
 *
 *   mpp/hal/rkdec/h265d/hal_h265d_vdpu383.c   hal_h265d_vdpu383_gen_regs(),
 *                                             hal_h265d_vdpu383_rps(),
 *                                             vdpu383_h265d_rcb_calc()
 *   mpp/hal/rkdec/h265d/hal_h265d_com.c       hal_h265d_vdpu38x_output_pps_packet(),
 *                                             hal_vdpu38x_record_scaling_list(),
 *                                             hal_h265d_vdpu38x_scalinglist_packet()
 *   mpp/hal/rkdec/common/vdpu383_com.c        vdpu383_init_ctrl_regs(HEVC)
 *   mpp/hal/rkdec/common/vdpu38x_com.c        hal_h265d_avs2d_calc_mv_size(), RCB bookkeeping
 *
 *   Copyright (c) 2024-2025 Rockchip Electronics Co., Ltd.
 *   SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * Every field of the SPS/PPS record was checked against the bit positions
 * of mainline Linux's rkvdec-vdpu383-hevc.c; the two agree but for bit 106,
 * where MPP writes strong_intra_smoothing_enabled_flag a second time among
 * the range extension flags and mainline writes 0. Mainline's is used
 * (V383_HEVC_MPP_BIT106 1 for MPP's). The short-term set stride in the RPS
 * table is 384 bits in both: MPP's 128-bit pad advances its bit writer by
 * one 64-bit word, not two.
 *
 * Differences from MPP, all deliberate:
 *
 * - The DXVA structures are the standard ones (dxva.h), not MPP's private
 *   superset: RefPicList and PicOrderCntValList have 15 entries, and the
 *   SPS's reference picture sets, the vps/sps/pps ids and the range
 *   extension flags are not in them. The sets and ids come from
 *   V383_HevcExtra when a parser has them; without it the pps id is read
 *   from the first slice header and the table is rebuilt (below). Range
 *   extension flags are 0: Main and Main 10 only.
 * - The CABAC table is given (register 130), as mainline gives it; MPP's
 *   VDPU383 HAL leaves the register 0.
 * - RCB sizes use integer arithmetic; with one tile and the bit depth fixed
 *   at 10, as MPP fixes it, the float results are exact.
 * - Only 4:2:0, 8 bit, raster NV12 output; no thumbnail (scale-down) output.
 *
 * Rebuilding the RPS table from DXVA (v383_hevc_rebuild_rps):
 *
 *   DXVA states the picture's reference sets as they come out
 *   (RefPicSetStCurrBefore/After, RefPicSetLtCurr and the pictures in
 *   RefPicList), not the SPS's sets the slice headers name. All slices of a
 *   picture have the same sets, so:
 *
 *   - every SPS set the table has is the picture's short-term set (its
 *     pictures with their used flags, those not used being in RefPicList
 *     too): a slice naming any set by index derives the right one;
 *   - a slice that codes its set with inter prediction (7.4.8, from set
 *     RefRpsIdx) gets, at RefRpsIdx, a set that the prediction turns into
 *     the picture's set. Such a set exists when the slice's flags allow
 *     it; the search below finds one or reports V383_E_UNSUPPORTED;
 *   - the SPS's long-term pictures a slice names get the POC LSB of the
 *     long-term pictures in RefPicSetLtCurr, in order.
 *
 *   v383_hevc_check_rps() then derives every slice's sets from the table
 *   as the decoder does and compares them with DXVA's.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "vdpu383_hevc.h"

#ifndef V383_HEVC_MPP_BIT106
#define V383_HEVC_MPP_BIT106 0
#endif

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
 * rkvdec_set_bw_field() on a little-endian CPU (as vdpu383_h264.c). */
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

/* A short message without the C library. */
static void msg(char *why, unsigned int len, const char *s, int num, int with_num)
{
    unsigned int n = 0;
    char d[12];
    int k = 0;
    unsigned int u;

    if (!why || !len)
        return;
    while (n + 1 < len && *s)
        why[n++] = *s++;
    if (with_num) {
        u = (num < 0) ? (unsigned int)(-num) : (unsigned int)num;
        do {
            d[k++] = (char)('0' + u % 10);
            u /= 10;
        } while (u && k < 11);
        if (num < 0 && n + 1 < len)
            why[n++] = '-';
        while (k && n + 1 < len)
            why[n++] = d[--k];
    }
    why[n] = 0;
}

/* ------------------------------------------------------------- sizes */

static unsigned int log2_min_cb(const V383_DXVA_PicParams_HEVC *pp)
{
    return pp->log2_min_luma_coding_block_size_minus3 + 3u;
}

static unsigned int log2_ctb(const V383_DXVA_PicParams_HEVC *pp)
{
    return log2_min_cb(pp) + pp->log2_diff_max_min_luma_coding_block_size;
}

unsigned int v383_hevc_width(const V383_DXVA_PicParams_HEVC *pp)
{
    return (unsigned int)pp->PicWidthInMinCbsY << log2_min_cb(pp);
}

unsigned int v383_hevc_height(const V383_DXVA_PicParams_HEVC *pp)
{
    return (unsigned int)pp->PicHeightInMinCbsY << log2_min_cb(pp);
}

unsigned int v383_hevc_stream_read_len(unsigned int stream_len)
{
    return ALIGN_UP(stream_len, 16u) + 64;
}

unsigned int v383_hevc_colmv_size(unsigned int width, unsigned int height, unsigned int cb_size)
{
    /* MPP passes the minimum coding block size as "ctu_w", and doubles. */
    unsigned int seg_w = 64 * 16 * 16 / cb_size;
    unsigned int seg_cnt_w = ALIGN_UP(width, seg_w) / seg_w;
    unsigned int seg_cnt_h = ALIGN_UP(height, cb_size) / cb_size;

    return seg_cnt_w * seg_cnt_h * 64 * 16 * 2;
}

/*
 * The RCB buffers: MPP vdpu383_h265d_rcb_calc() with one tile covering the
 * picture (in_tl_row = on_tl_row = width), 4:2:0 (intra_uv 2, filter_row_uv
 * 1, filter_col_uv 1) and the bit depth fixed at 10 as MPP fixes it.
 * Values are bits; rcb_bytes() is MPP_RCB_BYTES(). Order and register
 * numbers are MPP's Vdpu38xRcbType.
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
    unsigned long long w = width, w64 = ALIGN_UP(w, 64ull);
    unsigned long long append = (width > 4096) ? 256 * 16 * 8 : 864 * 16 * 8;
    unsigned long long fltd;

    (void)height;
    size[RCB_STRMD_IN_ROW] = 0;
    size[RCB_STRMD_ON_ROW] = 0;
    size[RCB_INTER_IN_ROW] = rcb_bytes(DIV_UP(w, 8ull) * 174);
    size[RCB_INTER_ON_ROW] = size[RCB_INTER_IN_ROW];
    /* ROUNDUP(512, w * (10 + 2) * 1 * 2) */
    size[RCB_INTRA_IN_ROW] = rcb_bytes(ALIGN_UP(w * 24, 512ull));
    size[RCB_INTRA_ON_ROW] = size[RCB_INTRA_IN_ROW];
    /* ALIGN(w, 64) * (1.6 * 10 + 0.5) * (7.5 + 5 * 1) = ALIGN(w, 64) * 825 / 4,
     * exact; the in-tile row and protect buffers take half and an append. */
    fltd = w64 * 825 / 4;
    size[RCB_FLTD_IN_ROW] = rcb_bytes(fltd / 2 + append);
    size[RCB_FLTD_PROT_IN_ROW] = size[RCB_FLTD_IN_ROW];
    size[RCB_FLTD_ON_ROW] = rcb_bytes(fltd);
    /* ALIGN(on_tl_row, 64) * 16.5 * (16.5 + 5.5 * 1) = ALIGN(w, 64) * 363 */
    size[RCB_FLTD_ON_COL] = rcb_bytes(w64 * 363);
    size[RCB_FLTD_UPSC_ON_COL] = 0;
}

unsigned int v383_hevc_rcb_size(unsigned int width, unsigned int height)
{
    unsigned int size[RCB_COUNT];
    unsigned int i, total = 0;

    rcb_sizes(width, height, size);
    for (i = 0; i < RCB_COUNT; i++)
        total += size[i];
    return total;
}

/* -------------------------------------------------- slice header reader */

/* RBSP reader over the start of one NAL unit, emulation prevention bytes
 * removed. A slice header up to its long-term pictures fits. */
#define SH_BYTES 320

typedef struct BitGet {
    unsigned char b[SH_BYTES];
    unsigned int  n, pos;
    int           err;
} BitGet;

static void bg_init(BitGet *g, const unsigned char *nal, unsigned int len)
{
    unsigned int i, zeros = 0;

    g->n = 0;
    g->pos = 0;
    g->err = 0;
    for (i = 0; i < len && g->n < SH_BYTES; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        g->b[g->n++] = nal[i];
    }
}

static unsigned int bg_u(BitGet *g, unsigned int bits)
{
    unsigned int v = 0;

    while (bits--) {
        if (g->pos >= g->n * 8) {
            g->err = 1;
            return 0;
        }
        v = (v << 1) | ((g->b[g->pos >> 3] >> (7 - (g->pos & 7))) & 1u);
        g->pos++;
    }
    return v;
}

static unsigned int bg_ue(BitGet *g)
{
    unsigned int lz = 0;

    while (!bg_u(g, 1)) {
        if (g->err || ++lz > 31) {
            g->err = 1;
            return 0;
        }
    }
    return lz ? ((1u << lz) - 1 + bg_u(g, lz)) : 0;
}

static unsigned int ceil_log2(unsigned int v)
{
    unsigned int n = 0;

    while ((1u << n) < v)
        n++;
    return n;
}

/* What a slice segment header says about the reference sets. */
typedef struct SliceRps {
    unsigned int nal_type, first, pps_id, dependent, idr;
    unsigned int poc_lsb;
    unsigned int sps_flag, st_idx;              /* named SPS set */
    unsigned int inter, ref_idx;                /* in-slice set predicted from set ref_idx */
    int          delta_rps;
    unsigned char used[17], use_delta[17];
    V383_HevcStRps own;                         /* in-slice set, not predicted */
    unsigned int num_lt_sps, num_lt;            /* num_long_term_sps, + num_long_term_pics */
    unsigned char lt_sps[32];                   /* entry from the SPS (lt_idx_sps) */
    unsigned int lt_idx[32], lt_lsb[32];
    unsigned char lt_used[32], lt_msb_present[32];
    unsigned int lt_msb_cycle[32];
} SliceRps;

static int parse_own_set(BitGet *g, V383_HevcStRps *s)
{
    unsigned int i, d;
    int poc = 0;

    s->num_negative = (unsigned char)bg_ue(g);
    s->num_positive = (unsigned char)bg_ue(g);
    if (s->num_negative > 16 || s->num_positive > 16 || s->num_negative + s->num_positive > 16)
        return -1;
    for (i = 0; i < s->num_negative; i++) {
        d = bg_ue(g) + 1;
        poc -= (int)d;
        s->delta_poc_s0[i] = (short)poc;
        s->used_s0[i] = (unsigned char)bg_u(g, 1);
    }
    poc = 0;
    for (i = 0; i < s->num_positive; i++) {
        d = bg_ue(g) + 1;
        poc += (int)d;
        s->delta_poc_s1[i] = (short)poc;
        s->used_s1[i] = (unsigned char)bg_u(g, 1);
    }
    return g->err ? -1 : 0;
}

/*
 * Parse one slice segment header (7.3.6.1) up to its long-term pictures.
 * nal points at the NAL unit header. 0, 1 for a dependent slice segment
 * (which repeats the previous one's sets), -1 on a header that cannot be
 * read.
 */
static int parse_slice(const V383_DXVA_PicParams_HEVC *pp, const unsigned char *nal, unsigned int len,
                       SliceRps *s)
{
    BitGet g;
    unsigned int ctb = log2_ctb(pp), ctbs_w, ctbs_h, i, n;
    unsigned int num_st = pp->num_short_term_ref_pic_sets;

    v_zero(s, sizeof(*s));
    bg_init(&g, nal, len);
    if (bg_u(&g, 1))                    /* forbidden_zero_bit */
        return -1;
    s->nal_type = bg_u(&g, 6);
    bg_u(&g, 6 + 3);                    /* nuh_layer_id, nuh_temporal_id_plus1 */
    if (s->nal_type > 21)
        return -1;

    s->first = bg_u(&g, 1);
    if (s->nal_type >= 16 && s->nal_type <= 23)
        bg_u(&g, 1);                    /* no_output_of_prior_pics_flag */
    s->pps_id = bg_ue(&g);
    if (!s->first) {
        if (V383_HEVC_DEPENDENT_SLICES(pp))
            s->dependent = bg_u(&g, 1);
        ctbs_w = DIV_UP(v383_hevc_width(pp), 1u << ctb);
        ctbs_h = DIV_UP(v383_hevc_height(pp), 1u << ctb);
        bg_u(&g, ceil_log2(ctbs_w * ctbs_h));       /* slice_segment_address */
    }
    if (g.err)
        return -1;
    if (s->dependent)
        return 1;

    bg_u(&g, V383_HEVC_NUM_EXTRA_SH_BITS(pp));      /* slice_reserved_flag[] */
    bg_ue(&g);                                      /* slice_type */
    if (V383_HEVC_OUTPUT_FLAG_PRESENT(pp))
        bg_u(&g, 1);
    if (V383_HEVC_SEPARATE_COLOUR_PLANE(pp))
        bg_u(&g, 2);
    if (s->nal_type == 19 || s->nal_type == 20) {
        s->idr = 1;
        return g.err ? -1 : 0;
    }

    s->poc_lsb = bg_u(&g, V383_HEVC_LOG2_MAX_POC_LSB_MINUS4(pp) + 4);
    s->sps_flag = bg_u(&g, 1);
    if (!s->sps_flag) {
        /* st_ref_pic_set(num_short_term_ref_pic_sets), 7.3.7 */
        if (num_st)
            s->inter = bg_u(&g, 1);
        if (s->inter) {
            unsigned int delta_idx = bg_ue(&g) + 1;
            unsigned int sign, abs_m1;

            if (delta_idx > num_st)
                return -1;
            s->ref_idx = num_st - delta_idx;
            sign = bg_u(&g, 1);
            abs_m1 = bg_ue(&g);
            s->delta_rps = sign ? -(int)(abs_m1 + 1) : (int)(abs_m1 + 1);
            /* NumDeltaPocs[RefRpsIdx]: DXVA carries it for exactly this. */
            n = pp->ucNumDeltaPocsOfRefRpsIdx;
            if (n > 16)
                return -1;
            for (i = 0; i <= n; i++) {
                s->used[i] = (unsigned char)bg_u(&g, 1);
                s->use_delta[i] = s->used[i] ? 1 : (unsigned char)bg_u(&g, 1);
            }
        } else if (parse_own_set(&g, &s->own)) {
            return -1;
        }
    } else if (num_st > 1) {
        s->st_idx = bg_u(&g, ceil_log2(num_st));
    }
    if (s->sps_flag && s->st_idx >= num_st)
        return -1;

    if (V383_HEVC_LONG_TERM_REFS_PRESENT(pp)) {
        if (pp->num_long_term_ref_pics_sps)
            s->num_lt_sps = bg_ue(&g);
        s->num_lt = s->num_lt_sps + bg_ue(&g);
        if (s->num_lt > 32 || s->num_lt_sps > pp->num_long_term_ref_pics_sps)
            return -1;
        for (i = 0; i < s->num_lt; i++) {
            if (i < s->num_lt_sps) {
                s->lt_sps[i] = 1;
                if (pp->num_long_term_ref_pics_sps > 1)
                    s->lt_idx[i] = bg_u(&g, ceil_log2(pp->num_long_term_ref_pics_sps));
            } else {
                s->lt_lsb[i] = bg_u(&g, V383_HEVC_LOG2_MAX_POC_LSB_MINUS4(pp) + 4);
                s->lt_used[i] = (unsigned char)bg_u(&g, 1);
            }
            s->lt_msb_present[i] = (unsigned char)bg_u(&g, 1);
            if (s->lt_msb_present[i])
                s->lt_msb_cycle[i] = bg_ue(&g);
        }
    }
    return g.err ? -1 : 0;
}

/* Start of the next NAL unit at or after *pos (past its start code), and
 * its length up to the next start code. */
static int next_nal(const unsigned char *s, unsigned int len, unsigned int *pos, unsigned int *nal,
                    unsigned int *nal_len)
{
    unsigned int i = *pos, j;

    for (; i + 3 <= len; i++)
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
            break;
    if (i + 3 > len)
        return 0;
    i += 3;
    for (j = i; j + 3 <= len; j++)
        if (s[j] == 0 && s[j + 1] == 0 && (s[j + 2] == 1 || s[j + 2] == 0))
            break;
    if (j + 3 > len)
        j = len;
    *nal = i;
    *nal_len = j - i;
    *pos = j;
    return 1;
}

int v383_hevc_parse_pps_id(const V383_DXVA_PicParams_HEVC *pp, const unsigned char *stream, unsigned int len)
{
    unsigned int pos = 0, nal, nal_len;
    SliceRps s;

    while (next_nal(stream, len, &pos, &nal, &nal_len)) {
        if (nal_len < 3 || ((stream[nal] >> 1) & 0x3F) > 31)
            continue;
        if (parse_slice(pp, stream + nal, nal_len, &s) < 0)
            return -1;
        return (int)s.pps_id;
    }
    return -1;
}

/* ------------------------------------------------- reference picture sets */

/* 7-61, 7-62: the set predicted from ref with delta_rps and the flags. */
static void predict_set(const V383_HevcStRps *ref, int delta_rps, const unsigned char *used,
                        const unsigned char *use_delta, V383_HevcStRps *out)
{
    int j, d;
    unsigned int i = 0, nneg = ref->num_negative, npos = ref->num_positive, n = nneg + npos;

    v_zero(out, sizeof(*out));
    for (j = (int)npos - 1; j >= 0; j--) {
        d = ref->delta_poc_s1[j] + delta_rps;
        if (d < 0 && use_delta[nneg + j] && i < 16) {
            out->delta_poc_s0[i] = (short)d;
            out->used_s0[i++] = used[nneg + j];
        }
    }
    if (delta_rps < 0 && use_delta[n] && i < 16) {
        out->delta_poc_s0[i] = (short)delta_rps;
        out->used_s0[i++] = used[n];
    }
    for (j = 0; j < (int)nneg; j++) {
        d = ref->delta_poc_s0[j] + delta_rps;
        if (d < 0 && use_delta[j] && i < 16) {
            out->delta_poc_s0[i] = (short)d;
            out->used_s0[i++] = used[j];
        }
    }
    out->num_negative = (unsigned char)i;

    i = 0;
    for (j = (int)nneg - 1; j >= 0; j--) {
        d = ref->delta_poc_s0[j] + delta_rps;
        if (d > 0 && use_delta[j] && i < 16) {
            out->delta_poc_s1[i] = (short)d;
            out->used_s1[i++] = used[j];
        }
    }
    if (delta_rps > 0 && use_delta[n] && i < 16) {
        out->delta_poc_s1[i] = (short)delta_rps;
        out->used_s1[i++] = used[n];
    }
    for (j = 0; j < (int)npos; j++) {
        d = ref->delta_poc_s1[j] + delta_rps;
        if (d > 0 && use_delta[nneg + j] && i < 16) {
            out->delta_poc_s1[i] = (short)d;
            out->used_s1[i++] = used[nneg + j];
        }
    }
    out->num_positive = (unsigned char)i;
}

static int ref_valid(const V383_DXVA_PicParams_HEVC *pp, unsigned int i)
{
    return i < 15 && pp->RefPicList[i].bPicEntry != 0xFF && pp->RefPicList[i].bPicEntry != 0x7F;
}

static int in_list(const unsigned char *l, unsigned int i)
{
    unsigned int k;

    for (k = 0; k < 8 && l[k] != 0xFF; k++)
        if (l[k] == i)
            return 1;
    return 0;
}

/* The picture's short-term set as DXVA states it: every short-term picture
 * of RefPicList, used when it is in RefPicSetStCurrBefore/After. */
static int current_set(const V383_DXVA_PicParams_HEVC *pp, V383_HevcStRps *c)
{
    int neg[16], pos[16];
    unsigned char nu[16], pu[16];
    unsigned int nn = 0, np = 0, i, a, b;

    for (i = 0; i < 15; i++) {
        int d, used;

        if (!ref_valid(pp, i) || V383_PIC_FLAG(pp->RefPicList[i]))
            continue;
        d = pp->PicOrderCntValList[i] - pp->CurrPicOrderCntVal;
        used = in_list(pp->RefPicSetStCurrBefore, i) || in_list(pp->RefPicSetStCurrAfter, i);
        if (d < 0 && nn < 16) {
            neg[nn] = d;
            nu[nn++] = (unsigned char)used;
        } else if (d > 0 && np < 16) {
            pos[np] = d;
            pu[np++] = (unsigned char)used;
        } else if (d == 0) {
            return -1;
        }
    }
    /* s0 closest first: decreasing; s1 increasing. */
    for (a = 0; a < nn; a++)
        for (b = a + 1; b < nn; b++)
            if (neg[b] > neg[a]) {
                int t = neg[a]; unsigned char u = nu[a];
                neg[a] = neg[b]; nu[a] = nu[b]; neg[b] = t; nu[b] = u;
            }
    for (a = 0; a < np; a++)
        for (b = a + 1; b < np; b++)
            if (pos[b] < pos[a]) {
                int t = pos[a]; unsigned char u = pu[a];
                pos[a] = pos[b]; pu[a] = pu[b]; pos[b] = t; pu[b] = u;
            }
    v_zero(c, sizeof(*c));
    if (nn + np > 15)
        return -1;
    c->num_negative = (unsigned char)nn;
    c->num_positive = (unsigned char)np;
    for (i = 0; i < nn; i++) {
        c->delta_poc_s0[i] = (short)neg[i];
        c->used_s0[i] = nu[i];
    }
    for (i = 0; i < np; i++) {
        c->delta_poc_s1[i] = (short)pos[i];
        c->used_s1[i] = pu[i];
    }
    return 0;
}

/*
 * A set R with NumDeltaPocs n from which predict_set() with this slice's
 * delta_rps and flags gives c's used pictures in c's order.
 *
 * predict_set() keeps the order of the values R_j + delta_rps along
 *   j = n0-1 .. 0 (R's s0, most negative first), n (R itself: delta_rps),
 *   j = n0 .. n-1 (R's s1),
 * which is ascending. So the kept-and-used positions along that walk take
 * c's used values in ascending order, position n takes delta_rps, and every
 * other position any value that keeps the walk strictly ascending. Tried
 * for every n0, the number of negative pictures of R.
 */
static int solve_ref_set(const SliceRps *s, unsigned int n, const V383_HevcStRps *c, V383_HevcStRps *r)
{
    int want[32];
    unsigned int nw = 0, i, n0, k;
    int val[17];
    unsigned char fixed[17];
    unsigned int walk[17];

    if (n > 15)
        return -1;
    for (i = c->num_negative; i-- > 0;)
        if (c->used_s0[i])
            want[nw++] = c->delta_poc_s0[i];
    for (i = 0; i < c->num_positive; i++)
        if (c->used_s1[i])
            want[nw++] = c->delta_poc_s1[i];
    for (k = 0, i = 0; i <= n; i++)
        if (s->used[i])
            k++;
    if (k != nw)
        return -1;

    for (n0 = 0; n0 <= n; n0++) {
        unsigned int w = 0, used_at = 0, ok = 1, p, q;

        for (i = n0; i-- > 0;)
            walk[w++] = i;
        walk[w++] = n;
        for (i = n0; i < n; i++)
            walk[w++] = i;

        for (p = 0; p < w; p++) {
            unsigned int j = walk[p];

            fixed[j] = 0;
            val[j] = 0;
            if (s->used[j]) {
                val[j] = want[used_at++];
                fixed[j] = 1;
            }
            if (j == n) {
                if (fixed[j] && val[j] != s->delta_rps)
                    ok = 0;
                val[j] = s->delta_rps;
                fixed[j] = 1;
            }
        }
        /* Fill the free positions between fixed ones, strictly ascending. */
        for (p = 0; ok && p < w; p++) {
            unsigned int j = walk[p], run;
            int lo, hi;

            if (fixed[j])
                continue;
            for (q = p; q < w && !fixed[walk[q]]; q++)
                ;
            run = q - p;
            hi = (q < w) ? val[walk[q]] : 0x7FFF;
            lo = (p > 0) ? val[walk[p - 1]] : -0x7FFF;
            if (p == 0 && q < w)
                lo = hi - (int)run - 1;
            if (hi - lo - 1 < (int)run) {
                ok = 0;
                break;
            }
            for (k = 0; k < run; k++)
                val[walk[p + k]] = lo + 1 + (int)k;
            p = q - 1;
        }
        for (p = 1; ok && p < w; p++)
            if (val[walk[p]] <= val[walk[p - 1]])
                ok = 0;
        for (p = 0; ok && p < w; p++) {
            int rv = val[walk[p]] - s->delta_rps;
            if (rv < -32768 || rv > 32767)
                ok = 0;
        }
        if (!ok)
            continue;

        v_zero(r, sizeof(*r));
        r->num_negative = (unsigned char)n0;
        r->num_positive = (unsigned char)(n - n0);
        for (i = 0; i < n0; i++) {
            r->delta_poc_s0[i] = (short)(val[i] - s->delta_rps);
            r->used_s0[i] = 1;
        }
        for (i = n0; i < n; i++) {
            r->delta_poc_s1[i - n0] = (short)(val[i] - s->delta_rps);
            r->used_s1[i - n0] = 1;
        }
        return 0;
    }
    return -1;
}

static unsigned int max_lsb(const V383_DXVA_PicParams_HEVC *pp)
{
    return 1u << (V383_HEVC_LOG2_MAX_POC_LSB_MINUS4(pp) + 4);
}

int v383_hevc_rebuild_rps(const V383_DXVA_PicParams_HEVC *pp, const unsigned char *stream,
                          unsigned int len, V383_HevcRpsTable *t)
{
    V383_HevcStRps c;
    unsigned int pos = 0, nal, nal_len, i;
    unsigned char pinned[64];
    SliceRps s;
    int done_lt = 0;

    v_zero(t, sizeof(*t));
    v_zero(pinned, sizeof(pinned));
    t->num_st = pp->num_short_term_ref_pic_sets;
    t->num_lt = pp->num_long_term_ref_pics_sps;
    if (t->num_st > 64 || t->num_lt > 32)
        return V383_E_INVALID;
    if (current_set(pp, &c))
        return V383_E_UNSUPPORTED;
    for (i = 0; i < t->num_st; i++)
        t->st[i] = c;

    while (next_nal(stream, len, &pos, &nal, &nal_len)) {
        int rc;

        if (nal_len < 3 || ((stream[nal] >> 1) & 0x3F) > 31)
            continue;
        rc = parse_slice(pp, stream + nal, nal_len, &s);
        if (rc < 0)
            return V383_E_STREAM;
        if (rc > 0 || s.idr)
            continue;
        if (s.sps_flag) {
            /* the picture's set is there already; keep it */
            if (pinned[s.st_idx] == 2)
                return V383_E_UNSUPPORTED;
            pinned[s.st_idx] = 1;
        } else if (s.inter) {
            V383_HevcStRps r;

            if (pinned[s.ref_idx] == 1)
                return V383_E_UNSUPPORTED;
            if (pinned[s.ref_idx] != 2) {
                if (solve_ref_set(&s, pp->ucNumDeltaPocsOfRefRpsIdx, &c, &r))
                    return V383_E_UNSUPPORTED;
                t->st[s.ref_idx] = r;
                pinned[s.ref_idx] = 2;
            }
        }

        /* The SPS's long-term pictures this slice names: the long-term
         * pictures of RefPicSetLtCurr in order, as many as are not taken
         * by the slice's own (explicit) used entries. */
        if (s.num_lt_sps && !done_lt) {
            unsigned int lt_curr = 0, own_used = 0, next = 0, k, q;

            for (k = 0; k < 8 && pp->RefPicSetLtCurr[k] != 0xFF; k++)
                lt_curr++;
            for (k = s.num_lt_sps; k < s.num_lt; k++)
                own_used += s.lt_used[k];
            q = (lt_curr > own_used) ? lt_curr - own_used : 0;
            for (k = 0; k < s.num_lt; k++) {
                if (!s.lt_sps[k]) {
                    if (s.lt_used[k])
                        next++;
                    continue;
                }
                if (s.lt_idx[k] >= t->num_lt)
                    return V383_E_STREAM;
                if (q && next < lt_curr) {
                    unsigned int ri = pp->RefPicSetLtCurr[next++];
                    if (!ref_valid(pp, ri))
                        return V383_E_UNSUPPORTED;
                    t->lt_poc_lsb[s.lt_idx[k]] =
                        (unsigned short)((unsigned int)pp->PicOrderCntValList[ri] & (max_lsb(pp) - 1));
                    t->lt_used[s.lt_idx[k]] = 1;
                    q--;
                }
            }
            done_lt = 1;
        }
    }
    return V383_OK;
}

/* The sets one slice derives with this table (8.3.2), as the decoder does. */
static int slice_sets(const V383_DXVA_PicParams_HEVC *pp, const SliceRps *s, const V383_HevcRpsTable *t,
                      V383_HevcStRps *st, int *lt_poc, unsigned char *lt_full, unsigned int *n_lt_used)
{
    unsigned int i, n = 0, cycle = 0;

    if (s->sps_flag) {
        *st = t->st[s->st_idx];
    } else if (s->inter) {
        const V383_HevcStRps *r = &t->st[s->ref_idx];
        if ((unsigned int)r->num_negative + r->num_positive != pp->ucNumDeltaPocsOfRefRpsIdx)
            return -1;
        predict_set(r, s->delta_rps, s->used, s->use_delta, st);
    } else {
        *st = s->own;
    }

    for (i = 0; i < s->num_lt; i++) {
        unsigned int lsb, used;

        if (s->lt_sps[i]) {
            if (s->lt_idx[i] >= 32)
                return -1;
            lsb = t->lt_poc_lsb[s->lt_idx[i]];
            used = t->lt_used[s->lt_idx[i]];
        } else {
            lsb = s->lt_lsb[i];
            used = s->lt_used[i];
        }
        /* 7-52: DeltaPocMsbCycleLt accumulates, but restarts at 0 and at
         * num_long_term_sps. */
        if (i == 0 || i == s->num_lt_sps)
            cycle = s->lt_msb_cycle[i];
        else
            cycle += s->lt_msb_cycle[i];
        if (!used)
            continue;
        if (s->lt_msb_present[i]) {
            lt_poc[n] = pp->CurrPicOrderCntVal - (int)(cycle * max_lsb(pp)) - (int)(s->poc_lsb - lsb);
            lt_full[n] = 1;
        } else {
            lt_poc[n] = (int)lsb;
            lt_full[n] = 0;
        }
        n++;
    }
    *n_lt_used = n;
    return 0;
}

int v383_hevc_check_rps(const V383_DXVA_PicParams_HEVC *pp, const unsigned char *stream, unsigned int len,
                        const V383_HevcRpsTable *t, char *why, unsigned int why_len)
{
    unsigned int pos = 0, nal, nal_len, slice = 0;
    SliceRps s;

    msg(why, why_len, "", 0, 0);
    while (next_nal(stream, len, &pos, &nal, &nal_len)) {
        V383_HevcStRps st;
        int lt_poc[32];
        unsigned char lt_full[32];
        unsigned int n_lt = 0, i, k;
        int rc;

        if (nal_len < 3 || ((stream[nal] >> 1) & 0x3F) > 31)
            continue;
        rc = parse_slice(pp, stream + nal, nal_len, &s);
        slice++;
        if (rc < 0) {
            msg(why, why_len, "slice header unreadable, slice ", (int)slice, 1);
            return -1;
        }
        if (rc > 0 || s.idr)
            continue;
        if (slice_sets(pp, &s, t, &st, lt_poc, lt_full, &n_lt)) {
            msg(why, why_len, "the set predicted from does not have NumDeltaPocsOfRefRpsIdx pictures, slice ",
                (int)slice, 1);
            return -1;
        }
        for (k = 0, i = 0; i < st.num_negative; i++) {
            if (!st.used_s0[i])
                continue;
            if (k >= 8 || pp->RefPicSetStCurrBefore[k] == 0xFF || !ref_valid(pp, pp->RefPicSetStCurrBefore[k]) ||
                pp->PicOrderCntValList[pp->RefPicSetStCurrBefore[k]] - pp->CurrPicOrderCntVal != st.delta_poc_s0[i]) {
                msg(why, why_len, "StCurrBefore differs at delta ", st.delta_poc_s0[i], 1);
                return -1;
            }
            k++;
        }
        if (k < 8 && pp->RefPicSetStCurrBefore[k] != 0xFF) {
            msg(why, why_len, "StCurrBefore has more pictures than the slice derives: ", (int)k, 1);
            return -1;
        }
        for (k = 0, i = 0; i < st.num_positive; i++) {
            if (!st.used_s1[i])
                continue;
            if (k >= 8 || pp->RefPicSetStCurrAfter[k] == 0xFF || !ref_valid(pp, pp->RefPicSetStCurrAfter[k]) ||
                pp->PicOrderCntValList[pp->RefPicSetStCurrAfter[k]] - pp->CurrPicOrderCntVal != st.delta_poc_s1[i]) {
                msg(why, why_len, "StCurrAfter differs at delta ", st.delta_poc_s1[i], 1);
                return -1;
            }
            k++;
        }
        if (k < 8 && pp->RefPicSetStCurrAfter[k] != 0xFF) {
            msg(why, why_len, "StCurrAfter has more pictures than the slice derives: ", (int)k, 1);
            return -1;
        }
        for (k = 0; k < n_lt; k++) {
            unsigned int ri = (k < 8) ? pp->RefPicSetLtCurr[k] : 0xFF;
            int poc;

            if (ri == 0xFF || !ref_valid(pp, ri)) {
                msg(why, why_len, "LtCurr has fewer pictures than the slice derives: ", (int)k, 1);
                return -1;
            }
            poc = pp->PicOrderCntValList[ri];
            if (lt_full[k] ? (poc != lt_poc[k])
                           : (((unsigned int)poc & (max_lsb(pp) - 1)) != (unsigned int)lt_poc[k])) {
                msg(why, why_len, "LtCurr differs at POC ", poc, 1);
                return -1;
            }
        }
        if (n_lt < 8 && pp->RefPicSetLtCurr[n_lt] != 0xFF) {
            msg(why, why_len, "LtCurr has more pictures than the slice derives: ", (int)n_lt, 1);
            return -1;
        }
    }
    return 0;
}

/* --------------------------------------------------- auxiliary buffer */

static void build_rps_table(const V383_HevcRpsTable *t, unsigned char *out)
{
    BitPut bp;
    unsigned int i, j;

    bp_init(&bp, out, V383_HEVC_RPS_BYTES);
    for (i = 0; i < 32 && i < t->num_lt; i++) {
        bp.pos = i * 32;
        bp_put(&bp, t->lt_poc_lsb[i], 16);
        bp_put(&bp, t->lt_used[i], 1);
    }
    for (i = 0; i < 64 && i < t->num_st; i++) {
        const V383_HevcStRps *s = &t->st[i];

        bp.pos = 1024 + i * 384;
        bp_put(&bp, s->num_negative, 4);
        bp_put(&bp, s->num_positive, 4);
        for (j = 0; j < s->num_negative && j < 15; j++) {
            bp_put(&bp, (unsigned short)s->delta_poc_s0[j], 16);
            bp_put(&bp, s->used_s0[j], 1);
        }
        for (j = 0; j < s->num_positive && s->num_negative + j < 15; j++) {
            bp_put(&bp, (unsigned short)s->delta_poc_s1[j], 16);
            bp_put(&bp, s->used_s1[j], 1);
        }
    }
}

/* 6.5.3, up-right diagonal scan: position k of a blk x blk block. */
static void diag_scan(unsigned int blk, unsigned char *xs, unsigned char *ys)
{
    unsigned int i = 0;
    int x = 0, y = 0;

    while (i < blk * blk) {
        while (y >= 0) {
            if (x < (int)blk && y < (int)blk) {
                xs[i] = (unsigned char)x;
                ys[i] = (unsigned char)y;
                i++;
            }
            y--;
            x++;
        }
        y = x;
        x = 0;
    }
}

/* MPP hal_h265d_vdpu38x_scalinglist_packet() + hal_vdpu38x_record_scaling_list(). */
static void build_scaling(const V383_DXVA_Qmatrix_HEVC *qm, unsigned char *out)
{
    unsigned char sl[4][6][64], dc[2][6];
    unsigned char x4[16], y4[16], x8[64], y8[64];
    unsigned int size, list, i, bx, by;
    BitPut bp;

    diag_scan(4, x4, y4);
    diag_scan(8, x8, y8);
    v_zero(sl, sizeof(sl));
    for (list = 0; list < 6; list++) {
        for (i = 0; i < 16; i++)
            sl[0][list][4 * y4[i] + x4[i]] = qm->ucScalingLists0[list][i];
        for (i = 0; i < 64; i++) {
            unsigned int p = 8u * y8[i] + x8[i];
            sl[1][list][p] = qm->ucScalingLists1[list][i];
            sl[2][list][p] = qm->ucScalingLists2[list][i];
            if (list == 0)
                sl[3][list][p] = qm->ucScalingLists3[0][i];
            else if (list == 3)
                sl[3][list][p] = qm->ucScalingLists3[1][i];
            else
                sl[3][list][p] = qm->ucScalingLists2[list][i];
        }
        dc[0][list] = qm->ucScalingListDCCoefSizeID2[list];
        dc[1][list] = (list == 0) ? qm->ucScalingListDCCoefSizeID3[0]
                    : (list == 3) ? qm->ucScalingListDCCoefSizeID3[1]
                                  : qm->ucScalingListDCCoefSizeID2[list];
    }

    bp_init(&bp, out, V383_HEVC_SCALING_BYTES);
    /* 4x4, column by column */
    for (list = 0; list < 6; list++)
        for (i = 0; i < 4; i++) {
            bp_put(&bp, sl[0][list][i + 0], 8);
            bp_put(&bp, sl[0][list][i + 4], 8);
            bp_put(&bp, sl[0][list][i + 8], 8);
            bp_put(&bp, sl[0][list][i + 12], 8);
        }
    /* 8x8, 16x16, 32x32 (each an 8x8 matrix): 4x4 blocks, columns in each */
    for (size = 1; size < 4; size++)
        for (list = 0; list < 6; list++)
            for (bx = 0; bx < 8; bx += 4)
                for (by = 0; by < 8; by += 4) {
                    unsigned int p = by * 8 + bx;
                    for (i = 0; i < 4; i++) {
                        bp_put(&bp, sl[size][list][p + i + 0], 8);
                        bp_put(&bp, sl[size][list][p + i + 8], 8);
                        bp_put(&bp, sl[size][list][p + i + 16], 8);
                        bp_put(&bp, sl[size][list][p + i + 24], 8);
                    }
                }
    /* 4x4 again, row by row */
    for (list = 0; list < 6; list++)
        for (i = 0; i < 16; i++)
            bp_put(&bp, sl[0][list][i], 8);
    for (i = 0; i < 6; i++)
        bp_put(&bp, dc[0][i], 8);
    for (i = 0; i < 6; i++)
        bp_put(&bp, dc[1][i], 8);
}

/* MPP hal_h265d_vdpu38x_output_pps_packet(): SPS, PPS, POCs, tiles. */
static void build_global(const V383_DXVA_PicParams_HEVC *pp, unsigned int vps_id, unsigned int sps_id,
                         unsigned int pps_id, unsigned char *out)
{
    BitPut bp;
    unsigned int cb = log2_min_cb(pp), ctb = log2_ctb(pp);
    unsigned int width = v383_hevc_width(pp), height = v383_hevc_height(pp);
    unsigned int pcm = V383_HEVC_PCM_ENABLED(pp), tiles = V383_HEVC_TILES_ENABLED(pp);
    unsigned short colw[20], rowh[22];
    unsigned int i, ctbs_w = DIV_UP(width, 1u << ctb), ctbs_h = DIV_UP(height, 1u << ctb);

    bp_init(&bp, out, V383_HEVC_GBL_BYTES);

    /* SPS */
    bp_put(&bp, vps_id, 4);
    bp_put(&bp, sps_id, 4);
    bp_put(&bp, V383_HEVC_CHROMA_FORMAT_IDC(pp), 2);
    bp_put(&bp, width, 16);
    bp_put(&bp, height, 16);
    bp_put(&bp, V383_HEVC_BIT_DEPTH_LUMA_MINUS8(pp), 3);
    bp_put(&bp, V383_HEVC_BIT_DEPTH_CHROMA_MINUS8(pp), 3);
    bp_put(&bp, V383_HEVC_LOG2_MAX_POC_LSB_MINUS4(pp) + 4, 5);
    bp_put(&bp, pp->log2_diff_max_min_luma_coding_block_size, 2);
    bp_put(&bp, cb, 3);
    bp_put(&bp, pp->log2_min_transform_block_size_minus2 + 2u, 3);
    bp_put(&bp, pp->log2_diff_max_min_transform_block_size, 2);
    bp_put(&bp, pp->max_transform_hierarchy_depth_inter, 3);
    bp_put(&bp, pp->max_transform_hierarchy_depth_intra, 3);
    bp_put(&bp, V383_HEVC_SCALING_LIST_ENABLED(pp), 1);
    bp_put(&bp, V383_HEVC_AMP_ENABLED(pp), 1);
    bp_put(&bp, V383_HEVC_SAO_ENABLED(pp), 1);
    bp_put(&bp, pcm, 1);
    bp_put(&bp, pcm ? V383_HEVC_PCM_BD_LUMA_MINUS1(pp) + 1 : 0, 4);
    bp_put(&bp, pcm ? V383_HEVC_PCM_BD_CHROMA_MINUS1(pp) + 1 : 0, 4);
    bp_put(&bp, V383_HEVC_PCM_LOOP_FILTER_DISABLED(pp), 1);
    bp_put(&bp, V383_HEVC_LOG2_DIFF_MAX_MIN_PCM_CB(pp), 3);
    bp_put(&bp, pcm ? V383_HEVC_LOG2_MIN_PCM_CB_MINUS3(pp) + 3 : 0, 3);
    bp_put(&bp, pp->num_short_term_ref_pic_sets, 7);
    bp_put(&bp, V383_HEVC_LONG_TERM_REFS_PRESENT(pp), 1);
    bp_put(&bp, pp->num_long_term_ref_pics_sps, 6);
    bp_put(&bp, V383_HEVC_SPS_TEMPORAL_MVP(pp), 1);
    bp_put(&bp, V383_HEVC_STRONG_INTRA_SMOOTHING(pp), 1);
    /* range extension flags (bits 104..110): 0 for Main and Main 10 */
    bp_put(&bp, 0, 2);
    bp_put(&bp, V383_HEVC_MPP_BIT106 ? V383_HEVC_STRONG_INTRA_SMOOTHING(pp) : 0, 1);
    bp_put(&bp, 0, 4);
    bp_put(&bp, pp->sps_max_dec_pic_buffering_minus1, 4);
    bp_put(&bp, V383_HEVC_SEPARATE_COLOUR_PLANE(pp), 1);
    bp_put(&bp, 0, 1);                  /* high_precision_offsets_enabled_flag */
    bp_put(&bp, 0, 1);                  /* persistent_rice_adaptation_enabled_flag */

    /* PPS (bit 118) */
    bp_put(&bp, pps_id, 6);
    bp_put(&bp, sps_id, 4);
    bp_put(&bp, V383_HEVC_DEPENDENT_SLICES(pp), 1);
    bp_put(&bp, V383_HEVC_OUTPUT_FLAG_PRESENT(pp), 1);
    bp_put(&bp, V383_HEVC_NUM_EXTRA_SH_BITS(pp), 13);
    bp_put(&bp, V383_HEVC_SIGN_DATA_HIDING(pp), 1);
    bp_put(&bp, V383_HEVC_CABAC_INIT_PRESENT(pp), 1);
    bp_put(&bp, pp->num_ref_idx_l0_default_active_minus1 + 1u, 4);
    bp_put(&bp, pp->num_ref_idx_l1_default_active_minus1 + 1u, 4);
    bp_put(&bp, (unsigned int)(int)pp->init_qp_minus26, 7);
    bp_put(&bp, V383_HEVC_CONSTRAINED_INTRA_PRED(pp), 1);
    bp_put(&bp, V383_HEVC_TRANSFORM_SKIP(pp), 1);
    bp_put(&bp, V383_HEVC_CU_QP_DELTA(pp), 1);
    bp_put(&bp, ctb - pp->diff_cu_qp_delta_depth, 3);
    bp_put(&bp, (unsigned int)(int)pp->pps_cb_qp_offset, 5);
    bp_put(&bp, (unsigned int)(int)pp->pps_cr_qp_offset, 5);
    bp_put(&bp, V383_HEVC_SLICE_CHROMA_QP_OFFSETS(pp), 1);
    bp_put(&bp, V383_HEVC_WEIGHTED_PRED(pp), 1);
    bp_put(&bp, V383_HEVC_WEIGHTED_BIPRED(pp), 1);
    bp_put(&bp, V383_HEVC_TRANSQUANT_BYPASS(pp), 1);
    bp_put(&bp, tiles, 1);
    bp_put(&bp, V383_HEVC_ENTROPY_CODING_SYNC(pp), 1);
    bp_put(&bp, V383_HEVC_LF_ACROSS_SLICES(pp), 1);
    bp_put(&bp, V383_HEVC_LF_ACROSS_TILES(pp), 1);
    bp_put(&bp, V383_HEVC_DEBLOCKING_OVERRIDE(pp), 1);
    bp_put(&bp, V383_HEVC_DEBLOCKING_DISABLED(pp), 1);
    bp_put(&bp, (unsigned int)(int)pp->pps_beta_offset_div2, 4);
    bp_put(&bp, (unsigned int)(int)pp->pps_tc_offset_div2, 4);
    bp_put(&bp, V383_HEVC_LISTS_MODIFICATION(pp), 1);
    bp_put(&bp, pp->log2_parallel_merge_level_minus2 + 2u, 3);
    bp_put(&bp, V383_HEVC_SH_EXTENSION_PRESENT(pp), 1);
    bp_put(&bp, 0, 3);
    /* PPS range extension (bit 202): none; Log2MinCuChromaQpOffsetSize
     * with diff_cu_chroma_qp_offset_depth 0 */
    bp_put(&bp, 0, 2);
    bp_put(&bp, 0, 1);
    bp_put(&bp, 0, 1);
    bp_put(&bp, ctb, 3);
    bp_put(&bp, 0, 30);
    bp_put(&bp, 0, 30);
    bp_put(&bp, 0, 3);
    /* mvc (bit 272) */
    bp_put(&bp, 0xFFFF, 16);
    bp_put(&bp, 0, 9);

    /* POC (bit 297) */
    bp_put(&bp, 0, 3);
    bp_put(&bp, (unsigned int)pp->CurrPicOrderCntVal, 32);
    for (i = 0; i < 15; i++)
        bp_put(&bp, ref_valid(pp, i) ? (unsigned int)pp->PicOrderCntValList[i] : 0, 32);
    bp_put(&bp, 0, 32);
    for (i = 0; i < 15; i++)
        bp_put(&bp, ref_valid(pp, i) ? 1 : 0, 1);
    bp_put(&bp, 0, 1);

    /* tiles (bit 860) */
    v_zero(colw, sizeof(colw));
    v_zero(rowh, sizeof(rowh));
    if (tiles) {
        unsigned int nc = pp->num_tile_columns_minus1 + 1u, nr = pp->num_tile_rows_minus1 + 1u;

        if (nc > 20)
            nc = 20;
        if (nr > 22)
            nr = 22;
        if (V383_HEVC_UNIFORM_SPACING(pp)) {
            for (i = 0; i < nc; i++)
                colw[i] = (unsigned short)(((i + 1) * ctbs_w) / nc - (i * ctbs_w) / nc);
            for (i = 0; i < nr; i++)
                rowh[i] = (unsigned short)(((i + 1) * ctbs_h) / nr - (i * ctbs_h) / nr);
        } else {
            unsigned int sum = 0;
            for (i = 0; i + 1 < nc && i < 19; i++) {
                colw[i] = (unsigned short)(pp->column_width_minus1[i] + 1u);
                sum += colw[i];
            }
            colw[i] = (unsigned short)(ctbs_w - sum);
            sum = 0;
            for (i = 0; i + 1 < nr && i < 21; i++) {
                rowh[i] = (unsigned short)(pp->row_height_minus1[i] + 1u);
                sum += rowh[i];
            }
            rowh[i] = (unsigned short)(ctbs_h - sum);
        }
        bp_put(&bp, nc, 5);
        bp_put(&bp, nr, 5);
    } else {
        colw[0] = (unsigned short)ctbs_w;
        rowh[0] = (unsigned short)ctbs_h;
        bp_put(&bp, 1, 5);
        bp_put(&bp, 1, 5);
    }
    for (i = 0; i < 20; i++)
        bp_put(&bp, colw[i], 12);
    for (i = 0; i < 22; i++)
        bp_put(&bp, rowh[i], 12);
}

/* ------------------------------------------------------------- build */

int v383_hevc_build(const V383_HevcInput *in, V383_RegFile *rf)
{
    const V383_DXVA_PicParams_HEVC *pp = in->pp;
    const V383_HevcRpsTable *table;
    V383_HevcRpsTable *scratch = in->rebuild_scratch;
    unsigned int rcb[RCB_COUNT];
    unsigned int width, height, cur, i, off, vps_id = 0, sps_id = 0, pps_id;
    unsigned int err_ref, err_dist = 0xFFFFFFFFu, scaling;
    int rc;

    if (!pp || !in->aux_cpu || !in->surfaces || !rf || !in->stream_cpu)
        return V383_E_INVALID;
    if (V383_HEVC_CHROMA_FORMAT_IDC(pp) != 1 || V383_HEVC_BIT_DEPTH_LUMA_MINUS8(pp) ||
        V383_HEVC_BIT_DEPTH_CHROMA_MINUS8(pp) || V383_HEVC_SEPARATE_COLOUR_PLANE(pp))
        return V383_E_UNSUPPORTED;
    cur = V383_PIC_INDEX(pp->CurrPic);
    if (cur >= in->num_surfaces)
        return V383_E_INVALID;
    for (i = 0; i < 15; i++)
        if (ref_valid(pp, i) && V383_PIC_INDEX(pp->RefPicList[i]) >= in->num_surfaces)
            return V383_E_INVALID;
    if ((in->hor_stride & 15) || (in->ver_stride & 7))
        return V383_E_INVALID;
    scaling = V383_HEVC_SCALING_LIST_ENABLED(pp);
    if (scaling && !in->qm)
        return V383_E_INVALID;

    width = v383_hevc_width(pp);
    height = v383_hevc_height(pp);

    if (in->extra && in->extra->valid) {
        table = &in->extra->rps;
        vps_id = in->extra->vps_id;
        sps_id = in->extra->sps_id;
        pps_id = in->extra->pps_id;
    } else {
        int id = v383_hevc_parse_pps_id(pp, in->stream_cpu, in->stream_len);
        if (id < 0 || id > 63)
            return V383_E_STREAM;
        pps_id = (unsigned int)id;
        if (!scratch)
            return V383_E_INVALID;
        rc = v383_hevc_rebuild_rps(pp, in->stream_cpu, in->stream_len, scratch);
        if (rc)
            return rc;
        table = scratch;
    }

    /* ---- auxiliary buffer */
    v_zero(in->aux_cpu, V383_HEVC_AUX_CABAC_OFF);
    build_global(pp, vps_id, sps_id, pps_id, in->aux_cpu + V383_HEVC_AUX_GBL_OFF);
    build_rps_table(table, in->aux_cpu + V383_HEVC_AUX_RPS_OFF);
    if (scaling)
        build_scaling(in->qm, in->aux_cpu + V383_HEVC_AUX_SCALING_OFF);
    v_copy(in->aux_cpu + V383_HEVC_AUX_CABAC_OFF, v383_hevc_cabac_table, V383_HEVC_CABAC_BYTES);

    /* ---- registers */
    v_zero(rf, sizeof(*rf));

    /* vdpu383_init_ctrl_regs(MPP_VIDEO_CodingHEVC) */
    reg_set(rf, 8, 0);                          /* dec_mode: HEVC */
    reg_set(rf, 9, 0);                          /* raster, no FBC, no scale-down, buf_empty_en 0 */
    reg_set(rf, 10, 0x3FF);                     /* every block auto-gating enable */
    reg_set(rf, 13, 0xFFFFFF);                  /* core timeout */
    reg_set(rf, 16, 1);                         /* error_proc_disable */
    reg_set(rf, 20, 0xFFFFFFFF);                /* cabac error enable, low */
    reg_set(rf, 21, 0x3FF3F9FF);                /* cabac error enable, high */
    /* vdpu383_setup_statistic() */
    reg_set(rf, 28, 1u | (1u << 3) | (11u << 4));
    reg_set(rf, 29, 1u | (1u << 3) | (17u << 4));
    reg_set(rf, 30, 0);
    rf->core_timeout = rf->regs[13];

    reg_set(rf, 66, v383_hevc_stream_read_len(in->stream_len));
    reg_set(rf, 67, V383_HEVC_GBL_LEN);

    /* vdpu383_setup_cur_stride_info(), raster */
    reg_set(rf, 68, in->hor_stride >> 4);
    reg_set(rf, 69, in->hor_stride >> 4);
    reg_set(rf, 70, (in->hor_stride * in->ver_stride) >> 4);
    reg_set(rf, 80, rf->regs[68]);
    reg_set(rf, 81, rf->regs[69]);
    reg_set(rf, 82, rf->regs[70]);

    reg_set(rf, 128, in->stream_dma);
    reg_set(rf, 129, in->aux_dma + V383_HEVC_AUX_RPS_OFF);
    reg_set(rf, 130, in->aux_dma + V383_HEVC_AUX_CABAC_OFF);
    reg_set(rf, 131, in->aux_dma + V383_HEVC_AUX_GBL_OFF);
    reg_set(rf, 132, scaling ? in->aux_dma + V383_HEVC_AUX_SCALING_OFF : 0);

    rcb_sizes(width, height, rcb);
    off = 0;
    for (i = 0; i < RCB_COUNT; i++) {
        reg_set(rf, 140 + 2 * i, in->rcb_dma + off);
        reg_set(rf, 141 + 2 * i, rcb[i]);
        off += rcb[i];
    }

    /* Current picture */
    reg_set(rf, 168, in->surfaces[cur].dma);
    reg_set(rf, 192, in->surfaces[cur].dma);
    reg_set(rf, 216, in->surfaces[cur].colmv_dma);

    /* The error reference: the valid reference closest in POC, else the
     * current picture (MPP). */
    err_ref = cur;
    for (i = 0; i < 15; i++) {
        int d;
        unsigned int ad;

        if (!ref_valid(pp, i))
            continue;
        d = pp->PicOrderCntValList[i] - pp->CurrPicOrderCntVal;
        ad = (d < 0) ? (unsigned int)(-d) : (unsigned int)d;
        if (ad < err_dist) {
            err_dist = ad;
            err_ref = V383_PIC_INDEX(pp->RefPicList[i]);
        }
    }
    reg_set(rf, 169, in->surfaces[err_ref].dma);
    for (i = 0; i < 16; i++) {
        unsigned int s = (i < 15 && ref_valid(pp, i)) ? V383_PIC_INDEX(pp->RefPicList[i]) : err_ref;

        reg_set(rf, 170 + i, in->surfaces[s].dma);
        reg_set(rf, 195 + i, in->surfaces[s].dma);
        reg_set(rf, 217 + i, in->surfaces[s].colmv_dma);
    }

    mask_range(rf, 8, 30);
    mask_range(rf, 64, 106);
    mask_range(rf, 128, 232);
    return V383_OK;
}
