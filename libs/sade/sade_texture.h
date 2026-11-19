#pragma once

#include "sade.h"
#include "sade_math.h"

// -------------------------------------------------------------------------
// Texture descriptor
//   planes[] : planar bitplane data in chip RAM, one pointer per bitplane.
//              Layout matches Screen planes — row-major, rowBytes wide.
//   width/height must be powers of two (enables fast masking instead of mod).
//   depth must match the Screen depth it will be drawn into.
//   rowBytes = width / 8
// -------------------------------------------------------------------------
#define TEX_MAX_PLANES 5

typedef struct {
    UBYTE *planes[TEX_MAX_PLANES];
    UWORD  width;
    UWORD  height;
    UBYTE  depth;
    UWORD  rowBytes;
    UWORD  wMask;    // width  - 1  (for fast U wrap)
    UWORD  hMask;    // height - 1  (for fast V wrap)
    UBYTE  wShift;   // log2(width)  — U >> wShift gives column byte
    UBYTE  hShift;   // log2(height) — V >> hShift gives row
} Texture;

// -------------------------------------------------------------------------
// Render mode — passed to GfxTexTri / GfxTexQuad
// -------------------------------------------------------------------------
#define TEX_AFFINE       0   // linear UV interpolation — fast, slight skew
#define TEX_PERSPECTIVE  1   // per-pixel 1/z correct — slow, perfect
#define TEX_SUBDIVIDE    2   // affine in 16px sub-spans — good quality/cost

// -------------------------------------------------------------------------
// TexAlloc / TexFree
// -------------------------------------------------------------------------
static Texture *TexAlloc(UWORD width, UWORD height, UBYTE depth) {
    Texture *t = (Texture*)SadeAlloc(sizeof(Texture), MEMF_PUBLIC | MEMF_CLEAR);
    if (!t) return 0;

    ULONG planeSize = (ULONG)(width / 8) * height;
    for (UBYTE p = 0; p < depth; p++) {
        t->planes[p] = (UBYTE*)SadeAlloc(planeSize, MEMF_CHIP | MEMF_CLEAR);
        if (!t->planes[p]) {
            for (UBYTE q = 0; q < p; q++) SadeFree(t->planes[q], planeSize);
            SadeFree(t, sizeof(Texture));
            return 0;
        }
    }

    t->width    = width;
    t->height   = height;
    t->depth    = depth;
    t->rowBytes = width / 8;
    t->wMask    = width  - 1;
    t->hMask    = height - 1;

    // compute log2 shifts — only valid for power-of-two dimensions
    UBYTE sh = 0; UWORD v = width;  while (v > 1) { sh++; v >>= 1; } t->wShift = sh;
           sh = 0; v = height; while (v > 1) { sh++; v >>= 1; } t->hShift = sh;

    return t;
}

static void TexFree(Texture *t) {
    if (!t) return;
    ULONG planeSize = (ULONG)t->rowBytes * t->height;
    for (UBYTE p = 0; p < t->depth; p++)
        if (t->planes[p]) SadeFree(t->planes[p], planeSize);
    SadeFree(t, sizeof(Texture));
}

// -------------------------------------------------------------------------
// TexCheckerboard — fill texture with a 2-color checker pattern.
//   colA / colB are palette color indices.
//   tileShift controls checker size: tileShift=3 → 8px tiles.
// -------------------------------------------------------------------------
static void TexCheckerboard(Texture *t, UBYTE colA, UBYTE colB, UBYTE tileShift) {
    for (UWORD y = 0; y < t->height; y++) {
        for (UWORD x = 0; x < t->width; x++) {
            UBYTE check = (UBYTE)(((x >> tileShift) ^ (y >> tileShift)) & 1);
            UBYTE col   = check ? colB : colA;
            UWORD byteOff = y * t->rowBytes + (x >> 3);
            UBYTE bit     = 0x80 >> (x & 7);
            for (UBYTE p = 0; p < t->depth; p++) {
                if (col & (1 << p)) t->planes[p][byteOff] |=  bit;
                else                t->planes[p][byteOff] &= ~bit;
            }
        }
    }
}

// -------------------------------------------------------------------------
// Internal: fetch one texel color index from the texture at (u, v).
// u and v are pixel coords, already wrapped to [0, width/height).
// -------------------------------------------------------------------------
static inline UBYTE _TexFetch(const Texture *t, UWORD u, UWORD v) {
    UWORD byteOff = (UWORD)(v * t->rowBytes) + (u >> 3);
    UBYTE bit     = 0x80 >> (u & 7);
    UBYTE col     = 0;
    for (UBYTE p = 0; p < t->depth; p++)
        if (t->planes[p][byteOff] & bit) col |= (1 << p);
    return col;
}

