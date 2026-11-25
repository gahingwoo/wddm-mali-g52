/*
 * vdec.c - DXVA H.264 pictures on the RK3576 VDPU383, for the user-mode
 * driver (vdec.h says how and why).
 *
 * The decode sequence is vdpu383test's (woa-rk3576 tools/vdpu383test,
 * main.c decode_cb), which matched the FFmpeg references bit for bit on
 * the TaishanPi 3M on 2026-11-24 in its --dxva-only mode: the same
 * surface layout (luma stride 64-aligned, chroma right after the luma,
 * co-located motion vectors at twice the luma size), the same buffers,
 * the same run flags. What differs: the picture parameters come from the
 * application through DXVA instead of from vdpu383test's parser, so the
 * picture parameter set id is read from the first slice header and the
 * profile is derived (V383_H264Extra absent), and the scaling matrix is
 * taken as present whenever the DXVA matrix is not flat.
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

#define VDEC_MAX_SURFACES   128         /* DXVA Index7Bits */
#define VDEC_MIN_STREAM     (4u << 20)

typedef struct vdec_buf {
    unsigned handle;
    uint32_t dma;
    uint32_t size;
    uint8_t *cpu;
} vdec_buf;

struct vdec_h264 {
    unsigned max_w, max_h;
    unsigned width, height, hor, ver;
    unsigned frame_mbs_only;
    unsigned colmv_off, surf_size;
    vdec_buf surf[VDEC_MAX_SURFACES];
    V383_Surface vs[VDEC_MAX_SURFACES];
    vdec_buf aux, rcb, strm;
    VDPU383_RUN_IN *run_in;
    VDPU383_RUN_OUT *run_out;
};

static int g_state;                     /* 0 not tried, 1 available, -1 not */
static int g_fake;
static uint32_t g_fake_dma = 0x10000000u;
static vdec_stats g_stats;
#ifdef _WIN32
static HANDLE g_dev = INVALID_HANDLE_VALUE;
static SRWLOCK g_lock = SRWLOCK_INIT;
#endif

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

static void free_surfaces(vdec_h264 *d)
{
    unsigned i;

    for (i = 0; i < VDEC_MAX_SURFACES; i++) {
        buf_free(&d->surf[i]);
        d->vs[i].dma = 0;
        d->vs[i].colmv_dma = 0;
    }
    buf_free(&d->rcb);
    d->width = d->height = 0;
}

/* The geometry of this picture; a change drops every surface. */
static int setup_geometry(vdec_h264 *d, const V383_DXVA_PicParams_H264 *pp, char *err, size_t errlen)
{
    unsigned w = (pp->wFrameWidthInMbsMinus1 + 1u) * 16u;
    unsigned h = (pp->wFrameHeightInMbsMinus1 + 1u) * 16u;
    unsigned fmo = V383_PP_GET(pp, V383_PP_FRAME_MBS_ONLY, 1);
    unsigned colmv;

    if (d->width == w && d->height == h && d->frame_mbs_only == fmo && d->rcb.handle)
        return 0;
    if (w > d->max_w || h > d->max_h) {
        set_err(err, errlen, "picture %ux%u is larger than the decoder", w, h);
        return -1;
    }
    free_surfaces(d);
    d->width = w;
    d->height = h;
    d->frame_mbs_only = fmo;
    d->hor = align_up(w, 64);
    d->ver = align_up(h, 16);
    colmv = v383_h264_colmv_size(w, h, fmo);
    /* As vdpu383test (MPP sizes frame buffers for 4:2:2). */
    d->colmv_off = align_up(d->hor * d->ver * 2, 4096);
    d->surf_size = d->colmv_off + align_up(colmv, 4096);
    if (buf_alloc(v383_h264_rcb_size(w, h, 1) + 4096, &d->rcb)) {
        set_err(err, errlen, "row buffer (%u bytes) not allocated", v383_h264_rcb_size(w, h, 1), 0);
        return -1;
    }
    memset(d->rcb.cpu, 0, d->rcb.size);
    return 0;
}

