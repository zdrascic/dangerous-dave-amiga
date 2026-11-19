#pragma once
#include "sade.h"

// ILBM loader — parses IFF ILBM files from chip RAM.
//
// Usage:
//   UBYTE *data = ...; ULONG size = ...;   // raw file bytes already in memory
//   IlbmImage *img = IlbmLoad(data, size);
//   if (img) {
//       IlbmApplyPalette(img, scr);         // load CMAP into screen palette
//       IlbmBlit(img, scr, 0, 0);           // blit top-left at (0,0)
//       IlbmFree(img);
//   }
//
// Constraints:
//   - Compression 0 (none) and 1 (PackBits) are both supported.
//   - Masking plane is ignored.
//   - CRNG/CAMG chunks are ignored.
//   - Image depth must be <= SADE_MAX_PLANES (8; more than 5 need an AGA
//     screen to show).
//   - Plane data is allocated in chip RAM.

#define ILBM_MAX_PLANES  SADE_MAX_PLANES
#define ILBM_MAX_COLORS 256

typedef struct {
    UBYTE *planes[ILBM_MAX_PLANES];
    UWORD  width;
    UWORD  height;
    UBYTE  depth;
    UWORD  rowBytes;    // bytes per row per plane (may be wider than visWidth/8)
    UWORD  numColors;
    ULONG  palette[ILBM_MAX_COLORS]; // CMAP as $00RRGGBB (RGB444() for OCS)
} IlbmImage;

// -------------------------------------------------------------------------
// Internal: read big-endian ULONG from raw bytes
// -------------------------------------------------------------------------
static inline ULONG _IlbmU32(const UBYTE *p) {
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}
static inline UWORD _IlbmU16(const UBYTE *p) {
    return ((UWORD)p[0] << 8) | p[1];
}
static inline UBYTE _IlbmTag(const UBYTE *p, const char *tag) {
    return p[0]==tag[0] && p[1]==tag[1] && p[2]==tag[2] && p[3]==tag[3];
}

// -------------------------------------------------------------------------
// Internal: decompress one PackBits row into dst (rowBytes bytes).
// src advances by the number of bytes consumed; returns 0 on overrun.
// -------------------------------------------------------------------------
static UBYTE _IlbmUnpackRow(UBYTE *dst, UWORD rowBytes,
                             const UBYTE **src, const UBYTE *srcEnd)
{
    UWORD out = 0;
    while (out < rowBytes) {
        if (*src >= srcEnd) return 0;
        BYTE n = (BYTE)**src; (*src)++;
        if (n >= 0) {
            // literal run: copy n+1 bytes
            UWORD cnt = (UWORD)(n + 1);
            if (*src + cnt > srcEnd || out + cnt > rowBytes) return 0;
            while (cnt--) { dst[out++] = **src; (*src)++; }
        } else if (n != -128) {
            // replicate run: repeat next byte (-n+1) times
            UWORD cnt = (UWORD)(-(WORD)n + 1);
            if (*src >= srcEnd || out + cnt > rowBytes) return 0;
            UBYTE val = **src; (*src)++;
            while (cnt--) dst[out++] = val;
        }
        // n == -128: nop, skip
    }
    return 1;
}

