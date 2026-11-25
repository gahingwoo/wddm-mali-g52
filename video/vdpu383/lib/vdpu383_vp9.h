/*
 * vdpu383_vp9.h - DXVA VP9 picture -> VDPU383 register file
 *
 * The VP9 counterpart of vdpu383_h264.h and vdpu383_hevc.h, under the same
 * rules: no KMDF, WDM, Win32 or C library calls, no allocation, no floating
 * point.
 *
 * Input is what the D3D11 video runtime hands a decoder for one frame:
 * DXVA_PicParams_VP9 and the bitstream buffer holding the whole frame, plus
 * the bus addresses of the surfaces and scratch buffers. Output is the
 * register file and the contents of the auxiliary buffer.
 *
 * What the decoder does itself, and what it is given:
 *
 * - It reads the compressed header (the probability updates) and the tile
 *   data from the bitstream, starting after the uncompressed header, and
 *   adapts the probabilities at the end of the frame (backward adaptation)
 *   in hardware. No boolean decoder and no symbol counts are needed here:
 *   MPP's VDPU383 HAL runs with HW_PROB 1 and its count callback compiled
 *   out.
 * - It is given the uncompressed header as a 352-byte record (the "global"
 *   buffer, register 131), the probability tables it starts from (register
 *   184: one of the four frame contexts it wrote earlier, or the default
 *   tables), where to write the adapted context (register 185), the
 *   key-frame mode tables (register 183), the previous frame's segment map
 *   and motion vectors.
 *
 * The decoder's memory between frames is V383_Vp9State, which the caller
 * keeps per stream (a D3D11 driver: per decoder object), and the frame
 * contexts and segment maps in the auxiliary buffer, which must therefore
 * also be kept per stream and not be shared between decoders.
 *
 * Where the layouts come from: Rockchip MPP, mpp/hal/rkdec/vp9d/
 * hal_vp9d_vdpu383.c (Apache-2.0 OR MIT, used here under MIT) for the
 * registers and buffer handling, and hal_vp9d_com.c (Apache-2.0) as the
 * reference for the bit layout of the global record and the probability
 * tables, which are written fresh here (vdpu383_vp9.c). See vdpu383_vp9.c
 * for where this differs from MPP.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_VP9_H
#define VDPU383_VP9_H

#include "vdpu383_h264.h"       /* V383_RegFile, V383_Surface, V383_OK, register numbers */
#include "v383_dxva_vp9.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The auxiliary buffer: V383_VP9_AUX_SIZE bytes, 4 KiB aligned, below
 * 4 GiB, device readable and writable, zeroed before the first frame and
 * kept for the whole stream (the decoder writes the frame contexts and the
 * segment maps into it).
 */
#define V383_VP9_AUX_GBL_OFF        0x00000     /* global record, rewritten per frame */
#define V383_VP9_GBL_BYTES          352
#define V383_VP9_GBL_LEN            22          /* register 67, 128-bit units */
#define V383_VP9_AUX_KFPROB_OFF     0x01000     /* key-frame mode tables, constant */
#define V383_VP9_KFPROB_BYTES       1312
#define V383_VP9_AUX_DEFPROB_OFF    0x02000     /* default probabilities, constant */
#define V383_VP9_PROB_BYTES         4864
#define V383_VP9_AUX_SCRATCH_OFF    0x04000     /* adapted context of a frame that keeps none */
#define V383_VP9_AUX_CTX_OFF(i)     (0x06000 + (i) * 0x2000)   /* frame contexts 0..3 */
#define V383_VP9_AUX_SEGID_OFF(i)   (0x10000 + (i) * 0x48000)  /* segment maps 0, 1 */
#define V383_VP9_SEGID_BYTES        (73728 * 4)
#define V383_VP9_AUX_SIZE           0xA0000

/* The VP9 probability tables, the fields of struct v4l2_vp9_frame_context
 * in its order (and the specification's): contexts and modes as libvpx
 * numbers them (DC_PRED first; partition context 0 is 8x8). */
typedef struct V383_Vp9MvProbs {
    unsigned char joint[3];
    unsigned char sign[2];
    unsigned char classes[2][10];
    unsigned char class0_bit[2];
    unsigned char bits[2][10];
    unsigned char class0_fr[2][2][3];
    unsigned char fr[2][3];
    unsigned char class0_hp[2];
    unsigned char hp[2];
} V383_Vp9MvProbs;

typedef struct V383_Vp9Probs {
    unsigned char tx8[2][1];
    unsigned char tx16[2][2];
    unsigned char tx32[2][3];
    unsigned char coef[4][2][2][6][6][3];
    unsigned char skip[3];
    unsigned char inter_mode[7][3];
    unsigned char interp_filter[4][2];
    unsigned char is_inter[4];
    unsigned char comp_mode[5];
    unsigned char single_ref[5][2];
    unsigned char comp_ref[5];
    unsigned char y_mode[4][9];
    unsigned char uv_mode[10][9];
    unsigned char partition[16][3];
    V383_Vp9MvProbs mv;
} V383_Vp9Probs;

