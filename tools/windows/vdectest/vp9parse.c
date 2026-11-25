/*
 * vp9parse.c - VP9 in IVF for vdpu383test (see vp9parse.h)
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <string.h>

#include "vp9parse.h"

/* ------------------------------------------------------------ bit reader */

typedef struct Bits {
    const uint8_t *d;
    size_t len, pos;            /* pos in bits */
    int over;
} Bits;

static unsigned rb(Bits *b, unsigned n)
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

/* su(n): magnitude then sign */
static int rs(Bits *b, unsigned n)
{
    int v = (int)rb(b, n);
    return rb(b, 1) ? -v : v;
}

static unsigned read_prob(Bits *b)
{
    return rb(b, 1) ? rb(b, 8) : 255;
}

static void fail(Vp9Parser *p, const char *what, unsigned n)
{
    snprintf(p->err, sizeof(p->err), "%s (%u)", what, n);
}

/* --------------------------------------------------------------- header */

typedef struct Hdr {
    unsigned profile, show_existing, existing_idx;
    unsigned frame_type, show_frame, error_res, intra_only, reset_ctx;
    unsigned bit_depth, color_space, color_range, ss_x, ss_y;
    unsigned refresh_flags, ref_idx[3], sign_bias[3];
    unsigned width, height, found_ref;
    unsigned allow_hp, interp_filter;
    unsigned refresh_ctx, parallel, ctx_idx;
    unsigned lf_level, lf_sharpness, lf_delta_enabled, lf_delta_update;
    unsigned base_q;
    int      y_dc, uv_dc, uv_ac;
    unsigned log2_tile_cols, log2_tile_rows;
    unsigned header_size, uncomp_size;
} Hdr;

static void color_config(Vp9Parser *p, Bits *b, Hdr *h)
{
    h->bit_depth = 8;
    if (h->profile >= 2)
        h->bit_depth = rb(b, 1) ? 12 : 10;
    h->color_space = rb(b, 3);
    if (h->color_space != 7) {          /* CS_RGB */
        h->color_range = rb(b, 1);
        if (h->profile == 1 || h->profile == 3) {
            h->ss_x = rb(b, 1);
            h->ss_y = rb(b, 1);
            rb(b, 1);
        } else {
            h->ss_x = h->ss_y = 1;
        }
    } else {
        h->color_range = 1;
        h->ss_x = h->ss_y = 0;
        if (h->profile == 1 || h->profile == 3)
            rb(b, 1);
    }
    p->bit_depth = h->bit_depth;
    p->color_space = h->color_space;
    p->color_range = h->color_range;
    p->subsampling_x = h->ss_x;
    p->subsampling_y = h->ss_y;
}

static void frame_size(Bits *b, Hdr *h)
{
    h->width = rb(b, 16) + 1;
    h->height = rb(b, 16) + 1;
}

static void render_size(Bits *b)
{
    if (rb(b, 1)) {
        rb(b, 16);
        rb(b, 16);
    }
}

static void setup_past_independence(Vp9Parser *p)
{
    memset(p->seg_data, 0, sizeof(p->seg_data));
    memset(p->seg_mask, 0, sizeof(p->seg_mask));
    p->seg_abs_delta = 0;
    p->ref_deltas[0] = 1;
    p->ref_deltas[1] = 0;
    p->ref_deltas[2] = -1;
    p->ref_deltas[3] = -1;
    p->mode_deltas[0] = p->mode_deltas[1] = 0;
}

