#pragma once
#include "sade.h"

// Bob blitter object system.
//
// Image layout: visWidth+16 pixels wide (the extra 16px = one word pad so the
// blitter shift never reads past end of row). All data in chip RAM.
//
// Single-buffer loop:
//   SadeWaitVbl()
//   BobEraseAll(bobs, n, s, 0)
//   <move bobs>
//   BobDrawAll(bobs, n, s, 0)
//
// Double-buffer loop (buf alternates 0/1 each frame):
//   SadeWaitVbl()
//   BobEraseAll(bobs, n, s, buf)   -- erase this buffer's 2-frame-old position
//   <move bobs>
//   BobDrawAll(bobs, n, s, buf)    -- draw into back buffer, record position
//   ScreenFlip(s)
//   buf ^= 1
//
// Masked blit (cookie-cut): D = A*B + ~A*C  (minterm 0xCA)
//   A = shape mask (1 wherever any pixel exists)
//   B = per-plane image data
//   C = screen (unshifted, word-aligned)
//   D = screen output
// Per-plane image used as its own mask so mask=1 only where image=1 —
// no holes punched through other Bobs on planes where this Bob has no data.

#define BOB_MAX_PLANES SADE_MAX_PLANES

typedef struct {
    UBYTE *planeData[BOB_MAX_PLANES];
    UBYTE *maskData;    // single shape mask (1 wherever any pixel exists)
    UBYTE  planeMask;   // bitmask of which planes have any image data
    UWORD  visWidth;    // visible pixel width (what caller specifies)
    UWORD  width;       // allocated width = visWidth + 16
    UWORD  height;
    UBYTE  depth;
    UWORD  rowBytes;    // width / 8
    UWORD  wordWidth;   // width / 16
    UWORD  drawTop;     // BobImageTrim: first row with pixels
    UWORD  drawH;       //   rows drawn from there (0 = height, untrimmed)
    UWORD  drawWords;   //   words blitted (0 = wordWidth)
} BobImage;

// Rows / words a draw actually blits (all of them unless trimmed).
static inline UWORD _BobRows(const BobImage *img)  { return img->drawH ? img->drawH : img->height; }
static inline UWORD _BobWords(const BobImage *img) { return img->drawWords ? img->drawWords : img->wordWidth; }

typedef struct {
    short savedX, savedY;
    UWORD savedH, savedWords;
    UBYTE hasSave;
} BobSlot;

typedef struct {
    BobImage *image;
    short     x, y;
    short     vx, vy;
    UBYTE     visible;
    BobSlot   slot[2]; // one slot per buffer (index 0 and 1)
} Bob;

// -------------------------------------------------------------------------
static BobImage *BobImageCreate(UWORD visWidth, UWORD height, UBYTE depth) {
    BobImage *img = (BobImage*)SadeAlloc(sizeof(BobImage), MEMF_PUBLIC|MEMF_CLEAR);
    if (!img) return 0;
    img->visWidth  = visWidth;
    img->width     = visWidth + 16;
    img->height    = height;
    img->depth     = depth;
    img->rowBytes  = img->width / 8;
    img->wordWidth = img->width / 16;
    ULONG sz = (ULONG)img->rowBytes * height;
    for (UBYTE p = 0; p < depth; p++) {
        img->planeData[p] = (UBYTE*)SadeAlloc(sz, MEMF_CHIP|MEMF_CLEAR);
        if (!img->planeData[p]) goto fail;
    }
    img->maskData = (UBYTE*)SadeAlloc(sz, MEMF_CHIP|MEMF_CLEAR);
    if (!img->maskData) goto fail;
    return img;
fail:
    for (UBYTE p = 0; p < depth; p++)
        if (img->planeData[p]) SadeFree(img->planeData[p], sz);
    if (img->maskData) SadeFree(img->maskData, sz);
    SadeFree(img, sizeof(BobImage));
    return 0;
}

static void BobImageFree(BobImage *img) {
    if (!img) return;
    ULONG sz = (ULONG)img->rowBytes * img->height;
    for (UBYTE p = 0; p < img->depth; p++)
        if (img->planeData[p]) SadeFree(img->planeData[p], sz);
    if (img->maskData) SadeFree(img->maskData, sz);
    SadeFree(img, sizeof(BobImage));
}

