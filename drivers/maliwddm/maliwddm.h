/* SPDX-License-Identifier: GPL-2.0 */
/*
 * maliwddm: the WDDM 1.3 driver for the RK3576's Mali-G52 and its display
 * (milestone M5.2, docs/M5.2-PLAN.md). The display half is malidod's; the
 * GPU half is drivers/common/maligpu; this layer is what WDDM asks of a full
 * driver: allocations in one aperture segment mapped into the Mali MMU, one
 * engine node, submissions of job chains with fences.
 *
 * Shape and several WDDM details follow virtio-win's viogpu3d (BSD-3), the
 * one open WDDM driver with a Mesa d3d10umd user-mode side.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
#include "../common/maligpu.h"
#include "maliwddm_abi.h"

#define MW_TAG                  'dwlM'
#define MW_SEGMENT_ID           1           /* the aperture segment */
#define MW_SEGMENT_SIZE         (1024ULL * 1024 * 1024)
#define MW_QUEUE_DEPTH          16          /* submissions the KMD holds */

/* 1: a render-only adapter (no VidPN sources or children; another device's
 * display-only driver keeps the screen). 0: a full adapter, display and
 * render on GPU0.
 *
 * Render-only is the WDDM pairing for a separate display device, but on
 * this ACPI GPU it never got past StartDevice: dxgkrnl reported the adapter
 * as a POST device (adapter type 0x9) and refused it with
 * STATUS_GRAPHICS_INVALID_DRIVER_MODEL whatever the DSDT order, resources,
 * INF class or caps (WinPE runs 5-11). A full adapter started (run 6 on the
 * installed Windows); it was black there only because DWM cannot render
 * through WARP, and WinPE has no DWM. */
#ifndef MW_RENDER_ONLY
#define MW_RENDER_ONLY          0
#endif