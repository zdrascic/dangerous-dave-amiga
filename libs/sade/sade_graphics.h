#pragma once

// SADE Graphics — CPU/blitter drawing into Screen bitplanes.
// All coordinates are pixels, clipped to screen bounds.
// colorIndex maps to palette register; each bit selects a bitplane.

#include "sade.h"

// -------------------------------------------------------------------------
// Internal: set a single pixel across all planes for the given color index
// -------------------------------------------------------------------------
static inline void _GfxSetPixel(Screen *s, short x, short y, UBYTE colorIndex) {
    if ((UWORD)x >= s->width || (UWORD)y >= s->height) return;
    UWORD byteOffset = (UWORD)y * s->rowBytes + ((UWORD)x >> 3);
    UBYTE bit        = 0x80 >> ((UWORD)x & 7);
    for (UBYTE p = 0; p < s->depth; p++) {
        if (colorIndex & (1 << p))
            s->planes[p][byteOffset] |=  bit;
        else
            s->planes[p][byteOffset] &= ~bit;
    }
}

// -------------------------------------------------------------------------
// GfxClear  — zero all bitplanes using the blitter
// -------------------------------------------------------------------------
static void GfxClear(Screen *s) {
    UWORD wordsPerRow = s->rowBytes / 2;
    for (UBYTE p = 0; p < s->depth; p++) {
        SadeWaitBlt();
        custom->bltcon0 = 0x0100; // minterm 0x00 -> D=0
        custom->bltcon1 = 0x0000;
        custom->bltafwm = 0xffff;
        custom->bltalwm = 0xffff;
        custom->bltadat = 0x0000;
        custom->bltdpt  = (APTR)s->planes[p];
        custom->bltdmod = 0;
        custom->bltsize = (UWORD)((s->height << 6) | wordsPerRow);
    }
    SadeWaitBlt();
}

