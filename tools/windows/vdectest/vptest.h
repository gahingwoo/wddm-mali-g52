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
                     uint8_t luma, uint8_t chroma);

#endif
