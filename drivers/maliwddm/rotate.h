/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Display rotation, the pixel side: where a pixel of the desktop as Windows
 * sees it (the "logical" picture, rotated) lands in the framebuffer the
 * display controller scans out (the panel's own orientation), and a copy
 * that puts a rotated picture there.
 *
 * The VOP2 cannot rotate a linear window by 90 or 270 degrees, so when the
 * driver has to rotate (display.c says when) it is done here, on the CPU.
 *
 * The mapping is the one Microsoft's Basic Display Driver sample uses
 * (Windows-driver-samples, video/KMDOD/bltfuncs.cxx, GetRowStart and
 * GetPitches): angles are counter-clockwise, as in GDI and in
 * D3DKMDT_VIDPN_PRESENT_PATH_ROTATION. With the framebuffer W x H and a
 * logical pixel (x, y):
 *
 *     90:  framebuffer column y,         row H - 1 - x
 *     180: framebuffer column W - 1 - x, row H - 1 - y
 *     270: framebuffer column W - 1 - y, row x
 *
 * tests/rotate-host checks this file against a transcription of the
 * sample's code.
 *
 * Plain C with no WDK header, so that the host test can build it. 32-bit
 * pixels only, as everything else in the driver.
 */
#pragma once

#ifndef MW_ROT_INLINE
#ifdef _MSC_VER
#define MW_ROT_INLINE static __inline
#else
#define MW_ROT_INLINE static inline
#endif
#endif

typedef struct _MW_ROT_RECT {
    long Left, Top, Right, Bottom;      /* right and bottom exclusive */
} MW_ROT_RECT;

/* The size of the logical picture for a framebuffer of W x H. */
MW_ROT_INLINE void MwRotLogicalSize(unsigned Deg, unsigned W, unsigned H, unsigned *Lw, unsigned *Lh)
{
    if (Deg == 90 || Deg == 270) {
        *Lw = H;
        *Lh = W;
    } else {
        *Lw = W;
        *Lh = H;
    }
}

/* A logical pixel's place in the framebuffer. */
MW_ROT_INLINE void MwRotPoint(unsigned Deg, unsigned W, unsigned H, long X, long Y, long *C, long *R)
{
    switch (Deg) {
    case 90:  *C = Y;                 *R = (long)H - 1 - X; break;
    case 180: *C = (long)W - 1 - X;   *R = (long)H - 1 - Y; break;
    case 270: *C = (long)W - 1 - Y;   *R = X;               break;
    default:  *C = X;                 *R = Y;               break;
    }
}

/* A logical rectangle's place in the framebuffer: the same pixels, as a
 * framebuffer rectangle. */
MW_ROT_INLINE MW_ROT_RECT MwRotRect(unsigned Deg, unsigned W, unsigned H, const MW_ROT_RECT *L)
{
    MW_ROT_RECT f;

    switch (Deg) {
    case 90:
        f.Left = L->Top;                f.Right = L->Bottom;
        f.Top = (long)H - L->Right;     f.Bottom = (long)H - L->Left;
        break;
    case 180:
        f.Left = (long)W - L->Right;    f.Right = (long)W - L->Left;
        f.Top = (long)H - L->Bottom;    f.Bottom = (long)H - L->Top;
        break;
    case 270:
        f.Left = (long)W - L->Bottom;   f.Right = (long)W - L->Top;
        f.Top = L->Left;                f.Bottom = L->Right;
        break;
    default:
        f = *L;
        break;
    }
    return f;
}

/* Copy the logical rectangle L of a picture into the framebuffer, rotated.
 * Fb is W x H with FbPitch bytes a row. The logical pixel (x, y) is read
 * from Src at column x + Dx, row y + Dy, SrcPitch bytes a row. L must lie
 * inside the logical picture and, moved by (Dx, Dy), inside the source:
 * the caller clips.
 *
 * In 16 x 16 tiles of the framebuffer: one tile reads 16 source rows of 64
 * bytes (one cache line each on the Cortex-A53 and A72) and writes 16
 * framebuffer rows of 64 bytes, which the write-combining mapping of the
 * framebuffer turns into whole bursts. A straight row by row walk would
 * read a source column, one 4-byte pixel a cache line, for every
 * framebuffer row. */
#define MW_ROT_TILE 16

MW_ROT_INLINE void MwRotCopy(unsigned Deg, unsigned char *Fb, long FbPitch, unsigned W, unsigned H,
                             const unsigned char *Src, long SrcPitch, long Dx, long Dy,
                             const MW_ROT_RECT *L)
{
    MW_ROT_RECT f = MwRotRect(Deg, W, H, L);
    long step;          /* bytes in the source from one framebuffer column to the next */

    switch (Deg) {
    case 90:  step = SrcPitch;  break;
    case 180: step = -4;        break;
    case 270: step = -SrcPitch; break;
    default:  step = 4;         break;
    }
    for (long tr = f.Top; tr < f.Bottom; tr += MW_ROT_TILE) {
        long re = tr + MW_ROT_TILE < f.Bottom ? tr + MW_ROT_TILE : f.Bottom;
        for (long tc = f.Left; tc < f.Right; tc += MW_ROT_TILE) {
            long ce = tc + MW_ROT_TILE < f.Right ? tc + MW_ROT_TILE : f.Right;
            for (long r = tr; r < re; r++) {
                unsigned *d = (unsigned *)(Fb + r * FbPitch) + tc;
                const unsigned char *s;
                long x, y;

                /* The logical pixel framebuffer (tc, r) shows. */
                switch (Deg) {
                case 90:  x = (long)H - 1 - r;  y = tc;                 break;
                case 180: x = (long)W - 1 - tc; y = (long)H - 1 - r;    break;
                case 270: x = r;                y = (long)W - 1 - tc;   break;
                default:  x = tc;               y = r;                  break;
                }
                s = Src + (y + Dy) * SrcPitch + (x + Dx) * 4;
                for (long c = tc; c < ce; c++) {
                    *d++ = *(const unsigned *)s;
                    s += step;
                }
            }
        }
    }
}
