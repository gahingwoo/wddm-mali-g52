/*
 * vdpu383_av1.h - DXVA AV1 picture -> VDPU383 register file
 *
 * The AV1 counterpart of vdpu383_vp9.h, under the same rules: no KMDF, WDM,
 * Win32 or C library calls, no allocation, no floating point.
 *
 * What the decoder does itself, and what it is given:
 *
 * - It reads the OBUs of the frame from the stream buffer (register 128,
 *   start bit 0) and decodes the tiles. It keeps the CDFs in hardware: it
 *   loads them from a reference's CDF buffer or the default tables and
 *   writes the adapted ones into the current surface's CDF buffer, with the
 *   segment map behind them. No CDF arithmetic is done here.
 * - It is given the frame header as a 672-byte record (the "global" buffer,
 *   register 131), with what it needs to know of the eight reference slots
 *   (sizes, frame types, order hints, the order hints of their own
 *   references).
 *
 * DXVA_PicParams_AV1 does not carry the slot numbers of the references, the
 * order hints of the reference frames or their own references, the frame
 * types and coded sizes of the slots, the frame header's size or the tile
 * size field width. The builder keeps per surface what it learns when a
 * frame is decoded into it (V383_Av1State) and finds slot numbers through
 * RefFrameMapTextureIndex. A parser that knows more passes V383_Av1Extra.
 *
 * The stream: with a parser, the frame's OBUs as they are in the file (as
 * MPP hands them over). A D3D11 driver has only the tile data;
 * v383_av1_synth_stream() wraps it in OBUs (two ways; which one the
 * decoder accepts is to be found on the board).
 *
 * Where the layouts come from: Rockchip MPP (Apache-2.0 OR MIT, used here
 * under MIT), mpp/hal/rkdec/av1d/hal_av1d_vdpu383.c and hal_av1d_com.c,
 * and its parser's derivations (mpp/codec/dec/av1). See vdpu383_av1.c.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_AV1_H
#define VDPU383_AV1_H

#include "vdpu383_h264.h"       /* V383_RegFile, V383_OK, register numbers */
#include "v383_dxva_av1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define V383_AV1_MAX_SURFACES       32

#define V383_AV1_NON_COEF_CDF_BYTES (434 * 16)
#define V383_AV1_COEF_CDF_BYTES     (354 * 16)
#define V383_AV1_ALL_CDF_BYTES      (V383_AV1_NON_COEF_CDF_BYTES + 4 * V383_AV1_COEF_CDF_BYTES)
#define V383_AV1_CDF_WORDS          (V383_AV1_ALL_CDF_BYTES / 4)

/* The auxiliary buffer: v383_av1_aux_size() bytes, 4 KiB aligned, below
 * 4 GiB, device readable, zeroed before the first frame. */
#define V383_AV1_AUX_GBL_OFF        0x0000
#define V383_AV1_GBL_BYTES          672         /* 5160 bits, to 128, + 16 bytes */
#define V383_AV1_GBL_LEN            42          /* register 67, 128-bit units */
#define V383_AV1_AUX_DEFCDF_OFF     0x1000      /* default CDFs, then a zero segment map */

extern const unsigned int v383_av1_default_cdf[V383_AV1_CDF_WORDS];

/* One surface: the picture, its co-located motion vectors and its CDF and
 * segment map buffer (written by the decoder, read by later frames). */
typedef struct V383_Av1Surface {
    unsigned int dma;           /* luma; chroma at dma + hor_stride * ver_stride */
    unsigned int colmv_dma;     /* v383_av1_colmv_size() */
    unsigned int cdf_dma;       /* v383_av1_cdf_size() */
} V383_Av1Surface;

/* What the builder remembers of a surface's last frame. */
typedef struct V383_Av1SurfState {
    unsigned char  valid;
    unsigned char  frame_type;
    unsigned char  order_hint;
    unsigned char  ref_hints[7];        /* order hints of its LAST..ALTREF */
    unsigned char  cdf_valid;           /* its CDF buffer holds adapted CDFs */
    unsigned char  coeff_idx;           /* else: the default coefficient class it stands for */
    unsigned short width, upscaled_width, height;
} V383_Av1SurfState;

typedef struct V383_Av1State {
    V383_Av1SurfState surf[V383_AV1_MAX_SURFACES];
    unsigned int frames;
} V383_Av1State;