// -------------------------------------------------------------------------
// Internal: write one texel to the screen at (x, y).
// -------------------------------------------------------------------------
static inline void _TexWritePixel(Screen *s, short x, short y, UBYTE col) {
    if ((UWORD)x >= s->width || (UWORD)y >= s->height) return;
    UWORD byteOff = (UWORD)y * s->rowBytes + ((UWORD)x >> 3);
    UBYTE bit     = 0x80 >> ((UWORD)x & 7);
    for (UBYTE p = 0; p < s->depth; p++) {
        if (col & (1 << p)) s->planes[p][byteOff] |=  bit;
        else                s->planes[p][byteOff] &= ~bit;
    }
}

// -------------------------------------------------------------------------
// Internal: reciprocal lookup for perspective correction.
// Table covers 1/z for z in [1..511], scaled by 65536 (16.16 fixed-point).
// Built once at startup by TexInitRecipTable().
// -------------------------------------------------------------------------
#define _TEX_RECIP_SIZE 512
static ULONG _texRecipTable[_TEX_RECIP_SIZE];
static UBYTE _texRecipReady = 0;

static void TexInitRecipTable(void) {
    if (_texRecipReady) return;
    _texRecipTable[0] = 0xffffffff; // z=0 guard
    for (UWORD z = 1; z < _TEX_RECIP_SIZE; z++)
        _texRecipTable[z] = 65536UL / z;
    _texRecipReady = 1;
}

static inline ULONG _TexRecip(short z) {
    if (z < 1) z = 1;
    if ((UWORD)z < _TEX_RECIP_SIZE) return _texRecipTable[(UWORD)z];
    return 65536UL / (ULONG)(UWORD)z;
}

// -------------------------------------------------------------------------
// Internal: draw one horizontal span [x0..x1) on scanline y,
// sampling the texture with affine UV interpolation.
// u0,v0 and u1,v1 are 16.16 fixed-point texture coordinates.
// -------------------------------------------------------------------------
static void _TexSpanAffine(Screen *s, const Texture *t,
                            short x0, short x1, short y,
                            ULONG u0, ULONG v0, ULONG u1, ULONG v1)
{
    short len = x1 - x0;
    if (len <= 0) return;

    // clip to screen
    if (x0 < 0) {
        ULONG skip = (ULONG)(-x0);
        ULONG du = (u1 - u0) / (ULONG)len;
        ULONG dv = (v1 - v0) / (ULONG)len;
        u0 += du * skip; v0 += dv * skip;
        x0 = 0;
        len = x1 - x0;
        if (len <= 0) return;
    }
    if (x1 > (short)s->width) { x1 = (short)s->width; len = x1 - x0; }
    if (len <= 0) return;

    ULONG du = (u1 - u0) / (ULONG)len;
    ULONG dv = (v1 - v0) / (ULONG)len;
    ULONG u  = u0, v = v0;

    for (short x = x0; x < x1; x++) {
        UWORD tu = (UWORD)(u >> 16) & t->wMask;
        UWORD tv = (UWORD)(v >> 16) & t->hMask;
        _TexWritePixel(s, x, y, _TexFetch(t, tu, tv));
        u += du; v += dv;
    }
}

