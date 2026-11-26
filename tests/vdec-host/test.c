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
 *             [--import-fail N] [--tex-rows-align N] [--kmd [--revoke-at N]]
 *
 * --zero-copy: every picture but every fourth is given a target, a slice of
 * one "texture array" of this test's (zero copy, vdec_set_target), laid out
 * with 64 more rows than the decoder's own surfaces; the pattern must be in
 * the slice, the others (decoded into surfaces of the decoder's own and
 * copied by the caller) where vdec_picture says, and every address the
 * decoder is given in a buffer of the backend or the texture.
 *
 * --import-fail N (with --zero-copy): the fake refuses the first N imports
 * and a refused one is asked again every 4th use (D3D10UMD_VDEC_IMPORT_RETRY),
 * each time with a new export token from the target's refresh callback.
 *
 * --tex-rows-align N (with --zero-copy): the texture has exactly the
 * picture's rows rounded up to N (Chromium's 1080-row textures with 8):
 * H.264, HEVC and AV1 must still decode into it, VP9 (64-row superblocks)
 * only when that is a multiple of 64.
 *
 * --kmd: every call goes through maliwddm's escape (video/vdec_kmd.h) to a
 * session of vdpu383.sys, both faked (fakekmd.c) with the drivers' own
 * checks: the register log must be the same, every decode accepted. Then,
 * by escapes of their own: an address outside the process's buffers, the
 * same with VDPU383_RUN_NO_ADDR_CHECK, another process naming its buffers,
 * freeing them, importing its export, calls a session may not make, and
 * the process's exit; each must be refused. --revoke-at N (with
 * --zero-copy): after N pictures given a texture the display driver takes
 * its pages back; the next decode must be refused as revoked, refreshed
 * with a new export, and run.
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
#include "fakekmd.h"

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
/* the export token of the texture, a new one at each refresh */
static uint64_t tex_token = 0x5a5a0001ull;
static unsigned import_fail, refresh_calls;
static unsigned tex_rows_align, revoke_at, revoked;
/* --kmd: the decoding process, and another one */
static int kmd;
static fk_process proc_a = { 100, 1 }, proc_b = { 200, 1 };

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t tex_refresh(void *ctx, unsigned *pages)
{
    CHECK(ctx == &tex_token);
    refresh_calls++;
    *pages = tex_pages;
    /* the display driver exports the allocation again (its new pages) */
    if (kmd)
        return tex_token = fk_export(&proc_a, tex_pages, tex);
    return ++tex_token;
}

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
        tex_rows = tex_rows_align ? align_to(h, tex_rows_align) : align_to(h, 64) + 64;
        tex_layer = align_to(tex_stride * tex_rows * 3 / 2, 4096);
        tex_pages = (unsigned)(tex_layer * TEX_SLICES / 4096);
        tex = calloc(TEX_SLICES, tex_layer);
        if (kmd)
            tex_token = fk_export(&proc_a, tex_pages, tex);
    }
    if (decode_index % 4 == 3 || slot >= TEX_SLICES)
        return;
    /* eviction: the pages go while the decoder has them imported */
    if (revoke_at && zc_given == revoke_at && !revoked) {
        fk_gone(tex_token);
        revoked = 1;
    }
    memset(&t, 0, sizeof(t));
    t.token = tex_token;
    t.pages = tex_pages;
    t.refresh = tex_refresh;
    t.refresh_ctx = &tex_token;
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
            bad += p->data[(size_t)(p->luma_rows + y) * p->stride + x] !=
                   ((x & 1) ? vdec_fake_chroma_v((unsigned)slot) : vdec_fake_chroma((unsigned)slot));
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

/* ------------------------------------------------- --kmd: refusals */

