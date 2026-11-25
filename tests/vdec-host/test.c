/*
 * The UMD's video backend (video/vdec.c) on the host, with the fake
 * decoder: every picture of a stream, as vdpu383test's parsers turn it
 * into DXVA, goes through vdec_*_decode, which builds the VDPU383 register
 * file from the DXVA parameters alone (what the UMD has) and writes its
 * pattern. Checks: every picture accepted, the pattern where the output
 * callback expects it, and, with an expected MD5, that the register files
 * the backend built (D3D10UMD_VDEC_REGLOG, addresses as buffer+offset) are
 * the ones vdpu383test --dxva-only builds for the same stream
 * (vdpu383test-reglog.patch, README.md). `make run`.
 *
 *   vdec-host STREAM [--frames N] [--reglog-md5 MD5] [--av1-synth 0|1] [--log FILE] [--zero-copy]
 *
 * --zero-copy: every picture but every fourth is given a target, a slice of
 * one "texture array" of this test's (zero copy, vdec_set_target), laid out
 * with 64 more rows than the decoder's own surfaces; the pattern must be in
 * the slice, the others (decoded into surfaces of the decoder's own and
 * copied by the caller) where vdec_picture says, and every address the
 * decoder is given in a buffer of the backend or the texture.
 *
 * The codec is told from the stream as vdpu383test does (IVF fourcc, then
 * the first NAL unit).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tools/windows/vdectest/h264parse.h"
#include "../../tools/windows/vdectest/hevcparse.h"
#include "../../tools/windows/vdectest/vp9parse.h"
#include "../../tools/windows/vdectest/av1parse.h"
#include "../../tools/windows/vdectest/md5.h"
#include "../../video/vdec.h"
#include "../../video/vdpu383/lib/vdpu383_av1.h"

#define MAX_SLOTS 128

static vdec *dec;
static vdec_options opts;
static unsigned max_w, max_h;
static unsigned status_of[MAX_SLOTS];
static vdec_picture last_pic[MAX_SLOTS];
static unsigned pictures, outputs, failures;

/* --zero-copy: the texture array, one slice per DXVA index */
#define TEX_SLICES 32
static int zero_copy;
static uint8_t *tex;
static unsigned tex_stride, tex_rows, tex_pages, zc_given, zc_in;
static size_t tex_layer;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static unsigned align_to(unsigned v, unsigned a)
{
    return (v + a - 1) / a * a;
}

/* The texture for picture decode_index, in slice slot; none for every
 * fourth picture. The array is laid out for the stream's largest picture. */
static void give_target(unsigned slot, unsigned decode_index, unsigned w, unsigned h)
{
    vdec_target t;

    if (!zero_copy)
        return;
    if (!tex) {
        tex_stride = align_to(w, 64);
        tex_rows = align_to(h, 64) + 64;
        tex_layer = align_to(tex_stride * tex_rows * 3 / 2, 4096);
        tex_pages = (unsigned)(tex_layer * TEX_SLICES / 4096);
        tex = calloc(TEX_SLICES, tex_layer);
    }
    if (decode_index % 4 == 3 || slot >= TEX_SLICES)
        return;
    memset(&t, 0, sizeof(t));
    t.token = 0x5a5a0001ull;
    t.pages = tex_pages;
    t.offset = slot * tex_layer;
    t.stride = tex_stride;
    t.luma_rows = tex_rows;
    t.fake_map = tex;
    vdec_set_target(dec, &t);
    zc_given++;
}

static int make_decoder(vdec_codec c, unsigned w, unsigned h)
{
    char err[160] = "";

    if (dec)
        return 0;
    dec = vdec_create(c, w, h, &opts, err, sizeof err);
    CHECK(dec != NULL);
    if (!dec) {
        printf("create: %s\n", err);
        return -1;
    }
    return 0;
}

static int decoded(unsigned slot, unsigned status, int rc, const vdec_picture *out, const char *err,
                   unsigned index)
{
    if (rc) {
        printf("pic %u: %s\n", index, err);
        failures++;
        return -1;
    }
    CHECK(slot < MAX_SLOTS);
    CHECK(out->stride >= out->width && out->stride % 64 == 0);
    CHECK(out->luma_rows >= out->height);
    status_of[slot] = status;
    last_pic[slot] = *out;
    if (out->in_target) {
        CHECK(zero_copy && out->data == NULL && out->stride == tex_stride && out->luma_rows == tex_rows);
        last_pic[slot].data = tex + slot * tex_layer;
        zc_in++;
    }
    pictures++;
    return 0;
}

