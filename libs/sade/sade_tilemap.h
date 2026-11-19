#pragma once

#include "sade.h"
#include "sade_ilbm.h"

// SADE Tile Maps — Stage A: flip-screen rooms.
//
// A TileSet is an IFF spritesheet kept resident in chip RAM (the IlbmImage
// from IlbmLoad — do NOT IlbmFree it while the TileSet lives). Tiles are
// opaque and word-aligned, so they blit straight out of the sheet with
// D = A — no masks, no per-tile copies, no extra chip RAM.
//
// A TileMap is a zero-copy view over a .map file produced by
// tools/tiled2sade.py from a Tiled (mapeditor.org) map: a header, one or
// more layers of 0-based tile indices, and a 256-entry per-tile-type
// attribute table (solid/ladder/hurt/...). The file buffer must stay
// alive for the TileMap's lifetime (EMBED data, or a FileLoad buffer you
// don't FileUnload until TileMapFree).
//
// Rendering model (Stage A): blit a screen-sized "room" of tiles into an
// IlbmImage-compatible canvas (2-3 frames of blitter time — done on room
// entry, not per frame), then:
//   - IlbmBlit(canvas, scr, ...) paints it onto the screen buffers, and
//   - BobEraseAll(bobs, n, scr, buf, canvas) restores Bobs against it,
// so the whole existing Bob pipeline works over a tile background.
//
// Typical setup:
//   IlbmImage *sheet = IlbmLoad(tilesIff, size);
//   TileSet   *ts    = TileSetCreate(sheet, 16, 16);
//   TileMap   *tm    = TileMapLoad(mapData, mapSize, ts);
//   IlbmImage *canvas= TileMapCanvasCreate(ts, 20, 16);       // one screen
//   TileMapRenderRoom(tm, canvas, roomTx, roomTy);            // room entry
//
// Constraints: tile width multiple of 16; tile indices are bytes (<= 256
// tile types); Stage A renders layer 0 (extra layers are game data).

// Tile attribute bits — keep in sync with tools/tiled2sade.py
#define TILE_SOLID   0x01
#define TILE_LADDER  0x02
#define TILE_HURT    0x04
#define TILE_ONEWAY  0x08
#define TILE_EXIT    0x10   // level exit (doors) — game logic, not physics

// -------------------------------------------------------------------------
// Data
// -------------------------------------------------------------------------
typedef struct {
    const IlbmImage *sheet;   // resident tile sheet (chip RAM planes)
    UWORD tileW, tileH;
    UWORD perRow;             // tiles per sheet row
    UWORD count;              // total tiles on the sheet
} TileSet;

typedef struct {
    const TileSet *ts;
    const UBYTE   *cells;     // layer 0, mapW*mapH bytes, row-major
    const UBYTE   *tileFlags; // 256-entry attribute table
    UWORD mapW, mapH;         // in tiles
    UBYTE numLayers;
} TileMap;

// -------------------------------------------------------------------------
// TileSetCreate / TileSetFree
//   The sheet stays owned by the caller and must outlive the TileSet.
// -------------------------------------------------------------------------
static TileSet *TileSetCreate(const IlbmImage *sheet, UWORD tileW, UWORD tileH) {
    if (!sheet || tileW == 0 || tileH == 0) return NULL;
    if (tileW & 15) return NULL;               // blitter word constraint
    if (sheet->width % tileW || sheet->height % tileH) return NULL;

    TileSet *ts = (TileSet*)SadeAlloc(sizeof(TileSet), MEMF_PUBLIC | MEMF_CLEAR);
    if (!ts) return NULL;
    ts->sheet  = sheet;
    ts->tileW  = tileW;
    ts->tileH  = tileH;
    ts->perRow = sheet->width / tileW;
    ts->count  = (UWORD)muluw(ts->perRow, sheet->height / tileH);
    return ts;
}

static void TileSetFree(TileSet *ts) {
    if (ts) SadeFree(ts, sizeof(TileSet));
}

