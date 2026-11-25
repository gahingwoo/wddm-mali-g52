/*
 * vdec.c - DXVA pictures on the RK3576 VDPU383, for the user-mode driver
 * (vdec.h says how and why).
 *
 * The decode sequences are vdpu383test's (woa-rk3576 tools/vdpu383test,
 * main.c decode_cb, decode_cb_hevc, decode_cb_vp9, decode_cb_av1) in their
 * --dxva-only modes: the same surface layouts, the same buffers, the same
 * run flags. H.264 matched the FFmpeg references bit for bit on the
 * TaishanPi 3M on 2026-11-24 that way, HEVC on 2026-11-25 (vdpu383test,
 * 60 of 75 conformance streams; the rest failed allocations, fixed since by
 * the driver's IOMMU); VP9 and AV1 have not run on the board yet. What
 * differs from vdpu383test: the picture parameters come from the
 * application through DXVA instead of from vdpu383test's parser, and the
 * surfaces are allocated when a DXVA surface index is first used.
 *
 * H.264: the picture parameter set id is read from the first slice header
 * and the profile is derived (V383_H264Extra absent), and the scaling
 * matrix is taken as present whenever the DXVA matrix is not flat.
 * HEVC: the reference picture set table the decoder needs is rebuilt from
 * DXVA (v383_hevc_rebuild_rps). VP9 and AV1: the decoder's memory between
 * frames (V383_Vp9State, V383_Av1State, the auxiliary buffer, the
 * co-located motion vectors and the CDF buffers) belongs to the decoder
 * object, so two decoders in one process never share it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "vdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#define V383_IOCTL_STANDALONE 1
#endif
#include "vdpu383/vdpu383_ioctl.h"
#include "vdpu383/lib/vdpu383_h264.h"
#include "vdpu383/lib/vdpu383_hevc.h"
#include "vdpu383/lib/vdpu383_vp9.h"
#include "vdpu383/lib/vdpu383_av1.h"

#define VDEC_MAX_SURFACES   128         /* DXVA Index7Bits */
#define VDEC_MIN_STREAM     (4u << 20)

typedef struct vdec_buf {
    unsigned handle;
    uint32_t dma;
    uint32_t size;
    uint8_t *cpu;
} vdec_buf;

struct vdec {
    vdec_codec codec;
    vdec_options opt;
    unsigned max_w, max_h;
    unsigned width, height, hor, ver;
    unsigned frame_mbs_only, ctb;       /* H.264, HEVC: what the geometry was set up for */
    unsigned colmv_off, cdf_off, surf_size;
    vdec_buf surf[VDEC_MAX_SURFACES];
    V383_Surface vs[VDEC_MAX_SURFACES];
    V383_Av1Surface as[V383_AV1_MAX_SURFACES];
    vdec_buf aux, rcb, strm, colmv[2];
    unsigned aux_len;                   /* bytes of aux the builder writes (reglog) */
    unsigned pictures;
    VDPU383_RUN_IN *run_in;
    VDPU383_RUN_OUT *run_out;
    /* codec state, kept per decoder */
    V383_HevcRpsTable *rps;
    V383_Vp9State vp9;
    V383_Av1State av1;
};

static int g_state;                     /* 0 not tried, 1 available, -1 not */
static int g_fake;
static uint32_t g_fake_dma = 0x10000000u;
static vdec_stats g_stats;
static FILE *g_reglog;
#ifdef _WIN32
static HANDLE g_dev = INVALID_HANDLE_VALUE;
static SRWLOCK g_lock = SRWLOCK_INIT;
#endif

static const char *const codec_names[VDEC_CODEC_COUNT] = { "h264", "hevc", "vp9", "av1" };

static unsigned align_up(unsigned v, unsigned a)
{
    return (v + a - 1) & ~(a - 1);
}

static void set_err(char *err, size_t errlen, const char *fmt, unsigned a, unsigned b)
{
    if (err && errlen)
        snprintf(err, errlen, fmt, a, b);
}

static int env_fake(void)
{
    const char *v = getenv("D3D10UMD_VDEC_FAKE");
    return v && v[0] == '1';
}

int vdec_available(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_lock);
#endif
    if (g_state == 0) {
        const char *log = getenv("D3D10UMD_VDEC_REGLOG");

        if (log && log[0])
            g_reglog = fopen(log, "ab");
        g_fake = env_fake();
        g_stats.fake = (unsigned)g_fake;
        if (g_fake) {
            g_state = 1;
        } else {
#ifdef _WIN32
            g_dev = CreateFileW(VDPU383_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
            g_state = g_dev != INVALID_HANDLE_VALUE ? 1 : -1;
#else
            g_state = -1;
#endif
        }
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_lock);
#endif
    return g_state == 1;
}

