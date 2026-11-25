/*
 * codecs.h - vdectest for HEVC, VP9 and AV1 (codecs.cpp).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDECTEST_CODECS_H
#define VDECTEST_CODECS_H

#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

enum { CODEC_H264, CODEC_HEVC, CODEC_VP9, CODEC_AV1 };

struct CodecOptions {
   int fake;                   /* check the fake decoder's pattern */
   unsigned max_pictures;
   const char *ref;            /* I420 reference, frames as vdpu383test writes them */
   const char *out;            /* write the frames there */
};

/* CODEC_*, from the stream (IVF fourcc, then the first NAL unit). */
int codec_kind(const uint8_t *data, size_t len);

/* The name of a decoder profile GUID, "" when unknown. */
const char *codec_profile_name(const GUID &g);

/* Decode an HEVC, VP9 or AV1 stream; vdectest's exit codes. */
int codec_run(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *vdev, ID3D11VideoContext *vctx,
              int kind, const uint8_t *data, size_t len, const CodecOptions *opt);

#endif