// -------------------------------------------------------------------------
// TileMapLoad — zero-copy view over "SMAP" bytes (see tools/tiled2sade.py).
// Validates the header against the file size and the TileSet's tile size.
// TileMapFree releases the struct only, never the data.
// -------------------------------------------------------------------------
static TileMap *TileMapLoad(const void *data, ULONG size, const TileSet *ts) {
    const UBYTE *d = (const UBYTE*)data;
    if (!d || size < 12) return NULL;
    if (d[0] != 'S' || d[1] != 'M' || d[2] != 'A' || d[3] != 'P') return NULL;

    UWORD mapW = (UWORD)((d[4] << 8) | d[5]);
    UWORD mapH = (UWORD)((d[6] << 8) | d[7]);
    UBYTE tileW = d[8], tileH = d[9], numLayers = d[10];
    ULONG cellBytes = (ULONG)mapW * mapH * numLayers;
    if (mapW == 0 || mapH == 0 || numLayers == 0) return NULL;
    if (size < 12 + cellBytes + 256) return NULL;
    if (ts && (ts->tileW != tileW || ts->tileH != tileH)) return NULL;

    TileMap *tm = (TileMap*)SadeAlloc(sizeof(TileMap), MEMF_PUBLIC | MEMF_CLEAR);
    if (!tm) return NULL;
    tm->ts        = ts;
    tm->mapW      = mapW;
    tm->mapH      = mapH;
    tm->numLayers = numLayers;
    tm->cells     = d + 12;
    tm->tileFlags = d + 12 + cellBytes;
    return tm;
}

static void TileMapFree(TileMap *tm) {
    if (tm) SadeFree(tm, sizeof(TileMap));
}

// Extra layers (1..numLayers-1) as raw cell arrays — game data, not drawn.
static inline const UBYTE *TileMapLayer(const TileMap *tm, UBYTE layer) {
    if (layer >= tm->numLayers) return NULL;
    return tm->cells + (ULONG)layer * tm->mapW * tm->mapH;
}

// -------------------------------------------------------------------------
// Queries. Out-of-map tiles read as tile 0 but their flags read as
// TILE_SOLID — the world edge is a wall by default.
// -------------------------------------------------------------------------
static inline UBYTE TileAt(const TileMap *tm, UWORD tx, UWORD ty) {
    if (tx >= tm->mapW || ty >= tm->mapH) return 0;
    return tm->cells[(ULONG)ty * tm->mapW + tx];
}

static inline UBYTE TileFlags(const TileMap *tm, UWORD tx, UWORD ty) {
    if (tx >= tm->mapW || ty >= tm->mapH) return TILE_SOLID;
    return tm->tileFlags[tm->cells[(ULONG)ty * tm->mapW + tx]];
}

// World-pixel query — the collision workhorse:
//   if (TileFlagsAtPixel(tm, wx + 15, wy + 15) & TILE_SOLID) ...
static inline UBYTE TileFlagsAtPixel(const TileMap *tm, LONG wx, LONG wy) {
    if (wx < 0 || wy < 0) return TILE_SOLID;
    return TileFlags(tm, divuw((ULONG)wx, tm->ts->tileW),
                         divuw((ULONG)wy, tm->ts->tileH));
}

// -------------------------------------------------------------------------
// Internal: blit tile n from the sheet into an arbitrary plane set
// (screen draw buffer or canvas). D = A copy for the sheet's planes;
// destination planes beyond the sheet depth are cleared.
// dstX must be a multiple of 16; no clipping (callers stay in bounds).
// -------------------------------------------------------------------------
static void _TileBlitRaw(const TileSet *ts, UWORD n,
                         UBYTE *const *dstPlanes, UWORD dstRowBytes,
                         UBYTE dstDepth, UWORD dstX, UWORD dstY)
{
    const IlbmImage *sh = ts->sheet;
    if (n >= ts->count) n = 0;

    UWORD row  = divuw(n, ts->perRow);
    UWORD col  = (UWORD)(n - muluw(row, ts->perRow));
    UWORD srcX = (UWORD)muluw(col, ts->tileW);
    UWORD srcY = (UWORD)muluw(row, ts->tileH);

    UWORD words   = ts->tileW >> 4;
    WORD  srcMod  = (WORD)(sh->rowBytes - words * 2);
    WORD  dstMod  = (WORD)(dstRowBytes  - words * 2);
    UWORD bltsize = (UWORD)((ts->tileH << 6) | words);
    ULONG dstOff  = muluw(dstY, dstRowBytes) + (dstX >> 4) * 2;      // muluw: no __mulsi3
    ULONG srcOff  = muluw(srcY, sh->rowBytes) + (srcX >> 4) * 2;

    UBYTE copyDepth = sh->depth < dstDepth ? sh->depth : dstDepth;

    for (UBYTE p = 0; p < copyDepth; p++) {
        SadeWaitBlt();
        custom->bltcon0 = 0x09F0;               // D = A
        custom->bltcon1 = 0x0000;
        custom->bltafwm = 0xffff;
        custom->bltalwm = 0xffff;
        custom->bltapt  = (APTR)(sh->planes[p] + srcOff);
        custom->bltdpt  = (APTR)(dstPlanes[p] + dstOff);
        custom->bltamod = srcMod;
        custom->bltdmod = dstMod;
        custom->bltsize = bltsize;
    }
    for (UBYTE p = copyDepth; p < dstDepth; p++) {
        SadeWaitBlt();                          // clear planes the sheet lacks
        custom->bltcon0 = 0x0100;               // D = 0 (A data constant)
        custom->bltcon1 = 0x0000;
        custom->bltafwm = 0xffff;
        custom->bltalwm = 0xffff;
        custom->bltadat = 0x0000;
        custom->bltdpt  = (APTR)(dstPlanes[p] + dstOff);
        custom->bltdmod = dstMod;
        custom->bltsize = bltsize;
    }
    SadeWaitBlt();
}