static int parse_header(Vp9Parser *p, const uint8_t *d, size_t len, Hdr *h)
{
    Bits b;
    unsigned i, intra, sb64, min_log2, max_log2;
    static const unsigned literal_to_type[4] = {
        V383_VP9_EIGHTTAP_SMOOTH, V383_VP9_EIGHTTAP, V383_VP9_EIGHTTAP_SHARP, V383_VP9_BILINEAR
    };

    memset(h, 0, sizeof(*h));
    b.d = d;
    b.len = len;
    b.pos = 0;
    b.over = 0;
    if (rb(&b, 2) != 2) {
        fail(p, "bad frame marker", p->decode_index);
        return -1;
    }
    h->profile = rb(&b, 1);
    h->profile |= rb(&b, 1) << 1;
    if (h->profile == 3)
        rb(&b, 1);
    h->show_existing = rb(&b, 1);
    if (h->show_existing) {
        h->existing_idx = rb(&b, 3);
        return 0;
    }
    h->frame_type = rb(&b, 1);
    h->show_frame = rb(&b, 1);
    h->error_res = rb(&b, 1);
    h->bit_depth = p->bit_depth;
    h->ss_x = p->subsampling_x;
    h->ss_y = p->subsampling_y;
    h->color_space = p->color_space;
    if (h->frame_type == 0) {
        if (rb(&b, 8) != 0x49 || rb(&b, 8) != 0x83 || rb(&b, 8) != 0x42) {
            fail(p, "bad sync code", p->decode_index);
            return -1;
        }
        color_config(p, &b, h);
        frame_size(&b, h);
        render_size(&b);
        h->refresh_flags = 0xFF;
        intra = 1;
    } else {
        h->intra_only = h->show_frame ? 0 : rb(&b, 1);
        h->reset_ctx = h->error_res ? 0 : rb(&b, 2);
        intra = h->intra_only;
        if (h->intra_only) {
            if (rb(&b, 8) != 0x49 || rb(&b, 8) != 0x83 || rb(&b, 8) != 0x42) {
                fail(p, "bad sync code", p->decode_index);
                return -1;
            }
            if (h->profile > 0) {
                color_config(p, &b, h);
            } else {
                h->bit_depth = 8;
                h->ss_x = h->ss_y = 1;
                h->color_space = 2;     /* CS_BT_601 */
                p->bit_depth = 8;
                p->subsampling_x = p->subsampling_y = 1;
                p->color_space = 2;
            }
            h->refresh_flags = rb(&b, 8);
            frame_size(&b, h);
            render_size(&b);
        } else {
            h->refresh_flags = rb(&b, 8);
            for (i = 0; i < 3; i++) {
                h->ref_idx[i] = rb(&b, 3);
                h->sign_bias[i] = rb(&b, 1);
            }
            for (i = 0; i < 3; i++) {
                if (rb(&b, 1)) {
                    const Vp9Slot *s = &p->ref[h->ref_idx[i]];
                    h->width = s->width;
                    h->height = s->height;
                    h->found_ref = 1;
                    break;
                }
            }
            if (!h->found_ref)
                frame_size(&b, h);
            render_size(&b);
            h->allow_hp = rb(&b, 1);
            h->interp_filter = rb(&b, 1) ? V383_VP9_SWITCHABLE : literal_to_type[rb(&b, 2)];
        }
    }
    if (!h->error_res) {
        h->refresh_ctx = rb(&b, 1);
        h->parallel = rb(&b, 1);
    } else {
        h->refresh_ctx = 0;
        h->parallel = 1;
    }
    h->ctx_idx = rb(&b, 2);
    if (intra || h->error_res)
        setup_past_independence(p);

    /* loop_filter_params() */
    h->lf_level = rb(&b, 6);
    h->lf_sharpness = rb(&b, 3);
    h->lf_delta_enabled = rb(&b, 1);
    if (h->lf_delta_enabled) {
        h->lf_delta_update = rb(&b, 1);
        if (h->lf_delta_update) {
            for (i = 0; i < 4; i++)
                if (rb(&b, 1))
                    p->ref_deltas[i] = (signed char)rs(&b, 6);
            for (i = 0; i < 2; i++)
                if (rb(&b, 1))
                    p->mode_deltas[i] = (signed char)rs(&b, 6);
        }
    }

    /* quantization_params() */
    h->base_q = rb(&b, 8);
    h->y_dc = rb(&b, 1) ? rs(&b, 4) : 0;
    h->uv_dc = rb(&b, 1) ? rs(&b, 4) : 0;
    h->uv_ac = rb(&b, 1) ? rs(&b, 4) : 0;

    /* segmentation_params() */
    p->seg_update_map = 0;
    p->seg_enabled = rb(&b, 1);
    if (p->seg_enabled) {
        p->seg_update_map = rb(&b, 1);
        if (p->seg_update_map) {
            for (i = 0; i < 7; i++)
                p->seg_tree_probs[i] = (unsigned char)read_prob(&b);
            p->seg_temporal = rb(&b, 1);
            for (i = 0; i < 3; i++)
                p->seg_pred_probs[i] = (unsigned char)(p->seg_temporal ? read_prob(&b) : 255);
        }
        if (rb(&b, 1)) {                /* segmentation_update_data */
            static const unsigned bits[4] = { 8, 6, 2, 0 }, sgn[4] = { 1, 1, 0, 0 };
            unsigned j;

            p->seg_abs_delta = rb(&b, 1);
            for (i = 0; i < 8; i++) {
                p->seg_mask[i] = 0;
                for (j = 0; j < 4; j++) {
                    int v = 0;
                    if (rb(&b, 1)) {
                        p->seg_mask[i] |= (unsigned char)(1u << j);
                        v = (int)rb(&b, bits[j]);
                        if (sgn[j] && rb(&b, 1))
                            v = -v;
                    }
                    p->seg_data[i][j] = (short)v;
                }
            }
        }
    }

    /* tile_info() */
    sb64 = (((h->width + 7) >> 3) + 7) >> 3;
    min_log2 = 0;
    while ((64u << min_log2) < sb64)
        min_log2++;
    max_log2 = 1;
    while ((sb64 >> max_log2) >= 4)
        max_log2++;
    max_log2--;
    h->log2_tile_cols = min_log2;
    while (h->log2_tile_cols < max_log2 && rb(&b, 1))
        h->log2_tile_cols++;
    h->log2_tile_rows = rb(&b, 1);
    if (h->log2_tile_rows)
        h->log2_tile_rows += rb(&b, 1);

    h->header_size = rb(&b, 16);
    h->uncomp_size = (unsigned)((b.pos + 7) >> 3);
    if (b.over || !h->header_size || h->uncomp_size + h->header_size > len) {
        fail(p, "truncated frame header", p->decode_index);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- frames */

static void fill_dxva(Vp9Parser *p, const Hdr *h, int cur, V383_DXVA_PicParams_VP9 *pp)
{
    unsigned i, j, intra = !h->frame_type || h->intra_only;
    unsigned ctx = h->ctx_idx;

    memset(pp, 0, sizeof(*pp));
    pp->CurrPic.bPicEntry = (unsigned char)cur;
    pp->profile = (unsigned char)h->profile;
    if (p->dxva_only && intra)
        ctx = 0;            /* what FFmpeg's and Chromium's parsers hand on */
    pp->wFormatAndPictureInfoFlags = (unsigned short)(
        (h->frame_type << V383_VP9_FLAG_FRAME_TYPE) | (h->show_frame << V383_VP9_FLAG_SHOW_FRAME) |
        (h->error_res << V383_VP9_FLAG_ERROR_RESILIENT) | (h->ss_x << V383_VP9_FLAG_SUBSAMPLING_X) |
        (h->ss_y << V383_VP9_FLAG_SUBSAMPLING_Y) | (h->refresh_ctx << V383_VP9_FLAG_REFRESH_CTX) |
        (h->parallel << V383_VP9_FLAG_PARALLEL_MODE) | (h->intra_only << V383_VP9_FLAG_INTRA_ONLY) |
        (ctx << V383_VP9_FLAG_FRAME_CTX_IDX) | (h->reset_ctx << V383_VP9_FLAG_RESET_CTX) |
        (h->allow_hp << V383_VP9_FLAG_ALLOW_HP_MV));
    pp->width = h->width;
    pp->height = h->height;
    pp->BitDepthMinus8Luma = (unsigned char)(h->bit_depth - 8);
    pp->BitDepthMinus8Chroma = (unsigned char)(h->bit_depth - 8);
    pp->interp_filter = (unsigned char)(intra ? 0 : h->interp_filter);
    for (i = 0; i < 8; i++) {
        if (p->ref[i].surface < 0) {
            pp->ref_frame_map[i].bPicEntry = 0xFF;
        } else {
            pp->ref_frame_map[i].bPicEntry = (unsigned char)p->ref[i].surface;
            pp->ref_frame_coded_width[i] = p->ref[i].width;
            pp->ref_frame_coded_height[i] = p->ref[i].height;
        }
    }
    for (i = 0; i < 3; i++) {
        pp->frame_refs[i].bPicEntry = 0xFF;
        if (!intra && p->ref[h->ref_idx[i]].surface >= 0)
            pp->frame_refs[i].bPicEntry = (unsigned char)p->ref[h->ref_idx[i]].surface;
        pp->ref_frame_sign_bias[1 + i] = intra ? 0 : (signed char)h->sign_bias[i];
    }
    pp->filter_level = (signed char)h->lf_level;
    pp->sharpness_level = (signed char)h->lf_sharpness;
    pp->wControlInfoFlags = (unsigned char)(h->lf_delta_enabled | (h->lf_delta_update << 1) |
        ((!h->error_res && !intra && p->last_show_frame && h->width == p->last_width &&
          h->height == p->last_height && !p->last_intra_only) << 2));
    for (i = 0; i < 4; i++)
        pp->ref_deltas[i] = p->ref_deltas[i];
    for (i = 0; i < 2; i++)
        pp->mode_deltas[i] = p->mode_deltas[i];
    pp->base_qindex = (short)h->base_q;
    pp->y_dc_delta_q = (signed char)h->y_dc;
    pp->uv_dc_delta_q = (signed char)h->uv_dc;
    pp->uv_ac_delta_q = (signed char)h->uv_ac;
    pp->stVP9Segments.wSegmentInfoFlags = (unsigned char)(p->seg_enabled | (p->seg_update_map << 1) |
                                                          (p->seg_temporal << 2) | (p->seg_abs_delta << 3));
    memcpy(pp->stVP9Segments.tree_probs, p->seg_tree_probs, 7);
    memcpy(pp->stVP9Segments.pred_probs, p->seg_pred_probs, 3);
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 4; j++)
            pp->stVP9Segments.feature_data[i][j] = p->seg_data[i][j];
        pp->stVP9Segments.feature_mask[i] = p->seg_mask[i];
    }
    pp->log2_tile_cols = (unsigned char)h->log2_tile_cols;
    pp->log2_tile_rows = (unsigned char)h->log2_tile_rows;
    pp->uncompressed_header_size_byte_aligned = (unsigned short)h->uncomp_size;
    pp->first_partition_size = (unsigned short)h->header_size;
    pp->StatusReportFeedbackNumber = p->decode_index + 1;
}