static void BobImageSetPixel(BobImage *img, UWORD x, UWORD y, UBYTE color) {
    if (x >= img->visWidth || y >= img->height) return;
    UWORD off = y * img->rowBytes + (x >> 3);
    UBYTE bit = 0x80 >> (x & 7);
    for (UBYTE p = 0; p < img->depth; p++) {
        if (color & (1 << p)) {
            img->planeData[p][off] |= bit;
            img->planeMask |= (1 << p);
        } else {
            img->planeData[p][off] &= ~bit;
        }
    }
    img->maskData[off] |= bit;
}

// BobImageTrim: draw only the rows and words that hold pixels (from the
// mask). A sprite cut from a fixed-size sheet cell (e.g. 18x8 art in a
// 32x24 cell) then costs a blit of its art, not of the cell; Bob x/y still
// mean the cell's top-left. Call again after changing the pixels.
static void BobImageTrim(BobImage *img) {
    UWORD top = img->height, bottom = 0, right = 0;   // right: last used word + 1
    const UWORD *m = (const UWORD*)img->maskData;
    for (UWORD y = 0; y < img->height; y++)
        for (UWORD w = 0; w < img->wordWidth; w++, m++)
            if (*m) {
                if (top == img->height) top = y;
                bottom = (UWORD)(y + 1);
                if (w >= right) right = (UWORD)(w + 1);
            }
    if (top == img->height) top = bottom = 0;         // empty: left untrimmed
    img->drawTop   = top;
    img->drawH     = (UWORD)(bottom - top);
    // one more word for the shifted spill, as BobImageCreate's +16
    img->drawWords = (UWORD)(right < img->wordWidth ? right + 1 : img->wordWidth);
    if (!img->drawH) img->drawWords = 0;
}

static void BobImageFillCircle(BobImage *img, short cx, short cy, short r, UBYTE color) {
    for (short dy = -r; dy <= r; dy++) {
        short xm = r;
        while (xm > 0 && (xm*xm + dy*dy) > r*r) xm--;
        for (short dx = -xm; dx <= xm; dx++)
            BobImageSetPixel(img, (UWORD)(cx+dx), (UWORD)(cy+dy), color);
    }
}

// -------------------------------------------------------------------------
static Bob *BobCreate(BobImage *img) {
    Bob *b = (Bob*)SadeAlloc(sizeof(Bob), MEMF_PUBLIC|MEMF_CLEAR);
    if (!b) return 0;
    b->image   = img;
    b->visible = 1;
    return b;
}

static void BobFree(Bob *b) {
    if (b) SadeFree(b, sizeof(Bob));
}

// -------------------------------------------------------------------------
// Clip helper: vertical clip + fully-off-screen test. Horizontal clipping
// is handled in BobDraw. Returns 0 if fully off-screen.
// -------------------------------------------------------------------------
static UBYTE _BobClip(const Bob *b, const Screen *s,
                      short *outY, UWORD *blitH, UWORD *skipRows)
{
    short x = b->x, y = (short)(b->y + b->image->drawTop);
    UWORD h = _BobRows(b->image);

    *skipRows = 0;
    if (y < 0) {
        if (-y >= (short)h) return 0;
        *skipRows = (UWORD)(-y);
        h -= *skipRows;
        y  = 0;
    }
    if (y >= (short)s->height) return 0;
    if ((UWORD)y + h > s->height) h = s->height - (UWORD)y;

    if (x >= (short)s->width) return 0;
    if (x + (short)b->image->visWidth <= 0) return 0;

    *outY  = y;
    *blitH = h;
    return 1;
}