static int ensure_surface(vdec_h264 *d, unsigned i, char *err, size_t errlen)
{
    if (d->surf[i].handle)
        return 0;
    if (buf_alloc(d->surf_size, &d->surf[i])) {
        set_err(err, errlen, "surface %u (%u bytes) not allocated", i, d->surf_size);
        return -1;
    }
    memset(d->surf[i].cpu, 0, d->surf_size);
    d->vs[i].dma = d->surf[i].dma;
    d->vs[i].colmv_dma = d->surf[i].dma + d->colmv_off;
    return 0;
}

/* ----------------------------------------------------------- decoder */

vdec_h264 *vdec_h264_create(unsigned width, unsigned height, char *err, size_t errlen)
{
    vdec_h264 *d;

    if (!vdec_available()) {
        set_err(err, errlen, "no VDPU383 device (\\\\.\\VDPU383 did not open)", 0, 0);
        return NULL;
    }
    d = (vdec_h264 *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->max_w = align_up(width ? width : 16, 16);
    d->max_h = align_up(height ? height : 16, 16);
    d->run_in = (VDPU383_RUN_IN *)calloc(1, sizeof(*d->run_in));
    d->run_out = (VDPU383_RUN_OUT *)calloc(1, sizeof(*d->run_out));
    if (!d->run_in || !d->run_out || buf_alloc(V383_H264_AUX_SIZE, &d->aux)) {
        set_err(err, errlen, "decoder buffers not allocated", 0, 0);
        vdec_h264_destroy(d);
        return NULL;
    }
    return d;
}

void vdec_h264_destroy(vdec_h264 *d)
{
    if (!d)
        return;
    free_surfaces(d);
    buf_free(&d->aux);
    buf_free(&d->strm);
    free(d->run_in);
    free(d->run_out);
    free(d);
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

static int run(vdec_h264 *d, const V383_RegFile *rf, char *err, size_t errlen)
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
        else
            set_err(err, errlen, "decode result %u, status 0x%08x", out->result, out->status);
        return -1;
    }
    return 0;
}

int vdec_h264_decode(vdec_h264 *d, const V383_DXVA_PicParams_H264 *pp,
                     const V383_DXVA_Qmatrix_H264 *qm, const uint8_t *bitstream,
                     unsigned bitstream_len, vdec_picture *out, char *err, size_t errlen)
{
    V383_H264Input bi;
    V383_RegFile rf;
    unsigned idx, i, need, rc;

    if (!d || !pp || !bitstream || !bitstream_len) {
        set_err(err, errlen, "nothing to decode", 0, 0);
        goto fail;
    }
    if (pp->bit_depth_luma_minus8 || pp->bit_depth_chroma_minus8 ||
        V383_PP_GET(pp, V383_PP_CHROMA_FORMAT_IDC, 2) != 1) {
        set_err(err, errlen, "only 8-bit 4:2:0 (bit depth %u, chroma format %u)",
                pp->bit_depth_luma_minus8 + 8u, V383_PP_GET(pp, V383_PP_CHROMA_FORMAT_IDC, 2));
        goto fail;
    }
    if (setup_geometry(d, pp, err, errlen))
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
    if (need > d->strm.size) {
        buf_free(&d->strm);
        if (buf_alloc(need > VDEC_MIN_STREAM ? need : VDEC_MIN_STREAM, &d->strm)) {
            set_err(err, errlen, "bitstream buffer (%u bytes) not allocated", need, 0);
            goto fail;
        }
    }
    memcpy(d->strm.cpu, bitstream, bitstream_len);
    memset(d->strm.cpu + bitstream_len, 0, need - bitstream_len);

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

    if (g_fake) {
        uint8_t *s = d->surf[idx].cpu;
        size_t luma = (size_t)d->hor * d->ver;
        memset(s, vdec_fake_luma(pp->StatusReportFeedbackNumber), luma);
        memset(s + luma, vdec_fake_chroma(idx), luma / 2);
    } else if (run(d, &rf, err, errlen)) {
        goto fail;
    }

    g_stats.pictures++;
    out->data = d->surf[idx].cpu;
    out->stride = d->hor;
    out->luma_rows = d->ver;
    out->width = d->width;
    out->height = d->height;
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
        if (DeviceIoControl(g_dev, IOCTL_VDPU383_GET_INFO, NULL, 0, &info, sizeof(info), &got, NULL))
            s->driver_runs = info.runs;
    }
#endif
}
