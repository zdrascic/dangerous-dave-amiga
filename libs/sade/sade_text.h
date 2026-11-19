#pragma once

#include "sade.h"
#include "sade_ilbm.h"

// SADE Text — bitmap font rendering (AMOS: Text / Print / Pen / Paper).
//
// A Font wraps an IFF sheet of 8-px-wide glyphs laid out left-to-right,
// top-to-bottom in ASCII order from `firstChar` (the standard sheet is
// assets/fonts/font8.iff: 8x8 glyphs, ASCII 32..95, uppercase only —
// lowercase input is mapped onto uppercase at draw time).
//
//   IlbmImage *fsheet = IlbmLoad(fontIff, size);
//   Font *font = FontCreate(fsheet, 8, 8, ' ');
//   TextDraw(font, scr, 8, 4, "SCORE", 15, 0);        // ink 15 on paper 0
//   TextDrawNum(font, scr, 56, 4, score, 6, 15, 0);   // zero-padded
//
// Glyph cells are drawn OPAQUE (ink on paper) with the CPU, one byte per
// row per plane — no blitter, no alignment shifts. That makes HUDs
// trivially correct: redraw the text every frame and the cells overwrite
// whatever was there. x is rounded down to a multiple of 8; any y.
// Cost: ~40 CPU ops per glyph per plane — hundreds of glyphs per frame
// are fine.
//
// The font sheet stays resident (like a TileSet's) — don't IlbmFree it
// while the Font lives. Only plane 0 of the sheet is read: a glyph pixel
// is "ink" wherever plane 0 has a 1 bit.

typedef struct {
    const IlbmImage *sheet;
    UWORD glyphW, glyphH;    // glyphW must be 8
    UWORD perRow;            // glyphs per sheet row
    UWORD count;
    UBYTE firstChar;         // ASCII code of glyph 0 (usually ' ')
} Font;

// -------------------------------------------------------------------------
// FontCreate / FontFree
// -------------------------------------------------------------------------
static Font *FontCreate(const IlbmImage *sheet, UWORD glyphW, UWORD glyphH,
                        UBYTE firstChar)
{
    if (!sheet || glyphW != 8 || glyphH == 0) return NULL;
    if (sheet->height % glyphH) return NULL;

    Font *f = (Font*)AllocMem(sizeof(Font), MEMF_PUBLIC | MEMF_CLEAR);
    if (!f) return NULL;
    f->sheet     = sheet;
    f->glyphW    = glyphW;
    f->glyphH    = glyphH;
    f->perRow    = sheet->width / glyphW;
    f->count     = (UWORD)muluw(f->perRow, sheet->height / glyphH);
    f->firstChar = firstChar;
    return f;
}

static void FontFree(Font *f) {
    if (f) FreeMem(f, sizeof(Font));
}

// -------------------------------------------------------------------------
// Internal: draw one glyph cell (opaque, ink on paper) at byte column bx.
// -------------------------------------------------------------------------
static void _FontGlyph(const Font *f, Screen *s, UWORD idx,
                       short bx, short y, UBYTE ink, UBYTE paper)
{
    const IlbmImage *sh = f->sheet;
    UWORD grow = divuw(idx, f->perRow);
    UWORD gcol = (UWORD)(idx - muluw(grow, f->perRow));
    const UBYTE *src = sh->planes[0]
                     + (ULONG)muluw(grow, f->glyphH) * sh->rowBytes + gcol;

    for (UWORD r = 0; r < f->glyphH; r++) {
        short sy = (short)(y + (short)r);
        if (sy < 0) { src += sh->rowBytes; continue; }
        if (sy >= (short)s->height) break;

        UBYTE m = *src;
        src += sh->rowBytes;
        ULONG off = (ULONG)(UWORD)sy * s->rowBytes + (UWORD)bx;
        for (UBYTE p = 0; p < s->depth; p++) {
            UBYTE v = 0;
            if (ink   & (1 << p)) v |= m;
            if (paper & (1 << p)) v |= (UBYTE)~m;
            s->planes[p][off] = v;
        }
    }
}

// -------------------------------------------------------------------------
// TextDraw — draw a 0-terminated string. x rounds down to a multiple of 8.
// Unknown characters print as blank cells; lowercase maps to uppercase
// when the font has no lowercase glyphs.
// -------------------------------------------------------------------------
static void TextDraw(const Font *f, Screen *s, short x, short y,
                     const char *txt, UBYTE ink, UBYTE paper)
{
    short bx = x >> 3;                        // byte column
    short maxBx = (short)(s->rowBytes);
    for (const char *p = txt; *p; p++, bx++) {
        if (bx < 0 || bx >= maxBx) continue;
        UBYTE c = (UBYTE)*p;
        if (c >= 'a' && c <= 'z' && (UWORD)(c - f->firstChar) >= f->count)
            c -= 32;                          // uppercase-only font (font8)
        UWORD idx = (UWORD)(c - f->firstChar);
        if (c < f->firstChar || idx >= f->count)
            idx = 0;                          // blank (first glyph = space)
        _FontGlyph(f, s, idx, bx, y, ink, paper);
    }
}

// -------------------------------------------------------------------------
// TextDrawNum — unsigned number, zero-padded to `digits`, e.g. "001500".
// Divides with divuw only (no 32-bit library division).
// -------------------------------------------------------------------------
static void TextDrawNum(const Font *f, Screen *s, short x, short y,
                        ULONG value, UBYTE digits, UBYTE ink, UBYTE paper)
{
    char buf[11];
    if (digits > 10) digits = 10;
    buf[digits] = 0;
    for (short i = (short)(digits - 1); i >= 0; i--) {
        // 32/10 divide using two 16-bit DIVUs
        UWORD hi  = (UWORD)(value >> 16);
        UWORD qhi = divuw(hi, 10);
        ULONG lo  = ((ULONG)(UWORD)(hi - muluw(qhi, 10)) << 16)
                  | (UWORD)value;
        UWORD qlo = divuw(lo, 10);
        UBYTE rem = (UBYTE)(lo - muluw(qlo, 10));
        buf[i] = (char)('0' + rem);
        value  = ((ULONG)qhi << 16) | qlo;
    }
    TextDraw(f, s, x, y, buf, ink, paper);
}