// -------------------------------------------------------------------------
// GfxDrawLine  — Bresenham line from (x0,y0) to (x1,y1)
// -------------------------------------------------------------------------
static void GfxDrawLine(Screen *s, short x0, short y0, short x1, short y1, UBYTE color) {
    short dx  =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    short dy  = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    short sx  = (x0 < x1) ? 1 : -1;
    short sy  = (y0 < y1) ? 1 : -1;
    short err = dx + dy;

    while (1) {
        _GfxSetPixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        short e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// -------------------------------------------------------------------------
// GfxDrawBox  — hollow rectangle
// -------------------------------------------------------------------------
static void GfxDrawBox(Screen *s, short x0, short y0, short x1, short y1, UBYTE color) {
    GfxDrawLine(s, x0, y0, x1, y0, color); // top
    GfxDrawLine(s, x0, y1, x1, y1, color); // bottom
    GfxDrawLine(s, x0, y0, x0, y1, color); // left
    GfxDrawLine(s, x1, y0, x1, y1, color); // right
}

// -------------------------------------------------------------------------
// GfxFillBox  — solid rectangle using the blitter
// Handles partial first/last word edges via first/last word masks.
// -------------------------------------------------------------------------
static void GfxFillBox(Screen *s, short x0, short y0, short x1, short y1, UBYTE color) {
    if (x0 > x1) { short t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { short t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (short)s->width)  x1 = (short)s->width  - 1;
    if (y1 >= (short)s->height) y1 = (short)s->height - 1;
    if (x0 > x1 || y0 > y1) return;

    UWORD fwm   = 0xffff >> ((UWORD)x0 & 15);
    UWORD lwm   = 0xffff << (15 - ((UWORD)x1 & 15));
    UWORD words = ((UWORD)(x1 >> 4) - (UWORD)(x0 >> 4)) + 1;
    UWORD lines = (UWORD)(y1 - y0 + 1);
    UWORD mod   = s->rowBytes - words * 2;

    // A (no DMA) = all ones, masked at the edge words; C = D = the plane.
    // Set: D = A | C, clear: D = ~A & C, so pixels outside the box stay.
    for (UBYTE p = 0; p < s->depth; p++) {
        UWORD bltcon0 = (color & (1 << p)) ? 0x03fa : 0x030a;
        APTR  dst     = (APTR)(s->planes[p] + (ULONG)(UWORD)y0 * s->rowBytes + ((UWORD)x0 >> 4) * 2);

        SadeWaitBlt();
        custom->bltcon0 = bltcon0;
        custom->bltcon1 = 0x0000;
        custom->bltafwm = fwm;
        custom->bltalwm = lwm;
        custom->bltadat = 0xffff;
        custom->bltcpt  = dst;
        custom->bltdpt  = dst;
        custom->bltcmod = mod;
        custom->bltdmod = mod;
        custom->bltsize = (UWORD)((lines << 6) | words);
    }
    SadeWaitBlt();
}

// -------------------------------------------------------------------------
// GfxDrawCircle  — Bresenham midpoint circle, hollow
// -------------------------------------------------------------------------
static void GfxDrawCircle(Screen *s, short cx, short cy, short r, UBYTE color) {
    short x = 0, y = r, d = 1 - r;
    while (x <= y) {
        _GfxSetPixel(s, cx + x, cy + y, color);
        _GfxSetPixel(s, cx - x, cy + y, color);
        _GfxSetPixel(s, cx + x, cy - y, color);
        _GfxSetPixel(s, cx - x, cy - y, color);
        _GfxSetPixel(s, cx + y, cy + x, color);
        _GfxSetPixel(s, cx - y, cy + x, color);
        _GfxSetPixel(s, cx + y, cy - x, color);
        _GfxSetPixel(s, cx - y, cy - x, color);
        if (d < 0) { d += (x << 1) + 3; }
        else       { d += ((x - y) << 1) + 5; y--; }
        x++;
    }
}

// -------------------------------------------------------------------------
// GfxFillCircle  — filled circle via horizontal line spans
// -------------------------------------------------------------------------
static void GfxFillCircle(Screen *s, short cx, short cy, short r, UBYTE color) {
    short x = 0, y = r, d = 1 - r;
    while (x <= y) {
        GfxDrawLine(s, cx - y, cy + x, cx + y, cy + x, color);
        GfxDrawLine(s, cx - y, cy - x, cx + y, cy - x, color);
        GfxDrawLine(s, cx - x, cy + y, cx + x, cy + y, color);
        GfxDrawLine(s, cx - x, cy - y, cx + x, cy - y, color);
        if (d < 0) { d += (x << 1) + 3; }
        else       { d += ((x - y) << 1) + 5; y--; }
        x++;
    }
}

// -------------------------------------------------------------------------
// GfxFillPoly — filled convex polygon using the Amiga blitter fill mode.
//
// How it works:
//   1. Clear a chip-RAM edge buffer (same dimensions as one screen plane).
//   2. CPU-rasterise each edge with Bresenham, writing exactly one pixel per
//      scanline into the edge buffer — the leftmost pixel where the edge
//      crosses that row. Two crossings per row toggle the blitter's fill
//      state on and off, producing a solid fill.
//   3. Run the blitter in descending inclusive-fill mode (BLTCON1 DESC|IFE)
//      over the edge buffer. The blitter reads each row right-to-left,
//      toggling fill state at each set bit, and writes the filled rows to
//      the destination screen plane with the chosen color minterm.
//      Repeat for each bitplane.
//
// Constraints:
//   - Polygon must be convex (or at least simple / non-self-intersecting).
//   - Max 8 vertices.
//   - edgeBuf must point to a chip-RAM scratch buffer of at least
//     s->rowBytes * s->height bytes, cleared to zero before first call.
//     Use GfxPolyBufAlloc / GfxPolyBufFree to manage it.
//   - Coordinates are clipped to screen bounds.
// -------------------------------------------------------------------------

// Allocate/free the edge buffer in chip RAM.
static UBYTE *GfxPolyBufAlloc(Screen *s) {
    return (UBYTE*)AllocMem((ULONG)s->rowBytes * s->height, MEMF_CHIP | MEMF_CLEAR);
}
static void GfxPolyBufFree(Screen *s, UBYTE *buf) {
    if (buf) FreeMem(buf, (ULONG)s->rowBytes * s->height);
}

// Internal: toggle one pixel in the flat edge buffer.
// XOR instead of OR so that two edges meeting at the same pixel (shared
// vertex at top/bottom of a polygon) cancel out — leaving an even number
// of toggles on that scanline and keeping the blitter fill latch correct.
static inline void _GfxEdgePixel(UBYTE *buf, UWORD rowBytes, short x, short y) {
    UWORD byteOff = (UWORD)y * rowBytes + ((UWORD)x >> 3);
    buf[byteOff] ^= 0x80 >> ((UWORD)x & 7);
}

// Internal: rasterise one edge into the edge buffer.
// Writes one pixel per scanline (at the x position where the edge crosses
// that row), which is exactly what the blitter fill mode needs.
static void _GfxRasterEdge(UBYTE *buf, UWORD rowBytes,
                            short screenW, short screenH,
                            short x0, short y0, short x1, short y1)
{
    // Always walk top-to-bottom so we touch each scanline exactly once.
    if (y0 > y1) {
        short t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    // Clip y range to screen.
    if (y1 < 0 || y0 >= screenH) return;

    short dx  =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    short dy  =  (y1 - y0); // always >= 0 after swap
    if (dy == 0) return;    // horizontal edge — no per-scanline crossing to record
    short sx  = (x0 < x1) ? 1 : -1;
    short err = dx - dy;    // Bresenham with dy as the driving axis

    // Skip rows above screen.
    if (y0 < 0) {
        // Advance Bresenham to y=0 without drawing.
        short skip = -y0;
        // Each row step: x advances by dx/dy (approximately).
        // Fast-forward: step 'skip' times in the y direction.
        // We use the standard Bresenham update but only commit y-steps.
        while (skip > 0 && y0 < 0) {
            short e2 = err << 1;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 <  dx) { err += dx; y0++;  skip--; }
        }
    }

    // Exclude the bottom scanline (y1) — the next edge starts there and will
    // record that vertex itself.  This ensures every shared vertex is toggled
    // exactly once across all edges, keeping the blitter fill latch correct.
    while (y0 < y1 && y0 < screenH) {
        // Clamp x to screen before writing.
        short px = x0;
        if (px < 0) px = 0;
        if (px >= screenW) px = screenW - 1;
        _GfxEdgePixel(buf, rowBytes, px, y0);

        // Advance until next y step (may take multiple x steps).
        while (1) {
            short e2 = err << 1;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 <  dx) { err += dx; y0++; break; }
        }
    }
}

#define GFXPOLY_MAX_VERTS 8

static void GfxFillPoly(Screen *s, UBYTE *edgeBuf,
                         const short *xs, const short *ys, UBYTE numVerts,
                         UBYTE color)
{
    if (numVerts < 3 || numVerts > GFXPOLY_MAX_VERTS) return;

    ULONG bufSize    = (ULONG)s->rowBytes * s->height;
    UWORD wordsPerRow = s->rowBytes / 2;

    // --- Pass 1: clear edge buffer ---
    SadeWaitBlt();
    custom->bltcon0 = 0x0100;           // minterm D=0
    custom->bltcon1 = 0x0000;
    custom->bltafwm = 0xffff;
    custom->bltalwm = 0xffff;
    custom->bltadat = 0x0000;
    custom->bltdpt  = (APTR)edgeBuf;
    custom->bltdmod = 0;
    custom->bltsize = (UWORD)((s->height << 6) | wordsPerRow);
    SadeWaitBlt();

    // --- Pass 2: CPU rasterise all edges into edge buffer ---
    for (UBYTE i = 0; i < numVerts; i++) {
        UBYTE j = (i + 1 < numVerts) ? i + 1 : 0;
        _GfxRasterEdge(edgeBuf, s->rowBytes,
                        (short)s->width, (short)s->height,
                        xs[i], ys[i], xs[j], ys[j]);
    }

    // --- Pass 3a: fill the edge buffer IN PLACE (descending + inclusive
    // fill turns the per-row edge toggles into a solid polygon mask).
    // The fill logic applies to the minterm OUTPUT, so it must run on the
    // clean edge data alone — merging screen content in the same blit
    // would let existing pixels toggle the fill latch.
    ULONG srcEnd = (ULONG)edgeBuf + bufSize - 2;   // descending start = end
    SadeWaitBlt();
    custom->bltcon0 = 0x09F0;                      // D = A
    custom->bltcon1 = 0x000A;                      // DESC | IFE
    custom->bltafwm = 0xffff;
    custom->bltalwm = 0xffff;
    custom->bltapt  = (APTR)srcEnd;
    custom->bltdpt  = (APTR)srcEnd;
    custom->bltamod = 0;
    custom->bltdmod = 0;
    custom->bltsize = (UWORD)((s->height << 6) | wordsPerRow);

    // --- Pass 3b: merge the solid mask into each screen plane (plain
    // ascending blits, A = mask, C = plane read, D = plane write):
    //   color bit set   → D = A + C   (minterm 0xFA: OR the shape in)
    //   color bit clear → D = ~A · C  (minterm 0x0A: punch the shape out)
    // This preserves everything already drawn outside the polygon.
    for (UBYTE p = 0; p < s->depth; p++) {
        UWORD minterm = (color & (1 << p)) ? 0x0BFA : 0x0B0A; // USEA|USEC|USED

        SadeWaitBlt();
        custom->bltcon0 = minterm;
        custom->bltcon1 = 0x0000;
        custom->bltafwm = 0xffff;
        custom->bltalwm = 0xffff;
        custom->bltapt  = (APTR)edgeBuf;
        custom->bltcpt  = (APTR)s->planes[p];
        custom->bltdpt  = (APTR)s->planes[p];
        custom->bltamod = 0;
        custom->bltcmod = 0;
        custom->bltdmod = 0;
        custom->bltsize = (UWORD)((s->height << 6) | wordsPerRow);
    }
    SadeWaitBlt();
}