// -------------------------------------------------------------------------
// Internal: perspective-correct span — compute per-pixel u/z and v/z,
// then multiply by z to recover u, v at each pixel.
// z0/z1 are world-space Z values (not fixed-point), u0..v1 are 16.16.
// -------------------------------------------------------------------------
static void _TexSpanPerspective(Screen *s, const Texture *t,
                                 short x0, short x1, short y,
                                 short z0, short z1,
                                 ULONG u0, ULONG v0, ULONG u1, ULONG v1)
{
    short len = x1 - x0;
    if (len <= 0) return;

    // clip
    if (x0 < 0) { x0 = 0; }
    if (x1 > (short)s->width) x1 = (short)s->width;
    len = x1 - x0;
    if (len <= 0) return;

    // u/z and v/z as 16.16, interpolated linearly
    ULONG r0  = _TexRecip(z0), r1 = _TexRecip(z1);
    // u/z = u * (1/z)  — keep as 32-bit, shift down later
    // We work in 16.16 throughout: uz = (u >> 8) * recip >> 8
    ULONG uz0 = (ULONG)((u0 >> 8) * r0 >> 8);
    ULONG vz0 = (ULONG)((v0 >> 8) * r0 >> 8);
    ULONG uz1 = (ULONG)((u1 >> 8) * r1 >> 8);
    ULONG vz1 = (ULONG)((v1 >> 8) * r1 >> 8);
    ULONG rz0 = r0, rz1 = r1;

    ULONG duz = (uz1 - uz0) / (ULONG)len;
    ULONG dvz = (vz1 - vz0) / (ULONG)len;
    ULONG drz = (rz1 - rz0) / (ULONG)len;

    ULONG uz = uz0, vz = vz0, rz = rz0;

    for (short x = x0; x < x1; x++) {
        // recover u = uz / (1/z) = uz * z — but we have 1/z=rz, so u = uz / rz
        // Both are 16.16; result must be pixel coord in texture space
        UWORD tu, tv;
        if (rz > 0) {
            tu = (UWORD)((uz / (rz >> 8)) >> 8) & t->wMask;
            tv = (UWORD)((vz / (rz >> 8)) >> 8) & t->hMask;
        } else {
            tu = 0; tv = 0;
        }
        _TexWritePixel(s, x, y, _TexFetch(t, tu, tv));
        uz += duz; vz += dvz; rz += drz;
    }
}

// -------------------------------------------------------------------------
// Internal: subdivided span — affine in 16px sub-spans, recalculate
// perspective endpoints at each sub-span boundary.
// -------------------------------------------------------------------------
#define _TEX_SUBDIV 16

static void _TexSpanSubdivide(Screen *s, const Texture *t,
                               short x0, short x1, short y,
                               short z0, short z1,
                               ULONG u0, ULONG v0, ULONG u1, ULONG v1)
{
    short len = x1 - x0;
    if (len <= 0) return;

    ULONG r0 = _TexRecip(z0), r1 = _TexRecip(z1);
    ULONG uz0 = (u0 >> 8) * r0 >> 8;
    ULONG vz0 = (v0 >> 8) * r0 >> 8;
    ULONG uz1 = (u1 >> 8) * r1 >> 8;
    ULONG vz1 = (v1 >> 8) * r1 >> 8;
    ULONG rz0 = r0, rz1 = r1;

    // recover perspective-correct u,v at x0 and walk in sub-spans
    ULONG rz   = rz0;
    ULONG uz   = uz0, vz = vz0;
    ULONG drz  = (rz1 - rz0) / (ULONG)len;
    ULONG duz  = (uz1 - uz0) / (ULONG)len;
    ULONG dvz  = (vz1 - vz0) / (ULONG)len;

    short x = x0;
    while (x < x1) {
        short xe = x + _TEX_SUBDIV;
        if (xe > x1) xe = x1;

        // perspective-correct endpoints of this sub-span
        ULONG rza = rz;
        ULONG uza = uz, vza = vz;
        short steps = xe - x;
        ULONG rzb = rz  + drz  * (ULONG)steps;
        ULONG uzb = uz  + duz  * (ULONG)steps;
        ULONG vzb = vz  + dvz  * (ULONG)steps;

        // recover pixel-space u,v at both ends
        ULONG ua = (rza > 0) ? ((uza / (rza >> 8)) >> 8) : 0;
        ULONG va = (rza > 0) ? ((vza / (rza >> 8)) >> 8) : 0;
        ULONG ub = (rzb > 0) ? ((uzb / (rzb >> 8)) >> 8) : 0;
        ULONG vb = (rzb > 0) ? ((vzb / (rzb >> 8)) >> 8) : 0;

        // affine span between the two corrected endpoints
        _TexSpanAffine(s, t, x, xe, y,
                       ua << 16, va << 16,
                       ub << 16, vb << 16);

        rz += drz * (ULONG)steps;
        uz += duz * (ULONG)steps;
        vz += dvz * (ULONG)steps;
        x = xe;
    }
}