/* ------------------------------------------------------------ buffers */

static int buf_alloc(uint32_t size, vdec_buf *b)
{
    memset(b, 0, sizeof(*b));
    size = align_up(size, 4096);
    if (g_fake) {
        b->cpu = (uint8_t *)calloc(1, size);
        if (!b->cpu)
            return -1;
        b->handle = 1;
        b->dma = g_fake_dma;
        b->size = size;
        g_fake_dma += size + 4096;
        return 0;
    }
#ifdef _WIN32
    {
        VDPU383_ALLOC_IN in;
        VDPU383_ALLOC_OUT out;
        DWORD got = 0;

        in.size = size;
        memset(&out, 0, sizeof(out));
        if (!DeviceIoControl(g_dev, IOCTL_VDPU383_ALLOC, &in, sizeof(in), &out, sizeof(out), &got, NULL) ||
            got < sizeof(out))
            return -1;
        b->handle = out.handle;
        b->dma = out.dma;
        b->size = out.size;
        b->cpu = (uint8_t *)(uintptr_t)out.user_va;
        if (out.flags & VDPU383_ALLOC_IOVA)
            g_stats.iommu = 1;
        return 0;
    }
#else
    return -1;
#endif
}

static void buf_free(vdec_buf *b)
{
    if (!b->handle)
        return;
    if (g_fake) {
        free(b->cpu);
    } else {
#ifdef _WIN32
        VDPU383_FREE_IN in;
        DWORD got = 0;
        in.handle = b->handle;
        DeviceIoControl(g_dev, IOCTL_VDPU383_FREE, &in, sizeof(in), NULL, 0, &got, NULL);
#endif
    }
    memset(b, 0, sizeof(*b));
}

static int buf_alloc_zero(uint32_t size, vdec_buf *b)
{
    if (buf_alloc(size, b))
        return -1;
    memset(b->cpu, 0, b->size);
    return 0;
}

static void free_surfaces(vdec *d)
{
    unsigned i;

    for (i = 0; i < VDEC_MAX_SURFACES; i++) {
        buf_free(&d->surf[i]);
        d->vs[i].dma = 0;
        d->vs[i].colmv_dma = 0;
        if (i < V383_AV1_MAX_SURFACES)
            memset(&d->as[i], 0, sizeof(d->as[i]));
    }
    buf_free(&d->rcb);
    d->width = d->height = 0;
}

/* The bitstream buffer, at least need bytes, the first need zeroed. */
static int stream_room(vdec *d, unsigned need, char *err, size_t errlen)
{
    if (need > d->strm.size) {
        buf_free(&d->strm);
        if (buf_alloc(need > VDEC_MIN_STREAM ? need : VDEC_MIN_STREAM, &d->strm)) {
            set_err(err, errlen, "bitstream buffer (%u bytes) not allocated", need, 0);
            return -1;
        }
    }
    memset(d->strm.cpu, 0, need);
    return 0;
}

/* H.264 and HEVC: the geometry of this picture; a change drops every
 * surface. Surfaces as vdpu383test's: luma stride 64-aligned, chroma right
 * after the luma, co-located motion vectors behind room for 4:2:2. */
static int setup_geometry(vdec *d, unsigned w, unsigned h, unsigned key, unsigned colmv, unsigned rcb,
                          char *err, size_t errlen)
{
    if (d->width == w && d->height == h && d->frame_mbs_only == key && d->rcb.handle)
        return 0;
    if (w > d->max_w || h > d->max_h) {
        set_err(err, errlen, "picture %ux%u is larger than the decoder", w, h);
        return -1;
    }
    free_surfaces(d);
    d->width = w;
    d->height = h;
    d->frame_mbs_only = key;
    d->hor = align_up(w, 64);
    d->ver = align_up(h, 16);
    d->colmv_off = align_up(d->hor * d->ver * 2, 4096);
    d->surf_size = d->colmv_off + align_up(colmv, 4096);
    if (buf_alloc_zero(rcb + 4096, &d->rcb)) {
        set_err(err, errlen, "row buffer (%u bytes) not allocated", rcb, 0);
        return -1;
    }
    return 0;
}