static int pick_surface(Vp9Parser *p)
{
    int s;

    for (s = 0; s < VP9_MAX_SLOTS; s++)
        if (!p->surface_busy[s])
            return s;
    return -1;
}

static int do_frame(Vp9Parser *p, const uint8_t *d, size_t len, Vp9DecodeFn dec, void *ctx)
{
    Hdr h;
    Vp9Picture pic;
    unsigned i, intra;
    int cur;

    if (parse_header(p, d, len, &h))
        return -1;
    p->profile = h.profile;
    if (h.show_existing) {
        const Vp9Slot *s = &p->ref[h.existing_idx];
        if (s->surface < 0) {
            fail(p, "show_existing_frame of an empty slot", h.existing_idx);
            return -1;
        }
        p->existing++;
        if (dec && p->out_fn)
            p->out_fn(p->out_ctx, s->surface, s->width, s->height, p->decode_index, 1);
        return 0;
    }
    if (h.profile & 1) {
        fail(p, "profile 1 or 3 (4:2:2, 4:4:0, 4:4:4) is not supported", h.profile);
        return -1;
    }
    if (h.bit_depth > 10) {
        fail(p, "12-bit is not supported", h.bit_depth);
        return -1;
    }
    intra = !h.frame_type || h.intra_only;
    /* A stream may start with intra-only frames. An inter frame that
     * names an empty slot is dropped, as FFmpeg drops it. */
    if (intra)
        p->have_key = 1;
    if (!p->have_key) {
        p->dropped++;
        return 0;
    }
    if (!intra) {
        for (i = 0; i < 3; i++)
            if (p->ref[h.ref_idx[i]].surface < 0) {
                p->dropped++;
                return 0;
            }
        for (i = 0; i < 3; i++) {
            const Vp9Slot *s = &p->ref[h.ref_idx[i]];
            if (s->width != h.width || s->height != h.height)
                p->scaled_refs++;
        }
    }
    cur = pick_surface(p);
    if (cur < 0) {
        fail(p, "no free surface", p->decode_index);
        return -1;
    }

    memset(&pic, 0, sizeof(pic));
    fill_dxva(p, &h, cur, &pic.pp);
    pic.data = d;
    pic.len = len;
    pic.decode_index = p->decode_index;
    pic.packet = p->packets;
    pic.width = h.width;
    pic.height = h.height;
    pic.bit_depth = h.bit_depth;
    pic.refresh_frame_flags = h.refresh_flags;
    pic.coded_ctx_idx = h.ctx_idx;
    pic.color_space = h.color_space;

    if (h.width > p->max_width)
        p->max_width = h.width;
    if (h.height > p->max_height)
        p->max_height = h.height;
    if (h.bit_depth > p->max_bit_depth)
        p->max_bit_depth = h.bit_depth;
    if (p->decode_index && (h.width != p->last_width || h.height != p->last_height))
        p->resized++;
    if (!h.frame_type)
        p->keyframes++;
    if (h.intra_only)
        p->intra_only++;
    if (!h.show_frame)
        p->hidden++;
    else
        p->shown++;

    if (dec && dec(ctx, &pic))
        return -1;

    for (i = 0; i < 8; i++)
        if (h.refresh_flags & (1u << i)) {
            p->ref[i].surface = cur;
            p->ref[i].width = h.width;
            p->ref[i].height = h.height;
            p->ref[i].bit_depth = h.bit_depth;
            p->ref[i].subsampling_x = h.ss_x;
            p->ref[i].subsampling_y = h.ss_y;
        }
    memset(p->surface_busy, 0, sizeof(p->surface_busy));
    for (i = 0; i < 8; i++)
        if (p->ref[i].surface >= 0)
            p->surface_busy[p->ref[i].surface] = 1;

    if (h.show_frame && dec && p->out_fn)
        p->out_fn(p->out_ctx, cur, h.width, h.height, p->decode_index, 0);

    p->last_width = h.width;
    p->last_height = h.height;
    p->last_show_frame = h.show_frame;
    p->last_intra_only = intra;
    p->decode_index++;
    return 0;
}