// -------------------------------------------------------------------------
// TileBlit — blit tile n onto the screen's current draw buffer.
// x must be a multiple of 16; the whole tile must fit on screen.
// Handy for HUDs and dirty-tile repairs, not just maps.
// -------------------------------------------------------------------------
static void TileBlit(const TileSet *ts, Screen *s, UWORD n, short x, short y) {
    if (x < 0 || y < 0) return;
    if ((UWORD)x + ts->tileW > s->width || (UWORD)y + ts->tileH > s->height)
        return;
    _TileBlitRaw(ts, n, s->planes, s->rowBytes, s->depth,
                 (UWORD)x & ~15u, (UWORD)y);
}

// -------------------------------------------------------------------------
// TileMapCanvasCreate — an offscreen, IlbmImage-compatible room buffer of
// wTiles x hTiles tiles (chip RAM planes, sheet depth + palette). Free it
// with IlbmFree. Sized like the screen it will be blitted to, it plugs
// straight into IlbmBlit and BobEraseAll.
// -------------------------------------------------------------------------
static IlbmImage *TileMapCanvasCreate(const TileSet *ts,
                                      UWORD wTiles, UWORD hTiles)
{
    IlbmImage *img = (IlbmImage*)SadeAlloc(sizeof(IlbmImage),
                                          MEMF_PUBLIC | MEMF_CLEAR);
    if (!img) return NULL;

    img->width    = (UWORD)muluw(wTiles, ts->tileW);
    img->height   = (UWORD)muluw(hTiles, ts->tileH);
    img->depth    = ts->sheet->depth;
    img->rowBytes = img->width >> 3;
    img->numColors = ts->sheet->numColors;
    for (UWORD i = 0; i < ILBM_MAX_COLORS; i++)
        img->palette[i] = ts->sheet->palette[i];

    ULONG planeSize = (ULONG)img->rowBytes * img->height;
    for (UBYTE p = 0; p < img->depth; p++) {
        img->planes[p] = (UBYTE*)SadeAlloc(planeSize, MEMF_CHIP | MEMF_CLEAR);
        if (!img->planes[p]) { IlbmFree(img); return NULL; }
    }
    return img;
}

// -------------------------------------------------------------------------
// TileMapRenderRoom — blit a canvas-full of tiles (layer 0) starting at
// map tile (originTx, originTy). Costs ~2-3 frames of blitter time for a
// 320x256 room — call it on room entry, then IlbmBlit the canvas onto
// both screen buffers. Tiles beyond the map edge render as tile 0.
// -------------------------------------------------------------------------
static void TileMapRenderRoom(const TileMap *tm, IlbmImage *canvas,
                              UWORD originTx, UWORD originTy)
{
    const TileSet *ts = tm->ts;
    UWORD cols = canvas->width  / ts->tileW;
    UWORD rows = canvas->height / ts->tileH;

    UWORD dstY = 0;
    for (UWORD r = 0; r < rows; r++, dstY += ts->tileH) {
        UWORD dstX = 0;
        for (UWORD c = 0; c < cols; c++, dstX += ts->tileW) {
            UWORD n = TileAt(tm, (UWORD)(originTx + c), (UWORD)(originTy + r));
            _TileBlitRaw(ts, n, canvas->planes, canvas->rowBytes,
                         canvas->depth, dstX, dstY);
        }
    }
}