// -------------------------------------------------------------------------
// BobDraw: masked blit onto screen, recording position into slot[buf].
//
// Left-edge clip (x < 0): skip the hidden source words. If the cut falls
// mid-word, the blit starts one word left of column 0 (into the previous
// row's last word, or the plane guard on row 0) with a left shift done as
// a right shift of 16-r; the first-word mask zeroes the hidden pixels so
// that extra word is written back unchanged. The last-word mask zeroes the
// bits that would otherwise carry into the next row's first word.
// -------------------------------------------------------------------------
static void BobDraw(Bob *b, Screen *s, UBYTE buf) {
    if (!b->visible) return;
    short dy; UWORD dh; UWORD skip;
    if (!_BobClip(b, s, &dy, &dh, &skip)) return;

    BobImage *img  = b->image;
    WORD  scrWords = (WORD)(s->rowBytes / 2);
    short x        = b->x;
    UWORD shift, srcWord, words;
    WORD  dstWord;
    UWORD fwm = 0xffff;
    UWORD imgWords = _BobWords(img);

    if (x >= 0) {
        shift   = (UWORD)x & 15;
        dstWord = (WORD)(x >> 4);
        srcWord = 0;
        words   = imgWords;
    } else {
        UWORD srcX = (UWORD)(-x);
        UWORD r    = srcX & 15;
        srcWord = srcX >> 4;
        if (srcWord >= imgWords) return;              // trimmed art all left of 0
        words   = imgWords - srcWord;
        if (r) {
            shift   = 16 - r;
            dstWord = -1;
            fwm     = (UWORD)(0xffff >> r);
        } else {
            shift   = 0;
            dstWord = 0;
        }
    }
    if (dstWord + (WORD)words > scrWords)
        words = (UWORD)(scrWords - dstWord);
    if (words == 0) return;
    UWORD lwm = (UWORD)(0xffff << shift);

    WORD srcMod = (WORD)img->rowBytes - (WORD)(words * 2);
    WORD dstMod = (WORD)s->rowBytes   - (WORD)(words * 2);

    UWORD bltcon0_draw = (UWORD)((shift << 12) | 0x0FCA); // D = A·B + ~A·C
    UWORD bltcon0_hole = (UWORD)((shift << 12) | 0x0F0A); // D = ~A·C  (punch hole)
    UWORD bltcon1 = (UWORD)(shift << 12);
    ULONG srcOff  = muluw((UWORD)(skip + img->drawTop), img->rowBytes) + (ULONG)srcWord * 2;
    UBYTE *shapeMsk = img->maskData + srcOff;

    LONG rowOff = (LONG)muluw((UWORD)dy, s->rowBytes) + (LONG)dstWord * 2;   // no __mulsi3
    for (UBYTE p = 0; p < s->depth; p++) {
        UBYTE *scr = s->planes[p] + rowOff;
        SadeWaitBlt();
        custom->bltcon1 = bltcon1;
        custom->bltafwm = fwm;
        custom->bltalwm = lwm;
        custom->bltapt  = (APTR)shapeMsk;
        custom->bltamod = srcMod;
        custom->bltcmod = dstMod;
        custom->bltdmod = dstMod;

        if (p < img->depth && (img->planeMask & (1 << p))) {
            // Plane has image data: cookie-cut blit D = A·B + ~A·C
            UBYTE *pix = img->planeData[p] + srcOff;
            custom->bltcon0 = bltcon0_draw;
            custom->bltbpt  = (APTR)pix;
            custom->bltbmod = srcMod;
            custom->bltcpt  = (APTR)scr;
            custom->bltdpt  = (APTR)scr;
        } else {
            // Plane has no Bob data: punch hole D = ~A·C (zero inside mask shape)
            custom->bltcon0 = bltcon0_hole;
            custom->bltbmod = srcMod; // bltb not used but set mod consistently
            custom->bltcpt  = (APTR)scr;
            custom->bltdpt  = (APTR)scr;
        }
        custom->bltsize = (UWORD)((dh << 6) | words);
    }
    SadeWaitBlt();

    // Record only the on-screen words (the dstWord=-1 spill is unchanged).
    BobSlot *sl  = &b->slot[buf & 1];
    sl->savedX     = (short)(dstWord < 0 ? 0 : dstWord * 16);
    sl->savedY     = dy;
    sl->savedH     = dh;
    sl->savedWords = (UWORD)(dstWord < 0 ? words - 1 : words);
    sl->hasSave    = 1;
}

// -------------------------------------------------------------------------
// _BobRestoreRect: restore a word-aligned rectangle from bg image (or zero).
// x and w must be multiples of 16.
// bg may be NULL — in that case zeroes the rect (solid black background).
// -------------------------------------------------------------------------
#include "sade_ilbm.h"