extern const V383_Vp9Probs v383_vp9_default_probs;
extern const unsigned char v383_vp9_kf_y_mode_prob[10][10][9];
extern const unsigned char v383_vp9_kf_partition_probs[16][3];
extern const unsigned char v383_vp9_kf_uv_mode_prob[10][9];

/*
 * What the decoder remembers between frames. v383_vp9_reset() before the
 * first frame (and after a flush or seek, from a key frame on);
 * v383_vp9_build() reads it and leaves it as the next frame needs it, so
 * call build exactly once per frame submitted to the decoder, in decode
 * order (show_existing_frame frames are not submitted and do not count).
 */
typedef struct V383_Vp9State {
    unsigned char  ctx_valid[4];        /* the decoder wrote context i (else: defaults) */
    unsigned char  segid_flag;          /* which segment map is the previous one (MPP last_segid_flag) */
    unsigned char  colmv_last;          /* which colmv buffer the previous frame wrote */
    unsigned char  tables_written;      /* constant tables are in the auxiliary buffer */
    unsigned char  frames;              /* frames built since the reset, saturating */
    unsigned char  last_show_frame;
    unsigned char  last_intra_only;     /* key frame or intra-only */
    unsigned char  last_frame_type;
    unsigned char  last_seg_enabled;    /* MPP: set once segmentation was on, never cleared */
    unsigned char  segid_zero;          /* bit i: segment map i is known to be all zero */
    unsigned int   last_width, last_height;
    signed char    last_ref_deltas[4];
    signed char    last_mode_deltas[2];
} V383_Vp9State;

/* Choices for which MPP and the VP9 specification differ (vdpu383_vp9.c). */
#define V383_VP9_OPT_MPP_INTRA_RESET    0x01    /* every intra frame resets all four contexts */
#define V383_VP9_OPT_MPP_UPDATE_BUF     0x02    /* adapted context written to context
                                                 * frame_context_idx even when it is not kept */

typedef struct V383_Vp9Input {
    const V383_DXVA_PicParams_VP9 *pp;
    V383_Vp9State *state;
    unsigned int   options;             /* V383_VP9_OPT_* */

    /* The frame: bus address of its first byte (the uncompressed header)
     * and its length. The decoder reads v383_vp9_stream_read_len(len)
     * bytes from the compressed header on, which the caller provides, zero
     * past stream_len. */
    unsigned int   stream_dma;
    unsigned int   stream_len;

    unsigned char *aux_cpu;             /* V383_VP9_AUX_SIZE, kept between frames */
    unsigned int   aux_dma;
    unsigned int   rcb_dma;             /* v383_vp9_rcb_size() bytes */
    unsigned int   colmv_dma[2];        /* two of v383_vp9_colmv_size() bytes, kept between frames */

    /* Surfaces, indexed by DXVA Index7Bits (CurrPic, ref_frame_map,
     * frame_refs), all with the same strides (colmv_dma unused). Luma
     * then chroma; 8 bit: NV12; 10 bit: the decoder's packed 10-bit
     * layout (4 samples in 5 bytes, least significant first). */
    const V383_Surface *surfaces;
    unsigned int   num_surfaces;
    unsigned int   hor_stride;          /* bytes, multiple of 16 */
    unsigned int   ver_stride;          /* lines, multiple of 8 */
} V383_Vp9Input;

void v383_vp9_reset(V383_Vp9State *s);

/* Bytes the decoder reads, from the start of the compressed header, for a
 * frame of stream_len bytes (MPP: to 16, + 128). The buffer must hold
 * uncompressed_header_size_byte_aligned rounded down to 16 more. */
unsigned int v383_vp9_stream_read_len(unsigned int stream_len);

/* Row buffer bytes for a frame of this size. */
unsigned int v383_vp9_rcb_size(unsigned int width, unsigned int height);

/* Bytes of one co-located motion vector buffer (MPP: width * height / 2). */
unsigned int v383_vp9_colmv_size(unsigned int width, unsigned int height);

/* Build the register file, fill the auxiliary buffer and advance the state.
 * V383_E_UNSUPPORTED: not 4:2:0, or more than 10 bits. */
int v383_vp9_build(const V383_Vp9Input *in, V383_RegFile *out);

/* The packers on their own (vdpu383test selftest). */
void v383_vp9_write_gbl(const V383_DXVA_PicParams_VP9 *pp, const V383_Vp9State *s, unsigned char *out);
void v383_vp9_write_probs(const V383_Vp9Probs *p, unsigned char *out);
void v383_vp9_write_kf_probs(unsigned char *out);

#ifdef __cplusplus
}
#endif

#endif /* VDPU383_VP9_H */