static int ensure_surface(vdec *d, unsigned i, char *err, size_t errlen)
{
    if (i >= VDEC_MAX_SURFACES) {
        set_err(err, errlen, "surface index %u out of range", i, 0);
        return -1;
    }
    if (d->surf[i].handle)
        return 0;
    if (buf_alloc_zero(d->surf_size, &d->surf[i])) {
        set_err(err, errlen, "surface %u (%u bytes) not allocated", i, d->surf_size);
        return -1;
    }
    d->vs[i].dma = d->surf[i].dma;
    d->vs[i].colmv_dma = d->codec == VDEC_CODEC_VP9 ? 0 : d->surf[i].dma + d->colmv_off;
    if (i < V383_AV1_MAX_SURFACES) {
        d->as[i].dma = d->surf[i].dma;
        d->as[i].colmv_dma = d->surf[i].dma + d->colmv_off;
        d->as[i].cdf_dma = d->surf[i].dma + d->cdf_off;
    }
    return 0;
}

/* ----------------------------------------------------- register log */

static const char *buf_role(const vdec *d, uint32_t a, uint32_t *off, unsigned *index)
{
    unsigned i;

    *index = ~0u;
#define VDEC_IN_BUF(b) ((b).handle && a >= (b).dma && a < (b).dma + (b).size)
    if (VDEC_IN_BUF(d->strm)) { *off = a - d->strm.dma; return "strm"; }
    if (VDEC_IN_BUF(d->aux)) { *off = a - d->aux.dma; return "aux"; }
    if (VDEC_IN_BUF(d->rcb)) { *off = a - d->rcb.dma; return "rcb"; }
    if (VDEC_IN_BUF(d->colmv[0])) { *off = a - d->colmv[0].dma; return "colmv0"; }
    if (VDEC_IN_BUF(d->colmv[1])) { *off = a - d->colmv[1].dma; return "colmv1"; }
    for (i = 0; i < VDEC_MAX_SURFACES; i++)
        if (VDEC_IN_BUF(d->surf[i])) {
            *off = a - d->surf[i].dma;
            *index = i;
            return "s";
        }
#undef VDEC_IN_BUF
    return NULL;
}

