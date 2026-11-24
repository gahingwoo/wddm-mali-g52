/*
 * h264parse.c - see h264parse.h.
 *
 * Section numbers are ITU-T H.264 (08/2021).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "h264parse.h"

/* ------------------------------------------------------------ bit reader */

typedef struct Bits {
    uint8_t *buf;       /* RBSP (emulation prevention removed) */
    size_t   len;
    size_t   pos;       /* bits */
    int      overrun;
} Bits;

static uint8_t *rbsp_alloc(const uint8_t *nal, size_t len, size_t *out_len)
{
    uint8_t *r = (uint8_t *)malloc(len ? len : 1);
    size_t i, n = 0;
    unsigned zeros = 0;

    if (!r)
        return NULL;
    for (i = 0; i < len; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        r[n++] = nal[i];
    }
    *out_len = n;
    return r;
}

static unsigned u1(Bits *b)
{
    unsigned v;
    if (b->pos >= b->len * 8) {
        b->overrun = 1;
        return 0;
    }
    v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static unsigned un(Bits *b, unsigned n)
{
    unsigned v = 0;
    while (n--)
        v = (v << 1) | u1(b);
    return v;
}

static unsigned ue(Bits *b)
{
    unsigned zeros = 0;
    while (!u1(b)) {
        if (b->overrun || ++zeros > 31) {
            b->overrun = 1;
            return 0;
        }
    }
    return ((1u << zeros) - 1) + un(b, zeros);
}

static int se(Bits *b)
{
    unsigned k = ue(b);
    return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

static int more_rbsp_data(Bits *b)
{
    size_t last;
    int bit;

    if (b->len == 0)
        return 0;
    /* the last 1 bit in the RBSP is the stop bit */
    last = b->len;
    while (last > 0 && b->buf[last - 1] == 0)
        last--;
    if (last == 0)
        return 0;
    for (bit = 0; bit < 8; bit++)
        if (b->buf[last - 1] & (1u << bit))
            break;
    return b->pos < (last - 1) * 8 + (7 - (size_t)bit);
}

static void set_err(H264Parser *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------ scaling lists */

static const uint8_t zz4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t zz8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* Table 7-3 and 7-4, zig-zag order as in the spec. */
static const uint8_t def4_intra[16] = { 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 };
static const uint8_t def4_inter[16] = { 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 };
static const uint8_t def8_intra[64] = {
     6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
    23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
    31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
};
static const uint8_t def8_inter[64] = {
     9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
    21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
};

/* 7.3.2.1.1.1: read one list in zig-zag order into raw[]; *use_default set
 * when useDefaultScalingMatrixFlag. */
static void read_scaling_list(Bits *b, uint8_t *raw, unsigned size, unsigned *use_default)
{
    int last = 8, next = 8;
    unsigned j;

    *use_default = 0;
    for (j = 0; j < size; j++) {
        if (next != 0) {
            int delta = se(b);
            next = (last + delta + 256) % 256;
            if (j == 0 && next == 0) {
                *use_default = 1;
                break;
            }
        }
        raw[j] = (uint8_t)(next == 0 ? last : next);
        last = raw[j];
    }
}

static void zz_to_raster4(const uint8_t *zz, uint8_t *raster)
{
    unsigned j;
    for (j = 0; j < 16; j++)
        raster[zz4[j]] = zz[j];
}

static void zz_to_raster8(const uint8_t *zz, uint8_t *raster)
{
    unsigned j;
    for (j = 0; j < 64; j++)
        raster[zz8[j]] = zz[j];
}

/*
 * Resolve lists i = 0..n-1 (6 4x4 lists, then the 8x8 lists) with
 * fall-back rule A (fallback = NULL) or B (fallback = the SPS lists).
 */
/* Lists are passed flat (6 x 16 and 6 x 64 bytes) so that const and
 * non-const arrays can be handed in without casts. */
static void resolve_lists(const unsigned *present, const unsigned *use_default,
                          const uint8_t *raw4, const uint8_t *raw8, unsigned n8,
                          const uint8_t *fb4, const uint8_t *fb8,
                          uint8_t *o4, uint8_t *o8)
{
    unsigned i;
#define OUT4(i) (o4 + (i) * 16)
#define OUT8(i) (o8 + (i) * 64)

    for (i = 0; i < 6; i++) {
        if (present[i]) {
            if (use_default[i])
                zz_to_raster4(i < 3 ? def4_intra : def4_inter, OUT4(i));
            else
                zz_to_raster4(raw4 + i * 16, OUT4(i));
        } else if (i == 0 || i == 3) {
            if (fb4)
                memcpy(OUT4(i), fb4 + i * 16, 16);
            else
                zz_to_raster4(i == 0 ? def4_intra : def4_inter, OUT4(i));
        } else {
            memcpy(OUT4(i), OUT4(i - 1), 16);
        }
    }
    for (i = 0; i < 6; i++) {
        if (i >= n8) {
            /* not signalled for this chroma format: keep something sane */
            zz_to_raster8((i & 1) ? def8_inter : def8_intra, OUT8(i));
            continue;
        }
        if (present[6 + i]) {
            if (use_default[6 + i])
                zz_to_raster8((i & 1) ? def8_inter : def8_intra, OUT8(i));
            else
                zz_to_raster8(raw8 + i * 64, OUT8(i));
        } else if (i < 2) {
            if (fb8)
                memcpy(OUT8(i), fb8 + i * 64, 64);
            else
                zz_to_raster8(i == 0 ? def8_intra : def8_inter, OUT8(i));
        } else {
            memcpy(OUT8(i), OUT8(i - 2), 64);
        }
    }
#undef OUT4
#undef OUT8
}

static void flat_lists(uint8_t *out4, uint8_t *out8)
{
    memset(out4, 16, 6 * 16);
    memset(out8, 16, 6 * 64);
}

/* --------------------------------------------------------------- SPS */

static void parse_hrd(Bits *b)
{
    unsigned i, cnt = ue(b) + 1;
    un(b, 4);
    un(b, 4);
    for (i = 0; i < cnt && i < 32; i++) {
        ue(b);
        ue(b);
        u1(b);
    }
    un(b, 5);
    un(b, 5);
    un(b, 5);
    un(b, 5);
}

static void parse_vui(Bits *b, H264Sps *s)
{
    unsigned nal_hrd, vcl_hrd;

    if (u1(b)) {                        /* aspect_ratio_info_present_flag */
        if (un(b, 8) == 255) {
            un(b, 16);
            un(b, 16);
        }
    }
    if (u1(b))                          /* overscan_info_present_flag */
        u1(b);
    if (u1(b)) {                        /* video_signal_type_present_flag */
        un(b, 3);
        u1(b);
        if (u1(b)) {
            un(b, 8);
            un(b, 8);
            un(b, 8);
        }
    }
    if (u1(b)) {                        /* chroma_loc_info_present_flag */
        ue(b);
        ue(b);
    }
    if (u1(b)) {                        /* timing_info_present_flag */
        un(b, 16); un(b, 16);
        un(b, 16); un(b, 16);
        u1(b);
    }
    nal_hrd = u1(b);
    if (nal_hrd)
        parse_hrd(b);
    vcl_hrd = u1(b);
    if (vcl_hrd)
        parse_hrd(b);
    if (nal_hrd || vcl_hrd)
        u1(b);                          /* low_delay_hrd_flag */
    u1(b);                              /* pic_struct_present_flag */
    if (u1(b)) {                        /* bitstream_restriction_flag */
        u1(b);
        ue(b);
        ue(b);
        ue(b);
        ue(b);
        ue(b);                          /* max_num_reorder_frames */
        s->max_dec_frame_buffering = (int)ue(b);
        if (b->overrun)
            s->max_dec_frame_buffering = -1;
    }
}

static int parse_sps(H264Parser *p, Bits *b)
{
    H264Sps s;
    unsigned i, n_lists;
    unsigned present[12] = { 0 }, use_default[12] = { 0 };
    uint8_t raw4[6][16], raw8[6][64];

    memset(&s, 0, sizeof(s));
    s.max_dec_frame_buffering = -1;
    s.profile_idc = un(b, 8);
    s.constraint_flags = un(b, 8);
    s.level_idc = un(b, 8);
    s.sps_id = ue(b);
    if (s.sps_id > 31) {
        set_err(p, "sps id %u", s.sps_id);
        return -1;
    }
    s.chroma_format_idc = 1;
    switch (s.profile_idc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        s.chroma_format_idc = ue(b);
        if (s.chroma_format_idc == 3)
            s.separate_colour_plane_flag = u1(b);
        s.bit_depth_luma_minus8 = ue(b);
        s.bit_depth_chroma_minus8 = ue(b);
        s.qpprime_y_zero_transform_bypass_flag = u1(b);
        s.seq_scaling_matrix_present_flag = u1(b);
        if (s.seq_scaling_matrix_present_flag) {
            n_lists = (s.chroma_format_idc != 3) ? 8 : 12;
            for (i = 0; i < n_lists; i++) {
                present[i] = u1(b);
                if (present[i]) {
                    if (i < 6)
                        read_scaling_list(b, raw4[i], 16, &use_default[i]);
                    else
                        read_scaling_list(b, raw8[i - 6], 64, &use_default[i]);
                }
            }
            resolve_lists(present, use_default, &raw4[0][0], &raw8[0][0], n_lists - 6, NULL, NULL,
                          &s.sl4[0][0], &s.sl8[0][0]);
        }
        break;
    default:
        break;
    }
    if (!s.seq_scaling_matrix_present_flag)
        flat_lists(&s.sl4[0][0], &s.sl8[0][0]);

    s.log2_max_frame_num_minus4 = ue(b);
    s.pic_order_cnt_type = ue(b);
    if (s.pic_order_cnt_type == 0) {
        s.log2_max_poc_lsb_minus4 = ue(b);
    } else if (s.pic_order_cnt_type == 1) {
        s.delta_pic_order_always_zero_flag = u1(b);
        s.offset_for_non_ref_pic = se(b);
        s.offset_for_top_to_bottom_field = se(b);
        s.num_ref_frames_in_poc_cycle = ue(b);
        if (s.num_ref_frames_in_poc_cycle > 255) {
            set_err(p, "num_ref_frames_in_pic_order_cnt_cycle %u", s.num_ref_frames_in_poc_cycle);
            return -1;
        }
        for (i = 0; i < s.num_ref_frames_in_poc_cycle; i++)
            s.offset_for_ref_frame[i] = se(b);
    }
    s.max_num_ref_frames = ue(b);
    s.gaps_in_frame_num_allowed_flag = u1(b);
    s.pic_width_in_mbs_minus1 = ue(b);
    s.pic_height_in_map_units_minus1 = ue(b);
    s.frame_mbs_only_flag = u1(b);
    if (!s.frame_mbs_only_flag)
        s.mb_adaptive_frame_field_flag = u1(b);
    s.direct_8x8_inference_flag = u1(b);
    if (u1(b)) {                        /* frame_cropping_flag */
        unsigned cx = 2, cy = 2 * (2 - s.frame_mbs_only_flag);
        if (s.chroma_format_idc == 0 || s.separate_colour_plane_flag) {
            cx = 1;
            cy = 2 - s.frame_mbs_only_flag;
        } else if (s.chroma_format_idc == 2) {
            cx = 2;
            cy = 2 - s.frame_mbs_only_flag;
        } else if (s.chroma_format_idc == 3) {
            cx = 1;
            cy = 2 - s.frame_mbs_only_flag;
        }
        s.crop_left = ue(b) * cx;
        s.crop_right = ue(b) * cx;
        s.crop_top = ue(b) * cy;
        s.crop_bottom = ue(b) * cy;
    }
    if (u1(b))
        parse_vui(b, &s);
    if (b->overrun) {
        set_err(p, "SPS %u truncated", s.sps_id);
        return -1;
    }
    s.valid = 1;
    p->sps[s.sps_id] = s;
    /* a new SPS invalidates PPS list resolution against it */
    for (i = 0; i < 256; i++)
        if (p->pps[i].valid && p->pps[i].sl_resolved_for_sps == (int)s.sps_id)
            p->pps[i].sl_resolved_for_sps = -1;
    return 0;
}

/* --------------------------------------------------------------- PPS */

static int parse_pps(H264Parser *p, Bits *b)
{
    H264Pps q;
    unsigned i;

    memset(&q, 0, sizeof(q));
    q.sl_resolved_for_sps = -1;
    q.pps_id = ue(b);
    q.sps_id = ue(b);
    if (q.pps_id > 255 || q.sps_id > 31) {
        set_err(p, "pps id %u / sps id %u", q.pps_id, q.sps_id);
        return -1;
    }
    q.entropy_coding_mode_flag = u1(b);
    q.bottom_field_pic_order_in_frame_present_flag = u1(b);
    q.num_slice_groups_minus1 = ue(b);
    if (q.num_slice_groups_minus1 > 0) {
        /* FMO: parsed far enough to be skipped; pictures using it are refused. */
        unsigned type = ue(b), g;
        if (type == 0) {
            for (g = 0; g <= q.num_slice_groups_minus1; g++)
                ue(b);
        } else if (type == 2) {
            for (g = 0; g < q.num_slice_groups_minus1; g++) {
                ue(b);
                ue(b);
            }
        } else if (type >= 3 && type <= 5) {
            u1(b);
            ue(b);
        } else if (type == 6) {
            unsigned n = ue(b) + 1, bits = 0;
            while ((1u << bits) < q.num_slice_groups_minus1 + 1)
                bits++;
            for (g = 0; g < n; g++)
                un(b, bits);
        }
    }
    q.num_ref_idx_l0_default_active_minus1 = ue(b);
    q.num_ref_idx_l1_default_active_minus1 = ue(b);
    q.weighted_pred_flag = u1(b);
    q.weighted_bipred_idc = un(b, 2);
    q.pic_init_qp_minus26 = se(b);
    q.pic_init_qs_minus26 = se(b);
    q.chroma_qp_index_offset = se(b);
    q.deblocking_filter_control_present_flag = u1(b);
    q.constrained_intra_pred_flag = u1(b);
    q.redundant_pic_cnt_present_flag = u1(b);
    q.second_chroma_qp_index_offset = q.chroma_qp_index_offset;
    if (more_rbsp_data(b)) {
        q.transform_8x8_mode_flag = u1(b);
        q.pic_scaling_matrix_present_flag = u1(b);
        if (q.pic_scaling_matrix_present_flag) {
            /* the list count depends on the SPS chroma format; the SPS
             * referred to must already exist (7.4.1.2.1) */
            const H264Sps *s = &p->sps[q.sps_id];
            unsigned n = 6 + ((s->valid && s->chroma_format_idc == 3) ? 6 : 2) * q.transform_8x8_mode_flag;
            for (i = 0; i < n; i++) {
                q.sl_present[i] = u1(b);
                if (q.sl_present[i]) {
                    if (i < 6)
                        read_scaling_list(b, q.sl_raw4[i], 16, &q.sl_default[i]);
                    else
                        read_scaling_list(b, q.sl_raw8[i - 6], 64, &q.sl_default[i]);
                }
            }
        }
        q.second_chroma_qp_index_offset = se(b);
    }
    if (b->overrun) {
        set_err(p, "PPS %u truncated", q.pps_id);
        return -1;
    }
    q.valid = 1;
    p->pps[q.pps_id] = q;
    return 0;
}

/* Lists in effect for a picture with this PPS (7.4.2.1.1, 7.4.2.2). */
static void resolve_pps_lists(H264Pps *q, const H264Sps *s)
{
    if (q->sl_resolved_for_sps == (int)s->sps_id)
        return;
    if (q->pic_scaling_matrix_present_flag) {
        unsigned n8 = q->transform_8x8_mode_flag ? (s->chroma_format_idc == 3 ? 6 : 2) : 0;
        if (s->seq_scaling_matrix_present_flag)
            resolve_lists(q->sl_present, q->sl_default, &q->sl_raw4[0][0], &q->sl_raw8[0][0], n8,
                          &s->sl4[0][0], &s->sl8[0][0], &q->sl4[0][0], &q->sl8[0][0]);
        else
            resolve_lists(q->sl_present, q->sl_default, &q->sl_raw4[0][0], &q->sl_raw8[0][0], n8,
                          NULL, NULL, &q->sl4[0][0], &q->sl8[0][0]);
    } else {
        memcpy(q->sl4, s->sl4, sizeof(q->sl4));
        memcpy(q->sl8, s->sl8, sizeof(q->sl8));
    }
    q->sl_resolved_for_sps = (int)s->sps_id;
}

/* ------------------------------------------------------- slice header */

typedef struct Mmco {
    unsigned op, diff_pic_nums_minus1, long_term_pic_num, long_term_frame_idx, max_lt_idx_plus1;
} Mmco;

typedef struct SliceHdr {
    unsigned nal_ref_idc, nal_type, idr;
    unsigned first_mb, slice_type, pps_id, frame_num;
    unsigned field_pic, bottom;
    unsigned idr_pic_id, poc_lsb;
    int      delta_bottom, delta0, delta1;
    unsigned redundant_pic_cnt;
    unsigned no_output_of_prior_pics, long_term_reference_flag;
    unsigned adaptive;
    Mmco     mmco[66];
    unsigned n_mmco;
} SliceHdr;

static void skip_ref_list_mod(Bits *b)
{
    unsigned idc, guard = 0;
    if (u1(b)) {
        do {
            idc = ue(b);
            if (idc == 0 || idc == 1 || idc == 2)
                ue(b);
        } while (idc != 3 && !b->overrun && ++guard < 100);
    }
}

static int parse_slice(H264Parser *p, Bits *b, unsigned nal_ref_idc, unsigned nal_type, SliceHdr *h)
{
    const H264Pps *q;
    const H264Sps *s;
    unsigned st, l0, l1, i, chroma_array_type;

    memset(h, 0, sizeof(*h));
    h->nal_ref_idc = nal_ref_idc;
    h->nal_type = nal_type;
    h->idr = nal_type == 5;
    h->first_mb = ue(b);
    h->slice_type = ue(b);
    h->pps_id = ue(b);
    if (h->pps_id > 255 || !p->pps[h->pps_id].valid) {
        set_err(p, "slice refers to missing PPS %u", h->pps_id);
        return -1;
    }
    q = &p->pps[h->pps_id];
    s = &p->sps[q->sps_id];
    if (!s->valid) {
        set_err(p, "PPS %u refers to missing SPS %u", q->pps_id, q->sps_id);
        return -1;
    }
    if (s->separate_colour_plane_flag)
        un(b, 2);
    h->frame_num = un(b, s->log2_max_frame_num_minus4 + 4);
    if (!s->frame_mbs_only_flag) {
        h->field_pic = u1(b);
        if (h->field_pic)
            h->bottom = u1(b);
    }
    if (h->idr)
        h->idr_pic_id = ue(b);
    if (s->pic_order_cnt_type == 0) {
        h->poc_lsb = un(b, s->log2_max_poc_lsb_minus4 + 4);
        if (q->bottom_field_pic_order_in_frame_present_flag && !h->field_pic)
            h->delta_bottom = se(b);
    }
    if (s->pic_order_cnt_type == 1 && !s->delta_pic_order_always_zero_flag) {
        h->delta0 = se(b);
        if (q->bottom_field_pic_order_in_frame_present_flag && !h->field_pic)
            h->delta1 = se(b);
    }
    if (q->redundant_pic_cnt_present_flag)
        h->redundant_pic_cnt = ue(b);

    st = h->slice_type % 5;     /* 0 P, 1 B, 2 I, 3 SP, 4 SI */
    if (st == 1)
        u1(b);                  /* direct_spatial_mv_pred_flag */
    l0 = q->num_ref_idx_l0_default_active_minus1;
    l1 = q->num_ref_idx_l1_default_active_minus1;
    if (st == 0 || st == 3 || st == 1) {
        if (u1(b)) {            /* num_ref_idx_active_override_flag */
            l0 = ue(b);
            if (st == 1)
                l1 = ue(b);
        }
    }
    if (st != 2 && st != 4)
        skip_ref_list_mod(b);
    if (st == 1)
        skip_ref_list_mod(b);

    chroma_array_type = s->separate_colour_plane_flag ? 0 : s->chroma_format_idc;
    if ((q->weighted_pred_flag && (st == 0 || st == 3)) || (q->weighted_bipred_idc == 1 && st == 1)) {
        unsigned list, nlists = (st == 1) ? 2 : 1;
        ue(b);                  /* luma_log2_weight_denom */
        if (chroma_array_type)
            ue(b);
        for (list = 0; list < nlists; list++) {
            unsigned n = (list == 0 ? l0 : l1) + 1;
            for (i = 0; i < n && i < 32; i++) {
                if (u1(b)) {
                    se(b);
                    se(b);
                }
                if (chroma_array_type && u1(b)) {
                    se(b); se(b);
                    se(b); se(b);
                }
            }
        }
    }

    if (nal_ref_idc) {
        if (h->idr) {
            h->no_output_of_prior_pics = u1(b);
            h->long_term_reference_flag = u1(b);
        } else {
            h->adaptive = u1(b);
            if (h->adaptive) {
                unsigned op;
                do {
                    Mmco *m = &h->mmco[h->n_mmco];
                    op = ue(b);
                    memset(m, 0, sizeof(*m));
                    m->op = op;
                    if (op == 1 || op == 3)
                        m->diff_pic_nums_minus1 = ue(b);
                    if (op == 2)
                        m->long_term_pic_num = ue(b);
                    if (op == 3 || op == 6)
                        m->long_term_frame_idx = ue(b);
                    if (op == 4)
                        m->max_lt_idx_plus1 = ue(b);
                    if (op != 0)
                        h->n_mmco++;
                } while (op != 0 && h->n_mmco < 65 && !b->overrun);
            }
        }
    }
    if (b->overrun) {
        set_err(p, "slice header truncated");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- DPB */

static unsigned max_dpb_frames(const H264Sps *s)
{
    static const struct { unsigned level; unsigned mbs; } t[] = {
        { 9, 396 }, { 10, 396 }, { 11, 900 }, { 12, 2376 }, { 13, 2376 }, { 20, 2376 },
        { 21, 4752 }, { 22, 8100 }, { 30, 8100 }, { 31, 18000 }, { 32, 20480 },
        { 40, 32768 }, { 41, 32768 }, { 42, 34816 }, { 50, 110400 }, { 51, 184320 },
        { 52, 184320 }, { 60, 696320 }, { 61, 696320 }, { 62, 696320 },
    };
    unsigned i, mbs = 696320, frame_mbs, n;
    unsigned level = s->level_idc;

    if (level == 11 && (s->constraint_flags & 0x10) && (s->profile_idc == 66 || s->profile_idc == 77))
        level = 9;  /* level 1b */
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (t[i].level == level)
            mbs = t[i].mbs;
    frame_mbs = (s->pic_width_in_mbs_minus1 + 1) * (s->pic_height_in_map_units_minus1 + 1) *
                (2 - s->frame_mbs_only_flag);
    n = mbs / frame_mbs;
    if (s->max_dec_frame_buffering >= 0)
        n = (unsigned)s->max_dec_frame_buffering;
    if (n < s->max_num_ref_frames)
        n = s->max_num_ref_frames;
    if (n < 1)
        n = 1;
    if (n > 16)
        n = 16;
    return n;
}

static unsigned dpb_count(const H264Parser *p)
{
    unsigned i, n = 0;
    for (i = 0; i < 17; i++)
        n += p->dpb[i].in_use;
    return n;
}

static void dpb_remove(H264Parser *p, H264Frame *f)
{
    p->slot_busy[f->slot] = 0;
    memset(f, 0, sizeof(*f));
}

static void dpb_sweep(H264Parser *p)
{
    unsigned i;
    for (i = 0; i < 17; i++)
        if (p->dpb[i].in_use && !p->dpb[i].is_ref && !p->dpb[i].needed_for_output)
            dpb_remove(p, &p->dpb[i]);
}

/* Output the waiting frame with the smallest POC. 0 if there was none. */
static int dpb_bump(H264Parser *p)
{
    H264Frame *best = NULL;
    unsigned i;

    for (i = 0; i < 17; i++) {
        H264Frame *f = &p->dpb[i];
        if (f->in_use && f->needed_for_output && (!best || f->poc < best->poc))
            best = f;
    }
    if (!best)
        return 0;
    if (p->out_fn)
        p->out_fn(p->out_ctx, best->slot, best->decode_index, best->poc);
    best->needed_for_output = 0;
    dpb_sweep(p);
    return 1;
}

static void dpb_flush(H264Parser *p)
{
    while (dpb_bump(p))
        ;
    dpb_sweep(p);
}

static int free_slot(H264Parser *p)
{
    int i;
    for (i = 0; i < H264_MAX_SLOTS; i++)
        if (!p->slot_busy[i])
            return i;
    return -1;
}

static void update_wraps(H264Parser *p, unsigned frame_num, unsigned max_frame_num)
{
    unsigned i;
    for (i = 0; i < 17; i++) {
        H264Frame *f = &p->dpb[i];
        if (f->in_use && f->is_ref == 1)
            f->frame_num_wrap = f->frame_num > frame_num ? (int)f->frame_num - (int)max_frame_num
                                                         : (int)f->frame_num;
    }
}

static void sliding_window(H264Parser *p, const H264Sps *s)
{
    unsigned i, nst = 0, nlt = 0;
    H264Frame *oldest = NULL;
    unsigned max = s->max_num_ref_frames ? s->max_num_ref_frames : 1;

    for (i = 0; i < 17; i++) {
        H264Frame *f = &p->dpb[i];
        if (!f->in_use)
            continue;
        if (f->is_ref == 1) {
            nst++;
            if (!oldest || f->frame_num_wrap < oldest->frame_num_wrap)
                oldest = f;
        } else if (f->is_ref == 2) {
            nlt++;
        }
    }
    if (nst + nlt >= max && nst > 0 && oldest) {
        oldest->is_ref = 0;
        dpb_sweep(p);
    }
}

/* Make room in the DPB and store f (C.4.5.1, C.4.5.2). */
static int dpb_store(H264Parser *p, const H264Frame *cur)
{
    unsigned i;

    dpb_sweep(p);
    if (!cur->is_ref && dpb_count(p) >= p->dpb_size) {
        /* a non-reference picture with a lower POC than everything waiting
         * is output at once */
        int lowest = 1;
        for (i = 0; i < 17; i++)
            if (p->dpb[i].in_use && p->dpb[i].needed_for_output && p->dpb[i].poc < cur->poc)
                lowest = 0;
        if (lowest) {
            if (p->out_fn && cur->needed_for_output)
                p->out_fn(p->out_ctx, cur->slot, cur->decode_index, cur->poc);
            return 0;
        }
    }
    while (dpb_count(p) >= p->dpb_size) {
        if (!dpb_bump(p)) {
            set_err(p, "DPB overflow: %u reference frames in a DPB of %u", dpb_count(p), p->dpb_size);
            return -1;
        }
    }
    for (i = 0; i < 17; i++) {
        if (!p->dpb[i].in_use) {
            p->dpb[i] = *cur;
            p->dpb[i].in_use = 1;
            p->slot_busy[cur->slot] = 1;
            return 0;
        }
    }
    set_err(p, "DPB has no free entry");
    return -1;
}

/* 8.2.5.2: frame_num gaps */
static int fill_frame_num_gap(H264Parser *p, const H264Sps *s, const SliceHdr *h)
{
    unsigned max_frame_num = 1u << (s->log2_max_frame_num_minus4 + 4);
    unsigned fn = (p->prev_ref_frame_num + 1) % max_frame_num;
    unsigned guard = 0;

    while (fn != h->frame_num && guard++ < max_frame_num) {
        H264Frame f;
        int slot;

        update_wraps(p, fn, max_frame_num);
        sliding_window(p, s);
        slot = free_slot(p);
        if (slot < 0) {
            set_err(p, "no free surface for a non-existing frame");
            return -1;
        }
        memset(&f, 0, sizeof(f));
        f.slot = slot;
        f.is_ref = 1;
        f.non_existing = 1;
        f.frame_num = fn;
        f.frame_num_wrap = (int)fn;
        f.needed_for_output = 0;
        if (dpb_store(p, &f))
            return -1;
        p->prev_ref_frame_num = fn;
        p->prev_frame_num = fn;
        fn = (fn + 1) % max_frame_num;
    }
    return 0;
}

/* 8.2.1 */
static void compute_poc(H264Parser *p, const H264Sps *s, const SliceHdr *h,
                        int *top, int *bottom, unsigned *frame_num_offset_out, int *msb_out)
{
    unsigned max_frame_num = 1u << (s->log2_max_frame_num_minus4 + 4);
    unsigned frame_num_offset = 0;

    *msb_out = 0;
    if (s->pic_order_cnt_type == 0) {
        int prev_msb, prev_lsb, msb;
        int max_lsb = 1 << (s->log2_max_poc_lsb_minus4 + 4);
        int lsb = (int)h->poc_lsb;

        if (h->idr) {
            prev_msb = 0;
            prev_lsb = 0;
        } else if (p->prev_ref_has_mmco5) {
            prev_msb = 0;
            prev_lsb = p->prev_ref_is_bottom ? 0 : p->prev_ref_top_poc_after_mmco5;
        } else {
            prev_msb = p->prev_poc_msb;
            prev_lsb = p->prev_poc_lsb;
        }
        if (lsb < prev_lsb && (prev_lsb - lsb) >= max_lsb / 2)
            msb = prev_msb + max_lsb;
        else if (lsb > prev_lsb && (lsb - prev_lsb) > max_lsb / 2)
            msb = prev_msb - max_lsb;
        else
            msb = prev_msb;
        *top = msb + lsb;
        *bottom = *top + h->delta_bottom;
        *msb_out = msb;
    } else {
        unsigned prev_offset = p->prev_has_mmco5 ? 0 : p->prev_frame_num_offset;

        if (h->idr)
            frame_num_offset = 0;
        else if (p->prev_frame_num > h->frame_num)
            frame_num_offset = prev_offset + max_frame_num;
        else
            frame_num_offset = prev_offset;

        if (s->pic_order_cnt_type == 1) {
            unsigned abs_frame_num = 0, i;
            int expected = 0, delta_cycle = 0;

            if (s->num_ref_frames_in_poc_cycle)
                abs_frame_num = frame_num_offset + h->frame_num;
            if (h->nal_ref_idc == 0 && abs_frame_num > 0)
                abs_frame_num--;
            for (i = 0; i < s->num_ref_frames_in_poc_cycle; i++)
                delta_cycle += s->offset_for_ref_frame[i];
            if (abs_frame_num > 0) {
                unsigned cycle = (abs_frame_num - 1) / s->num_ref_frames_in_poc_cycle;
                unsigned in_cycle = (abs_frame_num - 1) % s->num_ref_frames_in_poc_cycle;
                expected = (int)cycle * delta_cycle;
                for (i = 0; i <= in_cycle; i++)
                    expected += s->offset_for_ref_frame[i];
            }
            if (h->nal_ref_idc == 0)
                expected += s->offset_for_non_ref_pic;
            *top = expected + h->delta0;
            *bottom = *top + s->offset_for_top_to_bottom_field + h->delta1;
        } else {
            int t;
            if (h->idr)
                t = 0;
            else if (h->nal_ref_idc == 0)
                t = 2 * (int)(frame_num_offset + h->frame_num) - 1;
            else
                t = 2 * (int)(frame_num_offset + h->frame_num);
            *top = *bottom = t;
        }
    }
    *frame_num_offset_out = frame_num_offset;
}

static H264Frame *find_short(H264Parser *p, int pic_num)
{
    unsigned i;
    for (i = 0; i < 17; i++)
        if (p->dpb[i].in_use && p->dpb[i].is_ref == 1 && p->dpb[i].frame_num_wrap == pic_num)
            return &p->dpb[i];
    return NULL;
}

static H264Frame *find_long(H264Parser *p, unsigned idx)
{
    unsigned i;
    for (i = 0; i < 17; i++)
        if (p->dpb[i].in_use && p->dpb[i].is_ref == 2 && p->dpb[i].long_term_frame_idx == idx)
            return &p->dpb[i];
    return NULL;
}

/* 8.2.5.4, frames. Returns 1 if mmco 5 was seen; *cur_long gets the
 * current picture's long-term index from mmco 6, or -1. */
static int adaptive_marking(H264Parser *p, const SliceHdr *h, int *cur_long)
{
    unsigned k, i;
    int mmco5 = 0;
    int cur_pic_num = (int)h->frame_num;

    *cur_long = -1;
    for (k = 0; k < h->n_mmco; k++) {
        const Mmco *m = &h->mmco[k];
        H264Frame *f;

        switch (m->op) {
        case 1:
            f = find_short(p, cur_pic_num - (int)(m->diff_pic_nums_minus1 + 1));
            if (f)
                f->is_ref = 0;
            break;
        case 2:
            f = find_long(p, m->long_term_pic_num);
            if (f)
                f->is_ref = 0;
            break;
        case 3:
            f = find_short(p, cur_pic_num - (int)(m->diff_pic_nums_minus1 + 1));
            if (f) {
                H264Frame *old = find_long(p, m->long_term_frame_idx);
                if (old && old != f)
                    old->is_ref = 0;
                f->is_ref = 2;
                f->long_term_frame_idx = m->long_term_frame_idx;
            }
            break;
        case 4:
            p->max_long_term_frame_idx = (int)m->max_lt_idx_plus1 - 1;
            for (i = 0; i < 17; i++)
                if (p->dpb[i].in_use && p->dpb[i].is_ref == 2 &&
                    (int)p->dpb[i].long_term_frame_idx > p->max_long_term_frame_idx)
                    p->dpb[i].is_ref = 0;
            break;
        case 5:
            for (i = 0; i < 17; i++)
                if (p->dpb[i].in_use)
                    p->dpb[i].is_ref = 0;
            p->max_long_term_frame_idx = -1;
            mmco5 = 1;
            break;
        case 6:
            f = find_long(p, m->long_term_frame_idx);
            if (f)
                f->is_ref = 0;
            *cur_long = (int)m->long_term_frame_idx;
            break;
        default:
            break;
        }
    }
    return mmco5;
}

/* ----------------------------------------------------- picture decode */

static void build_dxva(H264Parser *p, const H264Sps *s, H264Pps *q, const SliceHdr *h,
                       int slot, int top, int bottom, unsigned slice_types, H264Picture *pic)
{
    V383_DXVA_PicParams_H264 *pp = &pic->pp;
    unsigned i, n = 0, j, k;
    unsigned scaling = s->seq_scaling_matrix_present_flag || q->pic_scaling_matrix_present_flag;
    unsigned mbaff = s->mb_adaptive_frame_field_flag && !h->field_pic;

    memset(pp, 0, sizeof(*pp));
    pp->wFrameWidthInMbsMinus1 = (unsigned short)s->pic_width_in_mbs_minus1;
    pp->wFrameHeightInMbsMinus1 = (unsigned short)((s->pic_height_in_map_units_minus1 + 1) *
                                                   (2 - s->frame_mbs_only_flag) - 1);
    pp->CurrPic.bPicEntry = V383_PIC_ENTRY(slot, 0);
    pp->num_ref_frames = (unsigned char)s->max_num_ref_frames;
    V383_PP_SET(pp, V383_PP_FIELD_PIC_FLAG, 1, h->field_pic);
    V383_PP_SET(pp, V383_PP_MBAFF_FRAME_FLAG, 1, mbaff);
    V383_PP_SET(pp, V383_PP_CHROMA_FORMAT_IDC, 2, s->chroma_format_idc);
    V383_PP_SET(pp, V383_PP_REF_PIC_FLAG, 1, h->nal_ref_idc != 0);
    V383_PP_SET(pp, V383_PP_CONSTRAINED_INTRA_PRED, 1, q->constrained_intra_pred_flag);
    V383_PP_SET(pp, V383_PP_WEIGHTED_PRED, 1, q->weighted_pred_flag);
    V383_PP_SET(pp, V383_PP_WEIGHTED_BIPRED_IDC, 2, q->weighted_bipred_idc);
    V383_PP_SET(pp, V383_PP_MBS_CONSECUTIVE, 1, 1);
    V383_PP_SET(pp, V383_PP_FRAME_MBS_ONLY, 1, s->frame_mbs_only_flag);
    V383_PP_SET(pp, V383_PP_TRANSFORM_8X8, 1, q->transform_8x8_mode_flag);
    V383_PP_SET(pp, V383_PP_MIN_LUMA_BIPRED_8X8, 1, s->level_idc >= 31);
    V383_PP_SET(pp, V383_PP_INTRA_PIC, 1, (slice_types & ~(1u << 2 | 1u << 4)) == 0);
    pp->bit_depth_luma_minus8 = (unsigned char)s->bit_depth_luma_minus8;
    pp->bit_depth_chroma_minus8 = (unsigned char)s->bit_depth_chroma_minus8;
    pp->Reserved16Bits = 3;
    pp->StatusReportFeedbackNumber = p->decode_count + 1;

    for (i = 0; i < 16; i++)
        pp->RefFrameList[i].bPicEntry = 0xFF;
    /* short-term first, then long-term, as MPP orders them */
    for (k = 1; k <= 2; k++) {
        for (j = 0; j < 17 && n < 16; j++) {
            const H264Frame *f = &p->dpb[j];
            if (!f->in_use || f->is_ref != (int)k)
                continue;
            pp->RefFrameList[n].bPicEntry = V383_PIC_ENTRY(f->slot, k == 2);
            pp->FieldOrderCntList[n][0] = f->top_poc;
            pp->FieldOrderCntList[n][1] = f->bottom_poc;
            pp->FrameNumList[n] = (unsigned short)(k == 2 ? f->long_term_frame_idx : f->frame_num);
            pp->UsedForReferenceFlags |= 3u << (2 * n);
            if (f->non_existing)
                pp->NonExistingFrameFlags = (unsigned short)(pp->NonExistingFrameFlags | (1u << n));
            n++;
        }
    }
    pp->CurrFieldOrderCnt[0] = top;
    pp->CurrFieldOrderCnt[1] = bottom;
    pp->pic_init_qs_minus26 = (signed char)q->pic_init_qs_minus26;
    pp->chroma_qp_index_offset = (signed char)q->chroma_qp_index_offset;
    pp->second_chroma_qp_index_offset = (signed char)q->second_chroma_qp_index_offset;
    pp->ContinuationFlag = 1;
    pp->pic_init_qp_minus26 = (signed char)q->pic_init_qp_minus26;
    pp->num_ref_idx_l0_active_minus1 = (unsigned char)q->num_ref_idx_l0_default_active_minus1;
    pp->num_ref_idx_l1_active_minus1 = (unsigned char)q->num_ref_idx_l1_default_active_minus1;
    pp->frame_num = (unsigned short)h->frame_num;
    pp->log2_max_frame_num_minus4 = (unsigned char)s->log2_max_frame_num_minus4;
    pp->pic_order_cnt_type = (unsigned char)s->pic_order_cnt_type;
    pp->log2_max_pic_order_cnt_lsb_minus4 = (unsigned char)s->log2_max_poc_lsb_minus4;
    pp->delta_pic_order_always_zero_flag = (unsigned char)s->delta_pic_order_always_zero_flag;
    pp->direct_8x8_inference_flag = (unsigned char)s->direct_8x8_inference_flag;
    pp->entropy_coding_mode_flag = (unsigned char)q->entropy_coding_mode_flag;
    pp->pic_order_present_flag = (unsigned char)q->bottom_field_pic_order_in_frame_present_flag;
    pp->num_slice_groups_minus1 = (unsigned char)q->num_slice_groups_minus1;
    pp->deblocking_filter_control_present_flag = (unsigned char)q->deblocking_filter_control_present_flag;
    pp->redundant_pic_cnt_present_flag = (unsigned char)q->redundant_pic_cnt_present_flag;

    /* Qmatrix, in DXVA's zig-zag order */
    resolve_pps_lists(q, s);
    for (i = 0; i < 6; i++)
        for (j = 0; j < 16; j++)
            pic->qm.bScalingLists4x4[i][j] = q->sl4[i][zz4[j]];
    for (i = 0; i < 2; i++)
        for (j = 0; j < 64; j++)
            pic->qm.bScalingLists8x8[i][j] = q->sl8[i][zz8[j]];
    pic->scaling_matrix_present = scaling;

    pic->extra.valid = 1;
    pic->extra.profile_idc = (unsigned char)s->profile_idc;
    pic->extra.seq_parameter_set_id = (unsigned char)s->sps_id;
    pic->extra.constraint_set3_flag = (unsigned char)((s->constraint_flags >> 4) & 1);
    pic->pps_id = q->pps_id;
    pic->idr = h->idr;
    pic->width = 16 * (s->pic_width_in_mbs_minus1 + 1);
    pic->height = 16 * (pp->wFrameHeightInMbsMinus1 + 1u);
    pic->crop_left = s->crop_left;
    pic->crop_right = s->crop_right;
    pic->crop_top = s->crop_top;
    pic->crop_bottom = s->crop_bottom;
}

static int finish_picture(H264Parser *p, H264DecodeFn decode, void *decode_ctx)
{
    SliceHdr *h = (SliceHdr *)p->cur_hdr;
    H264Pps *q;
    const H264Sps *s;
    H264Picture pic;
    H264Frame cur;
    unsigned max_frame_num, frame_num_offset;
    int top, bottom, msb, slot, mmco5 = 0, cur_long = -1;
    unsigned dpb;

    if (!p->au_open)
        return 0;
    p->au_open = 0;

    q = &p->pps[h->pps_id];
    s = &p->sps[q->sps_id];
    if (h->field_pic) {
        set_err(p, "field pictures are not supported (picture %u)", p->decode_count);
        return -1;
    }
    if (s->chroma_format_idc != 1 || s->bit_depth_luma_minus8 || s->bit_depth_chroma_minus8) {
        set_err(p, "only 4:2:0 8-bit is supported");
        return -1;
    }
    if (q->num_slice_groups_minus1) {
        set_err(p, "FMO (slice groups) is not supported");
        return -1;
    }
    max_frame_num = 1u << (s->log2_max_frame_num_minus4 + 4);

    dpb = max_dpb_frames(s);
    if (h->idr) {
        if (h->no_output_of_prior_pics) {
            unsigned i;
            for (i = 0; i < 17; i++)
                if (p->dpb[i].in_use)
                    dpb_remove(p, &p->dpb[i]);
        } else {
            unsigned i;
            for (i = 0; i < 17; i++)
                p->dpb[i].is_ref = 0;
            dpb_flush(p);
        }
        p->dpb_size = dpb;
        p->max_long_term_frame_idx = -1;
        p->prev_ref_frame_num = 0;
    } else {
        if (dpb != p->dpb_size && p->dpb_size)
            p->dpb_size = dpb > p->dpb_size ? dpb : p->dpb_size;
        if (!p->dpb_size)
            p->dpb_size = dpb;
        if (h->frame_num != p->prev_ref_frame_num &&
            h->frame_num != (p->prev_ref_frame_num + 1) % max_frame_num) {
            if (s->gaps_in_frame_num_allowed_flag) {
                if (fill_frame_num_gap(p, s, h))
                    return -1;
            }
            /* else: a lost reference; carry on as the hardware would */
        }
    }

    update_wraps(p, h->frame_num, max_frame_num);
    compute_poc(p, s, h, &top, &bottom, &frame_num_offset, &msb);

    slot = free_slot(p);
    if (slot < 0) {
        set_err(p, "no free surface");
        return -1;
    }

    memset(&pic, 0, sizeof(pic));
    build_dxva(p, s, q, h, slot, top, bottom, p->cur_slice_types, &pic);
    pic.stream = p->au;
    pic.stream_len = p->au_len;
    pic.slice_types = p->cur_slice_types;
    pic.num_slices = p->cur_num_slices;
    pic.decode_index = p->decode_count;
    pic.poc = top < bottom ? top : bottom;

    /* decode into a slot no DPB frame holds */
    p->slot_busy[slot] = 1;
    if (decode && decode(decode_ctx, &pic)) {
        p->slot_busy[slot] = 0;
        if (!p->err[0])
            set_err(p, "decode of picture %u failed", p->decode_count);
        return -1;
    }
    p->slot_busy[slot] = 0;

    /* marking (8.2.5) */
    memset(&cur, 0, sizeof(cur));
    cur.slot = slot;
    cur.frame_num = h->frame_num;
    cur.frame_num_wrap = (int)h->frame_num;
    cur.top_poc = top;
    cur.bottom_poc = bottom;
    cur.poc = pic.poc;
    cur.needed_for_output = 1;
    cur.decode_index = p->decode_count;

    if (h->nal_ref_idc) {
        if (h->idr) {
            if (h->long_term_reference_flag) {
                cur.is_ref = 2;
                cur.long_term_frame_idx = 0;
                p->max_long_term_frame_idx = 0;
            } else {
                cur.is_ref = 1;
                p->max_long_term_frame_idx = -1;
            }
        } else {
            if (h->adaptive)
                mmco5 = adaptive_marking(p, h, &cur_long);
            else
                sliding_window(p, s);
            if (cur_long >= 0) {
                cur.is_ref = 2;
                cur.long_term_frame_idx = (unsigned)cur_long;
            } else {
                cur.is_ref = 1;
            }
        }
    }

    if (mmco5) {
        int temp = top < bottom ? top : bottom;
        cur.top_poc = top - temp;
        cur.bottom_poc = bottom - temp;
        cur.poc = 0;
        cur.frame_num = 0;
        cur.frame_num_wrap = 0;
        /* C.4.4: everything before it is output first */
        dpb_flush(p);
    }

    dpb_sweep(p);
    if (dpb_store(p, &cur))
        return -1;

    /* state for the next picture */
    if (h->nal_ref_idc) {
        p->prev_ref_frame_num = mmco5 ? 0 : h->frame_num;
        p->prev_ref_has_mmco5 = mmco5;
        p->prev_ref_is_bottom = 0;
        p->prev_ref_top_poc_after_mmco5 = cur.top_poc;
        p->prev_poc_msb = mmco5 ? 0 : msb;
        p->prev_poc_lsb = mmco5 ? cur.top_poc : (int)h->poc_lsb;
    }
    p->prev_frame_num = mmco5 ? 0 : h->frame_num;
    p->prev_frame_num_offset = mmco5 ? 0 : frame_num_offset;
    p->prev_has_mmco5 = mmco5;
    p->decode_count++;
    return 0;
}

/* 7.4.1.2.4: does this slice start a new primary picture? */
static int new_picture(const SliceHdr *a, const SliceHdr *b, const H264Sps *s)
{
    if (a->frame_num != b->frame_num || a->pps_id != b->pps_id ||
        a->field_pic != b->field_pic || a->bottom != b->bottom)
        return 1;
    if ((a->nal_ref_idc == 0) != (b->nal_ref_idc == 0))
        return 1;
    if (s->pic_order_cnt_type == 0 && (a->poc_lsb != b->poc_lsb || a->delta_bottom != b->delta_bottom))
        return 1;
    if (s->pic_order_cnt_type == 1 && (a->delta0 != b->delta0 || a->delta1 != b->delta1))
        return 1;
    if (a->idr != b->idr)
        return 1;
    if (a->idr && b->idr && a->idr_pic_id != b->idr_pic_id)
        return 1;
    return 0;
}

static int au_append(H264Parser *p, const uint8_t *nal, size_t len)
{
    size_t need = p->au_len + 3 + len + 64;
    if (need > p->au_cap) {
        size_t cap = p->au_cap ? p->au_cap : 1 << 16;
        uint8_t *n;
        while (cap < need)
            cap *= 2;
        n = (uint8_t *)realloc(p->au, cap);
        if (!n) {
            set_err(p, "out of memory");
            return -1;
        }
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

/* ------------------------------------------------------------- driver */

void h264_init(H264Parser *p, H264OutputFn out, void *out_ctx)
{
    memset(p, 0, sizeof(*p));
    p->out_fn = out;
    p->out_ctx = out_ctx;
    p->max_long_term_frame_idx = -1;
    p->cur_hdr = calloc(1, sizeof(SliceHdr));
}

void h264_free(H264Parser *p)
{
    free(p->au);
    free(p->cur_hdr);
    p->au = NULL;
    p->cur_hdr = NULL;
}

static int handle_nal(H264Parser *p, const uint8_t *nal, size_t len, unsigned max_pictures,
                      H264DecodeFn decode, void *decode_ctx)
{
    unsigned nal_ref_idc, type;
    Bits b;
    int rc = 0;

    if (len < 1)
        return 0;
    nal_ref_idc = (nal[0] >> 5) & 3;
    type = nal[0] & 0x1F;

    if (type == 1 || type == 5) {
        SliceHdr h;
        SliceHdr *first = (SliceHdr *)p->cur_hdr;

        memset(&b, 0, sizeof(b));
        /* the slice header is within the first few hundred bytes */
        b.buf = rbsp_alloc(nal + 1, len - 1 < 4096 ? len - 1 : 4096, &b.len);
        if (!b.buf) {
            set_err(p, "out of memory");
            return -1;
        }
        rc = parse_slice(p, &b, nal_ref_idc, type, &h);
        free(b.buf);
        if (rc)
            return -1;
        if (h.redundant_pic_cnt > 0)
            return 0;               /* primary pictures only */
        if (p->au_open && new_picture(first, &h, &p->sps[p->pps[first->pps_id].sps_id])) {
            if (finish_picture(p, decode, decode_ctx))
                return -1;
        }
        if (max_pictures && p->decode_count >= max_pictures)
            return 1;
        if (!p->au_open) {
            *first = h;
            p->au_open = 1;
            p->au_len = 0;
            p->cur_slice_types = 0;
            p->cur_num_slices = 0;
        }
        p->cur_slice_types |= 1u << (h.slice_type % 5);
        p->cur_num_slices++;
        return au_append(p, nal, len);
    }

    /* Anything else that may start an access unit ends the current one. */
    if (type == 6 || type == 7 || type == 8 || type == 9 || type == 10 || type == 11 ||
        (type >= 14 && type <= 18)) {
        if (finish_picture(p, decode, decode_ctx))
            return -1;
        if (max_pictures && p->decode_count >= max_pictures)
            return 1;
    }

    if (type == 7 || type == 8) {
        memset(&b, 0, sizeof(b));
        b.buf = rbsp_alloc(nal + 1, len - 1, &b.len);
        if (!b.buf) {
            set_err(p, "out of memory");
            return -1;
        }
        rc = (type == 7) ? parse_sps(p, &b) : parse_pps(p, &b);
        free(b.buf);
        return rc;
    }
    return 0;
}

int h264_run(H264Parser *p, const uint8_t *d, size_t len, unsigned max_pictures,
             H264DecodeFn decode, void *decode_ctx)
{
    size_t i = 0, start = (size_t)-1;
    int rc;

    p->err[0] = 0;
    while (i + 3 <= len) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != (size_t)-1) {
                size_t end = i;
                while (end > start && d[end - 1] == 0)
                    end--;              /* trailing_zero_8bits and the 4th start code byte */
                rc = handle_nal(p, d + start, end - start, max_pictures, decode, decode_ctx);
                if (rc < 0)
                    return -1;
                if (rc > 0)
                    goto done;
            }
            i += 3;
            start = i;
            continue;
        }
        i++;
    }
    if (start != (size_t)-1 && start < len) {
        size_t end = len;
        while (end > start && d[end - 1] == 0)
            end--;
        rc = handle_nal(p, d + start, end - start, max_pictures, decode, decode_ctx);
        if (rc < 0)
            return -1;
        if (rc > 0)
            goto done;
    }
    if (finish_picture(p, decode, decode_ctx))
        return -1;
done:
    dpb_flush(p);
    return 0;
}