// -------------------------------------------------------------------------
// GfxTexTri — texture-mapped triangle.
//
// Vertices: (x0,y0,z0,u0,v0), (x1,y1,z1,u1,v1), (x2,y2,z2,u2,v2)
// z values are world-space depth (integer), used only for perspective modes.
// u,v are texture pixel coordinates in 16.16 fixed-point:
//   pass  (ULONG)texPixelU << 16  for an integer texel coordinate.
// mode: TEX_AFFINE, TEX_PERSPECTIVE, TEX_SUBDIVIDE
//
// Algorithm: sort vertices by Y, then walk two edges simultaneously
// (left and right), filling horizontal spans between them.
// -------------------------------------------------------------------------
static void GfxTexTri(Screen *s, const Texture *t, UBYTE mode,
                       short x0, short y0, short z0, ULONG u0, ULONG v0,
                       short x1, short y1, short z1, ULONG u1, ULONG v1,
                       short x2, short y2, short z2, ULONG u2, ULONG v2)
{
    // --- sort vertices top-to-bottom by y ---
    #define SWAP3(a,b) { short _t; ULONG _u; \
        _t=x##a;x##a=x##b;x##b=_t; _t=y##a;y##a=y##b;y##b=_t; \
        _t=z##a;z##a=z##b;z##b=_t; \
        _u=u##a;u##a=u##b;u##b=_u; _u=v##a;v##a=v##b;v##b=_u; }
    if (y0 > y1) { SWAP3(0,1) }
    if (y1 > y2) { SWAP3(1,2) }
    if (y0 > y1) { SWAP3(0,1) }
    #undef SWAP3

    short screenH = (short)s->height;
    if (y2 < 0 || y0 >= screenH) return;
    if (y0 == y2) return; // degenerate

    // Fixed-point 16.16 edge walkers for X, Z, U, V along left and right edges
    // We split the triangle at the middle vertex (y1) into top and bottom halves.
    // Long edge: v0→v2 spans the whole triangle.
    // Short edges: v0→v1 (top half), v1→v2 (bottom half).

    short dy02 = y2 - y0; // always > 0
    short dy01 = y1 - y0;
    short dy12 = y2 - y1;

    // Determine which side the long edge is on by comparing x at y1
    // mid-x on long edge at y1:
    short midX = (dy02 > 0)
        ? (short)(x0 + (long)(x2 - x0) * dy01 / dy02)
        : x0;
    #define INITEDGE(xa,ya,xb,yb, ua,va,za, ub,vb,zb, DY) \
        ULONG _idy = (DY) > 0 ? 65536UL/(ULONG)(DY) : 0; \
        ULONG lx  = (ULONG)(xa) << 16; \
        ULONG ldx = (ULONG)((long)((xb)-(xa)) * (long)_idy >> 16) << 16; /* recompute below */ \
        (void)lx; (void)ldx;

    // Simpler: compute all edge deltas directly
    // For each half, we need two edge interpolators running simultaneously.
    // Store as 16.16 current values + per-scanline delta.

    // We iterate top→bottom in two passes (top half, bottom half).
    // Each pass: left edge and right edge, both as 16.16 x, u, v, z interpolants.

    // Helper: given two endpoints (x0y0) and (x1y1), return 16.16 per-scanline deltas
    // Packed into a struct for clarity.
    typedef struct {
        ULONG cx, cu, cv; long cz;
        ULONG dx, du, dv; long dz;
    } EdgeWalk;

    #define EDGE_INIT(e, xa,ya,za,ua,va, xb,yb,zb,ub,vb) do { \
        short _dy = (yb) - (ya); \
        ULONG _idy = (_dy > 0) ? (65536UL / (ULONG)_dy) : 0; \
        (e).cx = (ULONG)(xa) << 16; \
        (e).cu = (ua); (e).cv = (va); (e).cz = (long)(za); \
        (e).dx = (ULONG)((long)((xb)-(xa)) * (long)_idy >> 0); /* 16.16 */ \
        (e).du = (ULONG)((long)((long)(ub)-(long)(ua)) / (_dy > 0 ? _dy : 1)); \
        (e).dv = (ULONG)((long)((long)(vb)-(long)(va)) / (_dy > 0 ? _dy : 1)); \
        (e).dz = (_dy > 0) ? (long)((long)((zb)-(za)) * 65536L / _dy) : 0; \
    } while(0)

    #define EDGE_STEP(e) do { \
        (e).cx += (e).dx; (e).cu += (e).du; (e).cv += (e).dv; (e).cz += (e).dz; \
    } while(0)

    // Determine left/right assignment: long edge is left if midX >= x1, else right
    UBYTE longIsLeft = (midX >= x1);

    EdgeWalk eL, eR, eLong;
    EDGE_INIT(eLong, x0,y0,z0,u0,v0, x2,y2,z2,u2,v2);

    // --- top half: y0..y1 ---
    if (dy01 > 0) {
        EdgeWalk eShort;
        EDGE_INIT(eShort, x0,y0,z0,u0,v0, x1,y1,z1,u1,v1);

        // reset long edge to y0
        EDGE_INIT(eLong, x0,y0,z0,u0,v0, x2,y2,z2,u2,v2);

        if (longIsLeft) { eL = eLong; eR = eShort; }
        else            { eL = eShort; eR = eLong; }

        short yStart = y0 < 0 ? 0 : y0;
        short yEnd   = y1 < screenH ? y1 : screenH;

        // advance to yStart if clipped
        for (short y = y0; y < yStart; y++) {
            EDGE_STEP(eL); EDGE_STEP(eR);
        }

        for (short y = yStart; y < yEnd; y++) {
            short lx = (short)(eL.cx >> 16);
            short rx = (short)(eR.cx >> 16);
            short lz = (short)(eL.cz >> 16);
            short rz = (short)(eR.cz >> 16);
            if (lx > rx) { short t=lx;lx=rx;rx=t; }

            switch (mode) {
                case TEX_AFFINE:
                    _TexSpanAffine(s, t, lx, rx, y, eL.cu, eL.cv, eR.cu, eR.cv);
                    break;
                case TEX_PERSPECTIVE:
                    _TexSpanPerspective(s, t, lx, rx, y, lz, rz, eL.cu, eL.cv, eR.cu, eR.cv);
                    break;
                case TEX_SUBDIVIDE:
                    _TexSpanSubdivide(s, t, lx, rx, y, lz, rz, eL.cu, eL.cv, eR.cu, eR.cv);
                    break;
            }
            EDGE_STEP(eL); EDGE_STEP(eR);
        }
    }

    // --- bottom half: y1..y2 ---
    if (dy12 > 0) {
        EdgeWalk eShort2;
        EDGE_INIT(eShort2, x1,y1,z1,u1,v1, x2,y2,z2,u2,v2);

        // reset long edge and advance it to y1
        EDGE_INIT(eLong, x0,y0,z0,u0,v0, x2,y2,z2,u2,v2);
        for (short y = y0; y < y1; y++) EDGE_STEP(eLong);

        if (longIsLeft) { eL = eLong; eR = eShort2; }
        else            { eL = eShort2; eR = eLong; }

        short yStart = y1 < 0 ? 0 : y1;
        short yEnd   = y2 < screenH ? y2 : screenH;

        for (short y = y1; y < yStart; y++) {
            EDGE_STEP(eL); EDGE_STEP(eR);
        }

        for (short y = yStart; y < yEnd; y++) {
            short lx = (short)(eL.cx >> 16);
            short rx = (short)(eR.cx >> 16);
            short lz = (short)(eL.cz >> 16);
            short rz = (short)(eR.cz >> 16);
            if (lx > rx) { short t=lx;lx=rx;rx=t; }

            switch (mode) {
                case TEX_AFFINE:
                    _TexSpanAffine(s, t, lx, rx, y, eL.cu, eL.cv, eR.cu, eR.cv);
                    break;
                case TEX_PERSPECTIVE:
                    _TexSpanPerspective(s, t, lx, rx, y, lz, rz, eL.cu, eL.cv, eR.cu, eR.cv);
                    break;
                case TEX_SUBDIVIDE:
                    _TexSpanSubdivide(s, t, lx, rx, y, lz, rz, eL.cu, eL.cv, eR.cu, eR.cv);
                    break;
            }
            EDGE_STEP(eL); EDGE_STEP(eR);
        }
    }

    #undef EDGE_INIT
    #undef EDGE_STEP
}