// -------------------------------------------------------------------------
// IlbmLoad — parse IFF ILBM from a buffer already loaded into memory.
// data/size point to the raw file bytes (need not be chip RAM).
// Returns NULL on parse error or allocation failure.
// -------------------------------------------------------------------------
static IlbmImage *IlbmLoad(const UBYTE *data, ULONG size) {
    if (size < 12) return NULL;
    if (!_IlbmTag(data,   "FORM")) return NULL;
    if (!_IlbmTag(data+8, "ILBM")) return NULL;

    IlbmImage *img = (IlbmImage*)SadeAlloc(sizeof(IlbmImage), MEMF_PUBLIC|MEMF_CLEAR);
    if (!img) return NULL;

    UBYTE hasBmhd = 0, hasCmap = 0;
    UBYTE compression = 0, masking = 0;

    const UBYTE *p   = data + 12;
    const UBYTE *end = data + size;

    while (p + 8 <= end) {
        ULONG chunkLen = _IlbmU32(p + 4);
        const UBYTE *body  = p + 8;
        const UBYTE *next  = body + chunkLen + (chunkLen & 1); // pad to even

        if (_IlbmTag(p, "BMHD")) {
            if (chunkLen < 20) goto fail;
            img->width    = _IlbmU16(body);
            img->height   = _IlbmU16(body + 2);
            img->depth    = body[8];
            masking       = body[9];
            compression   = body[10];
            img->rowBytes = (img->width + 15) / 16 * 2; // round up to word
            if (img->depth > ILBM_MAX_PLANES) goto fail;
            hasBmhd = 1;

        } else if (_IlbmTag(p, "CMAP")) {
            UWORD nColors = divuw(chunkLen < ILBM_MAX_COLORS * 3 ? chunkLen
                                                                 : ILBM_MAX_COLORS * 3, 3);
            img->numColors = nColors;
            for (UWORD i = 0; i < nColors; i++) {
                const UBYTE *c = body + i * 3;
                img->palette[i] = ((ULONG)c[0] << 16) | ((ULONG)c[1] << 8) | c[2];
            }
            hasCmap = 1;

        } else if (_IlbmTag(p, "BODY")) {
            if (!hasBmhd) goto fail;

            // Allocate chip RAM planes
            ULONG planeSize = (ULONG)img->rowBytes * img->height;
            for (UBYTE pl = 0; pl < img->depth; pl++) {
                img->planes[pl] = (UBYTE*)SadeAlloc(planeSize, MEMF_CHIP|MEMF_CLEAR);
                if (!img->planes[pl]) goto fail;
            }

            // Decode BODY: interleaved planes, one row per plane per scanline.
            // Masking plane (if present) is decoded and discarded.
            UBYTE planeCount = img->depth + (masking == 1 ? 1 : 0);
            const UBYTE *src = body;

            if (compression == 0) {
                // Uncompressed: raw rows
                for (UWORD y = 0; y < img->height; y++) {
                    for (UBYTE pl = 0; pl < planeCount; pl++) {
                        if (src + img->rowBytes > end) goto fail;
                        if (pl < img->depth)
                            for (UWORD b2 = 0; b2 < img->rowBytes; b2++)
                                img->planes[pl][y * img->rowBytes + b2] = src[b2];
                        src += img->rowBytes;
                    }
                }
            } else if (compression == 1) {
                // PackBits per row per plane
                for (UWORD y = 0; y < img->height; y++) {
                    for (UBYTE pl = 0; pl < planeCount; pl++) {
                        UBYTE *dst = (pl < img->depth)
                            ? img->planes[pl] + (ULONG)y * img->rowBytes
                            : NULL;
                        if (pl < img->depth) {
                            if (!_IlbmUnpackRow(dst, img->rowBytes, &src, end)) goto fail;
                        } else {
                            // discard mask plane row
                            UBYTE dummy[80];
                            const UBYTE *tmp = src;
                            if (!_IlbmUnpackRow(dummy, img->rowBytes < 80 ? img->rowBytes : 80, &src, end)) {
                                // try advancing manually — mask plane, just skip
                                src = tmp;
                                while (src < end && (src - tmp) < (WORD)img->rowBytes * 2) {
                                    BYTE n = (BYTE)*src++;
                                    if (n >= 0) src += n + 1;
                                    else if (n != -128) src += 1;
                                }
                            }
                        }
                    }
                }
            } else {
                goto fail; // unsupported compression
            }
        }

        p = next;
    }

    if (!hasBmhd) goto fail;
    if (!hasCmap) img->numColors = 0;
    return img;

fail:
    if (img) {
        if (hasBmhd) {
            ULONG planeSize = (ULONG)img->rowBytes * img->height;
            for (UBYTE pl = 0; pl < img->depth; pl++)
                if (img->planes[pl]) SadeFree(img->planes[pl], planeSize);
        }
        SadeFree(img, sizeof(IlbmImage));
    }
    return NULL;
}