/* vdpu383.sys's V383IsAddrReg: the registers it checks as addresses. */
static int is_addr_reg(unsigned r)
{
    return (r >= 128 && r <= 134) || (r >= 140 && r <= 160 && !(r & 1)) || (r >= 168 && r <= 185) ||
           r == 192 || (r >= 194 && r <= 210) || (r >= 216 && r <= 232);
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/*
 * One picture's register file, addresses as buffer+offset, then a hash of
 * the auxiliary buffer and of the stream bytes the decoder is told to read.
 * tests/vdec-host/vdpu383test-reglog.patch makes vdpu383test write the same
 * lines for its --dxva-only runs.
 */
static void reglog(const vdec *d, const V383_RegFile *rf)
{
    unsigned r, idx;
    uint32_t off, sa, sl;

    if (!g_reglog)
        return;
    fprintf(g_reglog, "pic %s %u\n", codec_names[d->codec], d->pictures);
    for (r = 0; r < V383_NUM_REGS; r++) {
        const char *role;

        if (!(rf->write_mask[r / 32] & (1u << (r % 32))))
            continue;
        role = (is_addr_reg(r) && rf->regs[r]) ? buf_role(d, rf->regs[r], &off, &idx) : NULL;
        if (role && idx != ~0u)
            fprintf(g_reglog, "r%u s%u+0x%x\n", r, idx, (unsigned)off);
        else if (role)
            fprintf(g_reglog, "r%u %s+0x%x\n", r, role, (unsigned)off);
        else
            fprintf(g_reglog, "r%u 0x%08x\n", r, rf->regs[r]);
    }
    fprintf(g_reglog, "timeout 0x%08x\n", rf->core_timeout);
    fprintf(g_reglog, "aux %u %016llx\n", d->aux_len,
            (unsigned long long)fnv1a(d->aux.cpu, d->aux_len));
    sa = rf->regs[128];
    sl = rf->regs[66];
    if (sa >= d->strm.dma && sa - d->strm.dma + (uint64_t)sl <= d->strm.size)
        fprintf(g_reglog, "strm %u %016llx\n", (unsigned)sl,
                (unsigned long long)fnv1a(d->strm.cpu + (sa - d->strm.dma), sl));
    else
        fprintf(g_reglog, "strm outside the buffer\n");
    fflush(g_reglog);
}

/* ----------------------------------------------------------- decoder */

vdec *vdec_create(vdec_codec codec, unsigned width, unsigned height, const vdec_options *opt,
                  char *err, size_t errlen)
{
    vdec *d;
    unsigned aux;

    if ((unsigned)codec >= VDEC_CODEC_COUNT) {
        set_err(err, errlen, "codec %u not known", (unsigned)codec, 0);
        return NULL;
    }
    if (!vdec_available()) {
        set_err(err, errlen, "no VDPU383 device (\\\\.\\VDPU383 did not open)", 0, 0);
        return NULL;
    }
    d = (vdec *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->codec = codec;
    if (opt)
        d->opt = *opt;
    /* H.264, HEVC: a bound. VP9, AV1: the geometry of every surface, as
     * given (vdpu383test sizes them from the stream's largest frame). */
    d->max_w = width ? width : 16;
    d->max_h = height ? height : 16;
    if (codec == VDEC_CODEC_H264 || codec == VDEC_CODEC_HEVC) {
        d->max_w = align_up(d->max_w, 16);
        d->max_h = align_up(d->max_h, 16);
    }
    d->run_in = (VDPU383_RUN_IN *)calloc(1, sizeof(*d->run_in));
    d->run_out = (VDPU383_RUN_OUT *)calloc(1, sizeof(*d->run_out));
    if (!d->run_in || !d->run_out)
        goto nomem;

    switch (codec) {
    case VDEC_CODEC_H264:
        aux = V383_H264_AUX_SIZE;
        d->aux_len = V383_H264_AUX_SIZE;
        break;
    case VDEC_CODEC_HEVC:
        aux = V383_HEVC_AUX_SIZE;
        d->aux_len = V383_HEVC_AUX_SIZE;
        d->rps = (V383_HevcRpsTable *)calloc(1, sizeof(*d->rps));
        if (!d->rps)
            goto nomem;
        break;
    case VDEC_CODEC_VP9: {
        /* One geometry for the stream, as the texture array has (and as
         * vdpu383test sets up for the largest frame): 64-aligned both ways. */
        unsigned colmv = v383_vp9_colmv_size(d->max_w, d->max_h);
        aux = V383_VP9_AUX_SIZE;
        d->aux_len = V383_VP9_AUX_SIZE;
        d->width = d->max_w;
        d->height = d->max_h;
        d->hor = align_up(d->max_w, 64);
        d->ver = align_up(d->max_h, 64);
        d->surf_size = align_up(d->hor * d->ver * 3 / 2, 4096);
        if (buf_alloc_zero(v383_vp9_rcb_size(d->max_w, d->max_h) + 4096, &d->rcb) ||
            buf_alloc_zero(colmv, &d->colmv[0]) || buf_alloc_zero(colmv, &d->colmv[1]))
            goto nomem;
        v383_vp9_reset(&d->vp9);
        break;
    }
    default: {                          /* AV1 */
        unsigned colmv = v383_av1_colmv_size(d->max_w, d->max_h);
        aux = v383_av1_aux_size(d->max_w, d->max_h);
        d->aux_len = aux;
        d->width = d->max_w;
        d->height = d->max_h;
        d->hor = align_up(d->max_w, 64);
        d->ver = align_up(d->max_h, 64);
        d->colmv_off = align_up(d->hor * d->ver * 3 / 2, 4096);
        d->cdf_off = d->colmv_off + colmv;
        d->surf_size = d->cdf_off + v383_av1_cdf_size(d->max_w, d->max_h);
        v383_av1_reset(&d->av1);
        break;
    }
    }
    if (buf_alloc_zero(aux, &d->aux))
        goto nomem;
    return d;

nomem:
    set_err(err, errlen, "decoder buffers not allocated", 0, 0);
    vdec_destroy(d);
    return NULL;
}

vdec *vdec_h264_create(unsigned width, unsigned height, char *err, size_t errlen)
{
    return vdec_create(VDEC_CODEC_H264, width, height, NULL, err, errlen);
}

void vdec_destroy(vdec *d)
{
    if (!d)
        return;
    free_surfaces(d);
    buf_free(&d->aux);
    buf_free(&d->strm);
    buf_free(&d->colmv[0]);
    buf_free(&d->colmv[1]);
    free(d->rps);
    free(d->run_in);
    free(d->run_out);
    free(d);
}

void vdec_h264_destroy(vdec *d)
{
    vdec_destroy(d);
}

vdec_codec vdec_get_codec(const vdec *d)
{
    return d->codec;
}

static int qm_is_flat(const V383_DXVA_Qmatrix_H264 *qm)
{
    unsigned i, j;

    for (i = 0; i < 6; i++)
        for (j = 0; j < 16; j++)
            if (qm->bScalingLists4x4[i][j] != 16)
                return 0;
    for (i = 0; i < 2; i++)
        for (j = 0; j < 64; j++)
            if (qm->bScalingLists8x8[i][j] != 16)
                return 0;
    return 1;
}

static int run(vdec *d, const V383_RegFile *rf, char *err, size_t errlen)
{
    VDPU383_RUN_IN *in = d->run_in;
    VDPU383_RUN_OUT *out = d->run_out;

    memset(in, 0, sizeof(*in));
    memcpy(in->regs, rf->regs, sizeof(in->regs));
    memcpy(in->write_mask, rf->write_mask, sizeof(in->write_mask));
    in->link_ip_timeout = rf->core_timeout;
    in->link_ip_enable = 0;
    in->timeout_ms = 2000;
    in->flags = VDPU383_RUN_RESET_ON_ERROR;
    memset(out, 0, sizeof(*out));
#ifdef _WIN32
    {
        DWORD got = 0;
        if (!DeviceIoControl(g_dev, IOCTL_VDPU383_RUN, in, sizeof(*in), out, sizeof(*out), &got, NULL)) {
            set_err(err, errlen, "IOCTL_VDPU383_RUN failed (%u)", (unsigned)GetLastError(), 0);
            return -1;
        }
    }
#endif
    g_stats.hw_us_total += out->elapsed_us;
    if (out->result != VDPU383_RUN_OK) {
        if (out->result == VDPU383_RUN_BAD_ADDRESS)
            set_err(err, errlen, "decoder refused register %u (address outside the buffers), status 0x%x",
                    out->bad_reg, out->status);
        else if (out->result == VDPU383_RUN_MMU_FAULT)
            set_err(err, errlen, "decoder IOMMU fault at IO address 0x%08x, status 0x%x",
                    out->bad_reg, out->status);
        else
            set_err(err, errlen, "decode result %u, status 0x%08x", out->result, out->status);
        return -1;
    }
    return 0;
}

/* The register file is built: log it, run it (or write the fake pattern),
 * describe the result. */
static int finish(vdec *d, const V383_RegFile *rf, unsigned idx, unsigned status_report, unsigned w,
                  unsigned h, vdec_picture *out, char *err, size_t errlen)
{
    reglog(d, rf);
    d->pictures++;
    if (g_fake) {
        uint8_t *s = d->surf[idx].cpu;
        size_t luma = (size_t)d->hor * d->ver;
        memset(s, vdec_fake_luma(status_report), luma);
        memset(s + luma, vdec_fake_chroma(idx), luma / 2);
    } else if (run(d, rf, err, errlen)) {
        return -1;
    }
    g_stats.pictures++;
    out->data = d->surf[idx].cpu;
    out->stride = d->hor;
    out->luma_rows = d->ver;
    out->width = w;
    out->height = h;
    return 0;
}

static int check_codec(vdec *d, vdec_codec c, char *err, size_t errlen)
{
    if (!d || d->codec != c) {
        if (err && errlen)
            snprintf(err, errlen, "not a %s decoder", codec_names[c]);
        return -1;
    }
    return 0;
}

int vdec_h264_decode(vdec *d, const V383_DXVA_PicParams_H264 *pp,
                     const V383_DXVA_Qmatrix_H264 *qm, const uint8_t *bitstream,
                     unsigned bitstream_len, vdec_picture *out, char *err, size_t errlen)
{
    V383_H264Input bi;
    V383_RegFile rf;
    unsigned idx, i, need, rc, w, h, fmo;

    if (check_codec(d, VDEC_CODEC_H264, err, errlen))
        goto fail;
    if (!pp || !bitstream || !bitstream_len) {
        set_err(err, errlen, "nothing to decode", 0, 0);
        goto fail;
    }
    if (pp->bit_depth_luma_minus8 || pp->bit_depth_chroma_minus8 ||
        V383_PP_GET(pp, V383_PP_CHROMA_FORMAT_IDC, 2) != 1) {
        set_err(err, errlen, "only 8-bit 4:2:0 (bit depth %u, chroma format %u)",
                pp->bit_depth_luma_minus8 + 8u, V383_PP_GET(pp, V383_PP_CHROMA_FORMAT_IDC, 2));
        goto fail;
    }
    w = (pp->wFrameWidthInMbsMinus1 + 1u) * 16u;
    h = (pp->wFrameHeightInMbsMinus1 + 1u) * 16u;
    fmo = V383_PP_GET(pp, V383_PP_FRAME_MBS_ONLY, 1);
    if (setup_geometry(d, w, h, fmo, v383_h264_colmv_size(w, h, fmo), v383_h264_rcb_size(w, h, 1),
                       err, errlen))
        goto fail;

    idx = V383_PIC_INDEX(pp->CurrPic);
    if (ensure_surface(d, idx, err, errlen))
        goto fail;
    /* A reference this process never decoded (a stream that starts on a
     * P picture): a zeroed surface, so every address is a buffer of ours. */
    for (i = 0; i < 16; i++)
        if (!V383_PIC_UNUSED(pp->RefFrameList[i]) &&
            ensure_surface(d, V383_PIC_INDEX(pp->RefFrameList[i]), err, errlen))
            goto fail;

    need = align_up(bitstream_len, 16) + 128;
    if (stream_room(d, need, err, errlen))
        goto fail;
    memcpy(d->strm.cpu, bitstream, bitstream_len);

    memset(&bi, 0, sizeof(bi));
    bi.pp = pp;
    bi.qm = qm;
    bi.scaling_matrix_present = qm && !qm_is_flat(qm);
    bi.extra = NULL;
    bi.pps_id = V383_PPS_ID_FROM_STREAM;
    bi.stream_cpu = d->strm.cpu;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = align_up(bitstream_len, 16);
    bi.aux_cpu = d->aux.cpu;
    bi.aux_dma = d->aux.dma;
    bi.rcb_dma = d->rcb.dma;
    bi.surfaces = d->vs;
    bi.num_surfaces = VDEC_MAX_SURFACES;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;

    rc = (unsigned)v383_h264_build(&bi, &rf);
    if (rc) {
        set_err(err, errlen, "register builder refused the picture (%d)", rc, 0);
        goto fail;
    }
    if (finish(d, &rf, idx, pp->StatusReportFeedbackNumber, d->width, d->height, out, err, errlen))
        goto fail;
    return 0;

fail:
    g_stats.errors++;
    return -1;
}

int vdec_hevc_decode(vdec *d, const V383_DXVA_PicParams_HEVC *pp, const V383_DXVA_Qmatrix_HEVC *qm,
                     const uint8_t *bitstream, unsigned bitstream_len, vdec_picture *out,
                     char *err, size_t errlen)
{
    V383_HevcInput bi;
    V383_RegFile rf;
    unsigned idx, i, need, w, h, cb;
    int rc;

    if (check_codec(d, VDEC_CODEC_HEVC, err, errlen))
        goto fail;
    if (!pp || !bitstream || !bitstream_len) {
        set_err(err, errlen, "nothing to decode", 0, 0);
        goto fail;
    }
    if (V383_HEVC_CHROMA_FORMAT_IDC(pp) != 1 || V383_HEVC_BIT_DEPTH_LUMA_MINUS8(pp) ||
        V383_HEVC_BIT_DEPTH_CHROMA_MINUS8(pp)) {
        set_err(err, errlen, "HEVC Main only (bit depth %u, chroma format %u)",
                V383_HEVC_BIT_DEPTH_LUMA_MINUS8(pp) + 8u, V383_HEVC_CHROMA_FORMAT_IDC(pp));
        goto fail;
    }
    if (V383_HEVC_SCALING_LIST_ENABLED(pp) && !qm) {
        set_err(err, errlen, "scaling lists enabled and no matrix buffer", 0, 0);
        goto fail;
    }
    w = v383_hevc_width(pp);
    h = v383_hevc_height(pp);
    /* As vdpu383test: the co-located MV size from the minimum coding block. */
    cb = 1u << (pp->log2_min_luma_coding_block_size_minus3 + 3);
    if (setup_geometry(d, w, h, cb, v383_hevc_colmv_size(w, h, cb), v383_hevc_rcb_size(w, h), err, errlen))
        goto fail;

    idx = V383_PIC_INDEX(pp->CurrPic);
    if (ensure_surface(d, idx, err, errlen))
        goto fail;
    for (i = 0; i < 15; i++)
        if (pp->RefPicList[i].bPicEntry != 0xFF &&
            ensure_surface(d, V383_PIC_INDEX(pp->RefPicList[i]), err, errlen))
            goto fail;

    need = v383_hevc_stream_read_len(bitstream_len);
    if (stream_room(d, need, err, errlen))
        goto fail;
    memcpy(d->strm.cpu, bitstream, bitstream_len);

    memset(&bi, 0, sizeof(bi));
    bi.pp = pp;
    bi.qm = qm;
    bi.extra = NULL;
    bi.stream_cpu = d->strm.cpu;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = bitstream_len;
    bi.aux_cpu = d->aux.cpu;
    bi.aux_dma = d->aux.dma;
    bi.rcb_dma = d->rcb.dma;
    bi.surfaces = d->vs;
    bi.num_surfaces = VDEC_MAX_SURFACES;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;
    bi.rebuild_scratch = d->rps;

    rc = v383_hevc_build(&bi, &rf);
    if (rc) {
        set_err(err, errlen, rc == V383_E_UNSUPPORTED ? "no reference set table fits the picture's slices (%d)"
                                                      : "register builder refused the picture (%d)",
                (unsigned)rc, 0);
        goto fail;
    }
    if (finish(d, &rf, idx, pp->StatusReportFeedbackNumber, w, h, out, err, errlen))
        goto fail;
    return 0;

fail:
    g_stats.errors++;
    return -1;
}

int vdec_vp9_decode(vdec *d, const V383_DXVA_PicParams_VP9 *pp, const uint8_t *frame,
                    unsigned frame_len, vdec_picture *out, char *err, size_t errlen)
{
    V383_Vp9Input bi;
    V383_RegFile rf;
    unsigned idx, i, need;
    int rc;

    if (check_codec(d, VDEC_CODEC_VP9, err, errlen))
        goto fail;
    if (!pp || !frame || !frame_len) {
        set_err(err, errlen, "nothing to decode", 0, 0);
        goto fail;
    }
    if (pp->BitDepthMinus8Luma || pp->BitDepthMinus8Chroma || !V383_VP9_SUBSAMPLING_X(pp) ||
        !V383_VP9_SUBSAMPLING_Y(pp)) {
        set_err(err, errlen, "VP9 profile 0 only (bit depth %u, profile %u)", pp->BitDepthMinus8Luma + 8u,
                pp->profile);
        goto fail;
    }
    if (pp->width > d->max_w || pp->height > d->max_h || !pp->width || !pp->height) {
        set_err(err, errlen, "frame %ux%u does not fit the decoder", pp->width, pp->height);
        goto fail;
    }
    idx = V383_PIC_INDEX(pp->CurrPic);
    if (ensure_surface(d, idx, err, errlen))
        goto fail;
    for (i = 0; i < 3; i++)
        if (!V383_VP9_NO_SURFACE(pp->frame_refs[i]) &&
            ensure_surface(d, V383_PIC_INDEX(pp->frame_refs[i]), err, errlen))
            goto fail;
    for (i = 0; i < 8; i++)
        if (!V383_VP9_NO_SURFACE(pp->ref_frame_map[i]) &&
            ensure_surface(d, V383_PIC_INDEX(pp->ref_frame_map[i]), err, errlen))
            goto fail;

    need = (pp->uncompressed_header_size_byte_aligned & ~15u) + v383_vp9_stream_read_len(frame_len) + 16;
    if (stream_room(d, need, err, errlen))
        goto fail;
    memcpy(d->strm.cpu, frame, frame_len);

    memset(&bi, 0, sizeof(bi));
    bi.pp = pp;
    bi.state = &d->vp9;
    bi.options = d->opt.vp9_options;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = frame_len;
    bi.aux_cpu = d->aux.cpu;
    bi.aux_dma = d->aux.dma;
    bi.rcb_dma = d->rcb.dma;
    bi.colmv_dma[0] = d->colmv[0].dma;
    bi.colmv_dma[1] = d->colmv[1].dma;
    bi.surfaces = d->vs;
    bi.num_surfaces = VDEC_MAX_SURFACES;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;

    rc = v383_vp9_build(&bi, &rf);
    if (rc) {
        set_err(err, errlen, "register builder refused the frame (%d)", (unsigned)rc, 0);
        goto fail;
    }
    if (finish(d, &rf, idx, pp->StatusReportFeedbackNumber, pp->width, pp->height, out, err, errlen))
        goto fail;
    return 0;

fail:
    g_stats.errors++;
    return -1;
}

int vdec_av1_decode(vdec *d, const V383_DXVA_PicParams_AV1 *pp, const V383_DXVA_Tile_AV1 *tiles,
                    unsigned num_tiles, const uint8_t *data, unsigned data_len, vdec_picture *out,
                    char *err, size_t errlen)
{
    V383_Av1Input bi;
    V383_RegFile rf;
    unsigned idx, i, need, len, sum = 0, hdr = d && d->opt.av1_synth ? 16 : 0, rcb;
    int rc;

    if (check_codec(d, VDEC_CODEC_AV1, err, errlen))
        goto fail;
    if (!pp || !tiles || !num_tiles || !data) {
        set_err(err, errlen, "nothing to decode", 0, 0);
        goto fail;
    }
    if (pp->bitdepth != 8 || pp->seq_profile != 0) {
        set_err(err, errlen, "AV1 Main 8 bit only (bit depth %u, profile %u)", pp->bitdepth, pp->seq_profile);
        goto fail;
    }
    if (pp->width > d->max_w || pp->height > d->max_h || !pp->width || !pp->height) {
        set_err(err, errlen, "frame %ux%u does not fit the decoder", pp->width, pp->height);
        goto fail;
    }
    for (i = 0; i < num_tiles; i++) {
        if ((uint64_t)tiles[i].DataOffset + tiles[i].DataSize > data_len || !tiles[i].DataSize) {
            set_err(err, errlen, "tile %u outside the bitstream (%u bytes)", i, data_len);
            goto fail;
        }
        sum += tiles[i].DataSize + 4;
    }
    idx = pp->CurrPicTextureIndex;
    if (idx >= V383_AV1_MAX_SURFACES) {
        set_err(err, errlen, "texture index %u: the AV1 decoder keeps state for %u surfaces", idx,
                V383_AV1_MAX_SURFACES);
        goto fail;
    }
    if (ensure_surface(d, idx, err, errlen))
        goto fail;
    for (i = 0; i < 8; i++)
        if (pp->RefFrameMapTextureIndex[i] < V383_AV1_MAX_SURFACES &&
            ensure_surface(d, pp->RefFrameMapTextureIndex[i], err, errlen))
            goto fail;
    /* frame_refs[i].Index is a slot of RefFrameMapTextureIndex, whose
     * surfaces are all there now. */

    need = v383_av1_stream_read_len(sum + 16 + hdr) + 64;
    if (stream_room(d, need, err, errlen))
        goto fail;
    len = v383_av1_synth_stream(pp, tiles, num_tiles, data, d->opt.av1_synth, hdr, d->strm.cpu, d->strm.size);
    if (!len) {
        set_err(err, errlen, "no room for the stream (%u tiles)", num_tiles, 0);
        goto fail;
    }
    rcb = v383_av1_rcb_size(pp) + 4096;
    if (d->rcb.size < rcb) {
        buf_free(&d->rcb);
        if (buf_alloc_zero(rcb, &d->rcb)) {
            set_err(err, errlen, "row buffer (%u bytes) not allocated", rcb, 0);
            goto fail;
        }
    }

    memset(&bi, 0, sizeof(bi));
    bi.pp = pp;
    bi.extra = NULL;
    bi.state = &d->av1;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = len;
    bi.tile_size_bytes = 4;
    bi.frame_header_bits = hdr * 8;
    bi.aux_cpu = d->aux.cpu;
    bi.aux_dma = d->aux.dma;
    bi.rcb_dma = d->rcb.dma;
    bi.surfaces = d->as;
    bi.num_surfaces = V383_AV1_MAX_SURFACES;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;

    rc = v383_av1_build(&bi, &rf);
    if (rc) {
        set_err(err, errlen, "register builder refused the frame (%d)", (unsigned)rc, 0);
        goto fail;
    }
    if (finish(d, &rf, idx, pp->StatusReportFeedbackNumber, pp->width, pp->height, out, err, errlen))
        goto fail;
    return 0;

fail:
    g_stats.errors++;
    return -1;
}

void vdec_get_stats(vdec_stats *s)
{
    *s = g_stats;
    s->driver_runs = 0;
#ifdef _WIN32
    if (g_state == 1 && !g_fake) {
        VDPU383_INFO info;
        DWORD got = 0;
        memset(&info, 0, sizeof(info));
        if (DeviceIoControl(g_dev, IOCTL_VDPU383_GET_INFO, NULL, 0, &info, sizeof(info), &got, NULL)) {
            s->driver_runs = info.runs;
            if (got >= offsetof(VDPU383_INFO, iommu) + sizeof(info.iommu))
                s->iommu = info.iommu;
        }
    }
#endif
}
