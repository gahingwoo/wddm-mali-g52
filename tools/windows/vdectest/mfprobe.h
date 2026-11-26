/*
 * mfprobe.h - Media Foundation's H.264 decoder on a D3D11 device (mfprobe.cpp).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef MFPROBE_H
#define MFPROBE_H

#include <d3d11.h>

/* 0: the decoder took the device and a 1080p H.264 / NV12 pair; 4 not. */
int mf_probe(ID3D11Device *dev);

#endif