// -------------------------------------------------------------------------
// IlbmFree — release all chip RAM allocated by IlbmLoad.
// -------------------------------------------------------------------------
static void IlbmFree(IlbmImage *img) {
    if (!img) return;
    ULONG planeSize = (ULONG)img->rowBytes * img->height;
    for (UBYTE pl = 0; pl < img->depth; pl++)
        if (img->planes[pl]) SadeFree(img->planes[pl], planeSize);
    SadeFree(img, sizeof(IlbmImage));
}

// -------------------------------------------------------------------------
// IlbmApplyPalette — load the image CMAP into the screen's copper palette.
// Only loads min(img->numColors, scr->numColors) entries; an AGA screen
// gets all 8 bits per gun, an OCS screen the top 4.
// -------------------------------------------------------------------------
static void IlbmApplyPalette(const IlbmImage *img, Screen *scr) {
    UWORD n = img->numColors < scr->numColors ? img->numColors : scr->numColors;
    for (UWORD i = 0; i < n; i++)
        ScreenSetRGB(scr, i, img->palette[i]);
}

// -------------------------------------------------------------------------
// IlbmBlit — copy image planes to screen at pixel position (dstX, dstY)
// using the blitter (D=A, straight copy, no masking).
// Only copies min(img->depth, scr->depth) planes.
// srcX/srcY let you pan within the image (must be word-aligned: multiple of 16).
// Width/height are clamped to both image and screen bounds.
// -------------------------------------------------------------------------
static void IlbmBlit(const IlbmImage *img, Screen *scr,
                     short dstX, short dstY,
                     UWORD srcX,  UWORD srcY)
{
    // Clamp destination to screen
    if (dstX >= (short)scr->width || dstY >= (short)scr->height) return;
    if (dstX < 0) { srcX += (UWORD)(-dstX); dstX = 0; }
    if (dstY < 0) { srcY += (UWORD)(-dstY); dstY = 0; }

    // srcX must be word-aligned
    srcX &= ~15;

    UWORD w = img->width  - srcX;
    UWORD h = img->height - srcY;
    if (srcX >= img->width || srcY >= img->height) return;

    // clip to screen
    if ((UWORD)dstX + w > scr->width)  w = scr->width  - (UWORD)dstX;
    if ((UWORD)dstY + h > scr->height) h = scr->height - (UWORD)dstY;
    if (w == 0 || h == 0) return;

    UWORD words    = w >> 4; // full words only (word-aligned src)
    if (words == 0) return;
    UWORD srcWords = img->rowBytes >> 1;
    UWORD dstWords = scr->rowBytes >> 1;
    UWORD srcWordOff = srcX >> 4;

    WORD  srcMod = (WORD)(img->rowBytes  - words * 2);
    WORD  dstMod = (WORD)(scr->rowBytes  - words * 2);

    UBYTE depth = img->depth < scr->depth ? img->depth : scr->depth;

    for (UBYTE pl = 0; pl < depth; pl++) {
        UBYTE *src = img->planes[pl] + (ULONG)srcY * img->rowBytes + srcWordOff * 2;
        UBYTE *dst = scr->planes[pl] + (ULONG)(UWORD)dstY * scr->rowBytes + (ULONG)(UWORD)dstX / 16 * 2;

        SadeWaitBlt();
        custom->bltcon0 = 0x09F0; // D = A (copy)
        custom->bltcon1 = 0x0000;
        custom->bltafwm = 0xffff;
        custom->bltalwm = 0xffff;
        custom->bltapt  = (APTR)src;
        custom->bltdpt  = (APTR)dst;
        custom->bltamod = srcMod;
        custom->bltdmod = dstMod;
        custom->bltsize = (UWORD)((h << 6) | words);
    }
    SadeWaitBlt();
}
