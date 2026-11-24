/*
 * vdpu383_h264.h - DXVA H.264 picture -> VDPU383 register file
 *
 * A self-contained C library: no KMDF, WDM, Win32 or C library calls, no
 * allocation, no floating point. It can be compiled into a kernel-mode
 * driver (the standalone vdpu383.sys today; the VIDEO_DECODE node of
 * maliwddm later), a user-mode driver or a test program unchanged.
 *
 * Input is what the D3D11 video runtime hands a decoder for one picture:
 * DXVA_PicParams_H264, DXVA_Qmatrix_H264 and the bitstream buffer (the
 * picture's slice NAL units, each behind a 00 00 01 start code). The
 * caller adds the bus addresses of its surfaces and of two scratch buffers.
 * Output is the decoder's register file (registers 0..383 of the function
 * block at 0x27B00100) and the contents of the auxiliary buffer it points
 * at. Starting the decode (link registers) is the caller's job; see
 * V383_LINK_*.
 *
 * Where the register values come from: Rockchip MPP (Apache-2.0 OR MIT),
 * mpp/hal/rkdec/h264d/hal_h264d_vdpu383.c, hal_h264d_com.c,
 * mpp/hal/rkdec/common/vdpu383_com.c and vdpu38x_com.c, which already work
 * from a DXVA_PicParams_H264-shaped structure. The behaviour MPP gets from
 * its own parser and DXVA does not carry (the default reference lists and
 * the picture parameter set id) is derived here as mainline Linux's rkvdec
 * driver and v4l2-h264.c derive it, written fresh. See vdpu383_h264.c.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_H264_H
#define VDPU383_H264_H

#include "v383_dxva.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ hardware */

#define V383_REG_BLOCK_SIZE     0x1000      /* the ACPI memory resource */
#define V383_LINK_OFFSET        0x000       /* "link" registers */
#define V383_FUNC_OFFSET        0x100       /* register 0 of the function block */
#define V383_MMU0_OFFSET        0x800
#define V383_MMU1_OFFSET        0x900

#define V383_NUM_REGS           384         /* function registers 0..383 (0x100..0x6FF) */
#define V383_MASK_WORDS         (V383_NUM_REGS / 32)

/* Link registers (byte offsets from the block), MPP Vdpu383RegLlp. */
#define V383_LINK_CCU_MODE      0x00
#define V383_LINK_CFG_ADDR      0x04
#define V383_LINK_MODE          0x08
#define V383_LINK_CFG_CTRL      0x0C
#define V383_LINK_EN            0x18
#define V383_LINK_DEC_E         0x40        /* write 1: start */
#define V383_LINK_IP_RESET      0x44        /* write 1: reset core and IOMMU */
#define V383_LINK_IRQ           0x48        /* [1:0] irq, line irq; write-mask in [17:16] */
#define V383_LINK_STATUS        0x4C        /* [15:0] as register 15; write-mask in [31:16] */
#define V383_LINK_IP_TIMEOUT    0x54
#define V383_LINK_IP_ENABLE     0x58        /* bit 15 irq_dis, [25:24] clk_cru_mode */

/* Reference read cache, the "cache" region at +0x600 (mainline
 * rkvdec-vdpu383-regs.h, "configure the reference read cache"). Its power-on
 * values vary between power-ups and make decoding up to 18 times slower. */
#define V383_CACHE_OFFSET       0x600
#define V383_CACHE_CLR_CACHE0   0x10        /* write 1: clear */
#define V383_CACHE_MAX_READS    0x18
#define V383_CACHE_CACHE0_CFG   0x1C
#define V383_CACHE_CFG_CACHEABLE        0x01
#define V383_CACHE_CFG_READ_ALLOCATE    0x02
#define V383_CACHE_CFG_LINE_64B         0x10
#define V383_CACHE_MAX_READS_DEFAULT    0x1C    /* the vendor driver's */

#define V383_STA_FRAME_RDY      0x0001
#define V383_STA_ERROR_MASK     0x03FE      /* MPP's err_mask */
#define V383_STA_IP_RST_RDY     0x0800

/* Register numbers (word index into the function block). */
#define V383_REG_DEC_MODE       8
#define V383_REG_IRQ_STA        15
#define V383_REG_STRM_LEN       66
#define V383_REG_STRM_BASE      128

/* ----------------------------------------------------------- buffers */

/*
 * The auxiliary buffer, one per decoder (it is rewritten for every
 * picture). 64 KiB, 4 KiB aligned, below 4 GiB, device readable.
 */