/* A superframe's frames, or the packet as one frame. */
static int do_packet(Vp9Parser *p, const uint8_t *d, size_t len, Vp9DecodeFn dec, void *ctx)
{
    uint8_t m;

    if (!len)
        return 0;
    m = d[len - 1];
    if ((m & 0xE0) == 0xC0) {
        unsigned n = (m & 7) + 1, sz = ((m >> 3) & 3) + 1, i, k;
        size_t idx = 2 + (size_t)sz * n;

        if (len >= idx && d[len - idx] == m) {
            const uint8_t *q = d + len - idx + 1;
            size_t off = 0;

            p->superframes++;
            for (i = 0; i < n; i++) {
                size_t fs = 0;
                for (k = 0; k < sz; k++)
                    fs |= (size_t)q[i * sz + k] << (8 * k);
                if (off + fs > len - idx) {
                    fail(p, "superframe index past the packet", p->packets);
                    return -1;
                }
                if (fs) {
                    if (p->max_pictures && p->decode_index >= p->max_pictures)
                        return 0;
                    if (do_frame(p, d + off, fs, dec, ctx))
                        return -1;
                }
                off += fs;
            }
            return 0;
        }
    }
    if (p->max_pictures && p->decode_index >= p->max_pictures)
        return 0;
    return do_frame(p, d, len, dec, ctx);
}