/* Optional: what an AV1 parser knows and DXVA_PicParams_AV1 does not. */
typedef struct V383_Av1Extra {
    unsigned char  valid;
    unsigned char  ref_frame_idx[7];    /* slots of LAST..ALTREF */
    unsigned char  tile_size_bytes;     /* tile_size_bytes_minus_1 + 1 */
    unsigned char  tile_cols_log2, tile_rows_log2;
    unsigned char  matrix_coefficients;
    unsigned short operating_point_idc;
    unsigned int   frame_header_bits;   /* the uncompressed header's size */
} V383_Av1Extra;

typedef struct V383_Av1Input {
    const V383_DXVA_PicParams_AV1 *pp;
    const V383_Av1Extra *extra;         /* NULL: DXVA only */
    V383_Av1State *state;

    /* the frame's OBUs (v383_av1_synth_stream() for DXVA only) */
    unsigned int   stream_dma;
    unsigned int   stream_len;
    unsigned int   tile_size_bytes;     /* DXVA only: as v383_av1_synth_stream() wrote them */
    unsigned int   frame_header_bits;   /* DXVA only: as v383_av1_synth_stream() wrote them */

    unsigned char *aux_cpu;
    unsigned int   aux_dma;
    unsigned int   rcb_dma;             /* v383_av1_rcb_size() */

    const V383_Av1Surface *surfaces;    /* indexed by DXVA texture index */
    unsigned int   num_surfaces;
    unsigned int   hor_stride, ver_stride;
} V383_Av1Input;

void v383_av1_reset(V383_Av1State *s);

unsigned int v383_av1_aux_size(unsigned int width, unsigned int height);
unsigned int v383_av1_cdf_size(unsigned int width, unsigned int height);
unsigned int v383_av1_colmv_size(unsigned int width, unsigned int height);
unsigned int v383_av1_rcb_size(const V383_DXVA_PicParams_AV1 *pp);
/* Bytes the decoder reads for stream_len bytes (MPP: + 15, to 128). */
unsigned int v383_av1_stream_read_len(unsigned int stream_len);

/*
 * The tile data of a DXVA frame as OBUs, for the decoder to parse:
 * mode 0: one OBU_TILE_GROUP with every tile; mode 1: an OBU_FRAME whose
 * frame header part is frame_header_bytes zero bytes, followed by the tile
 * group. Tile sizes are written in 4 bytes. Returns the length, 0 when
 * out does not have room.
 */
unsigned int v383_av1_synth_stream(const V383_DXVA_PicParams_AV1 *pp, const V383_DXVA_Tile_AV1 *tiles,
                                   unsigned int num_tiles, const unsigned char *data, unsigned int mode,
                                   unsigned int frame_header_bytes, unsigned char *out, unsigned int out_cap);

/* Build the register file and the global record, and advance the state.
 * V383_E_UNSUPPORTED: not 4:2:0, more than 10 bits. */
int v383_av1_build(const V383_Av1Input *in, V383_RegFile *out);

/* The global record on its own: the fields as hal_av1d_com.c's
 * vdpu38x_av1d_uncomp_hdr() takes them (vdpu383test selftest, and a
 * comparison with MPP off the board). */
typedef struct V383_Av1Hdr {
    const V383_DXVA_PicParams_AV1 *pp;
    unsigned int   frame_width, upscaled_width;     /* coded and upscaled */
    unsigned int   coded_denom;                     /* MPP: the 3 coded bits with superres, else 8 */
    unsigned int   operating_point_idc, matrix_coefficients;
    unsigned int   frame_header_bits, tile_size_bytes, tile_cols_log2, tile_rows_log2;
    unsigned int   ref_frame_valued;
    unsigned int   ref_frame_idx[7];
    unsigned int   slot_surface[8];                 /* 0xFF: empty */
    unsigned int   slot_order_hint[8];
    unsigned int   slot_ref_hints[8][7];
    unsigned int   slot_width[8], slot_upscaled_width[8], slot_height[8], slot_frame_type[8];
    unsigned int   skip_ref0, skip_ref1;
    unsigned int   coded_lossless, all_lossless, last_active, preskip, using_qmatrix;
    int            wmmat_val[7][6];
} V383_Av1Hdr;

void v383_av1_derive(const V383_DXVA_PicParams_AV1 *pp, const V383_Av1Extra *extra, const V383_Av1State *s,
                     V383_Av1Hdr *h);
void v383_av1_write_gbl(const V383_Av1Hdr *h, unsigned char *out);

#ifdef __cplusplus
}
#endif

#endif /* VDPU383_AV1_H */