/* The pattern over the shown part of the picture. */
static void check_output(int slot, unsigned w, unsigned h)
{
    const vdec_picture *p;
    unsigned x, y, bad = 0;

    if (slot < 0 || slot >= MAX_SLOTS || !last_pic[slot].data) {
        printf("output of slot %d, never decoded\n", slot);
        failures++;
        return;
    }
    p = &last_pic[slot];
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            bad += p->data[(size_t)y * p->stride + x] != vdec_fake_luma(status_of[slot]);
    for (y = 0; y < (h + 1) / 2; y++)
        for (x = 0; x < (w + 1) / 2 * 2; x++)
            bad += p->data[(size_t)(p->luma_rows + y) * p->stride + x] != vdec_fake_chroma((unsigned)slot);
    CHECK(bad == 0);
    outputs++;
}

/* ---------------------------------------------------------------- H.264 */

static int h264_cb(void *ctx, const H264Picture *pic)
{
    V383_DXVA_PicParams_H264 pp = pic->pp;
    vdec_picture out;
    char err[160] = "";
    int rc;
    (void)ctx;

    if (make_decoder(VDEC_CODEC_H264, pic->width, pic->height))
        return -1;
    pp.StatusReportFeedbackNumber = pic->decode_index + 1;
    give_target(V383_PIC_INDEX(pp.CurrPic), pic->decode_index, pic->width, pic->height);
    rc = vdec_h264_decode(dec, &pp, &pic->qm, pic->stream, (unsigned)pic->stream_len, &out, err, sizeof err);
    if (!rc)
        CHECK(out.width == pic->width && out.height == pic->height);
    return decoded(V383_PIC_INDEX(pp.CurrPic), pp.StatusReportFeedbackNumber, rc, &out, err, pic->decode_index);
}

static void h264_out(void *ctx, int slot, unsigned decode_index, int poc)
{
    (void)ctx; (void)decode_index; (void)poc;
    check_output(slot, last_pic[slot].width, last_pic[slot].height);
}

/* ----------------------------------------------------------------- HEVC */

static int hevc_cb(void *ctx, const HevcPicture *pic)
{
    V383_DXVA_PicParams_HEVC pp = pic->pp;
    vdec_picture out;
    char err[160] = "";
    int rc;
    (void)ctx;

    if (make_decoder(VDEC_CODEC_HEVC, pic->width, pic->height))
        return -1;
    pp.StatusReportFeedbackNumber = pic->decode_index + 1;
    give_target(V383_PIC_INDEX(pp.CurrPic), pic->decode_index, pic->width, pic->height);
    /* The matrix as FFmpeg hands it over: only with scaling lists on. */
    rc = vdec_hevc_decode(dec, &pp, V383_HEVC_SCALING_LIST_ENABLED(&pp) ? &pic->qm : NULL, pic->stream,
                          (unsigned)pic->stream_len, &out, err, sizeof err);
    if (!rc)
        CHECK(out.width == pic->width && out.height == pic->height);
    return decoded(V383_PIC_INDEX(pp.CurrPic), pp.StatusReportFeedbackNumber, rc, &out, err, pic->decode_index);
}

/* ------------------------------------------------------------------ VP9 */

static int vp9_cb(void *ctx, const Vp9Picture *pic)
{
    V383_DXVA_PicParams_VP9 pp = pic->pp;
    vdec_picture out;
    char err[160] = "";
    int rc;
    (void)ctx;

    if (make_decoder(VDEC_CODEC_VP9, max_w, max_h))
        return -1;
    pp.StatusReportFeedbackNumber = pic->decode_index + 1;
    give_target(V383_PIC_INDEX(pp.CurrPic), pic->decode_index, max_w, max_h);
    rc = vdec_vp9_decode(dec, &pp, pic->data, (unsigned)pic->len, &out, err, sizeof err);
    if (!rc)
        CHECK(out.width == pp.width && out.height == pp.height);
    return decoded(V383_PIC_INDEX(pp.CurrPic), pp.StatusReportFeedbackNumber, rc, &out, err, pic->decode_index);
}

static void vpx_out(void *ctx, int slot, unsigned w, unsigned h, unsigned decode_index, int existing)
{
    (void)ctx; (void)decode_index; (void)existing;
    check_output(slot, w, h);
}

/* ------------------------------------------------------------------ AV1 */