// -------------------------------------------------------------------------
// GfxTexQuad — texture-mapped quad split into two triangles.
//
// Vertices in order: v0(top-left), v1(top-right), v2(bottom-right), v3(bottom-left)
// UV coordinates map the texture corners to quad corners:
//   v0 → (0,0), v1 → (texW,0), v2 → (texW,texH), v3 → (0,texH)
// You can supply custom UVs for sub-texture regions.
// -------------------------------------------------------------------------
static void GfxTexQuad(Screen *s, const Texture *t, UBYTE mode,
                        short x0, short y0, short z0,
                        short x1, short y1, short z1,
                        short x2, short y2, short z2,
                        short x3, short y3, short z3,
                        ULONG u0, ULONG v0,
                        ULONG u1, ULONG v1,
                        ULONG u2, ULONG v2,
                        ULONG u3, ULONG v3)
{
    // Split quad into two triangles: (v0,v1,v2) and (v0,v2,v3)
    GfxTexTri(s, t, mode,
               x0,y0,z0,u0,v0,
               x1,y1,z1,u1,v1,
               x2,y2,z2,u2,v2);
    GfxTexTri(s, t, mode,
               x0,y0,z0,u0,v0,
               x2,y2,z2,u2,v2,
               x3,y3,z3,u3,v3);
}