// =========================================================================
// Stage B — hardware scrolling (AMOS: Screen Offset)
// -------------------------------------------------------------------------
// The whole map (layer 0) is pre-rendered once into a canvas as wide as
// the map itself; scrolling is then pure register math — zero blits per
// frame. The display window slides over the wide canvas using:
//
//   - bitplane pointers   : coarse scroll, 16 px per word step
//   - BPLCON1             : fine scroll, 0-15 px pixel delay
//   - BPLxMOD             : skip the canvas bytes outside the display
//   - DDFSTRT pulled 8    : fetch one extra word so the fine scroller
//                           always has data to shift in
//
// TileScrollerAttach patches these into the Screen's own copper list (the
// documented fixed layout), so palette, copper effects (CopperAttach) and
// ScreenSetColor all keep working. Call TileScrollerSetX once per frame
// right after SadeWaitVbl().
//
// Constraints (Stage B):
//   - lores, SADE_SINGLE screens (ScreenFlip would fight the pointers)
//   - map height (tiles*tileH) must equal the screen height
//   - the screen's own bitplanes are unused while attached (GfxDraw* and
//     Bobs target them, i.e. they are invisible) — this stage is for the
//     scrolling background; Bobs-over-scroll comes later
//   - chip cost: (mapPx/8 + 4) * height * depth bytes, e.g. a 640x256x4
//     map = 86 KB
//
// AGA screens (Screen.aga, 4x fetch) fetch whole 64-pixel blocks from
// 8-byte aligned addresses, so the coarse step is 64 px, BPLCON1 delays
// 0-63 px (its AGA bits 11-10 / 15-14 hold delay bits 5-4 — bits 9-8 /
// 13-12 are the quarter-pixel H1-H0) and DDFSTRT is
// pulled one block (32 clocks) early. The canvas keeps its 16 px left
// margin (canvas x = world x + 16 either way) but its rows are 8-byte
// multiples with at least 64 px of margin on the right. A scroller made
// with TileScrollerCreate gets the AGA layout when the sheet has more than
// 5 planes; Attach refuses a screen of the other kind.
// =========================================================================
typedef struct {
    const TileMap *tm;
    UBYTE *planes[SADE_MAX_PLANES];
    UWORD  rowBytes;      // OCS mapPx/8 + 4 (one margin word each side),
                          // AGA (mapPx/8 + 10) rounded up to 8
    UWORD  heightPx;      // canvas rows: top + map pixels
    UWORD  top;           // blank rows above map row 0
    UBYTE  depth;
    UBYTE  aga;           // AGA canvas layout (see above)
    UWORD  maxScroll;     // map pixels - screen width
    UWORD  savedDDF, savedCon1, savedMod1, savedMod2;
    UBYTE  attached;
} TileScroller;

// DDFSTRT pull (colour clocks) and its fetch unit in bytes: one word on
// OCS, one 64-bit block with 4x fetch.
#define _TILE_PULL(aga)       ((aga) ? 0x20 : 8)
#define _TILE_PULL_BYTES(aga) ((aga) ? 8 : 2)

// Bytes fetched per row: the screen width plus the pulled unit.
static inline UWORD _TileFetchBytes(UBYTE aga, UWORD width) {
    return aga ? (UWORD)((width >> 3) + 8) : (UWORD)(((width >> 4) + 1) * 2);
}

// Where to point the planes (byte offset into a canvas row) and the
// BPLCON1 value that show canvas pixel x + 16 at the display's left edge,
// with the fetch pulled one unit early.
static UWORD _TileScrollPos(UBYTE aga, UWORD x, UWORD *con1) {
    if (!aga) {
        UWORD fine = (UWORD)((16 - (x & 15)) & 15);    // pixel delay
        *con1 = (UWORD)(fine | (fine << 4));           // both playfields
        return (UWORD)(((x + fine) >> 4) * 2);
    }
    UWORD block = (UWORD)((x + 16 + 63) >> 6);         // first block right of x+16
    UWORD d = (UWORD)((block << 6) - (x + 16));        // 0-63 px delay
    UWORD v = (UWORD)((d & 15) | ((d & 48) << 6));     // PF1H5-2, PF1H7-6 (bits 11-10)
    *con1 = (UWORD)(v | (v << 4));                     // and PF2
    return (UWORD)((block - 1) << 3);
}

