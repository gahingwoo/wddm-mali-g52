/*
 * chromeprobe.h - Chromium's D3D11VideoDecoder requirements, probed (chromeprobe.cpp).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHROMEPROBE_H
#define CHROMEPROBE_H

#include <d3d11.h>

/* 0: everything is there; 4: something is missing (printed). */
int chrome_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx);

#endif
