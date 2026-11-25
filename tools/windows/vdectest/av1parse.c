/*
 * av1parse.c - AV1 in IVF for vdpu383test (see av1parse.h)
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "av1parse.h"

/* ------------------------------------------------------------ bit reader */

typedef struct Bits {
    const uint8_t *d;
    size_t len, pos;            /* bits */
    int over;
} Bits;

static void bits_init(Bits *b, const uint8_t *d, size_t len)
{
    b->d = d;
    b->len = len;
    b->pos = 0;
    b->over = 0;
}

static unsigned f(Bits *b, unsigned n)
{
    unsigned v = 0;

    while (n--) {
        unsigned bit = 0;
        if ((b->pos >> 3) < b->len)
            bit = (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
        else
            b->over = 1;
        v = (v << 1) | bit;
        b->pos++;
    }
    return v;
}

static int su(Bits *b, unsigned n)
{
    int v = (int)f(b, n), m = 1 << (n - 1);
    if (v & m)
        v -= 2 * m;
    return v;
}

static unsigned ns(Bits *b, unsigned n)
{
    unsigned w = 0, m, v, x = n;

    while (x) {
        w++;
        x >>= 1;
    }
    m = (1u << w) - n;
    v = f(b, w - 1);
    if (v < m)
        return v;
    return (v << 1) - m + f(b, 1);
}

static unsigned uvlc(Bits *b)
{
    unsigned lz = 0;

    while (!f(b, 1)) {
        lz++;
        if (lz >= 32 || b->over)
            return 0xFFFFFFFFu;
    }
    return f(b, lz) + (1u << lz) - 1;
}

static void byte_align(Bits *b)
{
    b->pos = (b->pos + 7) & ~(size_t)7;
}

static size_t leb128(const uint8_t *d, size_t len, size_t *pos, int *bad)
{
    size_t v = 0;
    unsigned i;

    for (i = 0; i < 8; i++) {
        uint8_t c;
        if (*pos >= len) {
            *bad = 1;
            return 0;
        }
        c = d[(*pos)++];
        v |= (size_t)(c & 0x7F) << (7 * i);
        if (!(c & 0x80))
            return v;
    }
    *bad = 1;
    return 0;
}

static void fail(Av1Parser *p, const char *what, unsigned n)
{
    snprintf(p->err, sizeof(p->err), "%s (%u)", what, n);
}

/* ------------------------------------------------------- sequence header */

static void color_config(Bits *b, Av1Seq *s)
{
    unsigned high = f(b, 1), cp = 2, tc = 2, mc = 2;

    if (s->profile == 2 && high)
        s->bit_depth = f(b, 1) ? 12 : 10;
    else
        s->bit_depth = high ? 10 : 8;
    s->mono = (s->profile == 1) ? 0 : f(b, 1);
    if (f(b, 1)) {
        cp = f(b, 8);
        tc = f(b, 8);
        mc = f(b, 8);
    }
    s->matrix_coefficients = mc;
    if (s->mono) {
        f(b, 1);
        s->ss_x = s->ss_y = 1;
        s->separate_uv_delta_q = 0;
        return;
    } else if (cp == 1 && tc == 13 && mc == 0) {
        s->ss_x = s->ss_y = 0;
    } else {
        f(b, 1);
        if (s->profile == 0) {
            s->ss_x = s->ss_y = 1;
        } else if (s->profile == 1) {
            s->ss_x = s->ss_y = 0;
        } else if (s->bit_depth == 12) {
            s->ss_x = f(b, 1);
            s->ss_y = s->ss_x ? f(b, 1) : 0;
        } else {
            s->ss_x = 1;
            s->ss_y = 0;
        }
        if (s->ss_x && s->ss_y)
            f(b, 2);
    }
    s->separate_uv_delta_q = f(b, 1);
}

static int parse_seq(Av1Parser *p, const uint8_t *d, size_t len)
{
    Bits b;
    Av1Seq s;
    unsigned i, buffer_delay_length = 0;

    memset(&s, 0, sizeof(s));
    bits_init(&b, d, len);
    s.profile = f(&b, 3);
    s.still_picture = f(&b, 1);
    s.reduced_still = f(&b, 1);
    if (s.reduced_still) {
        s.op_count = 1;
        f(&b, 5);
    } else {
        if (f(&b, 1)) {                         /* timing_info_present_flag */
            s.timing_info = 1;
            f(&b, 32);
            f(&b, 32);
            s.equal_picture_interval = f(&b, 1);
            if (s.equal_picture_interval)
                uvlc(&b);
            s.decoder_model_info = f(&b, 1);
            if (s.decoder_model_info) {
                buffer_delay_length = f(&b, 5) + 1;
                f(&b, 32);
                s.buffer_removal_time_length = f(&b, 5) + 1;
                s.frame_presentation_time_length = f(&b, 5) + 1;
            }
        }
        {
            unsigned idd = f(&b, 1);           /* initial_display_delay_present_flag */

            s.op_count = f(&b, 5) + 1;
            for (i = 0; i < s.op_count; i++) {
                unsigned lvl;

                s.op_idc[i] = f(&b, 12);
                lvl = f(&b, 5);
                if (lvl > 7)
                    f(&b, 1);
                if (s.decoder_model_info) {
                    s.decoder_model_present[i] = f(&b, 1);
                    if (s.decoder_model_present[i]) {
                        f(&b, buffer_delay_length);
                        f(&b, buffer_delay_length);
                        f(&b, 1);
                    }
                }
                if (idd && f(&b, 1))
                    f(&b, 4);
            }
        }
    }
    s.frame_width_bits = f(&b, 4) + 1;
    s.frame_height_bits = f(&b, 4) + 1;
    s.max_width = f(&b, s.frame_width_bits) + 1;
    s.max_height = f(&b, s.frame_height_bits) + 1;
    s.frame_id_numbers = s.reduced_still ? 0 : f(&b, 1);
    if (s.frame_id_numbers) {
        s.delta_frame_id_length = f(&b, 4) + 2;
        s.frame_id_length = f(&b, 3) + 1 + s.delta_frame_id_length;
    }
    s.sb128 = f(&b, 1);
    s.filter_intra = f(&b, 1);
    s.intra_edge = f(&b, 1);
    if (s.reduced_still) {
        s.force_screen_content_tools = 2;
        s.force_integer_mv = 2;
    } else {
        s.interintra = f(&b, 1);
        s.masked_compound = f(&b, 1);
        s.warped_motion = f(&b, 1);
        s.dual_filter = f(&b, 1);
        s.order_hint = f(&b, 1);
        if (s.order_hint) {
            s.jnt_comp = f(&b, 1);
            s.ref_frame_mvs = f(&b, 1);
        }
        s.force_screen_content_tools = f(&b, 1) ? 2 : f(&b, 1);
        if (s.force_screen_content_tools > 0)
            s.force_integer_mv = f(&b, 1) ? 2 : f(&b, 1);
        else
            s.force_integer_mv = 2;
        if (s.order_hint)
            s.order_hint_bits = f(&b, 3) + 1;
    }
    s.superres = f(&b, 1);
    s.cdef = f(&b, 1);
    s.restoration = f(&b, 1);
    color_config(&b, &s);
    s.film_grain_present = f(&b, 1);
    if (b.over) {
        fail(p, "truncated sequence header", 0);
        return -1;
    }
    s.valid = 1;
    p->seq = s;
    return 0;
}

/* ---------------------------------------------------------- frame header */

typedef struct Fh {
    unsigned show_existing, existing_idx, frame_type, intra, show_frame, showable, error_res;
    unsigned disable_cdf_update, sct, force_integer_mv, size_override, order_hint, primary;
    unsigned refresh, ref_idx[7], short_signaling;
    unsigned upscaled_width, width, height, render_width, render_height, use_superres, denom;
    unsigned intrabc, hp, interp, switchable_motion, use_ref_frame_mvs;
    unsigned disable_end_update;
    unsigned sb_cols, sb_rows, sb_shift, tile_cols, tile_rows, tile_cols_log2, tile_rows_log2;
    unsigned col_start_sb[65], row_start_sb[65];
    unsigned context_update_id, tile_size_bytes;
    unsigned base_q, using_qm, qm_y, qm_u, qm_v;
    int      dq_ydc, dq_udc, dq_uac, dq_vdc, dq_vac;
    unsigned seg_en, seg_update_map, seg_temporal, seg_update_data;
    unsigned char feat_en[8][8];
    short    feat[8][8];
    unsigned dq_present, dq_res, dlf_present, dlf_res, dlf_multi;
    unsigned coded_lossless, all_lossless;
    unsigned lf[4], lf_sharp, lf_delta_en, lf_delta_update;
    signed char lf_ref[8], lf_mode[2];
    unsigned cdef_damping_m3, cdef_bits, cdef_y[8], cdef_uv[8];
    unsigned lr_type[3], lr_log2[3];
    unsigned tx_mode, reference_select, skip_mode, warped, reduced_tx;
    unsigned gm_type[8];
    int      gm[8][6];
    Av1FilmGrain fg;
    unsigned header_bits;
    unsigned temporal_id, spatial_id;
} Fh;

static const int seg_bits[8] = { 8, 6, 6, 6, 6, 3, 0, 0 };
static const int seg_signed[8] = { 1, 1, 1, 1, 1, 0, 0, 0 };
static const int seg_max[8] = { 255, 63, 63, 63, 63, 7, 0, 0 };

static int rel_dist(const Av1Seq *s, int a, int b)
{
    int diff, m;

    if (!s->order_hint_bits)
        return 0;
    diff = a - b;
    m = 1 << (s->order_hint_bits - 1);
    return (diff & (m - 1)) - (diff & m);
}

static unsigned tile_log2(unsigned blk, unsigned target)
{
    unsigned k = 0;

    while ((blk << k) < target)
        k++;
    return k;
}

static void superres_params(Bits *b, const Av1Seq *s, Fh *h)
{
    h->use_superres = s->superres ? f(b, 1) : 0;
    h->denom = h->use_superres ? f(b, 3) + 9 : 8;
    h->upscaled_width = h->width;
    h->width = (h->upscaled_width * 8 + h->denom / 2) / h->denom;
}

static void frame_size(Bits *b, const Av1Seq *s, Fh *h)
{
    if (h->size_override) {
        h->width = f(b, s->frame_width_bits) + 1;
        h->height = f(b, s->frame_height_bits) + 1;
    } else {
        h->width = s->max_width;
        h->height = s->max_height;
    }
    superres_params(b, s, h);
}

static void render_size(Bits *b, Fh *h)
{
    if (f(b, 1)) {
        h->render_width = f(b, 16) + 1;
        h->render_height = f(b, 16) + 1;
    } else {
        h->render_width = h->upscaled_width;
        h->render_height = h->height;
    }
}

/* spec 7.8 */
static void set_frame_refs(Av1Parser *p, Fh *h, unsigned last, unsigned gold)
{
    const Av1Seq *s = &p->seq;
    int used[8] = { 0 }, shifted[8], idx[7], i, ref, cur = 1 << (s->order_hint_bits - 1);
    static const int list[5] = { 2, 3, 5, 6, 7 };     /* LAST2, LAST3, BWDREF, ALTREF2, ALTREF */
    int hint, best = 0;

    for (i = 0; i < 7; i++)
        idx[i] = -1;
    idx[0] = (int)last;
    idx[3] = (int)gold;
    used[last] = used[gold] = 1;
    for (i = 0; i < 8; i++)
        shifted[i] = cur + rel_dist(s, (int)p->ref[i].order_hint, (int)h->order_hint);

    /* latest backward -> ALTREF */
    ref = -1;
    for (i = 0; i < 8; i++) {
        hint = shifted[i];
        if (!used[i] && hint >= cur && (ref < 0 || hint >= best)) {
            ref = i;
            best = hint;
        }
    }
    if (ref >= 0) {
        idx[6] = ref;
        used[ref] = 1;
    }
    /* earliest backward -> BWDREF, then ALTREF2 */
    for (i = 0; i < 2; i++) {
        int j;
        ref = -1;
        for (j = 0; j < 8; j++) {
            hint = shifted[j];
            if (!used[j] && hint >= cur && (ref < 0 || hint < best)) {
                ref = j;
                best = hint;
            }
        }
        if (ref >= 0) {
            idx[i ? 5 : 4] = ref;
            used[ref] = 1;
        }
    }
    for (i = 0; i < 5; i++) {
        int rf = list[i] - 1, j;
        if (idx[rf] >= 0)
            continue;
        ref = -1;
        for (j = 0; j < 8; j++) {
            hint = shifted[j];
            if (!used[j] && hint < cur && (ref < 0 || hint >= best)) {
                ref = j;
                best = hint;
            }
        }
        if (ref >= 0) {
            idx[rf] = ref;
            used[ref] = 1;
        }
    }
    ref = -1;
    for (i = 0; i < 8; i++) {
        hint = shifted[i];
        if (ref < 0 || hint < best) {
            ref = i;
            best = hint;
        }
    }
    for (i = 0; i < 7; i++)
        h->ref_idx[i] = (unsigned)(idx[i] < 0 ? ref : idx[i]);
}

static void tile_info(Bits *b, const Av1Seq *s, Fh *h)
{
    unsigned mi_cols = 2 * ((h->width + 7) >> 3), mi_rows = 2 * ((h->height + 7) >> 3);
    unsigned sb_shift = s->sb128 ? 5 : 4, sb_size = sb_shift + 2;
    unsigned sb_cols = s->sb128 ? (mi_cols + 31) >> 5 : (mi_cols + 15) >> 4;
    unsigned sb_rows = s->sb128 ? (mi_rows + 31) >> 5 : (mi_rows + 15) >> 4;
    unsigned max_w_sb = 4096 >> sb_size, max_area_sb = (4096 * 2304) >> (2 * sb_size);
    unsigned min_log2_cols = tile_log2(max_w_sb, sb_cols);
    unsigned max_log2_cols = tile_log2(1, sb_cols < 64 ? sb_cols : 64);
    unsigned max_log2_rows = tile_log2(1, sb_rows < 64 ? sb_rows : 64);
    unsigned min_log2_tiles = tile_log2(max_area_sb, sb_rows * sb_cols), i, start;

    if (min_log2_tiles < min_log2_cols)
        min_log2_tiles = min_log2_cols;
    h->sb_cols = sb_cols;
    h->sb_rows = sb_rows;
    h->sb_shift = sb_shift;
    if (f(b, 1)) {                              /* uniform_tile_spacing_flag */
        unsigned w, ht, min_log2_rows;

        h->tile_cols_log2 = min_log2_cols;
        while (h->tile_cols_log2 < max_log2_cols && f(b, 1))
            h->tile_cols_log2++;
        w = (sb_cols + (1u << h->tile_cols_log2) - 1) >> h->tile_cols_log2;
        for (i = 0, start = 0; start < sb_cols && i < 64; start += w)
            h->col_start_sb[i++] = start;
        h->col_start_sb[i] = sb_cols;
        h->tile_cols = i;
        min_log2_rows = min_log2_tiles > h->tile_cols_log2 ? min_log2_tiles - h->tile_cols_log2 : 0;
        h->tile_rows_log2 = min_log2_rows;
        while (h->tile_rows_log2 < max_log2_rows && f(b, 1))
            h->tile_rows_log2++;
        ht = (sb_rows + (1u << h->tile_rows_log2) - 1) >> h->tile_rows_log2;
        for (i = 0, start = 0; start < sb_rows && i < 64; start += ht)
            h->row_start_sb[i++] = start;
        h->row_start_sb[i] = sb_rows;
        h->tile_rows = i;
    } else {
        unsigned widest = 0, max_h_sb, area;

        for (i = 0, start = 0; start < sb_cols && i < 64; i++) {
            unsigned mw = sb_cols - start < max_w_sb ? sb_cols - start : max_w_sb, sz;
            h->col_start_sb[i] = start;
            sz = ns(b, mw) + 1;
            if (sz > widest)
                widest = sz;
            start += sz;
        }
        h->col_start_sb[i] = sb_cols;
        h->tile_cols = i;
        h->tile_cols_log2 = tile_log2(1, h->tile_cols);
        area = min_log2_tiles > 0 ? (sb_rows * sb_cols) >> (min_log2_tiles + 1) : sb_rows * sb_cols;
        max_h_sb = widest ? area / widest : 1;
        if (max_h_sb < 1)
            max_h_sb = 1;
        for (i = 0, start = 0; start < sb_rows && i < 64; i++) {
            unsigned mh = sb_rows - start < max_h_sb ? sb_rows - start : max_h_sb;
            h->row_start_sb[i] = start;
            start += ns(b, mh) + 1;
        }
        h->row_start_sb[i] = sb_rows;
        h->tile_rows = i;
        h->tile_rows_log2 = tile_log2(1, h->tile_rows);
    }
    if (h->tile_cols_log2 > 0 || h->tile_rows_log2 > 0) {
        h->context_update_id = f(b, h->tile_rows_log2 + h->tile_cols_log2);
        h->tile_size_bytes = f(b, 2) + 1;
    } else {
        h->context_update_id = 0;
        h->tile_size_bytes = 4;
    }
}

static int read_delta_q(Bits *b)
{
    return f(b, 1) ? su(b, 7) : 0;
}

/* spec 5.9.24 / 5.9.25 */
static unsigned inverse_recenter(unsigned r, unsigned v)
{
    if (v > 2 * r)
        return v;
    if (v & 1)
        return r - ((v + 1) >> 1);
    return r + (v >> 1);
}

static unsigned decode_subexp(Bits *b, unsigned num)
{
    unsigned i = 0, mk = 0, k = 3;

    for (;;) {
        unsigned b2 = i ? k + i - 1 : k, a = 1u << b2;
        if (num <= mk + 3 * a)
            return ns(b, num - mk) + mk;
        if (!f(b, 1))
            return f(b, b2) + mk;
        i++;
        mk += a;
        if (b->over)
            return 0;
    }
}

static int decode_signed_subexp_with_ref(Bits *b, int low, int high, int r)
{
    unsigned mx = (unsigned)(high - low), rr = (unsigned)(r - low), v = decode_subexp(b, mx), x;

    if ((rr << 1) <= mx)
        x = inverse_recenter(rr, v);
    else
        x = mx - 1 - inverse_recenter(mx - 1 - rr, v);
    return (int)x + low;
}

static void read_global_param(Bits *b, Fh *h, const int prev[8][6], unsigned type, unsigned ref, unsigned idx)
{
    unsigned abs_bits = 12, prec_bits = 15, prec_diff;
    int round, sub, mx, r;

    if (idx < 2) {
        if (type == 1) {
            abs_bits = 9 - !h->hp;
            prec_bits = 3 - !h->hp;
        } else {
            abs_bits = 12;
            prec_bits = 6;
        }
    }
    prec_diff = 16 - prec_bits;
    round = (idx % 3 == 2) ? (1 << 16) : 0;
    sub = (idx % 3 == 2) ? (1 << prec_bits) : 0;
    mx = 1 << abs_bits;
    r = (prev[ref][idx] >> prec_diff) - sub;
    h->gm[ref][idx] = decode_signed_subexp_with_ref(b, -mx, mx + 1, r) * (1 << prec_diff) + round;
}

static void gm_default(int gm[8][6])
{
    unsigned i, j;

    for (i = 0; i < 8; i++)
        for (j = 0; j < 6; j++)
            gm[i][j] = (j % 3 == 2) ? 1 << 16 : 0;
}

static void film_grain_params(Av1Parser *p, Bits *b, Fh *h)
{
    const Av1Seq *s = &p->seq;
    Av1FilmGrain *g = &h->fg;
    unsigned i, pos_luma, pos_chroma;

    memset(g, 0, sizeof(*g));
    if (!s->film_grain_present || (!h->show_frame && !h->showable))
        return;
    g->apply = f(b, 1);
    if (!g->apply)
        return;
    g->seed = f(b, 16);
    g->update = (h->frame_type == V383_AV1_INTER_FRAME) ? f(b, 1) : 1;
    if (!g->update) {
        unsigned idx = f(b, 3), seed = g->seed;
        *g = p->ref[idx].fg;
        g->seed = seed;
        return;
    }
    g->num_y = f(b, 4);
    for (i = 0; i < g->num_y && i < 14; i++) {
        g->y_pts[i][0] = (unsigned char)f(b, 8);
        g->y_pts[i][1] = (unsigned char)f(b, 8);
    }
    g->csfl = s->mono ? 0 : f(b, 1);
    if (s->mono || g->csfl || (s->ss_x == 1 && s->ss_y == 1 && g->num_y == 0)) {
        g->num_cb = g->num_cr = 0;
    } else {
        g->num_cb = f(b, 4);
        for (i = 0; i < g->num_cb && i < 10; i++) {
            g->cb_pts[i][0] = (unsigned char)f(b, 8);
            g->cb_pts[i][1] = (unsigned char)f(b, 8);
        }
        g->num_cr = f(b, 4);
        for (i = 0; i < g->num_cr && i < 10; i++) {
            g->cr_pts[i][0] = (unsigned char)f(b, 8);
            g->cr_pts[i][1] = (unsigned char)f(b, 8);
        }
    }
    g->scaling_m8 = f(b, 2);
    g->lag = f(b, 2);
    pos_luma = 2 * g->lag * (g->lag + 1);
    if (g->num_y) {
        pos_chroma = pos_luma + 1;
        for (i = 0; i < pos_luma; i++)
            g->ar_y[i] = (unsigned char)f(b, 8);
    } else {
        pos_chroma = pos_luma;
    }
    if (g->csfl || g->num_cb)
        for (i = 0; i < pos_chroma; i++)
            g->ar_cb[i] = (unsigned char)f(b, 8);
    if (g->csfl || g->num_cr)
        for (i = 0; i < pos_chroma; i++)
            g->ar_cr[i] = (unsigned char)f(b, 8);
    g->ar_shift_m6 = f(b, 2);
    g->scale_shift = f(b, 2);
    if (g->num_cb) {
        g->cb_mult = f(b, 8);
        g->cb_luma_mult = f(b, 8);
        g->cb_offset = f(b, 9);
    }
    if (g->num_cr) {
        g->cr_mult = f(b, 8);
        g->cr_luma_mult = f(b, 8);
        g->cr_offset = f(b, 9);
    }
    g->overlap = f(b, 1);
    g->clip = f(b, 1);
}

/* uncompressed_header(); 0, or -1 with p->err */
static int parse_frame_header(Av1Parser *p, Bits *b, Fh *h)
{
    const Av1Seq *s = &p->seq;
    const Av1Slot *prim = NULL;
    unsigned i, j, all = 0xFF, id_len = s->frame_id_length;
    int prev_gm[8][6];
    size_t start = b->pos;

    if (s->reduced_still) {
        h->frame_type = V383_AV1_KEY_FRAME;
        h->intra = 1;
        h->show_frame = 1;
    } else {
        h->show_existing = f(b, 1);
        if (h->show_existing) {
            h->existing_idx = f(b, 3);
            if (s->decoder_model_info && !s->equal_picture_interval)
                f(b, s->frame_presentation_time_length);
            if (s->frame_id_numbers)
                f(b, id_len);
            h->frame_type = p->ref[h->existing_idx].frame_type;
            h->header_bits = (unsigned)(b->pos - start);
            return 0;
        }
        h->frame_type = f(b, 2);
        h->intra = h->frame_type == V383_AV1_KEY_FRAME || h->frame_type == V383_AV1_INTRA_ONLY_FRAME;
        h->show_frame = f(b, 1);
        if (h->show_frame && s->decoder_model_info && !s->equal_picture_interval)
            f(b, s->frame_presentation_time_length);
        h->showable = h->show_frame ? h->frame_type != V383_AV1_KEY_FRAME : f(b, 1);
        if (h->frame_type == V383_AV1_SWITCH_FRAME || (h->frame_type == V383_AV1_KEY_FRAME && h->show_frame))
            h->error_res = 1;
        else
            h->error_res = f(b, 1);
    }
    if (h->frame_type == V383_AV1_KEY_FRAME && h->show_frame)
        for (i = 0; i < 8; i++) {
            p->ref[i].valid = 0;
            p->ref[i].order_hint = 0;
        }
    h->disable_cdf_update = f(b, 1);
    h->sct = (s->force_screen_content_tools == 2) ? f(b, 1) : s->force_screen_content_tools;
    if (h->sct)
        h->force_integer_mv = (s->force_integer_mv == 2) ? f(b, 1) : s->force_integer_mv;
    if (h->intra)
        h->force_integer_mv = 1;
    if (s->frame_id_numbers)
        f(b, id_len);
    if (h->frame_type == V383_AV1_SWITCH_FRAME)
        h->size_override = 1;
    else if (s->reduced_still)
        h->size_override = 0;
    else
        h->size_override = f(b, 1);
    h->order_hint = f(b, s->order_hint_bits);
    h->primary = (h->intra || h->error_res) ? V383_AV1_PRIMARY_REF_NONE : f(b, 3);
    if (s->decoder_model_info && f(b, 1)) {     /* buffer_removal_time_present_flag */
        for (i = 0; i < s->op_count; i++)
            if (s->decoder_model_present[i]) {
                unsigned idc = s->op_idc[i];
                if (!idc || (((idc >> h->temporal_id) & 1) && ((idc >> (h->spatial_id + 8)) & 1)))
                    f(b, s->buffer_removal_time_length);
            }
    }
    h->refresh = (h->frame_type == V383_AV1_SWITCH_FRAME ||
                  (h->frame_type == V383_AV1_KEY_FRAME && h->show_frame)) ? all : f(b, 8);
    if ((!h->intra || h->refresh != all) && h->error_res && s->order_hint)
        for (i = 0; i < 8; i++) {
            unsigned roh = f(b, s->order_hint_bits);
            if (roh != p->ref[i].order_hint) {
                p->ref[i].valid = 0;
                p->ref[i].order_hint = roh;
            }
        }
    if (h->frame_type == V383_AV1_KEY_FRAME || h->frame_type == V383_AV1_INTRA_ONLY_FRAME) {
        frame_size(b, s, h);
        render_size(b, h);
        if (h->sct && h->upscaled_width == h->width)
            h->intrabc = f(b, 1);
    } else {
        unsigned found = 0;

        h->short_signaling = s->order_hint ? f(b, 1) : 0;
        if (h->short_signaling) {
            unsigned last = f(b, 3), gold = f(b, 3);
            set_frame_refs(p, h, last, gold);
            p->short_signaling++;
        }
        for (i = 0; i < 7; i++) {
            if (!h->short_signaling)
                h->ref_idx[i] = f(b, 3);
            if (s->frame_id_numbers)
                f(b, s->delta_frame_id_length);
        }
        if (h->size_override && !h->error_res) {
            for (i = 0; i < 7; i++)
                if (f(b, 1)) {
                    const Av1Slot *r = &p->ref[h->ref_idx[i]];
                    h->upscaled_width = r->upscaled_width;
                    h->width = h->upscaled_width;
                    h->height = r->height;
                    h->render_width = r->render_width;
                    h->render_height = r->render_height;
                    found = 1;
                    break;
                }
            if (!found) {
                frame_size(b, s, h);
                render_size(b, h);
            } else {
                superres_params(b, s, h);
            }
        } else {
            frame_size(b, s, h);
            render_size(b, h);
        }
        h->hp = h->force_integer_mv ? 0 : f(b, 1);
        h->interp = f(b, 1) ? 4 : f(b, 2);
        h->switchable_motion = f(b, 1);
        h->use_ref_frame_mvs = (h->error_res || !s->ref_frame_mvs) ? 0 : f(b, 1);
    }
    h->disable_end_update = (s->reduced_still || h->disable_cdf_update) ? 1 : f(b, 1);

    /* setup_past_independence() / load_previous() */
    if (h->primary == V383_AV1_PRIMARY_REF_NONE) {
        gm_default(prev_gm);
        memset(h->feat_en, 0, sizeof(h->feat_en));
        memset(h->feat, 0, sizeof(h->feat));
        h->lf_ref[0] = 1;
        h->lf_ref[1] = 0;
        h->lf_ref[2] = 0;
        h->lf_ref[3] = 0;
        h->lf_ref[4] = -1;
        h->lf_ref[5] = 0;
        h->lf_ref[6] = -1;
        h->lf_ref[7] = -1;
        h->lf_mode[0] = h->lf_mode[1] = 0;
    } else {
        prim = &p->ref[h->ref_idx[h->primary]];
        memcpy(prev_gm, prim->gm, sizeof(prev_gm));
        memcpy(h->feat_en, prim->feat_en, sizeof(h->feat_en));
        memcpy(h->feat, prim->feat, sizeof(h->feat));
        memcpy(h->lf_ref, prim->lf_ref, sizeof(h->lf_ref));
        memcpy(h->lf_mode, prim->lf_mode, sizeof(h->lf_mode));
    }

    tile_info(b, s, h);

    /* quantization_params() */
    h->base_q = f(b, 8);
    h->dq_ydc = read_delta_q(b);
    if (!s->mono) {
        unsigned diff_uv = s->separate_uv_delta_q ? f(b, 1) : 0;
        h->dq_udc = read_delta_q(b);
        h->dq_uac = read_delta_q(b);
        if (diff_uv) {
            h->dq_vdc = read_delta_q(b);
            h->dq_vac = read_delta_q(b);
        } else {
            h->dq_vdc = h->dq_udc;
            h->dq_vac = h->dq_uac;
        }
    }
    h->using_qm = f(b, 1);
    if (h->using_qm) {
        h->qm_y = f(b, 4);
        h->qm_u = f(b, 4);
        h->qm_v = s->separate_uv_delta_q ? f(b, 4) : h->qm_u;
    }

    /* segmentation_params() */
    h->seg_en = f(b, 1);
    if (h->seg_en) {
        if (h->primary == V383_AV1_PRIMARY_REF_NONE) {
            h->seg_update_map = 1;
            h->seg_temporal = 0;
            h->seg_update_data = 1;
        } else {
            h->seg_update_map = f(b, 1);
            h->seg_temporal = h->seg_update_map ? f(b, 1) : 0;
            h->seg_update_data = f(b, 1);
        }
        if (h->seg_update_data)
            for (i = 0; i < 8; i++)
                for (j = 0; j < 8; j++) {
                    int v = 0;
                    h->feat_en[i][j] = (unsigned char)f(b, 1);
                    if (h->feat_en[i][j]) {
                        if (seg_signed[j]) {
                            v = su(b, 1 + (unsigned)seg_bits[j]);
                            if (v < -seg_max[j])
                                v = -seg_max[j];
                            if (v > seg_max[j])
                                v = seg_max[j];
                        } else {
                            v = (int)f(b, (unsigned)seg_bits[j]);
                            if (v > seg_max[j])
                                v = seg_max[j];
                        }
                    }
                    h->feat[i][j] = (short)v;
                }
    } else {
        memset(h->feat_en, 0, sizeof(h->feat_en));
        memset(h->feat, 0, sizeof(h->feat));
    }

    /* delta_q_params(), delta_lf_params() */
    if (h->base_q > 0)
        h->dq_present = f(b, 1);
    if (h->dq_present)
        h->dq_res = f(b, 2);
    if (h->dq_present) {
        if (!h->intrabc)
            h->dlf_present = f(b, 1);
        if (h->dlf_present) {
            h->dlf_res = f(b, 2);
            h->dlf_multi = f(b, 1);
        }
    }

    h->coded_lossless = 1;
    for (i = 0; i < 8; i++) {
        int q = (int)h->base_q;
        if (h->seg_en && h->feat_en[i][0])
            q += h->feat[i][0];
        if (q < 0)
            q = 0;
        if (q > 255)
            q = 255;
        if (q || h->dq_ydc || h->dq_udc || h->dq_uac || h->dq_vdc || h->dq_vac)
            h->coded_lossless = 0;
    }
    h->all_lossless = h->coded_lossless && h->width == h->upscaled_width;

    /* loop_filter_params() */
    if (h->coded_lossless || h->intrabc) {
        h->lf[0] = h->lf[1] = 0;
        h->lf_ref[0] = 1;
        h->lf_ref[1] = 0;
        h->lf_ref[2] = 0;
        h->lf_ref[3] = 0;
        h->lf_ref[4] = -1;
        h->lf_ref[5] = 0;
        h->lf_ref[6] = -1;
        h->lf_ref[7] = -1;
        h->lf_mode[0] = h->lf_mode[1] = 0;
    } else {
        h->lf[0] = f(b, 6);
        h->lf[1] = f(b, 6);
        if (!s->mono && (h->lf[0] || h->lf[1])) {
            h->lf[2] = f(b, 6);
            h->lf[3] = f(b, 6);
        }
        h->lf_sharp = f(b, 3);
        h->lf_delta_en = f(b, 1);
        if (h->lf_delta_en) {
            h->lf_delta_update = f(b, 1);
            if (h->lf_delta_update) {
                for (i = 0; i < 8; i++)
                    if (f(b, 1))
                        h->lf_ref[i] = (signed char)su(b, 7);
                for (i = 0; i < 2; i++)
                    if (f(b, 1))
                        h->lf_mode[i] = (signed char)su(b, 7);
            }
        }
    }

    /* cdef_params() */
    if (h->coded_lossless || h->intrabc || !s->cdef) {
        h->cdef_bits = 0;
        h->cdef_damping_m3 = 0;
    } else {
        h->cdef_damping_m3 = f(b, 2);
        h->cdef_bits = f(b, 2);
        for (i = 0; i < (1u << h->cdef_bits); i++) {
            h->cdef_y[i] = f(b, 4);
            h->cdef_y[i] |= f(b, 2) << 6;
            if (!s->mono) {
                h->cdef_uv[i] = f(b, 4);
                h->cdef_uv[i] |= f(b, 2) << 6;
            }
        }
    }

    /* lr_params() */
    h->lr_log2[0] = h->lr_log2[1] = h->lr_log2[2] = 8;
    if (!(h->all_lossless || h->intrabc || !s->restoration)) {
        static const unsigned remap[4] = { 0, 3, 1, 2 };
        unsigned uses = 0, chroma = 0, shift, uv = 0;

        for (i = 0; i < (s->mono ? 1u : 3u); i++) {
            h->lr_type[i] = remap[f(b, 2)];
            if (h->lr_type[i]) {
                uses = 1;
                if (i)
                    chroma = 1;
            }
        }
        if (uses) {
            if (s->sb128) {
                shift = f(b, 1) + 1;
            } else {
                shift = f(b, 1);
                if (shift)
                    shift += f(b, 1);
            }
            if (s->ss_x && s->ss_y && chroma)
                uv = f(b, 1);
            h->lr_log2[0] = 6 + shift;
            h->lr_log2[1] = h->lr_log2[2] = 6 + shift - uv;
            p->lr_frames++;
        }
    }

    /* read_tx_mode(), frame_reference_mode(), skip_mode_params() */
    h->tx_mode = h->coded_lossless ? 0 : (f(b, 1) ? 2 : 1);
    h->reference_select = h->intra ? 0 : f(b, 1);
    if (!h->intra && h->reference_select && s->order_hint) {
        int fwd = -1, bwd = -1, fh = 0, bh = 0, allowed = 0;

        for (i = 0; i < 7; i++) {
            int rh = (int)p->ref[h->ref_idx[i]].order_hint, d = rel_dist(s, rh, (int)h->order_hint);
            if (d < 0) {
                if (fwd < 0 || rel_dist(s, rh, fh) > 0) {
                    fwd = (int)i;
                    fh = rh;
                }
            } else if (d > 0) {
                if (bwd < 0 || rel_dist(s, rh, bh) < 0) {
                    bwd = (int)i;
                    bh = rh;
                }
            }
        }
        if (fwd >= 0 && bwd >= 0) {
            allowed = 1;
        } else if (fwd >= 0) {
            int f2 = -1, f2h = 0;
            for (i = 0; i < 7; i++) {
                int rh = (int)p->ref[h->ref_idx[i]].order_hint;
                if (rel_dist(s, rh, fh) < 0 && (f2 < 0 || rel_dist(s, rh, f2h) > 0)) {
                    f2 = (int)i;
                    f2h = rh;
                }
            }
            allowed = f2 >= 0;
        }
        if (allowed)
            h->skip_mode = f(b, 1);
    }
    h->warped = (h->intra || h->error_res || !s->warped_motion) ? 0 : f(b, 1);
    h->reduced_tx = f(b, 1);

    /* global_motion_params() */
    gm_default(h->gm);
    memset(h->gm_type, 0, sizeof(h->gm_type));
    if (!h->intra)
        for (i = 1; i <= 7; i++) {
            unsigned type = 0;
            if (f(b, 1)) {
                if (f(b, 1))
                    type = 2;
                else
                    type = f(b, 1) ? 1 : 3;
            }
            h->gm_type[i] = type;
            if (type >= 2) {
                read_global_param(b, h, (const int (*)[6])prev_gm, type, i, 2);
                read_global_param(b, h, (const int (*)[6])prev_gm, type, i, 3);
                if (type == 3) {
                    read_global_param(b, h, (const int (*)[6])prev_gm, type, i, 4);
                    read_global_param(b, h, (const int (*)[6])prev_gm, type, i, 5);
                } else {
                    h->gm[i][4] = -h->gm[i][3];
                    h->gm[i][5] = h->gm[i][2];
                }
            }
            if (type >= 1) {
                read_global_param(b, h, (const int (*)[6])prev_gm, type, i, 0);
                read_global_param(b, h, (const int (*)[6])prev_gm, type, i, 1);
            }
        }

    film_grain_params(p, b, h);
    h->header_bits = (unsigned)(b->pos - start);
    if (b->over) {
        fail(p, "truncated frame header", p->decode_index);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- frames */

static void fill_dxva(Av1Parser *p, const Fh *h, int cur, Av1Picture *pic)
{
    const Av1Seq *s = &p->seq;
    V383_DXVA_PicParams_AV1 *pp = &pic->pp;
    unsigned i, j;

    memset(pp, 0, sizeof(*pp));
    pp->width = h->upscaled_width;
    pp->height = h->height;
    pp->max_width = s->max_width;
    pp->max_height = s->max_height;
    pp->CurrPicTextureIndex = (unsigned char)cur;
    pp->superres_denom = (unsigned char)(h->use_superres ? h->denom : 8);
    pp->bitdepth = (unsigned char)s->bit_depth;
    pp->seq_profile = (unsigned char)s->profile;
    pp->tiles.cols = (unsigned char)h->tile_cols;
    pp->tiles.rows = (unsigned char)h->tile_rows;
    pp->tiles.context_update_id = (unsigned short)h->context_update_id;
    for (i = 0; i < h->tile_cols; i++)
        pp->tiles.widths[i] = (unsigned short)(h->col_start_sb[i + 1] - h->col_start_sb[i]);
    for (i = 0; i < h->tile_rows; i++)
        pp->tiles.heights[i] = (unsigned short)(h->row_start_sb[i + 1] - h->row_start_sb[i]);
    pp->CodingParamToolFlags =
        s->sb128 | (s->intra_edge << 1) | (s->interintra << 2) | (s->masked_compound << 3) |
        (h->warped << 4) | (s->dual_filter << 5) | (s->jnt_comp << 6) | (h->sct << 7) |
        (h->force_integer_mv << 8) | (s->cdef << 9) | (s->restoration << 10) | (s->film_grain_present << 11) |
        (h->intrabc << 12) | (h->hp << 13) | (h->switchable_motion << 14) | (s->filter_intra << 15) |
        (h->disable_end_update << 16) | (h->disable_cdf_update << 17) | (h->reference_select << 18) |
        (h->skip_mode << 19) | (h->reduced_tx << 20) | (h->use_superres << 21) | (h->tx_mode << 22) |
        (h->use_ref_frame_mvs << 24) | (s->ref_frame_mvs << 25) | (1u << 26);
    pp->FormatAndPictureInfoFlags = (unsigned char)(h->frame_type | (h->show_frame << 2) | (h->showable << 3) |
                                                    (s->ss_x << 4) | (s->ss_y << 5) | (s->mono << 6));
    pp->primary_ref_frame = (unsigned char)h->primary;
    pp->order_hint = (unsigned char)h->order_hint;
    pp->order_hint_bits = (unsigned char)s->order_hint_bits;
    for (i = 0; i < 7; i++) {
        V383_DXVA_PicEntry_AV1 *e = &pp->frame_refs[i];
        unsigned t = h->gm_type[i + 1];

        e->Index = V383_AV1_NO_SURFACE;
        if (!h->intra && p->ref[h->ref_idx[i]].surface >= 0) {
            const Av1Slot *r = &p->ref[h->ref_idx[i]];
            e->Index = (unsigned char)r->surface;
            e->width = r->upscaled_width;
            e->height = r->height;
        }
        for (j = 0; j < 6; j++)
            e->wmmat[j] = h->gm[i + 1][j];
        e->GlobalMotionFlags = (unsigned char)((t == 0) | (t << 1));
    }
    for (i = 0; i < 8; i++)
        pp->RefFrameMapTextureIndex[i] = (unsigned char)(p->ref[i].surface < 0 ? V383_AV1_NO_SURFACE : p->ref[i].surface);
    pp->loop_filter.filter_level[0] = (unsigned char)h->lf[0];
    pp->loop_filter.filter_level[1] = (unsigned char)h->lf[1];
    pp->loop_filter.filter_level_u = (unsigned char)h->lf[2];
    pp->loop_filter.filter_level_v = (unsigned char)h->lf[3];
    pp->loop_filter.sharpness_level = (unsigned char)h->lf_sharp;
    pp->loop_filter.ControlFlags = (unsigned char)(h->lf_delta_en | (h->lf_delta_update << 1) | (h->dlf_multi << 2) |
                                                   (h->dlf_present << 3));
    for (i = 0; i < 8; i++)
        pp->loop_filter.ref_deltas[i] = h->lf_ref[i];
    pp->loop_filter.mode_deltas[0] = h->lf_mode[0];
    pp->loop_filter.mode_deltas[1] = h->lf_mode[1];
    pp->loop_filter.delta_lf_res = (unsigned char)h->dlf_res;
    for (i = 0; i < 3; i++) {
        pp->loop_filter.frame_restoration_type[i] = (unsigned char)h->lr_type[i];
        pp->loop_filter.log2_restoration_unit_size[i] = (unsigned short)h->lr_log2[i];
    }
    pp->quantization.ControlFlags = (unsigned char)(h->dq_present | (h->dq_res << 1));
    pp->quantization.base_qindex = (unsigned char)h->base_q;
    pp->quantization.y_dc_delta_q = (signed char)h->dq_ydc;
    pp->quantization.u_dc_delta_q = (signed char)h->dq_udc;
    pp->quantization.v_dc_delta_q = (signed char)h->dq_vdc;
    pp->quantization.u_ac_delta_q = (signed char)h->dq_uac;
    pp->quantization.v_ac_delta_q = (signed char)h->dq_vac;
    pp->quantization.qm_y = (unsigned char)(h->using_qm ? h->qm_y : 0xFF);
    pp->quantization.qm_u = (unsigned char)(h->using_qm ? h->qm_u : 0xFF);
    pp->quantization.qm_v = (unsigned char)(h->using_qm ? h->qm_v : 0xFF);
    pp->cdef.ControlFlags = (unsigned char)(h->cdef_damping_m3 | (h->cdef_bits << 2));
    for (i = 0; i < 8; i++) {
        pp->cdef.y_strengths[i] = (unsigned char)h->cdef_y[i];
        pp->cdef.uv_strengths[i] = (unsigned char)h->cdef_uv[i];
    }
    pp->interp_filter = (unsigned char)h->interp;
    pp->segmentation.ControlFlags = (unsigned char)(h->seg_en | (h->seg_update_map << 1) | (h->seg_update_data << 2) |
                                                    (h->seg_temporal << 3));
    for (i = 0; i < 8; i++) {
        unsigned char m = 0;
        for (j = 0; j < 8; j++) {
            if (h->feat_en[i][j])
                m |= (unsigned char)(1u << j);
            pp->segmentation.feature_data[i][j] = h->feat[i][j];
        }
        pp->segmentation.feature_mask[i] = m;
    }
    if (h->fg.apply) {
        const Av1FilmGrain *g = &h->fg;
        pp->film_grain.ControlFlags = (unsigned short)(1 | (g->scaling_m8 << 1) | (g->csfl << 3) | (g->lag << 4) |
                                                       (g->ar_shift_m6 << 6) | (g->scale_shift << 8) |
                                                       (g->overlap << 10) | (g->clip << 11) |
                                                       ((s->matrix_coefficients == 0) << 12));
        pp->film_grain.grain_seed = (unsigned short)g->seed;
        memcpy(pp->film_grain.scaling_points_y, g->y_pts, sizeof(g->y_pts));
        pp->film_grain.num_y_points = (unsigned char)g->num_y;
        memcpy(pp->film_grain.scaling_points_cb, g->cb_pts, sizeof(g->cb_pts));
        pp->film_grain.num_cb_points = (unsigned char)g->num_cb;
        memcpy(pp->film_grain.scaling_points_cr, g->cr_pts, sizeof(g->cr_pts));
        pp->film_grain.num_cr_points = (unsigned char)g->num_cr;
        memcpy(pp->film_grain.ar_coeffs_y, g->ar_y, 24);
        memcpy(pp->film_grain.ar_coeffs_cb, g->ar_cb, 25);
        memcpy(pp->film_grain.ar_coeffs_cr, g->ar_cr, 25);
        pp->film_grain.cb_mult = (unsigned char)g->cb_mult;
        pp->film_grain.cb_luma_mult = (unsigned char)g->cb_luma_mult;
        pp->film_grain.cr_mult = (unsigned char)g->cr_mult;
        pp->film_grain.cr_luma_mult = (unsigned char)g->cr_luma_mult;
        pp->film_grain.cb_offset = (short)g->cb_offset;
        pp->film_grain.cr_offset = (short)g->cr_offset;
    }
    pp->StatusReportFeedbackNumber = p->decode_index + 1;

    memset(&pic->extra, 0, sizeof(pic->extra));
    pic->extra.valid = 1;
    for (i = 0; i < 7; i++)
        pic->extra.ref_frame_idx[i] = (unsigned char)(h->intra ? 0 : h->ref_idx[i]);
    pic->extra.tile_size_bytes = (unsigned char)h->tile_size_bytes;
    pic->extra.tile_cols_log2 = (unsigned char)h->tile_cols_log2;
    pic->extra.tile_rows_log2 = (unsigned char)h->tile_rows_log2;
    pic->extra.matrix_coefficients = (unsigned char)s->matrix_coefficients;
    pic->extra.operating_point_idc = (unsigned short)s->op_idc[0];
    pic->extra.frame_header_bits = h->header_bits;
}

static int pick_surface(Av1Parser *p)
{
    int s;

    for (s = 0; s < AV1_MAX_SLOTS; s++)
        if (!p->surface_busy[s])
            return s;
    return -1;
}

static void update_busy(Av1Parser *p)
{
    unsigned i;

    memset(p->surface_busy, 0, sizeof(p->surface_busy));
    for (i = 0; i < 8; i++)
        if (p->ref[i].surface >= 0)
            p->surface_busy[p->ref[i].surface] = 1;
}

/* spec 7.20, for frame h decoded into surface cur */
static void reference_update(Av1Parser *p, const Fh *h, int cur)
{
    unsigned i, j;

    for (i = 0; i < 8; i++) {
        Av1Slot *r = &p->ref[i];
        if (!(h->refresh & (1u << i)))
            continue;
        r->surface = cur;
        r->valid = 1;
        r->upscaled_width = h->upscaled_width;
        r->width = h->width;
        r->height = h->height;
        r->render_width = h->render_width;
        r->render_height = h->render_height;
        r->frame_type = h->frame_type;
        r->order_hint = h->order_hint;
        r->bit_depth = p->seq.bit_depth;
        r->showable = h->showable;
        for (j = 0; j < 7; j++)
            r->saved_hints[j + 1] = h->intra ? 0 : p->ref[h->ref_idx[j]].order_hint;
        memcpy(r->gm, h->gm, sizeof(r->gm));
        memcpy(r->lf_ref, h->lf_ref, sizeof(r->lf_ref));
        memcpy(r->lf_mode, h->lf_mode, sizeof(r->lf_mode));
        memcpy(r->feat_en, h->feat_en, sizeof(r->feat_en));
        memcpy(r->feat, h->feat, sizeof(r->feat));
        r->fg = h->fg;
    }
}

/* the frame being collected */
typedef struct Cur {
    int      active;
    Fh       h;
    int      surface;
    unsigned tiles_seen;
    const uint8_t *obu_start;
} Cur;

static unsigned max_spatial(const Av1Seq *s)
{
    unsigned idc = s->op_idc[0] >> 8, m = 0, i;

    for (i = 0; i < 4; i++)
        if (idc & (1u << i))
            m = i;
    return m;
}

static int finish_frame(Av1Parser *p, Cur *c, const uint8_t *obu_end, Av1DecodeFn dec, void *ctx)
{
    Av1Picture *pic = p->pic;
    Fh *h = &c->h;
    int show;

    pic->obu = c->obu_start;
    pic->obu_len = (size_t)(obu_end - c->obu_start);
    pic->tile_data = p->tile_buf;
    pic->decode_index = p->decode_index;
    pic->packet = p->packets;
    pic->width = h->upscaled_width;
    pic->height = h->height;
    pic->bit_depth = p->seq.bit_depth;
    pic->refresh_frame_flags = h->refresh;
    pic->frame_header_bits = h->header_bits;
    if (dec && dec(ctx, pic))
        return -1;
    reference_update(p, h, c->surface);
    update_busy(p);
    show = h->show_frame && (!p->seq.op_idc[0] || h->spatial_id == max_spatial(&p->seq));
    if (h->show_frame && !show)
        p->dropped_layers++;
    if (show) {
        p->shown++;
        if (dec && p->out_fn)
            p->out_fn(p->out_ctx, c->surface, h->upscaled_width, h->height, p->decode_index, 0);
    } else if (!h->show_frame) {
        p->frames_hidden++;
    }
    p->decode_index++;
    c->active = 0;
    return 0;
}

static int start_frame(Av1Parser *p, Cur *c, const uint8_t *obu_start)
{
    Fh *h = &c->h;
    const Av1Seq *s = &p->seq;
    unsigned i;

    if (s->profile != 0 || s->mono || s->bit_depth > 10) {
        fail(p, "not Main profile 4:2:0 8/10 bit", s->profile);
        return -1;
    }
    if (!h->intra)
        for (i = 0; i < 7; i++)
            if (p->ref[h->ref_idx[i]].surface < 0) {
                fail(p, "reference to an empty slot", h->ref_idx[i]);
                return -1;
            }
    c->surface = pick_surface(p);
    if (c->surface < 0) {
        fail(p, "no free surface", p->decode_index);
        return -1;
    }
    fill_dxva(p, h, c->surface, p->pic);
    p->pic->num_tiles = 0;
    p->tile_len = 0;
    c->tiles_seen = 0;
    c->active = 1;
    c->obu_start = obu_start;
    if (h->upscaled_width > p->max_width)
        p->max_width = h->upscaled_width;
    if (h->height > p->max_height)
        p->max_height = h->height;
    if (s->bit_depth > p->max_bit_depth)
        p->max_bit_depth = s->bit_depth;
    if (h->frame_type == V383_AV1_KEY_FRAME)
        p->keyframes++;
    if (h->frame_type == V383_AV1_INTRA_ONLY_FRAME)
        p->intra_only++;
    if (h->frame_type == V383_AV1_SWITCH_FRAME)
        p->switch_frames++;
    if (h->use_superres)
        p->superres++;
    if (h->fg.apply)
        p->grain_frames++;
    if (s->sb128)
        p->sb128_frames++;
    if (h->tile_cols * h->tile_rows > p->tiles_max)
        p->tiles_max = h->tile_cols * h->tile_rows;
    if (!h->intra)
        for (i = 0; i < 7; i++) {
            const Av1Slot *r = &p->ref[h->ref_idx[i]];
            if (r->upscaled_width != h->upscaled_width || r->height != h->height) {
                p->scaled_refs++;
                break;
            }
        }
    return 0;
}

/* tile_group_obu(): 1 when the frame's last tile was in it */
static int tile_group(Av1Parser *p, Cur *c, const uint8_t *d, size_t sz)
{
    Fh *h = &c->h;
    Av1Picture *pic = p->pic;
    unsigned num = h->tile_cols * h->tile_rows, start, end, t, bits = h->tile_cols_log2 + h->tile_rows_log2;
    Bits b;
    size_t pos;

    bits_init(&b, d, sz);
    if (num > 1 && f(&b, 1)) {
        start = f(&b, bits);
        end = f(&b, bits);
    } else {
        start = 0;
        end = num - 1;
    }
    byte_align(&b);
    pos = b.pos >> 3;
    if (end >= num || start > end || pos > sz) {
        fail(p, "bad tile group", p->decode_index);
        return -1;
    }
    for (t = start; t <= end; t++) {
        size_t ts;
        V383_DXVA_Tile_AV1 *e;

        if (t == end) {
            ts = sz - pos;
        } else {
            unsigned k;
            ts = 0;
            if (pos + h->tile_size_bytes > sz) {
                fail(p, "tile size past the tile group", t);
                return -1;
            }
            for (k = 0; k < h->tile_size_bytes; k++)
                ts |= (size_t)d[pos + k] << (8 * k);
            ts++;
            pos += h->tile_size_bytes;
            if (pos + ts > sz) {
                fail(p, "tile past the tile group", t);
                return -1;
            }
        }
        if (pic->num_tiles >= AV1_MAX_TILES) {
            fail(p, "too many tiles", t);
            return -1;
        }
        if (p->tile_len + ts > p->tile_buf_cap) {
            size_t cap = (p->tile_len + ts) * 2 + 65536;
            uint8_t *nb = (uint8_t *)realloc(p->tile_buf, cap);
            if (!nb) {
                fail(p, "out of memory", 0);
                return -1;
            }
            p->tile_buf = nb;
            p->tile_buf_cap = cap;
        }
        memcpy(p->tile_buf + p->tile_len, d + pos, ts);
        e = &pic->tiles[pic->num_tiles++];
        memset(e, 0, sizeof(*e));
        e->DataOffset = (unsigned)p->tile_len;
        e->DataSize = (unsigned)ts;
        e->row = (unsigned short)(t / h->tile_cols);
        e->column = (unsigned short)(t % h->tile_cols);
        p->tile_len += ts;
        pos += ts;
    }
    c->tiles_seen = end + 1;
    return end == num - 1;
}

static int show_existing(Av1Parser *p, const Fh *h, Av1DecodeFn dec)
{
    Av1Slot *r = &p->ref[h->existing_idx];

    if (r->surface < 0) {
        fail(p, "show_existing_frame of an empty slot", h->existing_idx);
        return -1;
    }
    p->existing++;
    if (dec && p->out_fn)
        p->out_fn(p->out_ctx, r->surface, r->upscaled_width, r->height, p->decode_index, 1);
    if (r->frame_type == V383_AV1_KEY_FRAME) {  /* frame loading process: refresh every slot */
        Av1Slot k = *r;
        unsigned i;
        for (i = 0; i < 8; i++)
            p->ref[i] = k;
        update_busy(p);
    }
    return 0;
}

/* One temporal unit (IVF frame). */
static int do_tu(Av1Parser *p, const uint8_t *d, size_t len, Av1DecodeFn dec, void *ctx)
{
    size_t pos = 0;
    Cur c;
    int seen_header = 0;
    const uint8_t *chunk = d;

    memset(&c, 0, sizeof(c));
    while (pos < len) {
        size_t start = pos, hdr_len, osz;
        unsigned type, ext, has_size, tid = 0, sid = 0;
        int bad = 0;
        const uint8_t *payload;
        uint8_t b0 = d[pos++];

        type = (b0 >> 3) & 15;
        ext = (b0 >> 2) & 1;
        has_size = (b0 >> 1) & 1;
        if (ext) {
            if (pos >= len)
                break;
            tid = d[pos] >> 5;
            sid = (d[pos] >> 3) & 3;
            pos++;
        }
        osz = has_size ? leb128(d, len, &pos, &bad) : len - pos;
        if (bad || osz > len - pos) {
            fail(p, "OBU past the temporal unit", p->packets);
            return -1;
        }
        hdr_len = pos - start;
        (void)hdr_len;
        payload = d + pos;
        pos += osz;

        if (type != 1 && type != 2 && ext && p->seq.op_idc[0]) {
            unsigned idc = p->seq.op_idc[0];
            if (!((idc >> tid) & 1) || !((idc >> (sid + 8)) & 1))
                continue;
        }
        switch (type) {
        case 1:                                     /* OBU_SEQUENCE_HEADER */
            if (parse_seq(p, payload, osz))
                return -1;
            break;
        case 2:                                     /* OBU_TEMPORAL_DELIMITER */
            seen_header = 0;
            break;
        case 3:                                     /* OBU_FRAME_HEADER */
        case 6:                                     /* OBU_FRAME */
        case 7: {                                   /* OBU_REDUNDANT_FRAME_HEADER */
            Bits b;

            if (!p->seq.valid) {
                fail(p, "frame before a sequence header", p->packets);
                return -1;
            }
            if (seen_header && type != 6)
                break;
            if (p->max_pictures && p->decode_index >= p->max_pictures)
                return 0;
            bits_init(&b, payload, osz);
            memset(&c.h, 0, sizeof(c.h));
            c.h.temporal_id = tid;
            c.h.spatial_id = sid;
            if (parse_frame_header(p, &b, &c.h))
                return -1;
            if (c.h.show_existing) {
                if (show_existing(p, &c.h, dec))
                    return -1;
                chunk = d + pos;
                break;
            }
            seen_header = 1;
            if (start_frame(p, &c, chunk))
                return -1;
            if (type == 6) {
                int r;
                byte_align(&b);
                if ((b.pos >> 3) > osz) {
                    fail(p, "OBU_FRAME without tiles", p->decode_index);
                    return -1;
                }
                r = tile_group(p, &c, payload + (b.pos >> 3), osz - (b.pos >> 3));
                if (r < 0)
                    return -1;
                if (r) {
                    if (finish_frame(p, &c, d + pos, dec, ctx))
                        return -1;
                    seen_header = 0;
                    chunk = d + pos;
                }
            }
            break;
        }
        case 4: {                                   /* OBU_TILE_GROUP */
            int r;
            if (!c.active)
                break;
            r = tile_group(p, &c, payload, osz);
            if (r < 0)
                return -1;
            if (r) {
                if (finish_frame(p, &c, d + pos, dec, ctx))
                    return -1;
                seen_header = 0;
                chunk = d + pos;
            }
            break;
        }
        case 8:
            fail(p, "large-scale tile decoding is not supported", p->packets);
            return -1;
        default:
            break;
        }
    }
    if (c.active) {
        fail(p, "temporal unit ends inside a frame", p->packets);
        return -1;
    }
    return 0;
}

static unsigned le32(const uint8_t *d)
{
    return d[0] | ((unsigned)d[1] << 8) | ((unsigned)d[2] << 16) | ((unsigned)d[3] << 24);
}

int av1_probe(const uint8_t *data, size_t len)
{
    return len >= 32 && !memcmp(data, "DKIF", 4) && !memcmp(data + 8, "AV01", 4);
}

void av1_init(Av1Parser *p, Av1OutputFn out, void *ctx)
{
    unsigned i;
    uint8_t *tb = p->tile_buf;
    size_t cap = p->tile_buf_cap;
    Av1Picture *pic = p->pic;

    memset(p, 0, sizeof(*p));
    p->tile_buf = tb;
    p->tile_buf_cap = cap;
    p->pic = pic;
    for (i = 0; i < 8; i++)
        p->ref[i].surface = -1;
    p->out_fn = out;
    p->out_ctx = ctx;
}

void av1_free(Av1Parser *p)
{
    free(p->tile_buf);
    free(p->pic);
    p->tile_buf = NULL;
    p->pic = NULL;
}

int av1_run(Av1Parser *p, const uint8_t *data, size_t len, unsigned max_pictures, Av1DecodeFn dec, void *ctx)
{
    size_t pos, hdr;

    if (!p->pic) {
        p->pic = (Av1Picture *)calloc(1, sizeof(*p->pic));
        if (!p->pic) {
            fail(p, "out of memory", 0);
            return -1;
        }
    }
    if (!av1_probe(data, len)) {
        fail(p, "not an IVF file with AV1", 0);
        return -1;
    }
    hdr = data[6] | ((size_t)data[7] << 8);
    if (hdr < 32 || hdr > len) {
        fail(p, "bad IVF header length", (unsigned)hdr);
        return -1;
    }
    p->max_pictures = max_pictures;
    for (pos = hdr; pos + 12 <= len; ) {
        size_t fs = le32(data + pos);

        pos += 12;
        if (fs > len - pos) {
            fail(p, "IVF frame past the end of the file", p->packets);
            return -1;
        }
        if (do_tu(p, data + pos, fs, dec, ctx))
            return -1;
        p->packets++;
        pos += fs;
        if (p->max_pictures && p->decode_index >= p->max_pictures)
            break;
    }
    return 0;
}
