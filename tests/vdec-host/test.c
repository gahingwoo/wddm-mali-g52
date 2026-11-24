/*
 * The UMD's video backend (video/vdec.c) on the host, with the fake
 * decoder: every picture of an H.264 stream, as vdpu383test's parser turns
 * it into DXVA, goes through vdec_h264_decode, which builds the VDPU383
 * register file from the DXVA parameters alone (what the UMD has) and
 * writes its pattern. Checks: every picture accepted, the register
 * builder's stream and surface addresses inside the backend's buffers, the
 * pattern where the output callback expects it. `make run`.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tools/windows/vdectest/h264parse.h"
#include "../../video/vdec.h"

static vdec_h264 *dec;
static unsigned status_of[H264_MAX_SLOTS];
static const uint8_t *last_data[H264_MAX_SLOTS];
static vdec_picture last_pic[H264_MAX_SLOTS];
static unsigned pictures, outputs, failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int decode_cb(void *ctx, const H264Picture *pic)
{
    V383_DXVA_PicParams_H264 pp = pic->pp;
    unsigned slot = V383_PIC_INDEX(pp.CurrPic);
    vdec_picture out;
    char err[160] = "";
    (void)ctx;

    if (!dec) {
        dec = vdec_h264_create(pic->width, pic->height, err, sizeof err);
        CHECK(dec != NULL);
        if (!dec) {
            printf("create: %s\n", err);
            return -1;
        }
    }
    pp.StatusReportFeedbackNumber = pic->decode_index + 1;
    if (vdec_h264_decode(dec, &pp, &pic->qm, pic->stream, (unsigned)pic->stream_len, &out, err, sizeof err)) {
        printf("pic %u: %s\n", pic->decode_index, err);
        failures++;
        return -1;
    }
    CHECK(out.width == pic->width && out.height == pic->height);
    CHECK(out.stride >= out.width && out.stride % 64 == 0);
    CHECK(out.luma_rows >= out.height);
    status_of[slot] = pp.StatusReportFeedbackNumber;
    last_pic[slot] = out;
    last_data[slot] = out.data;
    pictures++;
    return 0;
}

static void output_cb(void *ctx, int slot, unsigned decode_index, int poc)
{
    const vdec_picture *p = &last_pic[slot];
    size_t luma = (size_t)p->stride * p->luma_rows, i;
    unsigned bad = 0;
    (void)ctx; (void)decode_index; (void)poc;

    for (i = 0; i < luma; i++)
        bad += last_data[slot][i] != vdec_fake_luma(status_of[slot]);
    for (i = 0; i < luma / 2; i++)
        bad += last_data[slot][luma + i] != vdec_fake_chroma((unsigned)slot);
    CHECK(bad == 0);
    outputs++;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "../data/testsrc-176x144-high.264";
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;
    H264Parser *p;
    vdec_stats st;

    setenv("D3D10UMD_VDEC_FAKE", "1", 1);
    if (!f) {
        printf("cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n)
        return 1;
    fclose(f);

    CHECK(vdec_available());
    p = calloc(1, sizeof(*p));
    h264_init(p, output_cb, NULL);
    CHECK(h264_run(p, data, (size_t)n, 0, decode_cb, NULL) == 0);
    h264_free(p);
    free(p);
    vdec_get_stats(&st);
    CHECK(st.fake == 1);
    CHECK(st.pictures == pictures && st.errors == 0);
    CHECK(pictures == 12 && outputs == 12);

    /* A picture larger than the decoder was made for is refused. */
    {
        V383_DXVA_PicParams_H264 big;
        vdec_picture out;
        char err[160];
        memset(&big, 0, sizeof(big));
        big.wFrameWidthInMbsMinus1 = 200;
        big.wFrameHeightInMbsMinus1 = 200;
        V383_PP_SET(&big, V383_PP_CHROMA_FORMAT_IDC, 2, 1);
        CHECK(vdec_h264_decode(dec, &big, NULL, data, 16, &out, err, sizeof err) == -1);
    }
    vdec_h264_destroy(dec);
    free(data);
    printf("%u pictures, %u frames out, %u failures\n", pictures, outputs, failures);
    return failures ? 1 : 0;
}