#define V383_H264_CABAC_WORDS       928
#define V383_H264_AUX_CABAC_OFF     0x0000      /* 3712 bytes */
#define V383_H264_AUX_RPS_OFF       0x1000      /* 128 bytes used */
#define V383_H264_AUX_SCALING_OFF   0x2000      /* 224 bytes used */
#define V383_H264_AUX_GBL_OFF       0x3000      /* 256 x 176 bytes, by pps id */
#define V383_H264_SPSPPS_BYTES      176         /* 1338 bits, aligned to 128 */
#define V383_H264_AUX_SIZE          0x10000

/* Output surface: NV12 (4:2:0, 8 bit), luma then chroma, same stride. */
typedef struct V383_Surface {
    unsigned int dma;           /* luma plane; chroma at dma + hor_stride * ver_stride */
    unsigned int colmv_dma;     /* this picture's co-located motion vectors */
} V383_Surface;

/* Optional: what an H.264 parser knows and DXVA_PicParams_H264 does not. */
typedef struct V383_H264Extra {
    unsigned char valid;                /* 0: use the defaults below */
    unsigned char profile_idc;          /* default: derived, see v383_h264_default_profile() */
    unsigned char seq_parameter_set_id; /* default 0 */
    unsigned char constraint_set3_flag; /* default 0 */
} V383_H264Extra;

#define V383_PPS_ID_FROM_STREAM 0xFFFF

typedef struct V383_H264Input {
    const V383_DXVA_PicParams_H264 *pp;
    const V383_DXVA_Qmatrix_H264   *qm;         /* NULL: flat (no scaling matrix) */
    const V383_H264Extra           *extra;      /* NULL: defaults */
    unsigned int scaling_matrix_present;        /* sps or pps carried one; ignored when qm is NULL */

    /* Bitstream: CPU view (to read pic_parameter_set_id) and bus address. */
    const unsigned char *stream_cpu;
    unsigned int  stream_dma;
    unsigned int  stream_len;           /* bytes the decoder may read; padded to 16 */
    unsigned int  pps_id;               /* V383_PPS_ID_FROM_STREAM: read the first slice header */

    /* Auxiliary buffer (V383_H264_AUX_SIZE). */
    unsigned char *aux_cpu;
    unsigned int  aux_dma;

    /* Row buffers: v383_h264_rcb_size() bytes. */
    unsigned int  rcb_dma;

    /* Surfaces, indexed by DXVA Index7Bits (CurrPic and RefFrameList). */
    const V383_Surface *surfaces;
    unsigned int  num_surfaces;
    unsigned int  hor_stride;           /* bytes, multiple of 16 */
    unsigned int  ver_stride;           /* lines, multiple of 16 */
} V383_H264Input;

typedef struct V383_RegFile {
    unsigned int regs[V383_NUM_REGS];
    unsigned int write_mask[V383_MASK_WORDS];   /* bit r: write register r */
    unsigned int core_timeout;                  /* register 13, for the link timeout */
} V383_RegFile;

/* Default reference lists, as indices into RefFrameList (0xFF = none). */
typedef struct V383_H264RefLists {
    unsigned char p[32];
    unsigned char b0[32];
    unsigned char b1[32];
    unsigned int  num_p, num_b0, num_b1;
} V383_H264RefLists;

/* ----------------------------------------------------------- functions */

#define V383_OK             0
#define V383_E_INVALID      (-1)
#define V383_E_UNSUPPORTED  (-2)
#define V383_E_STREAM       (-3)

/* Row buffer bytes for a picture of this size (luma, pixels). */
unsigned int v383_h264_rcb_size(unsigned int width, unsigned int height, unsigned int mbaff);

/* Co-located MV buffer bytes per surface. */
unsigned int v383_h264_colmv_size(unsigned int width, unsigned int height, unsigned int frame_mbs_only);

/* pic_parameter_set_id of the first slice in the buffer, or -1. */
int v383_h264_parse_pps_id(const unsigned char *stream, unsigned int len);

/* profile_idc to use when the parser's is not known. */
unsigned int v383_h264_default_profile(const V383_DXVA_PicParams_H264 *pp, unsigned int scaling);

/* The default (initial) P, B0 and B1 lists, from the picture parameters. */
void v383_h264_build_reflists(const V383_DXVA_PicParams_H264 *pp, V383_H264RefLists *out);

/* Build the register file and fill the auxiliary buffer. */
int v383_h264_build(const V383_H264Input *in, V383_RegFile *out);

extern const unsigned int v383_h264_cabac_table[V383_H264_CABAC_WORDS];

#ifdef __cplusplus
}
#endif

#endif /* VDPU383_H264_H */