static void _BobRestoreRect(Screen *s, const IlbmImage *bg,
                             short x, short y, UWORD w, UWORD h) {
    if (x < 0) { if ((UWORD)(-x) >= w) return; w -= (UWORD)(-x); x = 0; }
    if (y < 0) { if ((UWORD)(-y) >= h) return; h -= (UWORD)(-y); y = 0; }
    if (x >= (short)s->width || y >= (short)s->height) return;
    if ((UWORD)x + w > s->width)  w = s->width  - (UWORD)x;
    if ((UWORD)y + h > s->height) h = s->height - (UWORD)y;
    if (w == 0 || h == 0) return;

    UWORD words = w >> 4;
    if (words == 0) return;

    if (!bg) {
        // No background — zero to color 0
        UWORD mod = s->rowBytes - words * 2;
        for (UBYTE p = 0; p < s->depth; p++) {
            UBYTE *dst = s->planes[p] + (ULONG)y * s->rowBytes + ((ULONG)(UWORD)x >> 4) * 2;
            SadeWaitBlt();
            custom->bltcon0 = 0x0100;
            custom->bltcon1 = 0x0000;
            custom->bltafwm = 0xffff;
            custom->bltalwm = 0xffff;
            custom->bltadat = 0x0000;
            custom->bltdpt  = (APTR)dst;
            custom->bltdmod = mod;
            custom->bltsize = (UWORD)((h << 6) | words);
        }
    } else {
        // Restore from background image
        UWORD wordOff = (UWORD)x >> 4;
        UBYTE depth = bg->depth < s->depth ? bg->depth : s->depth;
        WORD  srcMod = (WORD)bg->rowBytes - (WORD)(words * 2);
        WORD  dstMod = (WORD)s->rowBytes  - (WORD)(words * 2);
        for (UBYTE p = 0; p < depth; p++) {
            UBYTE *src = bg->planes[p] + (ULONG)y * bg->rowBytes + (ULONG)wordOff * 2;
            UBYTE *dst = s->planes[p]  + (ULONG)y * s->rowBytes  + (ULONG)wordOff * 2;
            SadeWaitBlt();
            custom->bltcon0 = 0x09F0; // D = A
            custom->bltcon1 = 0x0000;
            custom->bltafwm = 0xffff;
            custom->bltalwm = 0xffff;
            custom->bltapt  = (APTR)src;
            custom->bltdpt  = (APTR)dst;
            custom->bltamod = srcMod;
            custom->bltdmod = dstMod;
            custom->bltsize = (UWORD)((h << 6) | words);
        }
    }
    SadeWaitBlt();
}

// -------------------------------------------------------------------------
// BobEraseAll: restore each Bob's saved position in slot[buf] from bg.
// bg may be NULL for solid-black background.
// With double buffering buf alternates 0/1 — each slot holds the position
// from 2 frames ago (last time this buffer was drawn into).
// -------------------------------------------------------------------------
static void BobEraseAll(Bob **bobs, UBYTE n, Screen *s, UBYTE buf,
                        const IlbmImage *bg) {
    for (UBYTE i = 0; i < n; i++) {
        Bob *b      = bobs[i];
        BobSlot *sl = &b->slot[buf & 1];
        if (!sl->hasSave) continue;
        UWORD wordOff = (UWORD)sl->savedX >> 4;
        UWORD clearW  = (sl->savedWords + 1) * 16;
        short clearX  = (short)(wordOff * 16);
        _BobRestoreRect(s, bg, clearX, sl->savedY, clearW, sl->savedH);
        sl->hasSave = 0;
    }
}

// -------------------------------------------------------------------------
// BobDrawAll: draw all Bobs back-to-front (last index drawn first).
// Earlier-index Bobs appear on top.
// -------------------------------------------------------------------------
static void BobDrawAll(Bob **bobs, UBYTE n, Screen *s, UBYTE buf) {
    for (UBYTE i = n; i-- > 0; )
        BobDraw(bobs[i], s, buf);
}

// -------------------------------------------------------------------------
// Save-under (BobBack): save the background a Bob is about to cover, draw
// it, and later put exactly those pixels back — for single-buffered
// drawing onto a background that has no clean copy (e.g. a TileBand
// canvas). Cost per Bob: depth small blits to save + depth to restore,
// whatever lies underneath.
//
//   BobBack back;  BobBackAlloc(&back, 32, 24, 5);      // max Bob size
//   each frame:    BobRestore(s, &back);                // last frame's spot
//                  <change the background: tiles, animation ...>
//                  BobDrawSave(bob, s, &back);
//
// With several Bobs, restore in the reverse order of drawing, so overlaps
// come back right. Change the background only between restore and save.
// -------------------------------------------------------------------------
typedef struct {
    UBYTE *data;              // chip RAM, words * 2 * h * depth bytes
    ULONG  size;
    UBYTE *at[BOB_MAX_PLANES];  // top-left saved word in each plane
    UWORD  words, h;          // saved rect
    UWORD  rowBytes;          // of the screen it was saved from
    UBYTE  depth, valid;
} BobBack;