/* One escape of process p, as vdec.c makes it; the escape's status. */
static long escape(fk_process *p, int with_device, unsigned ioctl, const void *in, unsigned in_size,
                   unsigned out_size, VDEC_KMD_CALL *c)
{
    memset(c, 0, sizeof(*c));
    c->Code = MW_ESCAPE_VDEC_CALL;
    c->Version = VDEC_KMD_VERSION;
    c->Ioctl = ioctl;
    c->InSize = in_size;
    c->OutSize = out_size;
    if (in_size)
        memcpy(c->Data, in, in_size);
    return fk_escape(p, with_device, c, sizeof(*c));
}

static unsigned run_result(fk_process *p, const VDPU383_RUN_IN *in, unsigned *bad)
{
    static VDEC_KMD_CALL c;
    const VDPU383_RUN_OUT *o = (const VDPU383_RUN_OUT *)(const void *)c.Data;

    if (escape(p, 1, IOCTL_VDPU383_RUN, in, sizeof(*in), sizeof(VDPU383_RUN_OUT), &c) != 0 || c.Status != 0)
        return 0xFFFFFFFFu;
    *bad = o->bad_reg;
    return o->result;
}

static void kmd_attacks(void)
{
    static VDPU383_RUN_IN in;
    static VDEC_KMD_CALL c;
    VDPU383_FREE_IN fr;
    VDPU383_IMPORT_IN im;
    uint32_t dma = 0, size = 0;
    unsigned handle = 0, bad = 0, r;
    uint64_t tok_a;
    uint8_t mem[4 * 4096];
    fk_counters k0, k1;

    /* a buffer of the process (the decoder's, still allocated: a new one) */
    {
        VDPU383_ALLOC_IN a = { 64 << 10 };
        const VDPU383_ALLOC_OUT *o = (const VDPU383_ALLOC_OUT *)(const void *)c.Data;
        CHECK(escape(&proc_a, 1, IOCTL_VDPU383_ALLOC, &a, sizeof(a), sizeof(VDPU383_ALLOC_OUT), &c) == 0 &&
              c.Status == 0 && o->handle);
        CHECK(fk_any_buffer(&proc_a, &dma, &size, &handle));
    }
    fk_get(&k0);

    memset(&in, 0, sizeof(in));
    in.regs[128] = dma;
    in.write_mask[128 / 32] |= 1u << (128 % 32);
    in.regs[168] = dma + 4096;
    in.write_mask[168 / 32] |= 1u << (168 % 32);
    in.flags = VDPU383_RUN_RESET_ON_ERROR;
    r = run_result(&proc_a, &in, &bad);
    printf("kmd: a decode into the process's own buffer: result %u\n", r);
    CHECK(r == VDPU383_RUN_OK);

    in.regs[168] = dma + size;          /* just past it */
    r = run_result(&proc_a, &in, &bad);
    printf("kmd: an address outside its buffers: result %u, register %u\n", r, bad);
    CHECK(r == VDPU383_RUN_BAD_ADDRESS && bad == 168);
    in.flags |= VDPU383_RUN_NO_ADDR_CHECK;
    r = run_result(&proc_a, &in, &bad);
    printf("kmd: the same with VDPU383_RUN_NO_ADDR_CHECK: result %u\n", r);
    CHECK(r == VDPU383_RUN_BAD_ADDRESS && bad == 168);
    in.flags = VDPU383_RUN_RESET_ON_ERROR;
    in.regs[168] = dma + 4096;

    /* another process */
    r = run_result(&proc_b, &in, &bad);
    printf("kmd: another process naming its buffers: result %u, register %u\n", r, bad);
    CHECK(r == VDPU383_RUN_BAD_ADDRESS && bad == 128);
    fr.handle = handle;
    CHECK(escape(&proc_b, 1, IOCTL_VDPU383_FREE, &fr, sizeof(fr), 0, &c) == 0 && c.Status == (int)0xC0000008);
    printf("kmd: another process freeing its buffer: status 0x%08x\n", (unsigned)c.Status);
    tok_a = fk_export(&proc_a, 4, mem);
    memset(&im, 0, sizeof(im));
    im.token = tok_a;
    im.pages = 4;
    CHECK(escape(&proc_b, 1, IOCTL_VDPU383_IMPORT, &im, sizeof(im), sizeof(VDPU383_IMPORT_OUT), &c) == 0 &&
          c.Status == (int)0xC0000225);
    printf("kmd: another process importing its export: status 0x%08x\n", (unsigned)c.Status);
    CHECK(escape(&proc_a, 1, IOCTL_VDPU383_IMPORT, &im, sizeof(im), sizeof(VDPU383_IMPORT_OUT), &c) == 0 &&
          c.Status == 0);
    printf("kmd: the process importing its own export: status 0x%08x\n", (unsigned)c.Status);
    r = run_result(&proc_a, &in, &bad);
    CHECK(r == VDPU383_RUN_OK);
    in.regs[168] = ((const VDPU383_IMPORT_OUT *)(const void *)c.Data)->dma;
    r = run_result(&proc_a, &in, &bad);
    CHECK(r == VDPU383_RUN_OK);
    fk_gone(tok_a);                     /* destroyed */
    r = run_result(&proc_a, &in, &bad);
    printf("kmd: decoding into an import after its allocation went: result %u\n", r);
    CHECK(r == VDPU383_RUN_REVOKED && bad == 168);
    in.regs[168] = dma + 4096;

    /* calls and shapes a session may not have */
    CHECK(escape(&proc_a, 1, IOCTL_VDPU383_WRITE_REGS, &in, 16, 0, &c) == (long)0xC0000022);
    CHECK(escape(&proc_a, 1, IOCTL_VDPU383_WARMUP, NULL, 0, sizeof(VDPU383_WARMUP_RESULT), &c) == (long)0xC0000022);
    CHECK(escape(&proc_a, 1, IOCTL_VDPU383_RESET, NULL, 0, 4, &c) == (long)0xC0000022);
    CHECK(escape(&proc_a, 1, IOCTL_VDPU383_READ_REGS, &in, 8, 4, &c) == (long)0xC0000022);
    CHECK(escape(&proc_a, 1, IOCTL_VDPU383_RUN, &in, sizeof(in) - 4, sizeof(VDPU383_RUN_OUT), &c) ==
          (long)0xC000000D);
    CHECK(escape(&proc_a, 0, IOCTL_VDPU383_RUN, &in, sizeof(in), sizeof(VDPU383_RUN_OUT), &c) == (long)0xC000000D);
    memset(&c, 0, sizeof(c));
    c.Code = MW_ESCAPE_VDEC_CALL;
    c.Version = 2;
    c.Ioctl = IOCTL_VDPU383_GET_INFO;
    c.OutSize = sizeof(VDPU383_INFO);
    CHECK(fk_escape(&proc_a, 1, &c, sizeof(c)) == (long)0xC000000D);
    CHECK(fk_escape(&proc_a, 1, &c, sizeof(c) - 8) == (long)0xC000000D);
    printf("kmd: register access, warm-up, reset, wrong sizes, no device, another version, short escape: refused\n");

    /* what a process is told */
    CHECK(escape(&proc_a, 1, IOCTL_VDPU383_GET_INFO, NULL, 0, sizeof(VDPU383_INFO), &c) == 0 && c.Status == 0 &&
          ((const VDPU383_INFO *)(const void *)c.Data)->iommu_dt_dma == 0 &&
          ((const VDPU383_INFO *)(const void *)c.Data)->interface_version >= 4);

    /* the process exits */
    fk_exit(&proc_a);
    r = run_result(&proc_b, &in, &bad);
    printf("kmd: another process naming them after the process exited: result %u\n", r);
    CHECK(r == VDPU383_RUN_BAD_ADDRESS);
    fk_get(&k1);
    CHECK(k1.imports_live == 0 && k1.refused_runs == k0.refused_runs + 4 && k1.revoked_runs == k0.revoked_runs + 1);
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
    int i, rc = 0, zc_none = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) want_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reglog-md5") && i + 1 < argc) want_md5 = argv[++i];
        else if (!strcmp(argv[i], "--av1-synth") && i + 1 < argc) opts.av1_synth = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
        else if (!strcmp(argv[i], "--zero-copy")) zero_copy = 1;
        else if (!strcmp(argv[i], "--import-fail") && i + 1 < argc) import_fail = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tex-rows-align") && i + 1 < argc) tex_rows_align = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--kmd")) kmd = 1;
        else if (!strcmp(argv[i], "--revoke-at") && i + 1 < argc) revoke_at = (unsigned)atoi(argv[++i]);
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
    if (import_fail) {
        char v[16];
        snprintf(v, sizeof v, "%u", import_fail);
        setenv("D3D10UMD_VDEC_FAKE_IMPORT_FAIL", v, 1);
        setenv("D3D10UMD_VDEC_IMPORT_RETRY", "4", 1);
    }
    setenv("D3D10UMD_VDEC_REGLOG", log, 1);
    if (kmd)
        vdec_set_escape(fk_escape, &proc_a);
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
    CHECK(st.path == (kmd ? VDEC_PATH_KMD : VDEC_PATH_MEM));
    if (kmd) {
        fk_counters k;
        fk_get(&k);
        printf("kmd: %u escapes made, %u calls, %u decodes, %u refused, %u revoked, session runs %u\n",
               st.kmd_calls, k.calls, k.runs, k.refused_runs, k.revoked_runs, st.session_runs);
        CHECK(st.kmd_calls > 0 && k.runs == pictures && k.refused_runs == 0 && k.refused_escapes == 0);
        CHECK(k.revoked_runs == (revoke_at ? 1u : 0u) && st.session_runs == k.runs);
        if (revoke_at)
            CHECK(revoked && st.refreshes == 1 && refresh_calls == 1);
    }
    /* VP9 writes 64-row superblocks: a texture with rows not a multiple of
     * 64 is not decoded into */
    zc_none = zero_copy && tex_rows_align && codec == VDEC_CODEC_VP9 && tex_rows % 64;
    if (zc_none) {
        printf("zero copy: %u of %u pictures given a %u-row texture, %u decoded into it (VP9: none expected)\n",
               zc_given, pictures, tex_rows, zc_in);
        CHECK(zc_in == 0 && st.imports == 0);
    } else if (zero_copy) {
        /* every target taken, the rest copied; one import for the array */
        printf("zero copy: %u of %u pictures given a texture, %u decoded into it, %u imports\n", zc_given,
               pictures, zc_in, st.imports);
        if (!import_fail) {
            CHECK(zc_given > 0 && zc_in == zc_given && st.zero_copy == zc_in && st.imports == 1);
            CHECK(st.import_failures == 0 && st.import_retries == 0 && refresh_calls == (revoke_at ? 1u : 0u));
        } else {
            /* refused at the first use and at every 4th after, N times; the
             * retry after the last refusal imports, with a new token each time */
            unsigned missed = 1 + 4 * (import_fail - 1) + 3;
            printf("import refused %u times: %u retries, %u refreshes, %u pictures copied, last error %u "
                   "for %u pages\n", st.import_failures, st.import_retries, refresh_calls, zc_given - zc_in,
                   st.last_import_error, st.last_import_pages);
            CHECK(zc_given > missed && zc_in == zc_given - missed && st.zero_copy == zc_in);
            CHECK(st.imports == 1 && st.import_failures == import_fail && st.import_retries == import_fail);
            CHECK(refresh_calls == import_fail && tex_token == 0x5a5a0001ull + import_fail);
            CHECK(st.last_import_error == 1168 && st.last_import_pages == tex_pages);
        }
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
    vdec_forget(tex_token);
    if (kmd) {
        fk_counters k;
        fk_get(&k);
        CHECK(k.imports_live == 0);     /* the texture's import let go */
        kmd_attacks();
    }
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
        CHECK(raw == 0 && (texs > 0 || zc_none));
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
