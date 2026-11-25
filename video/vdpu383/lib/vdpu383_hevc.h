/*
 * vdpu383_hevc.h - DXVA HEVC picture -> VDPU383 register file
 *
 * The HEVC counterpart of vdpu383_h264.h, under the same rules: no KMDF,
 * WDM, Win32 or C library calls, no allocation, no floating point.
 *
 * Input is what the D3D11 video runtime hands a decoder for one picture:
 * DXVA_PicParams_HEVC, DXVA_Qmatrix_HEVC and the bitstream buffer (the
 * picture's slice segment NAL units, each behind a 00 00 01 start code),
 * plus the bus addresses of the surfaces and two scratch buffers. Output
 * is the decoder's register file and the contents of the auxiliary buffer.
 *
 * Where the register values come from: Rockchip MPP (Apache-2.0 OR MIT),
 * mpp/hal/rkdec/h265d/hal_h265d_vdpu383.c and hal_h265d_com.c, checked
 * field by field against mainline Linux's rkvdec-vdpu383-hevc.c.
 *
 * The one thing the decoder needs that DXVA does not carry: the short-term
 * reference picture sets of the SPS (and its long-term reference pictures).
 * The VDPU383 parses each slice header itself, and a slice names an SPS set
 * by index or predicts its own set from one. MPP hands its HAL the SPS's
 * sets in a private extension of the structure; mainline gets them from
 * the V4L2 EXT_SPS_ST_RPS control. A parser that has the SPS gives them
 * here as V383_HevcExtra. Without them (a D3D11 driver), the builder
 * writes a table that makes every slice of this picture derive the
 * picture's reference sets as DXVA states them (v383_hevc_rebuild_rps; see
 * vdpu383_hevc.c), and v383_hevc_check_rps() says whether it does.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_HEVC_H
#define VDPU383_HEVC_H

#include "vdpu383_h264.h"       /* V383_RegFile, V383_Surface, V383_OK, register numbers */
#include "v383_dxva_hevc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The auxiliary buffer: 64 KiB, 4 KiB aligned, below 4 GiB, device
 * readable; rewritten for every picture.
 */
#define V383_HEVC_AUX_GBL_OFF       0x0000      /* SPS/PPS record */
#define V383_HEVC_GBL_BYTES         176         /* 22 x 64 bits */
#define V383_HEVC_GBL_LEN           12          /* register 67, 128-bit units (MPP's 0xc) */
#define V383_HEVC_AUX_RPS_OFF       0x1000
#define V383_HEVC_RPS_BYTES         3200        /* 32 x 32 bits long-term, 64 x 384 bits short-term */
#define V383_HEVC_AUX_SCALING_OFF   0x2000
#define V383_HEVC_SCALING_BYTES     1360        /* 170 x 64 bits */
#define V383_HEVC_AUX_CABAC_OFF     0x3000
#define V383_HEVC_CABAC_BYTES       27456
#define V383_HEVC_AUX_SIZE          0x10000

/* One short-term reference picture set, as the decoder's table holds it:
 * s0 closest first (decreasing), s1 closest first (increasing). */
typedef struct V383_HevcStRps {
    unsigned char num_negative, num_positive;
    short         delta_poc_s0[16], delta_poc_s1[16];
    unsigned char used_s0[16], used_s1[16];
} V383_HevcStRps;

/* What the decoder's RPS table holds: the SPS's sets and long-term
 * pictures (st_ref_pic_set(0..num_st-1), lt_ref_pic_poc_lsb_sps[]). */
typedef struct V383_HevcRpsTable {
    unsigned int   num_st;
    V383_HevcStRps st[64];
    unsigned int   num_lt;
    unsigned short lt_poc_lsb[32];
    unsigned char  lt_used[32];
} V383_HevcRpsTable;

/* Optional: what an HEVC parser knows and DXVA_PicParams_HEVC does not. */
typedef struct V383_HevcExtra {
    unsigned char valid;                /* 0: DXVA only */
    unsigned char vps_id, sps_id, pps_id;
    V383_HevcRpsTable rps;
} V383_HevcExtra;

typedef struct V383_HevcInput {
    const V383_DXVA_PicParams_HEVC *pp;
    const V383_DXVA_Qmatrix_HEVC   *qm;         /* used when scaling_list_enabled_flag */
    const V383_HevcExtra           *extra;      /* NULL: DXVA only */

    /*
     * Bitstream: CPU view (the slice headers are read) and bus address.
     * stream_len is the bytes of data; the decoder is told to read
     * v383_hevc_stream_read_len(stream_len) bytes, which the caller
     * provides, zero past stream_len.
     */
    const unsigned char *stream_cpu;
    unsigned int  stream_dma;
    unsigned int  stream_len;

    unsigned char *aux_cpu;             /* V383_HEVC_AUX_SIZE */
    unsigned int  aux_dma;
    unsigned int  rcb_dma;              /* v383_hevc_rcb_size() bytes */

    /* Surfaces, indexed by DXVA Index7Bits (CurrPic and RefPicList). */
    const V383_Surface *surfaces;
    unsigned int  num_surfaces;
    unsigned int  hor_stride;           /* bytes, multiple of 16 */
    unsigned int  ver_stride;           /* lines, multiple of 8 */

    /* DXVA only (extra NULL): room for the rebuilt table, ~6.8 KiB, kept
     * off the stack for a kernel-mode caller. */
    V383_HevcRpsTable *rebuild_scratch;
} V383_HevcInput;

/* Picture size in luma samples, from the DXVA parameters. */
unsigned int v383_hevc_width(const V383_DXVA_PicParams_HEVC *pp);
unsigned int v383_hevc_height(const V383_DXVA_PicParams_HEVC *pp);

/* Bytes the decoder reads for stream_len bytes of data (MPP: to 16, + 64). */
unsigned int v383_hevc_stream_read_len(unsigned int stream_len);

/* Row buffer bytes for a picture of this size. */
unsigned int v383_hevc_rcb_size(unsigned int width, unsigned int height);

/* Co-located MV buffer bytes per surface (MPP, twice its per-picture size). */
unsigned int v383_hevc_colmv_size(unsigned int width, unsigned int height, unsigned int ctb_size);

/* slice_pic_parameter_set_id of the first slice in the buffer, or -1. */
int v383_hevc_parse_pps_id(const V383_DXVA_PicParams_HEVC *pp, const unsigned char *stream,
                           unsigned int len);

/*
 * The table for this picture from DXVA alone (see vdpu383_hevc.c). 0, or
 * V383_E_UNSUPPORTED when no table makes the slices derive the stated
 * sets, V383_E_STREAM when a slice header cannot be read.
 */
int v383_hevc_rebuild_rps(const V383_DXVA_PicParams_HEVC *pp, const unsigned char *stream,
                          unsigned int len, V383_HevcRpsTable *out);

/*
 * The reference sets each slice of the picture derives with this table, as
 * the decoder derives them from its slice header, compared with
 * RefPicSetStCurrBefore, RefPicSetStCurrAfter and RefPicSetLtCurr. 0 when
 * every slice agrees; else nonzero, with the first difference in why.
 */
int v383_hevc_check_rps(const V383_DXVA_PicParams_HEVC *pp, const unsigned char *stream,
                        unsigned int len, const V383_HevcRpsTable *t, char *why, unsigned int why_len);

/* Build the register file and fill the auxiliary buffer. */
int v383_hevc_build(const V383_HevcInput *in, V383_RegFile *out);

extern const unsigned char v383_hevc_cabac_table[V383_HEVC_CABAC_BYTES];

#ifdef __cplusplus
}
#endif

#endif /* VDPU383_HEVC_H */