static int av1_cb(void *ctx, const Av1Picture *pic)
{
    V383_DXVA_PicParams_AV1 pp = pic->pp;
    vdec_picture out;
    char err[160] = "";
    unsigned i, end = 0;
    int rc;
    (void)ctx;

    if (make_decoder(VDEC_CODEC_AV1, max_w, max_h))
        return -1;
    for (i = 0; i < pic->num_tiles; i++)
        if (pic->tiles[i].DataOffset + pic->tiles[i].DataSize > end)
            end = pic->tiles[i].DataOffset + pic->tiles[i].DataSize;
    pp.StatusReportFeedbackNumber = pic->decode_index + 1;
    give_target(pp.CurrPicTextureIndex, pic->decode_index, max_w, max_h);
    rc = vdec_av1_decode(dec, &pp, pic->tiles, pic->num_tiles, pic->tile_data, end, &out, err, sizeof err);
    if (!rc)
        CHECK(out.width == pp.width && out.height == pp.height);
    return decoded(pp.CurrPicTextureIndex, pp.StatusReportFeedbackNumber, rc, &out, err, pic->decode_index);
}

/*
 * frame_refs[i].Index is the slot reference i uses (ref_frame_idx[i]), as
 * FFmpeg's dxva2_av1.c and the DXVA AV1 specification have it, not a
 * surface: slots that hold other surfaces than their numbers, two slots
 * holding one surface, and references whose slot number is some other
 * slot's surface.
 */
static void av1_slots_check(void)
{
    static V383_DXVA_PicParams_AV1 pp;
    static V383_Av1State s;
    static V383_Av1Hdr h;
    static const unsigned char map[8] = { 3, 3, 7, 0, 1, 5, 6, 2 };
    static const unsigned char ref[7] = { 1, 0, 4, 2, 7, 3, 6 };
    unsigned i;

    memset(&pp, 0, sizeof(pp));
    v383_av1_reset(&s);
    pp.width = pp.height = pp.max_width = pp.max_height = 64;
    pp.bitdepth = 8;
    pp.superres_denom = 8;
    pp.order_hint_bits = 7;
    pp.FormatAndPictureInfoFlags = V383_AV1_INTER_FRAME | (1 << 4) | (1 << 5);
    pp.CurrPicTextureIndex = 4;
    for (i = 0; i < 8; i++) {
        pp.RefFrameMapTextureIndex[i] = map[i];
        s.surf[map[i]].valid = 1;
    }
    for (i = 0; i < 7; i++)
        pp.frame_refs[i].Index = ref[i];
    v383_av1_derive(&pp, NULL, &s, &h);
    for (i = 0; i < 7; i++)
        CHECK(h.ref_frame_idx[i] == ref[i]);
}