// 1 if tile n is colour 0 everywhere on the sheet.
static UBYTE _TileIsBlank(const TileSet *ts, UWORD n) {
    const IlbmImage *sh = ts->sheet;
    if (n >= ts->count) n = 0;
    UWORD row = divuw(n, ts->perRow);
    UWORD col = (UWORD)(n - muluw(row, ts->perRow));
    UWORD words = ts->tileW >> 4;
    for (UBYTE p = 0; p < sh->depth; p++) {
        const UWORD *src = (const UWORD*)(sh->planes[p]
            + (ULONG)muluw(row, ts->tileH) * sh->rowBytes + muluw(col, ts->tileW) / 8);
        for (UWORD y = 0; y < ts->tileH; y++, src += sh->rowBytes >> 1)
            for (UWORD w = 0; w < words; w++)
                if (src[w]) return 0;
    }
    return 1;
}

// Pre-render the whole map into the wide canvas, `top` blank rows above
// it. ~1 frame of blitter time per 2 screens of map — do it at load/level
// time.
static TileScroller *_TileScrollerCreate(const TileMap *tm, UWORD top, UBYTE aga) {
    const TileSet *ts = tm->ts;
    TileScroller *sc = (TileScroller*)SadeAlloc(sizeof(TileScroller),
                                               MEMF_PUBLIC | MEMF_CLEAR);
    if (!sc) return NULL;

    UWORD mapPx = (UWORD)muluw(tm->mapW, ts->tileW);
    sc->tm       = tm;
    sc->depth    = ts->sheet->depth;
    sc->top      = top;
    sc->heightPx = (UWORD)(muluw(tm->mapH, ts->tileH) + top);
    sc->aga      = aga;
    sc->rowBytes = aga ? (UWORD)(((mapPx >> 3) + 10 + 7) & ~7) : (UWORD)((mapPx >> 3) + 4);
    sc->maxScroll = mapPx;                    // finalized by Attach

    ULONG planeSize = (ULONG)sc->rowBytes * sc->heightPx;
    for (UBYTE p = 0; p < sc->depth; p++) {
        sc->planes[p] = (UBYTE*)SadeAlloc(planeSize, MEMF_CHIP | MEMF_CLEAR);
        if (!sc->planes[p]) {
            for (UBYTE q = 0; q < p; q++) SadeFree(sc->planes[q], planeSize);
            SadeFree(sc, sizeof(TileScroller));
            return NULL;
        }
    }

    // The canvas starts cleared, so tiles that are all colour 0 need no
    // blit — usually most of a map. Checked once per tile type used.
    UBYTE state[256];                         // 0 unknown, 1 blank, 2 drawn
    for (UWORD i = 0; i < 256; i++) state[i] = 0;
    for (UWORD ty = 0; ty < tm->mapH; ty++)
        for (UWORD tx = 0; tx < tm->mapW; tx++) {
            UBYTE n = TileAt(tm, tx, ty);
            if (!state[n]) state[n] = _TileIsBlank(ts, n) ? 1 : 2;
            if (state[n] == 1) continue;
            _TileBlitRaw(ts, n, sc->planes, sc->rowBytes,
                         sc->depth,
                         (UWORD)(16 + muluw(tx, ts->tileW)),
                         (UWORD)(muluw(ty, ts->tileH) + top));
        }
    return sc;
}

static TileScroller *TileScrollerCreate(const TileMap *tm) {
    return _TileScrollerCreate(tm, 0, tm->ts->sheet->depth > 5);
}

static void TileScrollerFree(TileScroller *sc) {
    if (!sc) return;
    ULONG planeSize = (ULONG)sc->rowBytes * sc->heightPx;
    for (UBYTE p = 0; p < sc->depth; p++)
        if (sc->planes[p]) SadeFree(sc->planes[p], planeSize);
    SadeFree(sc, sizeof(TileScroller));
}

// Point the display at world pixel x (0 .. maxScroll). Call once per
// frame, right after SadeWaitVbl() — the copper picks it up at frame top.
static void TileScrollerSetX(TileScroller *sc, Screen *s, UWORD x) {
    if (x > sc->maxScroll) x = sc->maxScroll;
    UWORD con1;
    UWORD off = _TileScrollPos(sc->aga, x, &con1);

    s->copper[11] = con1;                              // BPLCON1
    for (UBYTE p = 0; p < sc->depth; p++) {
        ULONG addr = (ULONG)sc->planes[p] + off;
        s->copper[_SADE_COP_PLANES_OFFSET + p*4 + 1] = (UWORD)(addr >> 16);
        s->copper[_SADE_COP_PLANES_OFFSET + p*4 + 3] = (UWORD)addr;
    }
}

