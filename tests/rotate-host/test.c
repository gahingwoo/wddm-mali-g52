// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/maliwddm/rotate.h against the Basic Display Driver sample's own
 * rotated copy (Windows-driver-samples, video/KMDOD/bltfuncs.cxx:
 * GetPitches, GetRowStart and the 32 to 32 bit case of CopyBitsGeneric),
 * transcribed below. For every angle and many rectangles, both copy the
 * same picture into a framebuffer and the two framebuffers must be equal,
 * byte for byte. The framebuffer is 800 x 1280 with a padded pitch, the
 * TaishanPi 3M's panel; a small odd size catches what a square or a
 * multiple of the tile would hide.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/maliwddm/rotate.h"

static int fails, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            fails++;                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* ---- the sample, 32 bpp only ---- */

typedef struct {
    unsigned char *Bits;
    long Pitch;
    unsigned Deg;
    long OffX, OffY;
    unsigned Width, Height;     /* for the unrotated image */
} BLT_INFO;

static void GetPitches(const BLT_INFO *b, long *Pixel, long *Row)
{
    switch (b->Deg) {
    case 0:   *Pixel = 4;          *Row = b->Pitch;   break;
    case 90:  *Pixel = -b->Pitch;  *Row = 4;          break;
    case 180: *Pixel = -4;         *Row = -b->Pitch;  break;
    case 270: *Pixel = b->Pitch;   *Row = -4;         break;
    }
}

static unsigned char *GetRowStart(const BLT_INFO *b, const MW_ROT_RECT *r)
{
    long l = r->Left + b->OffX, t = r->Top + b->OffY;

    switch (b->Deg) {
    case 0:   return b->Bits + t * b->Pitch + l * 4;
    case 90:  return b->Bits + ((long)b->Height - 1 - l) * b->Pitch + t * 4;
    case 180: return b->Bits + ((long)b->Height - 1 - t) * b->Pitch + ((long)b->Width - 1 - l) * 4;
    case 270: return b->Bits + l * b->Pitch + ((long)b->Width - 1 - t) * 4;
    }
    return NULL;
}

static void CopyBitsGeneric(const BLT_INFO *Dst, const BLT_INFO *Src, const MW_ROT_RECT *r)
{
    long dpp, drp, spp, srp;
    unsigned char *drow = GetRowStart(Dst, r);
    const unsigned char *srow = GetRowStart(Src, r);

    GetPitches(Dst, &dpp, &drp);
    GetPitches(Src, &spp, &srp);
    for (long y = 0; y < r->Bottom - r->Top; y++) {
        unsigned char *d = drow;
        const unsigned char *s = srow;
        for (long x = 0; x < r->Right - r->Left; x++) {
            *(unsigned *)d = *(const unsigned *)s;
            d += dpp;
            s += spp;
        }
        drow += drp;
        srow += srp;
    }
}

/* ---- the test ---- */

static void one(unsigned Deg, unsigned W, unsigned H, long FbPitch, const MW_ROT_RECT *L, long Dx, long Dy)
{
    unsigned lw, lh;
    long srcPitch;
    unsigned char *src, *a, *b;
    size_t fbBytes = (size_t)FbPitch * H, srcBytes;
    BLT_INFO dst, s;

    MwRotLogicalSize(Deg, W, H, &lw, &lh);
    srcPitch = (long)(lw + Dx + 3) * 4;         /* a padded source, too */
    srcBytes = (size_t)srcPitch * (lh + Dy);
    src = malloc(srcBytes);
    a = malloc(fbBytes);
    b = malloc(fbBytes);
    for (size_t i = 0; i < srcBytes / 4; i++)
        ((unsigned *)src)[i] = (unsigned)(i * 2654435761u);
    memset(a, 0x5A, fbBytes);
    memset(b, 0x5A, fbBytes);

    MwRotCopy(Deg, a, FbPitch, W, H, src, srcPitch, Dx, Dy, L);

    /* The sample: the destination is the framebuffer with the path's
     * rotation and its own size; the source unrotated, offset so that the
     * rectangle's logical (0, 0) is the source's (Dx, Dy). */
    dst.Bits = b; dst.Pitch = FbPitch; dst.Deg = Deg; dst.OffX = 0; dst.OffY = 0;
    dst.Width = W; dst.Height = H;
    s.Bits = src; s.Pitch = srcPitch; s.Deg = 0; s.OffX = Dx; s.OffY = Dy;
    s.Width = lw; s.Height = lh;
    CopyBitsGeneric(&dst, &s, L);

    CHECK(memcmp(a, b, fbBytes) == 0, "deg %u fb %ux%u rect (%ld,%ld)-(%ld,%ld) d (%ld,%ld): differs",
          Deg, W, H, L->Left, L->Top, L->Right, L->Bottom, Dx, Dy);
    free(src);
    free(a);
    free(b);
}

static void points(unsigned Deg, unsigned W, unsigned H)
{
    unsigned lw, lh;
    unsigned char *seen = calloc((size_t)W * H, 1);
    int bad = 0;

    MwRotLogicalSize(Deg, W, H, &lw, &lh);
    for (long y = 0; y < (long)lh; y++)
        for (long x = 0; x < (long)lw; x++) {
            long c, r;
            MW_ROT_RECT one1 = { x, y, x + 1, y + 1 }, f;
            MwRotPoint(Deg, W, H, x, y, &c, &r);
            f = MwRotRect(Deg, W, H, &one1);
            if (c < 0 || r < 0 || c >= (long)W || r >= (long)H || seen[r * W + c] ||
                f.Left != c || f.Top != r || f.Right != c + 1 || f.Bottom != r + 1)
                bad++;
            else
                seen[r * W + c] = 1;
        }
    CHECK(bad == 0, "deg %u fb %ux%u: %d logical pixels map outside, twice, or not as their rectangle", Deg, W, H, bad);
    free(seen);
}

int main(void)
{
    static const unsigned degs[] = { 0, 90, 180, 270 };
    static const struct { unsigned W, H; long Pitch; } fbs[] = {
        { 800, 1280, 800 * 4 },         /* the TaishanPi 3M panel */
        { 37, 21, 40 * 4 },             /* odd, padded */
        { 1920, 1080, 1920 * 4 },       /* an HDMI monitor */
    };

    for (unsigned d = 0; d < 4; d++) {
        unsigned deg = degs[d];
        points(deg, 37, 21);
        points(deg, 800, 1280);
        for (unsigned f = 0; f < sizeof(fbs) / sizeof(fbs[0]); f++) {
            unsigned lw, lh;
            MwRotLogicalSize(deg, fbs[f].W, fbs[f].H, &lw, &lh);
            MW_ROT_RECT full = { 0, 0, (long)lw, (long)lh };
            one(deg, fbs[f].W, fbs[f].H, fbs[f].Pitch, &full, 0, 0);
            one(deg, fbs[f].W, fbs[f].H, fbs[f].Pitch, &full, 3, 5);
            srand(deg * 7 + f);
            for (int i = 0; i < (f == 1 ? 200 : 12); i++) {
                long l = rand() % lw, t = rand() % lh;
                long r = l + 1 + rand() % (lw - l), b = t + 1 + rand() % (lh - t);
                MW_ROT_RECT rc = { l, t, r, b };
                one(deg, fbs[f].W, fbs[f].H, fbs[f].Pitch, &rc, rand() % 4, rand() % 4);
            }
        }
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
