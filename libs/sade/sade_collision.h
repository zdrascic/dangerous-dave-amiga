#pragma once

#include "sade.h"
#include "sade_bob.h"

// SADE Collision — Bob-vs-Bob tests (AMOS: Col() / Bob Col).
//
// Two tiers, use them together:
//   RectOverlap / BobOverlap — cheap bounding-box broad phase (CPU integer
//                              compares; call freely every frame)
//   BobCollide               — pixel-perfect narrow phase: ANDs the two
//                              Bobs' shape masks over the overlap area.
//                              CPU word reads with bit-window shifts;
//                              early-outs on the first overlapping pixel.
//
// Both Bobs' x/y must be in the SAME coordinate space (screen or world —
// collision only cares that they match). Tile collision is separate and
// already covered by TileFlagsAtPixel() in sade_tilemap.h.
//
//   if (BobCollide(player, coin)) { score += 100; ... }

typedef struct { short x, y, w, h; } SadeRect;

static inline int RectOverlap(const SadeRect *a, const SadeRect *b) {
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}

// Bounding boxes of two Bobs (visWidth x height at their positions).
static inline int BobOverlap(const Bob *a, const Bob *b) {
    return !(a->x + (short)a->image->visWidth  <= b->x ||
             b->x + (short)b->image->visWidth  <= a->x ||
             a->y + (short)a->image->height    <= b->y ||
             b->y + (short)b->image->height    <= a->y);
}

// -------------------------------------------------------------------------
// Internal: 16 mask bits of Bob `b` starting at pixel column `off` of row
// `row` (off may be any bit position; rows are word-aligned and padded, so
// reading one word past the offset word is always in bounds).
// -------------------------------------------------------------------------
static inline UWORD _BobMaskBits(const Bob *b, UWORD row, UWORD off) {
    const UWORD *m = (const UWORD*)(b->image->maskData
                                    + (ULONG)row * b->image->rowBytes);
    UWORD wi = off >> 4;
    UWORD sh = off & 15;
    UWORD w  = m[wi];
    if (sh) w = (UWORD)((w << sh) | (m[wi + 1] >> (16 - sh)));
    return w;
}

// -------------------------------------------------------------------------
// BobCollide — 1 if the two Bobs' shape masks share at least one pixel.
// Runs the bbox test first, so it is safe to call unconditionally.
// -------------------------------------------------------------------------
static int BobCollide(const Bob *a, const Bob *b) {
    if (!BobOverlap(a, b)) return 0;

    // overlap rectangle in the shared coordinate space
    short x0 = a->x > b->x ? a->x : b->x;
    short y0 = a->y > b->y ? a->y : b->y;
    short ax1 = (short)(a->x + a->image->visWidth);
    short bx1 = (short)(b->x + b->image->visWidth);
    short ay1 = (short)(a->y + a->image->height);
    short by1 = (short)(b->y + b->image->height);
    short x1 = ax1 < bx1 ? ax1 : bx1;
    short y1 = ay1 < by1 ? ay1 : by1;

    UWORD w    = (UWORD)(x1 - x0);
    UWORD offA = (UWORD)(x0 - a->x);
    UWORD offB = (UWORD)(x0 - b->x);

    for (short y = y0; y < y1; y++) {
        UWORD rowA = (UWORD)(y - a->y);
        UWORD rowB = (UWORD)(y - b->y);
        for (UWORD dx = 0; dx < w; dx += 16) {
            UWORD bits = (UWORD)(_BobMaskBits(a, rowA, (UWORD)(offA + dx))
                               & _BobMaskBits(b, rowB, (UWORD)(offB + dx)));
            UWORD left = (UWORD)(w - dx);
            if (left < 16)
                bits &= (UWORD)(0xFFFF << (16 - left));
            if (bits) return 1;
        }
    }
    return 0;
}