// Patch the screen's copper list for scrolling and show world x = 0.
// Returns 0 if the screen/scroller combination is unusable.
static int TileScrollerAttach(TileScroller *sc, Screen *s) {
    UWORD mapPx = (UWORD)muluw(sc->tm->mapW, sc->tm->ts->tileW);
    if (s->doubleBuffer || s->depth != sc->depth || s->aga != sc->aga) return 0;
    if (s->height != sc->heightPx || s->width > mapPx) return 0;
    if (s->copper[4] != 0x0092) return 0;              // not the fixed layout

    sc->maxScroll = (UWORD)(mapPx - s->width);
    sc->savedDDF  = s->copper[5];
    sc->savedCon1 = s->copper[11];
    sc->savedMod1 = s->copper[15];
    sc->savedMod2 = s->copper[17];

    UWORD fetchBytes = _TileFetchBytes(sc->aga, s->width);
    s->copper[5]  = (UWORD)(s->copper[5] - _TILE_PULL(sc->aga));   // DDFSTRT one unit early
    s->copper[15] = (UWORD)(sc->rowBytes - fetchBytes);
    s->copper[17] = (UWORD)(sc->rowBytes - fetchBytes);

    sc->attached = 1;
    TileScrollerSetX(sc, s, 0);
    return 1;
}

// Restore the screen's copper list to its normal display.
static void TileScrollerDetach(TileScroller *sc, Screen *s) {
    if (!sc->attached) return;
    s->copper[5]  = sc->savedDDF;
    s->copper[11] = sc->savedCon1;
    s->copper[15] = sc->savedMod1;
    s->copper[17] = sc->savedMod2;
    _ScreenUpdateCopperPlanes(s);
    sc->attached = 0;
}

// =========================================================================
// Stage C — scrolling band (split screen) with Bobs
// -------------------------------------------------------------------------
// Like Stage B, the whole map is pre-rendered once into a wide canvas
// (TileScrollerCreate) and scrolled with plane pointers + BPLCON1, but only
// screen rows [y0, y0 + map height) show it. The rows above and below keep
// showing the Screen's own (currently displayed) planes, so status bars
// drawn there stay put. A chained copper list (sade_copper.h) switches
// plane pointers, modulos and BPLCON1 in the horizontal blank before the
// band's first row and after its last row; DDFSTRT is pulled one word
// early for the whole frame (the status rows fetch from 2 bytes before
// each row, modulo -2, so they look unchanged).
//
// The canvas is the band's only buffer. Draw moving objects straight into
// it through TileBandView() — a Screen-shaped view, canvas x = world x + 16
// — with BobDraw, and repair what they covered with TileBandSetTile. That
// is single-buffered, so do the per-frame work right after SadeWaitVbl()
// (beam >= 256): the band is not displayed again until its first row of the
// next frame (PAL, band at row 16: ~115 lines later).
//
//   TileBand *b = TileBandCreate(tm, scr, 16);   // render + attach
//   each frame, right after SadeWaitVbl():
//     TileBandSetX(b, scr, worldX);
//     <restore tiles under last frame's Bobs, BobDraw into TileBandView(b)>
//   TileBandFree(b, scr);                        // detach + free
//
// TileBandCreateTop(tm, scr, y0, top) puts `top` blank canvas rows above
// map row 0 (the band shows them from screen row y0, the map from y0 +
// top), so Bobs standing on the map's top row stay visible. View y = map
// y + top; TileBandSetTile takes map cells and adds it itself.
//
// AGA screens: 4x fetch takes the band's plane pointers longer to switch
// than a horizontal blank, so the copper turns bitplane DMA off for one
// line at each switch — the band's first row (row 0 of the canvas, blank
// with TileBandCreateTop's `top`) and the first status row under the band
// show colour 0. Canvas layout and scroll steps: see Stage B.
//
// Constraints: lores screen with ScreenOpen's copper layout; top + map
// height in pixels <= screen height - y0; y0 >= 1. Screens may be double-buffered
// (TileBandSetX re-reads the displayed buffer), but status rows are not
// flipped by the band — draw them into the displayed buffer, or both.
// Chip cost: the canvas, (mapPx/8 + 4) * mapPx_height * depth bytes
// (a 100x10 map of 16x16 tiles at depth 5 = 163 KB), plus 168 bytes.
// =========================================================================
#include "sade_copper.h"