// Room for a Bob of visWidth x height (visible pixels).
static int BobBackAlloc(BobBack *bb, UWORD visWidth, UWORD height, UBYTE depth) {
    UWORD words = (UWORD)((visWidth + 16 + 15) >> 4);
    bb->size  = (ULONG)words * 2 * height * depth;
    bb->data  = (UBYTE*)SadeAlloc(bb->size, MEMF_CHIP);
    bb->valid = 0;
    return bb->data != NULL;
}

static void BobBackFree(BobBack *bb) {
    if (bb->data) SadeFree(bb->data, bb->size);
    bb->data = NULL;
    bb->valid = 0;
}

// The on-screen words BobDraw writes (same clipping as BobDraw).
static UBYTE _BobDrawnRect(const Bob *b, const Screen *s,
                           WORD *word, UWORD *words, short *y, UWORD *h) {
    UWORD skip;
    if (!_BobClip(b, s, y, h, &skip)) return 0;
    WORD scrWords = (WORD)(s->rowBytes / 2);
    WORD w0, n;
    if (b->x >= 0) {
        w0 = (WORD)(b->x >> 4);
        n  = (WORD)_BobWords(b->image);
    } else {
        UWORD srcX = (UWORD)(-b->x);
        w0 = 0;
        n  = (WORD)(_BobWords(b->image) - (srcX >> 4) - ((srcX & 15) ? 1 : 0));
    }
    if (w0 + n > scrWords) n = (WORD)(scrWords - w0);
    if (n <= 0) return 0;
    *word = w0; *words = (UWORD)n;
    return 1;
}

static void _BobBackBlit(BobBack *bb, UBYTE toScreen) {
    WORD scrMod = (WORD)(bb->rowBytes - bb->words * 2);
    UWORD bltsize = (UWORD)((bb->h << 6) | bb->words);
    UBYTE *buf = bb->data;
    ULONG planeBytes = muluw((UWORD)(bb->words * 2), bb->h);
    for (UBYTE p = 0; p < bb->depth; p++, buf += planeBytes) {
        SadeWaitBlt();
        custom->bltcon0 = 0x09F0;                  // D = A
        custom->bltcon1 = 0x0000;
        custom->bltafwm = 0xffff;
        custom->bltalwm = 0xffff;
        custom->bltapt  = (APTR)(toScreen ? buf : bb->at[p]);
        custom->bltdpt  = (APTR)(toScreen ? bb->at[p] : buf);
        custom->bltamod = toScreen ? 0 : scrMod;
        custom->bltdmod = toScreen ? scrMod : 0;
        custom->bltsize = bltsize;
    }
    SadeWaitBlt();
}

// Save what the Bob will cover, then draw it. If the Bob is invisible,
// clipped away, or bigger than the buffer, nothing is saved (and a bigger
// Bob is still drawn — restore can't undo it then).
static void BobDrawSave(Bob *b, Screen *s, BobBack *bb) {
    bb->valid = 0;
    if (!b->visible) return;
    WORD word; UWORD words, h; short y;
    if (_BobDrawnRect(b, s, &word, &words, &y, &h)) {
        UBYTE depth = s->depth;
        if (muluw((UWORD)(words * 2 * depth), h) <= bb->size) {
            bb->words = words; bb->h = h; bb->depth = depth; bb->rowBytes = s->rowBytes;
            LONG off = (LONG)muluw((UWORD)y, s->rowBytes) + (LONG)word * 2;
            for (UBYTE p = 0; p < depth; p++)
                bb->at[p] = s->planes[p] + off;
            _BobBackBlit(bb, 0);
            bb->valid = 1;
        }
    }
    BobDraw(b, s, 0);
}

// Put the saved background back (once; then the save is spent).
static void BobRestore(BobBack *bb) {
    if (!bb->valid) return;
    _BobBackBlit(bb, 1);
    bb->valid = 0;
}