/* ----------------------------------------------------------------- main */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n > 0 ? (size_t)n : 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return data;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *want_md5 = NULL, *log = "vdec-host-reglog.txt";
    unsigned want_frames = 0, codec;
    uint8_t *data, *logdata;
    size_t n, loglen;
    vdec_stats st;
    char hex[33];
    int i, rc = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) want_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reglog-md5") && i + 1 < argc) want_md5 = argv[++i];
        else if (!strcmp(argv[i], "--av1-synth") && i + 1 < argc) opts.av1_synth = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
        else if (!strcmp(argv[i], "--zero-copy")) zero_copy = 1;
        else if (!path) path = argv[i];
        else {
            printf("unknown argument %s\n", argv[i]);
            return 1;
        }
    }
    if (!path)
        path = "../data/testsrc-176x144-high.264";
    data = read_file(path, &n);
    if (!data) {
        printf("cannot open %s\n", path);
        return 1;
    }
    av1_slots_check();
    remove(log);
    setenv("D3D10UMD_VDEC_FAKE", "1", 1);
    setenv("D3D10UMD_VDEC_REGLOG", log, 1);
    CHECK(vdec_available());

    codec = av1_probe(data, n) ? VDEC_CODEC_AV1 : vp9_probe(data, n) ? VDEC_CODEC_VP9 :
            hevc_probe(data, n) ? VDEC_CODEC_HEVC : VDEC_CODEC_H264;
    if (codec == VDEC_CODEC_H264) {
        H264Parser *p = calloc(1, sizeof(*p));
        h264_init(p, h264_out, NULL);
        rc = h264_run(p, data, n, 0, h264_cb, NULL);
        if (rc)
            printf("parser: %s\n", p->err);
        h264_free(p);
        free(p);
    } else if (codec == VDEC_CODEC_HEVC) {
        HevcParser *p = calloc(1, sizeof(*p));
        hevc_init(p, h264_out, NULL);
        rc = hevc_run(p, data, n, 0, hevc_cb, NULL);
        if (rc)
            printf("parser: %s\n", p->err);
        hevc_free(p);
        free(p);
    } else if (codec == VDEC_CODEC_VP9) {
        Vp9Parser *p = calloc(1, sizeof(*p));
        vp9_init(p, NULL, NULL);
        rc = vp9_run(p, data, n, 0, NULL, NULL);        /* the largest frame, as vdpu383test */
        max_w = p->max_width;
        max_h = p->max_height;
        vp9_init(p, vpx_out, NULL);
        p->dxva_only = 1;                               /* frame_context_idx as FFmpeg hands it over */
        if (!rc)
            rc = vp9_run(p, data, n, 0, vp9_cb, NULL);
        if (rc)
            printf("parser: %s\n", p->err);
        free(p);
    } else {
        Av1Parser *p = calloc(1, sizeof(*p));
        av1_init(p, NULL, NULL);
        rc = av1_run(p, data, n, 0, NULL, NULL);
        max_w = p->max_width;
        max_h = p->max_height;
        av1_init(p, vpx_out, NULL);
        if (!rc)
            rc = av1_run(p, data, n, 0, av1_cb, NULL);
        if (rc)
            printf("parser: %s\n", p->err);
        av1_free(p);
        free(p);
    }
    CHECK(rc == 0);
    vdec_get_stats(&st);
    CHECK(st.fake == 1);
    CHECK(st.pictures == pictures && st.errors == 0);
    if (zero_copy) {
        /* every target taken, the rest copied; one import for the array */
        printf("zero copy: %u of %u pictures given a texture, %u decoded into it, %u imports\n", zc_given,
               pictures, zc_in, st.imports);
        CHECK(zc_given > 0 && zc_in == zc_given && st.zero_copy == zc_in && st.imports == 1);
    }
    CHECK(pictures > 0 && outputs > 0);
    if (want_frames)
        CHECK(outputs == want_frames);

    /* A picture larger than the decoder was made for is refused. */
    if (codec == VDEC_CODEC_H264 && dec) {
        V383_DXVA_PicParams_H264 big;
        vdec_picture out;
        char err[160];
        memset(&big, 0, sizeof(big));
        big.wFrameWidthInMbsMinus1 = 200;
        big.wFrameHeightInMbsMinus1 = 200;
        V383_PP_SET(&big, V383_PP_CHROMA_FORMAT_IDC, 2, 1);
        CHECK(vdec_h264_decode(dec, &big, NULL, data, 16, &out, err, sizeof err) == -1);
    }
    /* A decoder of one codec refuses another's pictures. */
    if (codec != VDEC_CODEC_HEVC && dec) {
        V383_DXVA_PicParams_HEVC hp;
        vdec_picture out;
        char err[160];
        memset(&hp, 0, sizeof(hp));
        CHECK(vdec_hevc_decode(dec, &hp, NULL, data, 16, &out, err, sizeof err) == -1);
    }
    vdec_destroy(dec);
    vdec_forget(0x5a5a0001ull);
    free(tex);
    free(data);

    /* The register files, against vdpu383test's. */
    logdata = read_file(log, &loglen);
    CHECK(logdata != NULL);
    if (logdata && zero_copy) {
        /* vdpu383.sys's address registers, each 0 or in a buffer: with zero
         * copy also the texture ("tex") and the decoder's own pictures
         * ("fb"); a raw value would be an address outside them all */
        char *line = (char *)logdata, *end = (char *)logdata + loglen;
        unsigned raw = 0, texs = 0;
        while (line < end) {
            char *nl = memchr(line, '\n', (size_t)(end - line));
            unsigned r, v;
            if (!nl)
                break;
            *nl = 0;
            if (sscanf(line, "r%u 0x%x", &r, &v) == 2 && v &&
                ((r >= 128 && r <= 134) || (r >= 140 && r <= 160 && !(r & 1)) || (r >= 168 && r <= 185) ||
                 r == 192 || (r >= 194 && r <= 210) || (r >= 216 && r <= 232))) {
                printf("address register outside the buffers: %s\n", line);
                raw++;
            }
            texs += strstr(line, " tex+") != NULL;
            *nl = '\n';
            line = nl + 1;
        }
        CHECK(raw == 0 && texs > 0);
    }
    if (logdata) {
        Md5 m;
        md5_init(&m);
        md5_update(&m, logdata, loglen);
        md5_final(&m, hex);
        printf("register log %s: %zu bytes, md5 %s\n", log, loglen, hex);
        if (want_md5) {
            printf("register log against vdpu383test --dxva-only: %s\n", strcmp(hex, want_md5) ? "MISMATCH" : "MATCH");
            CHECK(!strcmp(hex, want_md5));
        }
        free(logdata);
    }
    printf("%s: %s, %u pictures, %u frames out, %u failures\n", path,
           codec == VDEC_CODEC_H264 ? "H.264" : codec == VDEC_CODEC_HEVC ? "HEVC" : codec == VDEC_CODEC_VP9 ? "VP9" : "AV1",
           pictures, outputs, failures);
    return failures ? 1 : 0;
}