static unsigned le32(const uint8_t *d)
{
    return d[0] | ((unsigned)d[1] << 8) | ((unsigned)d[2] << 16) | ((unsigned)d[3] << 24);
}

int vp9_probe(const uint8_t *data, size_t len)
{
    return len >= 32 && !memcmp(data, "DKIF", 4) && !memcmp(data + 8, "VP90", 4);
}

void vp9_init(Vp9Parser *p, Vp9OutputFn out, void *ctx)
{
    unsigned i;

    memset(p, 0, sizeof(*p));
    for (i = 0; i < 8; i++)
        p->ref[i].surface = -1;
    p->bit_depth = 8;
    p->subsampling_x = p->subsampling_y = 1;
    for (i = 0; i < 7; i++)
        p->seg_tree_probs[i] = 255;
    for (i = 0; i < 3; i++)
        p->seg_pred_probs[i] = 255;
    setup_past_independence(p);
    p->out_fn = out;
    p->out_ctx = ctx;
}

int vp9_run(Vp9Parser *p, const uint8_t *data, size_t len, unsigned max_pictures, Vp9DecodeFn dec, void *ctx)
{
    size_t pos, hdr;

    if (!vp9_probe(data, len)) {
        fail(p, "not an IVF file with VP9", 0);
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
        if (do_packet(p, data + pos, fs, dec, ctx))
            return -1;
        p->packets++;
        pos += fs;
        if (p->max_pictures && p->decode_index >= p->max_pictures)
            break;
    }
    return 0;
}