typedef struct {
    TileScroller *sc;
    CopperList   *cl;
    Screen        view;          // the canvas as a Screen (BobDraw target)
    UWORD y0, h;                 // band rows on screen
    UWORD maxScroll;             // map pixels - screen width
    UWORD fetchBytes;            // bytes fetched per row (one extra unit)
    UBYTE aga;                   // AGA screen: blank switch lines (see above)
    UWORD posTop, posBand, posCon1, posBot;   // value slots in cl->list
} TileBand;

// Emit a plane-pointer MOVE pair per plane; returns the first slot index.
static UWORD _TileBandPtrs(CopperList *cl, UBYTE depth) {
    UWORD pos = cl->pos;
    for (UBYTE p = 0; p < depth; p++)
        CopSetPtr(cl, (UWORD)(COP_BPL1PTH + p * 4), 0);
    return pos;
}

static void _TileBandPatchPtr(UWORD *list, UWORD pos, UBYTE p, ULONG addr) {
    list[pos + p * 4 + 1] = (UWORD)(addr >> 16);
    list[pos + p * 4 + 3] = (UWORD)addr;
}

static void TileBandSetX(TileBand *b, Screen *s, UWORD x);

static TileBand *TileBandCreateTop(const TileMap *tm, Screen *s, UWORD y0, UWORD top) {
    if (s->copper[4] != 0x0092 || s->copper[16] != 0x010a) return NULL;  // not the fixed layout
    if (s->copper[9] & 0x8000) return NULL;                 // hires
    UWORD h = (UWORD)(muluw(tm->mapH, tm->ts->tileH) + top);
    UWORD mapPx = (UWORD)muluw(tm->mapW, tm->ts->tileW);
    if (y0 < 1 || y0 + h > s->height || mapPx < s->width) return NULL;
    if (tm->ts->sheet->depth != s->depth) return NULL;

    TileBand *b = (TileBand*)SadeAlloc(sizeof(TileBand), MEMF_PUBLIC | MEMF_CLEAR);
    if (!b) return NULL;
    b->sc = _TileScrollerCreate(tm, top, s->aga);
    b->cl = CopperCreate(s->aga ? 160 : 100);
    if (!b->sc || !b->cl) {
        if (b->sc) TileScrollerFree(b->sc);
        if (b->cl) CopperFree(b->cl);
        SadeFree(b, sizeof(TileBand));
        return NULL;
    }
    b->y0 = y0;
    b->h  = h;
    b->maxScroll  = (UWORD)(mapPx - s->width);
    b->fetchBytes = _TileFetchBytes(s->aga, s->width);
    b->aga        = s->aga;

    b->view.width     = (UWORD)(b->sc->rowBytes << 3);
    b->view.height    = h;
    b->view.depth     = s->depth;
    b->view.rowBytes  = b->sc->rowBytes;
    b->view.numColors = s->numColors;
    for (UBYTE p = 0; p < s->depth; p++) b->view.planes[p] = b->sc->planes[p];

    // ---- copper: status rows / band / status rows ----
    CopperList *cl = b->cl;
    UWORD statusMod = (UWORD)(s->rowBytes - b->fetchBytes);       // -2 (AGA -8)
    UWORD bandMod   = (UWORD)(b->sc->rowBytes - b->fetchBytes);
    CopSet(cl, 0x0092, (UWORD)(s->copper[5] - _TILE_PULL(s->aga)));   // DDFSTRT
    CopSet(cl, 0x0108, statusMod);
    CopSet(cl, 0x010a, statusMod);
    b->posTop = _TileBandPtrs(cl, s->depth);

    if (!s->aga) {
        // after the last fetch of the row above the band (DDFSTOP ~0xd8)
        CopWait(cl, 0xd8, (UWORD)(CopScreenLine(y0) - 1));
        b->posBand = _TileBandPtrs(cl, s->depth);
        b->posCon1 = cl->pos;
        CopSet(cl, COP_BPLCON1, 0);
        CopSet(cl, 0x0108, bandMod);
        CopSet(cl, 0x010a, bandMod);

        b->posBot = 0;
        if (y0 + h < s->height) {
            CopWait(cl, 0xd8, (UWORD)(CopScreenLine(y0 + h) - 1));
            b->posBot = _TileBandPtrs(cl, s->depth);
            CopSet(cl, COP_BPLCON1, 0);
            CopSet(cl, 0x0108, statusMod);
            CopSet(cl, 0x010a, statusMod);
        }
    } else {
        // 8 planes = 16 pointer moves: more than fit between the last fetch
        // of one line and the first of the next. Bitplane DMA off for the
        // switch line (its display ended at ~0xe0, the next fetch is at
        // 0x18), so the pointers don't move while they are rewritten.
        UWORD con0 = s->copper[9], off = (UWORD)(con0 & ~0x7010);   // no planes
        CopWait(cl, 0, CopScreenLine(y0));
        CopSet(cl, COP_BPLCON0, off);
        b->posBand = _TileBandPtrs(cl, s->depth);
        b->posCon1 = cl->pos;
        CopSet(cl, COP_BPLCON1, 0);
        CopSet(cl, 0x0108, bandMod);
        CopSet(cl, 0x010a, bandMod);
        CopWait(cl, 0, (UWORD)(CopScreenLine(y0) + 1));
        CopSet(cl, COP_BPLCON0, con0);

        b->posBot = 0;
        if (y0 + h < s->height) {
            CopWait(cl, 0, CopScreenLine(y0 + h));
            CopSet(cl, COP_BPLCON0, off);
            b->posBot = _TileBandPtrs(cl, s->depth);
            CopSet(cl, COP_BPLCON1, 0);
            CopSet(cl, 0x0108, statusMod);
            CopSet(cl, 0x010a, statusMod);
            CopWait(cl, 0, (UWORD)(CopScreenLine(y0 + h) + 1));
            CopSet(cl, COP_BPLCON0, con0);
        }
    }
    CopEnd(cl);

    TileBandSetX(b, s, 0);
    CopperAttach(s, cl);
    return b;
}

