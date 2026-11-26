/*
 * vptest.h - the video processor and NV12 plane view tests (vptest.cpp).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VPTEST_H
#define VPTEST_H

#include <d3d11.h>
#include <stdint.h>

/* 0 every check matched, 2 a mismatch, 1 setup failed. */
int vp_run_tests(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *vdev,
                 ID3D11VideoContext *vctx);
int vp_check_decoded(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *vdev,
                     ID3D11VideoContext *vctx, ID3D11Texture2D *tex, UINT slice, UINT w, UINT h,
                     uint8_t luma, uint8_t cb, uint8_t cr);

/* A slice of the decoder's texture array as R8_UNORM and R8G8_UNORM views
 * (0 made, 1 refused), and read through them (slice 0 of the views): 0
 * every pixel of the w x h area at x0,y0 has the luma, Cb and Cr of the
 * I420 frame i420 (w x h, the U plane then the V plane), 2 not, 1 setup.
 * Any number of devices. */
int vp_make_plane_views(ID3D11Device *dev, ID3D11Texture2D *tex, UINT slice,
                        ID3D11ShaderResourceView *srv[2]);
int vp_check_plane_views(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *srv[2],
                         UINT slice, UINT tex_w, UINT tex_h, UINT x0, UINT y0, UINT w, UINT h,
                         const uint8_t *i420);

/* The fake decoder's chroma (video/vdec.h vdec_fake_chroma, _v): Cb and
 * Cr differ, so a swapped or dropped channel shows. */
static inline uint8_t vdec_fake_cb(unsigned index) { return (uint8_t)(0x80 ^ index); }
static inline uint8_t vdec_fake_cr(unsigned index) { return (uint8_t)(0x40 ^ index); }

#endif
