/*
 * hevcparse.c - an HEVC Annex B parser for vdpu383test (see hevcparse.h)
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "hevcparse.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- bit reader */

typedef struct Rb {
    uint8_t *b;
    size_t   n, pos;
    int      err;
} Rb;

/* The RBSP of a NAL unit: emulation prevention bytes removed. */
static int rb_init(Rb *r, const uint8_t *nal, size_t len)
{
    size_t i, zeros = 0;

    r->b = (uint8_t *)malloc(len + 8);
    r->n = 0;
    r->pos = 0;
    r->err = 0;
    if (!r->b)
        return -1;
    for (i = 0; i < len; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        r->b[r->n++] = nal[i];
    }
    memset(r->b + r->n, 0, 8);
    return 0;
}

static void rb_free(Rb *r)
{
    free(r->b);
    r->b = NULL;
}

static unsigned u(Rb *r, unsigned bits)
{
    unsigned v = 0;

    while (bits--) {
        if (r->pos >= r->n * 8) {
            r->err = 1;
            return 0;
        }
        v = (v << 1) | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}

static unsigned ue(Rb *r)
{
    unsigned lz = 0;

    while (!u(r, 1)) {
        if (r->err || ++lz > 31) {
            r->err = 1;
            return 0;
        }
    }
    return lz ? ((1u << lz) - 1 + u(r, lz)) : 0;
}

static int se(Rb *r)
{
    unsigned v = ue(r);
    return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

static unsigned ceil_log2(unsigned v)
{
    unsigned n = 0;
    while ((1u << n) < v)
        n++;
    return n;
}

static int fail(HevcParser *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    return -1;
}

/* -------------------------------------------------------- scaling lists */

static const uint8_t default_intra[64] = {    /* raster, Table 7-6 */
    16, 16, 16, 16, 17, 18, 21, 24,
    16, 16, 16, 16, 17, 19, 22, 25,
    16, 16, 17, 18, 20, 22, 25, 29,
    16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47,
    18, 19, 22, 27, 35, 44, 54, 65,
    21, 22, 25, 31, 41, 54, 70, 88,
    24, 25, 29, 36, 47, 65, 88, 115,
};
static const uint8_t default_inter[64] = {
    16, 16, 16, 16, 17, 18, 20, 24,
    16, 16, 16, 17, 18, 20, 24, 25,
    16, 16, 17, 18, 20, 24, 25, 28,
    16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41,
    18, 20, 24, 25, 28, 33, 41, 54,
    20, 24, 25, 28, 33, 41, 54, 71,
    24, 25, 28, 33, 41, 54, 71, 91,
};

static uint8_t diag4[16], diag8[64];    /* scan position -> raster index */

static void make_diag(unsigned blk, uint8_t *out)
{
    unsigned i = 0;
    int x = 0, y = 0;

    while (i < blk * blk) {
        while (y >= 0) {
            if (x < (int)blk && y < (int)blk)
                out[i++] = (uint8_t)(y * (int)blk + x);
            y--;
            x++;
        }
        y = x;
        x = 0;
    }
}

static void default_lists(uint8_t sl[4][6][64], uint8_t dc[2][6])
{
    unsigned s, m;

    for (m = 0; m < 6; m++) {
        memset(sl[0][m], 16, 16);
        for (s = 1; s < 4; s++)
            memcpy(sl[s][m], m < 3 ? default_intra : default_inter, 64);
        dc[0][m] = dc[1][m] = 16;
    }
}

/* 7.3.4 scaling_list_data(), into raster order. */
static int scaling_list_data(Rb *r, uint8_t sl[4][6][64], uint8_t dc[2][6])
{
    unsigned s, m, i;

    default_lists(sl, dc);
    for (s = 0; s < 4; s++) {
        unsigned step = (s == 3) ? 3 : 1;
        for (m = 0; m < 6; m += step) {
            if (!u(r, 1)) {                         /* scaling_list_pred_mode_flag */
                unsigned delta = ue(r) * step;      /* scaling_list_pred_matrix_id_delta */
                if (delta > m)
                    return -1;
                if (delta) {
                    memcpy(sl[s][m], sl[s][m - delta], s ? 64 : 16);
                    if (s > 1)
                        dc[s - 2][m] = dc[s - 2][m - delta];
                } else {
                    /* the default list, already there */
                    if (s > 1)
                        dc[s - 2][m] = 16;
                }
            } else {
                int next = 8;
                unsigned n = s ? 64 : 16;
                if (s > 1) {
                    int d = se(r);
                    if (d < -7 || d > 247)
                        return -1;
                    next = d + 8;
                    dc[s - 2][m] = (uint8_t)next;
                }
                for (i = 0; i < n; i++) {
                    int delta = se(r);
                    next = (next + delta + 256) % 256;
                    sl[s][m][s ? diag8[i] : diag4[i]] = (uint8_t)next;
                }
            }
        }
    }
    /* 32x32 chroma (4:4:4 only) from 16x16; harmless otherwise */
    for (m = 1; m < 6; m++)
        if (m != 3) {
            memcpy(sl[3][m], sl[2][m], 64);
            dc[1][m] = dc[0][m];
        }
    return r->err ? -1 : 0;
}

/* --------------------------------------------- reference picture sets */

/* 7.3.7 st_ref_pic_set(idx), 7.4.8. sets[] holds the SPS's sets 0..idx-1;
 * num_st is num_short_term_ref_pic_sets. In a slice, idx == num_st.
 * *ref_num_delta: NumDeltaPocs[RefRpsIdx] when predicted, else 0. */
static int st_ref_pic_set(Rb *r, unsigned idx, unsigned num_st, const V383_HevcStRps *sets,
                          V383_HevcStRps *out, unsigned *ref_num_delta)
{
    unsigned inter = idx ? u(r, 1) : 0, i;
    int poc;

    memset(out, 0, sizeof(*out));
    *ref_num_delta = 0;
    if (inter) {
        unsigned delta_idx = (idx == num_st) ? ue(r) + 1 : 1;
        unsigned sign, abs_m1, n, nneg, npos;
        const V383_HevcStRps *ref;
        unsigned char used[17], use_delta[17];
        int drps, j, d;

        if (delta_idx > idx)
            return -1;
        ref = &sets[idx - delta_idx];
        sign = u(r, 1);
        abs_m1 = ue(r);
        drps = sign ? -(int)(abs_m1 + 1) : (int)(abs_m1 + 1);
        nneg = ref->num_negative;
        npos = ref->num_positive;
        n = nneg + npos;
        *ref_num_delta = n;
        for (i = 0; i <= n; i++) {
            used[i] = (unsigned char)u(r, 1);
            use_delta[i] = used[i] ? 1 : (unsigned char)u(r, 1);
        }
        /* 7-61 */
        i = 0;
        for (j = (int)npos - 1; j >= 0; j--) {
            d = ref->delta_poc_s1[j] + drps;
            if (d < 0 && use_delta[nneg + j] && i < 16) {
                out->delta_poc_s0[i] = (short)d;
                out->used_s0[i++] = used[nneg + j];
            }
        }
        if (drps < 0 && use_delta[n] && i < 16) {
            out->delta_poc_s0[i] = (short)drps;
            out->used_s0[i++] = used[n];
        }
        for (j = 0; j < (int)nneg; j++) {
            d = ref->delta_poc_s0[j] + drps;
            if (d < 0 && use_delta[j] && i < 16) {
                out->delta_poc_s0[i] = (short)d;
                out->used_s0[i++] = used[j];
            }
        }
        out->num_negative = (unsigned char)i;
        /* 7-62 */
        i = 0;
        for (j = (int)nneg - 1; j >= 0; j--) {
            d = ref->delta_poc_s0[j] + drps;
            if (d > 0 && use_delta[j] && i < 16) {
                out->delta_poc_s1[i] = (short)d;
                out->used_s1[i++] = used[j];
            }
        }
        if (drps > 0 && use_delta[n] && i < 16) {
            out->delta_poc_s1[i] = (short)drps;
            out->used_s1[i++] = used[n];
        }
        for (j = 0; j < (int)npos; j++) {
            d = ref->delta_poc_s1[j] + drps;
            if (d > 0 && use_delta[nneg + j] && i < 16) {
                out->delta_poc_s1[i] = (short)d;
                out->used_s1[i++] = used[nneg + j];
            }
        }
        out->num_positive = (unsigned char)i;
    } else {
        unsigned nneg = ue(r), npos = ue(r);
        if (nneg > 16 || npos > 16 || nneg + npos > 16)
            return -1;
        out->num_negative = (unsigned char)nneg;
        out->num_positive = (unsigned char)npos;
        poc = 0;
        for (i = 0; i < nneg; i++) {
            poc -= (int)ue(r) + 1;
            out->delta_poc_s0[i] = (short)poc;
            out->used_s0[i] = (unsigned char)u(r, 1);
        }
        poc = 0;
        for (i = 0; i < npos; i++) {
            poc += (int)ue(r) + 1;
            out->delta_poc_s1[i] = (short)poc;
            out->used_s1[i] = (unsigned char)u(r, 1);
        }
    }
    return r->err ? -1 : 0;
}

/* ------------------------------------------------------------ SPS, PPS */

static void profile_tier_level(Rb *r, unsigned max_sub_layers_m1)
{
    unsigned i, pp[8] = { 0 }, lp[8] = { 0 };

    u(r, 8);            /* profile_space, tier, profile_idc */
    u(r, 32);           /* compatibility flags */
    u(r, 4);            /* progressive, interlaced, non_packed, frame_only */
    u(r, 32);
    u(r, 11);           /* 43 reserved bits */
    u(r, 1);
    u(r, 8);            /* general_level_idc */
    for (i = 0; i < max_sub_layers_m1; i++) {
        pp[i] = u(r, 1);
        lp[i] = u(r, 1);
    }
    if (max_sub_layers_m1)
        for (i = max_sub_layers_m1; i < 8; i++)
            u(r, 2);
    for (i = 0; i < max_sub_layers_m1; i++) {
        if (pp[i]) {
            u(r, 32);
            u(r, 32);
            u(r, 24);
        }
        if (lp[i])
            u(r, 8);
    }
}

static int parse_sps(HevcParser *p, Rb *r)
{
    HevcSps s;
    unsigned msl_m1, i, ordering, sub_w, sub_h, dummy;

    memset(&s, 0, sizeof(s));
    s.vps_id = u(r, 4);
    msl_m1 = u(r, 3);
    s.max_sub_layers = msl_m1 + 1;
    u(r, 1);
    profile_tier_level(r, msl_m1);
    s.sps_id = ue(r);
    if (s.sps_id > 15)
        return fail(p, "sps id %u", s.sps_id);
    s.chroma_format_idc = ue(r);
    if (s.chroma_format_idc == 3)
        s.separate_colour_plane_flag = u(r, 1);
    s.width = ue(r);
    s.height = ue(r);
    sub_w = (s.chroma_format_idc == 1 || s.chroma_format_idc == 2) ? 2 : 1;
    sub_h = (s.chroma_format_idc == 1) ? 2 : 1;
    if (u(r, 1)) {
        s.crop_left = ue(r) * sub_w;
        s.crop_right = ue(r) * sub_w;
        s.crop_top = ue(r) * sub_h;
        s.crop_bottom = ue(r) * sub_h;
    }
    s.bit_depth_luma_minus8 = ue(r);
    s.bit_depth_chroma_minus8 = ue(r);
    s.log2_max_poc_lsb = ue(r) + 4;
    if (s.log2_max_poc_lsb > 16)
        return fail(p, "log2_max_pic_order_cnt_lsb %u", s.log2_max_poc_lsb);
    ordering = u(r, 1);
    for (i = ordering ? 0 : msl_m1; i <= msl_m1; i++) {
        s.max_dec_pic_buffering[i] = ue(r) + 1;
        s.max_num_reorder[i] = ue(r);
        s.max_latency_plus1[i] = ue(r);
    }
    if (!ordering)
        for (i = 0; i < msl_m1; i++) {
            s.max_dec_pic_buffering[i] = s.max_dec_pic_buffering[msl_m1];
            s.max_num_reorder[i] = s.max_num_reorder[msl_m1];
            s.max_latency_plus1[i] = s.max_latency_plus1[msl_m1];
        }
    s.log2_min_cb = ue(r) + 3;
    s.log2_diff_cb = ue(r);
    s.log2_min_tb = ue(r) + 2;
    s.log2_diff_tb = ue(r);
    s.max_th_depth_inter = ue(r);
    s.max_th_depth_intra = ue(r);
    s.scaling_list_enabled = u(r, 1);
    default_lists(s.sl, s.sl_dc);
    if (s.scaling_list_enabled) {
        s.scaling_list_data_present = u(r, 1);
        if (s.scaling_list_data_present && scaling_list_data(r, s.sl, s.sl_dc))
            return fail(p, "SPS scaling list");
    }
    s.amp = u(r, 1);
    s.sao = u(r, 1);
    s.pcm = u(r, 1);
    if (s.pcm) {
        s.pcm_bd_luma = u(r, 4) + 1;
        s.pcm_bd_chroma = u(r, 4) + 1;
        s.log2_min_pcm = ue(r) + 3;
        s.log2_diff_pcm = ue(r);
        s.pcm_loop_filter_disabled = u(r, 1);
    }
    s.num_st = ue(r);
    if (s.num_st > 64)
        return fail(p, "num_short_term_ref_pic_sets %u", s.num_st);
    for (i = 0; i < s.num_st; i++)
        if (st_ref_pic_set(r, i, s.num_st, s.st, &s.st[i], &dummy))
            return fail(p, "SPS st_ref_pic_set %u", i);
    s.lt_present = u(r, 1);
    if (s.lt_present) {
        s.num_lt_sps = ue(r);
        if (s.num_lt_sps > 32)
            return fail(p, "num_long_term_ref_pics_sps %u", s.num_lt_sps);
        for (i = 0; i < s.num_lt_sps; i++) {
            s.lt_lsb[i] = u(r, s.log2_max_poc_lsb);
            s.lt_used[i] = u(r, 1);
        }
    }
    s.temporal_mvp = u(r, 1);
    s.strong_intra = u(r, 1);
    if (r->err)
        return fail(p, "SPS %u truncated", s.sps_id);
    s.valid = 1;
    p->sps[s.sps_id] = s;
    return 0;
}

static int parse_pps(HevcParser *p, Rb *r)
{
    HevcPps s;
    unsigned i;

    memset(&s, 0, sizeof(s));
    s.pps_id = ue(r);
    s.sps_id = ue(r);
    if (s.pps_id > 63 || s.sps_id > 15)
        return fail(p, "pps id %u, sps id %u", s.pps_id, s.sps_id);
    s.dependent_slices = u(r, 1);
    s.output_flag_present = u(r, 1);
    s.num_extra_bits = u(r, 3);
    s.sign_data_hiding = u(r, 1);
    s.cabac_init_present = u(r, 1);
    s.num_ref_idx_l0_m1 = ue(r);
    s.num_ref_idx_l1_m1 = ue(r);
    s.init_qp_m26 = se(r);
    s.constrained_intra = u(r, 1);
    s.transform_skip = u(r, 1);
    s.cu_qp_delta = u(r, 1);
    if (s.cu_qp_delta)
        s.diff_cu_qp_delta_depth = ue(r);
    s.cb_qp_offset = se(r);
    s.cr_qp_offset = se(r);
    s.slice_chroma_qp_offsets = u(r, 1);
    s.weighted_pred = u(r, 1);
    s.weighted_bipred = u(r, 1);
    s.transquant_bypass = u(r, 1);
    s.tiles = u(r, 1);
    s.entropy_sync = u(r, 1);
    s.lf_across_tiles = 1;
    if (s.tiles) {
        s.num_tile_cols_m1 = ue(r);
        s.num_tile_rows_m1 = ue(r);
        if (s.num_tile_cols_m1 > 19 || s.num_tile_rows_m1 > 21)
            return fail(p, "tiles %u x %u", s.num_tile_cols_m1 + 1, s.num_tile_rows_m1 + 1);
        s.uniform = u(r, 1);
        if (!s.uniform) {
            for (i = 0; i < s.num_tile_cols_m1; i++)
                s.col_w_m1[i] = ue(r);
            for (i = 0; i < s.num_tile_rows_m1; i++)
                s.row_h_m1[i] = ue(r);
        }
        s.lf_across_tiles = u(r, 1);
    }
    s.lf_across_slices = u(r, 1);
    if (u(r, 1)) {                      /* deblocking_filter_control_present_flag */
        s.deblocking_override = u(r, 1);
        s.deblocking_disabled = u(r, 1);
        if (!s.deblocking_disabled) {
            s.beta_div2 = se(r);
            s.tc_div2 = se(r);
        }
    }
    s.scaling_list_data_present = u(r, 1);
    if (s.scaling_list_data_present && scaling_list_data(r, s.sl, s.sl_dc))
        return fail(p, "PPS scaling list");
    s.lists_modification = u(r, 1);
    s.log2_parallel_merge_m2 = ue(r);
    s.sh_ext_present = u(r, 1);
    if (r->err)
        return fail(p, "PPS %u truncated", s.pps_id);
    s.valid = 1;
    p->pps[s.pps_id] = s;
    return 0;
}

/* -------------------------------------------------------- slice header */

typedef struct Slice {
    unsigned nal_type, tid, first, no_output_prior, pps_id, dependent, slice_type, output_flag;
    unsigned poc_lsb;
    unsigned sps_flag, st_idx, st_bits, ref_num_delta;
    V383_HevcStRps st;
    unsigned num_lt_sps, num_lt;
    unsigned lt_sps[32], lt_idx[32], lt_lsb[32], lt_used[32], lt_msb_present[32], lt_msb_cycle[32];
} Slice;

static int is_irap(unsigned t) { return t >= 16 && t <= 23; }
static int is_idr(unsigned t)  { return t == 19 || t == 20; }
static int is_rasl(unsigned t) { return t == 8 || t == 9; }

static int parse_slice(HevcParser *p, Rb *r, Slice *s)
{
    const HevcPps *pps;
    const HevcSps *sps;
    unsigned i, ctb, ctbs, start;

    memset(s, 0, sizeof(*s));
    u(r, 1);
    s->nal_type = u(r, 6);
    u(r, 6);
    s->tid = u(r, 3) - 1;
    s->first = u(r, 1);
    if (is_irap(s->nal_type))
        s->no_output_prior = u(r, 1);
    s->pps_id = ue(r);
    if (s->pps_id > 63 || !p->pps[s->pps_id].valid)
        return fail(p, "slice names PPS %u, which is not there", s->pps_id);
    pps = &p->pps[s->pps_id];
    sps = &p->sps[pps->sps_id];
    if (!sps->valid)
        return fail(p, "PPS %u names SPS %u, which is not there", s->pps_id, pps->sps_id);
    s->output_flag = 1;
    if (!s->first) {
        if (pps->dependent_slices)
            s->dependent = u(r, 1);
        ctb = sps->log2_min_cb + sps->log2_diff_cb;
        ctbs = ((sps->width + (1u << ctb) - 1) >> ctb) * ((sps->height + (1u << ctb) - 1) >> ctb);
        u(r, ceil_log2(ctbs));
    }
    if (s->dependent)
        return r->err ? fail(p, "slice header truncated") : 0;
    u(r, pps->num_extra_bits);
    s->slice_type = ue(r);
    if (pps->output_flag_present)
        s->output_flag = u(r, 1);
    if (sps->separate_colour_plane_flag)
        u(r, 2);
    if (!is_idr(s->nal_type)) {
        s->poc_lsb = u(r, sps->log2_max_poc_lsb);
        s->sps_flag = u(r, 1);
        if (!s->sps_flag) {
            start = (unsigned)r->pos;
            if (st_ref_pic_set(r, sps->num_st, sps->num_st, sps->st, &s->st, &s->ref_num_delta))
                return fail(p, "slice st_ref_pic_set");
            s->st_bits = (unsigned)r->pos - start;
        } else {
            if (sps->num_st == 0)
                return fail(p, "slice names an SPS set, the SPS has none");
            if (sps->num_st > 1)
                s->st_idx = u(r, ceil_log2(sps->num_st));
            if (s->st_idx >= sps->num_st)
                return fail(p, "short_term_ref_pic_set_idx %u", s->st_idx);
            s->st = sps->st[s->st_idx];
        }
        if (sps->lt_present) {
            if (sps->num_lt_sps)
                s->num_lt_sps = ue(r);
            s->num_lt = s->num_lt_sps + ue(r);
            if (s->num_lt > 32 || s->num_lt_sps > sps->num_lt_sps)
                return fail(p, "long-term pictures %u", s->num_lt);
            for (i = 0; i < s->num_lt; i++) {
                if (i < s->num_lt_sps) {
                    s->lt_sps[i] = 1;
                    if (sps->num_lt_sps > 1)
                        s->lt_idx[i] = u(r, ceil_log2(sps->num_lt_sps));
                    s->lt_lsb[i] = sps->lt_lsb[s->lt_idx[i]];
                    s->lt_used[i] = sps->lt_used[s->lt_idx[i]];
                } else {
                    s->lt_lsb[i] = u(r, sps->log2_max_poc_lsb);
                    s->lt_used[i] = u(r, 1);
                }
                s->lt_msb_present[i] = u(r, 1);
                if (s->lt_msb_present[i])
                    s->lt_msb_cycle[i] = ue(r);
            }
        }
    }
    return r->err ? fail(p, "slice header truncated") : 0;
}

/* ----------------------------------------------------------------- DPB */

static void release(HevcParser *p, HevcFrame *f)
{
    if (f->in_use && !f->ref && !f->needed_for_output) {
        p->slot_busy[f->slot] = 0;
        f->in_use = 0;
    }
}

/* C.5.2.4: output the picture with the smallest POC. 0 if none. */
static int bump(HevcParser *p)
{
    HevcFrame *best = NULL;
    unsigned i;

    for (i = 0; i < HEVC_DPB_SIZE; i++) {
        HevcFrame *f = &p->dpb[i];
        if (f->in_use && f->needed_for_output && f != p->cur && (!best || f->poc < best->poc))
            best = f;
    }
    if (!best)
        return 0;
    if (p->out_fn)
        p->out_fn(p->out_ctx, best->slot, best->decode_index, best->poc);
    best->needed_for_output = 0;
    release(p, best);
    return 1;
}

static unsigned count_output(HevcParser *p)
{
    unsigned i, n = 0;
    for (i = 0; i < HEVC_DPB_SIZE; i++)
        if (p->dpb[i].in_use && p->dpb[i].needed_for_output && &p->dpb[i] != p->cur)
            n++;
    return n;
}

static unsigned count_in_use(HevcParser *p)
{
    unsigned i, n = 0;
    for (i = 0; i < HEVC_DPB_SIZE; i++)
        if (p->dpb[i].in_use && &p->dpb[i] != p->cur)
            n++;
    return n;
}

static HevcFrame *find_poc(HevcParser *p, int poc, int lsb_only, unsigned mask)
{
    unsigned i;
    for (i = 0; i < HEVC_DPB_SIZE; i++) {
        HevcFrame *f = &p->dpb[i];
        if (!f->in_use || !f->ref)
            continue;
        if (lsb_only ? (((unsigned)f->poc & mask) == (unsigned)poc) : (f->poc == poc))
            return f;
    }
    return NULL;
}

/* ------------------------------------------------------------- picture */

static int au_append(HevcParser *p, const uint8_t *nal, size_t len)
{
    if (p->au_len + len + 3 > p->au_cap) {
        size_t cap = (p->au_len + len + 3) * 2 + 65536;
        uint8_t *n = (uint8_t *)realloc(p->au, cap);
        if (!n)
            return fail(p, "out of memory");
        p->au = n;
        p->au_cap = cap;
    }
    p->au[p->au_len++] = 0;
    p->au[p->au_len++] = 0;
    p->au[p->au_len++] = 1;
    memcpy(p->au + p->au_len, nal, len);
    p->au_len += len;
    return 0;
}

static int finish_picture(HevcParser *p)
{
    HevcFrame *cur = p->cur;
    unsigned i;
    int rc = 0;

    if (!p->open)
        return 0;
    p->open = 0;
    if (p->skipping) {
        p->skipping = 0;
        return 0;
    }
    p->pic.stream = p->au;
    p->pic.stream_len = p->au_len;
    if (p->pic.slice_types == 4)        /* IntraPicFlag: every slice I */
        p->pic.pp.dwCodingSettingPicturePropertyFlags |= 1u << 18;
    if (p->dec_fn)
        rc = p->dec_fn(p->dec_ctx, &p->pic);
    p->decode_index++;

    /* C.5.2.3: the current picture is a short-term reference now, and
     * waits for output. */
    cur->ref = 1;
    p->cur = NULL;
    for (i = 0; i < HEVC_DPB_SIZE; i++)
        if (p->dpb[i].in_use && p->dpb[i].needed_for_output && &p->dpb[i] != cur)
            p->dpb[i].latency++;
    while (count_output(p) > p->max_reorder)
        if (!bump(p))
            break;
    if (p->max_latency)
        for (;;) {
            int late = 0;
            for (i = 0; i < HEVC_DPB_SIZE; i++)
                if (p->dpb[i].in_use && p->dpb[i].needed_for_output && p->dpb[i].latency >= p->max_latency)
                    late = 1;
            if (!late || !bump(p))
                break;
        }
    return rc ? fail(p, "decode of picture %u failed", p->decode_index - 1) : 0;
}

static void fill_qm(V383_DXVA_Qmatrix_HEVC *qm, const uint8_t sl[4][6][64], const uint8_t dc[2][6])
{
    unsigned m, i;

    for (m = 0; m < 6; m++) {
        for (i = 0; i < 16; i++)
            qm->ucScalingLists0[m][i] = sl[0][m][diag4[i]];
        for (i = 0; i < 64; i++) {
            qm->ucScalingLists1[m][i] = sl[1][m][diag8[i]];
            qm->ucScalingLists2[m][i] = sl[2][m][diag8[i]];
        }
        qm->ucScalingListDCCoefSizeID2[m] = dc[0][m];
    }
    for (i = 0; i < 64; i++) {
        qm->ucScalingLists3[0][i] = sl[3][0][diag8[i]];
        qm->ucScalingLists3[1][i] = sl[3][3][diag8[i]];
    }
    qm->ucScalingListDCCoefSizeID3[0] = dc[1][0];
    qm->ucScalingListDCCoefSizeID3[1] = dc[1][3];
}

static int start_picture(HevcParser *p, const Slice *s)
{
    const HevcPps *pps = &p->pps[s->pps_id];
    const HevcSps *sps = &p->sps[pps->sps_id];
    V383_DXVA_PicParams_HEVC *pp = &p->pic.pp;
    unsigned htid = sps->max_sub_layers - 1, i, k, n;
    unsigned mask = (1u << sps->log2_max_poc_lsb) - 1;
    int no_rasl, poc, msb;
    HevcFrame *before[16], *after[16], *lt[16], *foll[32];
    unsigned nb = 0, na = 0, nl = 0, nf = 0;
    int ref_index[HEVC_DPB_SIZE];

    if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8)
        return fail(p, "not 8-bit 4:2:0 (chroma format %u, bit depth %u)", sps->chroma_format_idc,
                    sps->bit_depth_luma_minus8 + 8);

    /* NoRaslOutputFlag, and the RASL pictures of such an IRAP are skipped */
    no_rasl = is_idr(s->nal_type) || (s->nal_type >= 16 && s->nal_type <= 18) || p->first_picture || p->after_eos;
    if (is_irap(s->nal_type))
        p->skip_rasl = no_rasl;
    if (is_rasl(s->nal_type) && p->skip_rasl) {
        p->open = 1;
        p->skipping = 1;
        p->skipped++;
        return 0;
    }

    /* 8.3.1 picture order count */
    if (is_irap(s->nal_type) && no_rasl) {
        msb = 0;
    } else {
        int prev_lsb = (int)((unsigned)p->prev_tid0_poc & mask);
        int prev_msb = p->prev_tid0_poc - prev_lsb;
        int max = (int)mask + 1, lsb = (int)s->poc_lsb;
        if (lsb < prev_lsb && prev_lsb - lsb >= max / 2)
            msb = prev_msb + max;
        else if (lsb > prev_lsb && lsb - prev_lsb > max / 2)
            msb = prev_msb - max;
        else
            msb = prev_msb;
    }
    poc = msb + (int)s->poc_lsb;
    if (s->tid == 0 && !(s->nal_type >= 6 && s->nal_type <= 9) &&
        !(s->nal_type <= 14 && !(s->nal_type & 1)))
        p->prev_tid0_poc = poc;

    /* 8.3.2 reference picture set, and marking */
    if (is_idr(s->nal_type)) {
        for (i = 0; i < HEVC_DPB_SIZE; i++)
            p->dpb[i].ref = 0;
    } else {
        int keep[HEVC_DPB_SIZE];
        unsigned cycle = 0;

        memset(keep, 0, sizeof(keep));
        /* long-term first: a picture may move from short- to long-term */
        for (i = 0; i < s->num_lt; i++) {
            int full = s->lt_msb_present[i], v;
            HevcFrame *f;

            if (i == 0 || i == s->num_lt_sps)
                cycle = s->lt_msb_cycle[i];
            else
                cycle += s->lt_msb_cycle[i];
            v = full ? poc - (int)(cycle << sps->log2_max_poc_lsb) - (int)(s->poc_lsb - s->lt_lsb[i])
                     : (int)s->lt_lsb[i];
            f = find_poc(p, v, !full, mask);
            if (!f)
                continue;   /* "no reference picture" */
            keep[f - p->dpb] = 2;
            if (s->lt_used[i] && nl < 16)
                lt[nl++] = f;
        }
        for (i = 0; i < s->st.num_negative; i++) {
            HevcFrame *f = find_poc(p, poc + s->st.delta_poc_s0[i], 0, 0);
            if (!f || keep[f - p->dpb] == 2)
                continue;
            keep[f - p->dpb] = 1;
            if (s->st.used_s0[i])
                before[nb++] = f;
            else if (nf < 32)
                foll[nf++] = f;
        }
        for (i = 0; i < s->st.num_positive; i++) {
            HevcFrame *f = find_poc(p, poc + s->st.delta_poc_s1[i], 0, 0);
            if (!f || keep[f - p->dpb] == 2)
                continue;
            keep[f - p->dpb] = 1;
            if (s->st.used_s1[i])
                after[na++] = f;
            else if (nf < 32)
                foll[nf++] = f;
        }
        for (i = 0; i < HEVC_DPB_SIZE; i++)
            if (p->dpb[i].in_use)
                p->dpb[i].ref = keep[i];
        (void)foll;
    }

    /* C.5.2.2 removal of pictures before the current one is decoded */
    p->max_reorder = sps->max_num_reorder[htid];
    p->max_latency = sps->max_latency_plus1[htid]
                         ? sps->max_num_reorder[htid] + sps->max_latency_plus1[htid] - 1 : 0;
    p->max_dec = sps->max_dec_pic_buffering[htid];
    if (is_irap(s->nal_type) && no_rasl && !p->first_picture) {
        int no_output = (s->nal_type == 21) ? 1 : (int)s->no_output_prior;
        if (!no_output)
            while (bump(p))
                ;
        for (i = 0; i < HEVC_DPB_SIZE; i++) {
            p->dpb[i].needed_for_output = 0;
            p->dpb[i].ref = 0;
            release(p, &p->dpb[i]);
        }
    } else {
        for (i = 0; i < HEVC_DPB_SIZE; i++)
            release(p, &p->dpb[i]);
        while (count_output(p) > p->max_reorder || count_in_use(p) >= p->max_dec)
            if (!bump(p))
                break;
    }
    p->first_picture = 0;
    p->after_eos = 0;

    /* the current picture's frame and surface */
    for (i = 0; i < HEVC_DPB_SIZE && p->dpb[i].in_use; i++)
        ;
    if (i == HEVC_DPB_SIZE)
        return fail(p, "DPB full");
    p->cur = &p->dpb[i];
    memset(p->cur, 0, sizeof(*p->cur));
    for (k = 0; k < HEVC_MAX_SLOTS && p->slot_busy[k]; k++)
        ;
    if (k == HEVC_MAX_SLOTS)
        return fail(p, "no free surface");
    p->slot_busy[k] = 1;
    p->cur->in_use = 1;
    p->cur->slot = (int)k;
    p->cur->poc = poc;
    p->cur->needed_for_output = (int)s->output_flag;
    p->cur->decode_index = p->decode_index;

    /* DXVA_PicParams_HEVC, as FFmpeg's dxva2_hevc.c fills it */
    memset(&p->pic, 0, sizeof(p->pic));
    pp->PicWidthInMinCbsY = (unsigned short)(sps->width >> sps->log2_min_cb);
    pp->PicHeightInMinCbsY = (unsigned short)(sps->height >> sps->log2_min_cb);
    pp->wFormatAndSequenceInfoFlags = (unsigned short)(sps->chroma_format_idc |
        (sps->separate_colour_plane_flag << 2) | (sps->bit_depth_luma_minus8 << 3) |
        (sps->bit_depth_chroma_minus8 << 6) | ((sps->log2_max_poc_lsb - 4) << 9) |
        ((sps->max_num_reorder[htid] == 0) << 13));
    pp->CurrPic.bPicEntry = (unsigned char)k;
    pp->sps_max_dec_pic_buffering_minus1 = (unsigned char)(sps->max_dec_pic_buffering[htid] - 1);
    pp->log2_min_luma_coding_block_size_minus3 = (unsigned char)(sps->log2_min_cb - 3);
    pp->log2_diff_max_min_luma_coding_block_size = (unsigned char)sps->log2_diff_cb;
    pp->log2_min_transform_block_size_minus2 = (unsigned char)(sps->log2_min_tb - 2);
    pp->log2_diff_max_min_transform_block_size = (unsigned char)sps->log2_diff_tb;
    pp->max_transform_hierarchy_depth_inter = (unsigned char)sps->max_th_depth_inter;
    pp->max_transform_hierarchy_depth_intra = (unsigned char)sps->max_th_depth_intra;
    pp->num_short_term_ref_pic_sets = (unsigned char)sps->num_st;
    pp->num_long_term_ref_pics_sps = (unsigned char)sps->num_lt_sps;
    pp->num_ref_idx_l0_default_active_minus1 = (unsigned char)pps->num_ref_idx_l0_m1;
    pp->num_ref_idx_l1_default_active_minus1 = (unsigned char)pps->num_ref_idx_l1_m1;
    pp->init_qp_minus26 = (signed char)pps->init_qp_m26;
    if (!s->sps_flag && !is_idr(s->nal_type)) {
        pp->ucNumDeltaPocsOfRefRpsIdx = (unsigned char)s->ref_num_delta;
        pp->wNumBitsForShortTermRPSInSlice = (unsigned short)s->st_bits;
    }
    pp->dwCodingParamToolFlags = sps->scaling_list_enabled | (sps->amp << 1) | (sps->sao << 2) |
        (sps->pcm << 3) |
        (sps->pcm ? ((sps->pcm_bd_luma - 1) << 4) | ((sps->pcm_bd_chroma - 1) << 8) |
                    ((sps->log2_min_pcm - 3) << 12) | (sps->log2_diff_pcm << 14) : 0) |
        (sps->pcm_loop_filter_disabled << 16) | (sps->lt_present << 17) | (sps->temporal_mvp << 18) |
        (sps->strong_intra << 19) | (pps->dependent_slices << 20) | (pps->output_flag_present << 21) |
        (pps->num_extra_bits << 22) | (pps->sign_data_hiding << 25) | (pps->cabac_init_present << 26);
    pp->dwCodingSettingPicturePropertyFlags = pps->constrained_intra | (pps->transform_skip << 1) |
        (pps->cu_qp_delta << 2) | (pps->slice_chroma_qp_offsets << 3) | (pps->weighted_pred << 4) |
        (pps->weighted_bipred << 5) | (pps->transquant_bypass << 6) | (pps->tiles << 7) |
        (pps->entropy_sync << 8) | (pps->uniform << 9) | (pps->lf_across_tiles << 10) |
        (pps->lf_across_slices << 11) | (pps->deblocking_override << 12) |
        (pps->deblocking_disabled << 13) | (pps->lists_modification << 14) | (pps->sh_ext_present << 15) |
        ((unsigned)is_irap(s->nal_type) << 16) | ((unsigned)is_idr(s->nal_type) << 17);
    /* IntraPicFlag (bit 18) once every slice is seen: finish_picture() */
    pp->pps_cb_qp_offset = (signed char)pps->cb_qp_offset;
    pp->pps_cr_qp_offset = (signed char)pps->cr_qp_offset;
    if (pps->tiles) {
        pp->num_tile_columns_minus1 = (unsigned char)pps->num_tile_cols_m1;
        pp->num_tile_rows_minus1 = (unsigned char)pps->num_tile_rows_m1;
        if (!pps->uniform) {
            for (i = 0; i < pps->num_tile_cols_m1 && i < 19; i++)
                pp->column_width_minus1[i] = (unsigned short)pps->col_w_m1[i];
            for (i = 0; i < pps->num_tile_rows_m1 && i < 21; i++)
                pp->row_height_minus1[i] = (unsigned short)pps->row_h_m1[i];
        }
    }
    pp->diff_cu_qp_delta_depth = (unsigned char)pps->diff_cu_qp_delta_depth;
    pp->pps_beta_offset_div2 = (signed char)pps->beta_div2;
    pp->pps_tc_offset_div2 = (signed char)pps->tc_div2;
    pp->log2_parallel_merge_level_minus2 = (unsigned char)pps->log2_parallel_merge_m2;
    pp->CurrPicOrderCntVal = poc;

    memset(pp->RefPicList, 0xFF, sizeof(pp->RefPicList));
    memset(pp->RefPicSetStCurrBefore, 0xFF, sizeof(pp->RefPicSetStCurrBefore));
    memset(pp->RefPicSetStCurrAfter, 0xFF, sizeof(pp->RefPicSetStCurrAfter));
    memset(pp->RefPicSetLtCurr, 0xFF, sizeof(pp->RefPicSetLtCurr));
    for (i = 0, n = 0; i < HEVC_DPB_SIZE; i++) {
        ref_index[i] = -1;
        if (!p->dpb[i].in_use || !p->dpb[i].ref || &p->dpb[i] == p->cur || n >= 15)
            continue;
        pp->RefPicList[n].bPicEntry = (unsigned char)(p->dpb[i].slot | (p->dpb[i].ref == 2 ? 0x80 : 0));
        pp->PicOrderCntValList[n] = p->dpb[i].poc;
        ref_index[i] = (int)n++;
    }
    for (i = 0; i < nb && i < 8; i++)
        pp->RefPicSetStCurrBefore[i] = (unsigned char)ref_index[before[i] - p->dpb];
    for (i = 0; i < na && i < 8; i++)
        pp->RefPicSetStCurrAfter[i] = (unsigned char)ref_index[after[i] - p->dpb];
    for (i = 0; i < nl && i < 8; i++)
        pp->RefPicSetLtCurr[i] = (unsigned char)ref_index[lt[i] - p->dpb];
    pp->StatusReportFeedbackNumber = p->decode_index + 1;

    if (sps->scaling_list_enabled) {
        if (pps->scaling_list_data_present)
            fill_qm(&p->pic.qm, pps->sl, pps->sl_dc);
        else
            fill_qm(&p->pic.qm, sps->sl, sps->sl_dc);
    }

    p->pic.extra.valid = 1;
    p->pic.extra.vps_id = (unsigned char)sps->vps_id;
    p->pic.extra.sps_id = (unsigned char)sps->sps_id;
    p->pic.extra.pps_id = (unsigned char)pps->pps_id;
    p->pic.extra.rps.num_st = sps->num_st;
    for (i = 0; i < sps->num_st; i++)
        p->pic.extra.rps.st[i] = sps->st[i];
    p->pic.extra.rps.num_lt = sps->num_lt_sps;
    for (i = 0; i < sps->num_lt_sps; i++) {
        p->pic.extra.rps.lt_poc_lsb[i] = (unsigned short)sps->lt_lsb[i];
        p->pic.extra.rps.lt_used[i] = (unsigned char)sps->lt_used[i];
    }

    p->pic.decode_index = p->decode_index;
    p->pic.nal_type = s->nal_type;
    p->pic.poc = poc;
    p->pic.width = sps->width;
    p->pic.height = sps->height;
    p->pic.crop_left = sps->crop_left;
    p->pic.crop_right = sps->crop_right;
    p->pic.crop_top = sps->crop_top;
    p->pic.crop_bottom = sps->crop_bottom;
    p->cur_sps = sps;
    p->au_len = 0;
    p->open = 1;
    p->skipping = 0;
    return 0;
}

/* --------------------------------------------------------------- driver */

int hevc_probe(const uint8_t *d, size_t len)
{
    size_t i;

    for (i = 0; i + 5 <= len; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            unsigned type = (d[i + 3] >> 1) & 0x3F;
            unsigned layer = ((d[i + 3] & 1u) << 5) | (d[i + 4] >> 3);
            unsigned tid1 = d[i + 4] & 7;
            if (d[i + 3] & 0x80)
                return 0;
            return layer == 0 && tid1 != 0 &&
                   (type == 32 || type == 33 || type == 34 || type == 35 || type == 39 || type <= 21);
        }
    return 0;
}

void hevc_init(HevcParser *p, HevcOutputFn out, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->out_fn = out;
    p->out_ctx = ctx;
    p->first_picture = 1;
    make_diag(4, diag4);
    make_diag(8, diag8);
}

void hevc_free(HevcParser *p)
{
    free(p->au);
    p->au = NULL;
}

static int handle_nal(HevcParser *p, const uint8_t *nal, size_t len)
{
    unsigned type;
    Rb r;
    int rc = 0;

    if (len < 2)
        return 0;
    type = (nal[0] >> 1) & 0x3F;
    if (((nal[0] & 1u) << 5 | (nal[1] >> 3)) != 0)
        return 0;                       /* nuh_layer_id > 0: not for a Main decoder */

    if (type <= 21) {
        Slice s;

        if (rb_init(&r, nal, len))
            return fail(p, "out of memory");
        rc = parse_slice(p, &r, &s);
        rb_free(&r);
        if (rc)
            return rc;
        if (s.first) {
            if (finish_picture(p))
                return -1;
            if (p->max_pictures && p->decode_index >= p->max_pictures)
                return 1;
            if (start_picture(p, &s))
                return -1;
        } else if (!p->open) {
            return fail(p, "a slice segment with no first slice segment before it");
        }
        if (p->skipping)
            return 0;
        if (!s.dependent) {
            static const unsigned bit[3] = { 1, 2, 4 };
            if (s.slice_type < 3)
                p->pic.slice_types |= bit[s.slice_type];
        }
        p->pic.num_slices++;
        return au_append(p, nal, len);
    }

    switch (type) {
    case 33:
    case 34:
        if (finish_picture(p))
            return -1;
        if (rb_init(&r, nal + 2, len - 2))
            return fail(p, "out of memory");
        rc = (type == 33) ? parse_sps(p, &r) : parse_pps(p, &r);
        rb_free(&r);
        return rc;
    case 36:                            /* end of sequence */
    case 37:                            /* end of bitstream */
        if (finish_picture(p))
            return -1;
        p->after_eos = 1;
        return 0;
    default:
        return 0;                       /* VPS, AUD, SEI, filler, reserved */
    }
}

int hevc_run(HevcParser *p, const uint8_t *data, size_t len, unsigned max_pictures, HevcDecodeFn dec, void *ctx)
{
    size_t i = 0, start = 0;
    int have = 0, rc;

    p->dec_fn = dec;
    p->dec_ctx = ctx;
    p->max_pictures = max_pictures;
    while (i + 3 <= len) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (have) {
                size_t end = i;
                while (end > start && data[end - 1] == 0)
                    end--;
                rc = handle_nal(p, data + start, end - start);
                if (rc < 0)
                    return -1;
                if (rc > 0)
                    goto out;
            }
            start = i + 3;
            have = 1;
            i += 3;
            continue;
        }
        i++;
    }
    if (have && start < len) {
        rc = handle_nal(p, data + start, len - start);
        if (rc < 0)
            return -1;
    }
out:
    if (finish_picture(p))
        return -1;
    while (bump(p))
        ;
    return 0;
}