static TileBand *TileBandCreate(const TileMap *tm, Screen *s, UWORD y0) {
    return TileBandCreateTop(tm, s, y0, 0);
}

// Show world pixel x (0 .. maxScroll) in the band. Call right after
// SadeWaitVbl(), and again after a ScreenFlip.
static void TileBandSetX(TileBand *b, Screen *s, UWORD x) {
    if (x > b->maxScroll) x = b->maxScroll;
    UWORD con1;
    UWORD off = _TileScrollPos(b->aga, x, &con1);
    UWORD *list = b->cl->list;
    list[b->posCon1 + 1] = con1;

    // AGA: the switch lines are not fetched, so the band starts at canvas
    // row 1 and the status rows under it one row further down
    UWORD pull = _TILE_PULL_BYTES(b->aga);
    UWORD skip = b->aga ? 1 : 0;
    ULONG bandOff = (ULONG)muluw(skip, b->sc->rowBytes) + off;
    ULONG botOff  = (ULONG)muluw((UWORD)(b->y0 + b->h + skip), s->rowBytes) - pull;
    for (UBYTE p = 0; p < s->depth; p++) {
        // the buffer the Screen copper displays right now
        ULONG shown = ((ULONG)s->copper[_SADE_COP_PLANES_OFFSET + p * 4 + 1] << 16)
                    | s->copper[_SADE_COP_PLANES_OFFSET + p * 4 + 3];
        _TileBandPatchPtr(list, b->posTop, p, shown - pull);
        _TileBandPatchPtr(list, b->posBand, p, (ULONG)b->sc->planes[p] + bandOff);
        if (b->posBot) _TileBandPatchPtr(list, b->posBot, p, shown + botOff);
    }
}

// Screen-shaped view of the canvas for BobDraw (canvas x = world x + 16).
static inline Screen *TileBandView(TileBand *b) { return &b->view; }

// Redraw map cell (tx, ty) in the canvas as tile n (pickups, animation,
// repairing what a Bob covered).
static inline void TileBandSetTile(TileBand *b, UWORD tx, UWORD ty, UWORD n) {
    const TileSet *ts = b->sc->tm->ts;
    _TileBlitRaw(ts, n, b->sc->planes, b->sc->rowBytes, b->view.depth,
                 (UWORD)(16 + muluw(tx, ts->tileW)), (UWORD)(muluw(ty, ts->tileH) + b->sc->top));
}

static void TileBandFree(TileBand *b, Screen *s) {
    if (!b) return;
    CopperDetach(s);
    SadeWaitVbl();                 // let the copper finish any frame that
    SadeWaitVbl();                 // still jumped into the band list
    CopperFree(b->cl);
    TileScrollerFree(b->sc);
    SadeFree(b, sizeof(TileBand));
}
