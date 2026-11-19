#pragma once

#include "sade.h"

// Dynamic copper list builder.
//
// The Screen's own copper list (built by ScreenOpen) stays in charge of the
// display setup and keeps working with ScreenFlip/ScreenSetColor. A dynamic
// CopperList holds *effects* (raster bars, splits, sprite multiplexing) and
// is chained after the screen list via COP2LC: CopperAttach patches the
// screen list's END marker into a strobe of COPJMP2, so the copper jumps
// into the dynamic list every frame after the display setup has run.
//
// Usage per frame (double-list to avoid the copper reading a half-built list):
//   CopRestart(cl);
//   CopWait(cl, 0, line); CopSet(cl, COLOR00, rgb); ...
//   CopEnd(cl);
//   CopperAttach(scr, cl);       // swap in the freshly built list
//
// Beam coordinates: CopWait takes raw beam positions. The first visible
// screen row is at beam line 0x2c (PAL) / 0x18 (NTSC) — use CopScreenLine
// to convert a screen row to a beam line.

typedef struct {
    UWORD *list;      // chip RAM instruction buffer
    UWORD  capacity;  // total words allocated
    UWORD  pos;       // current write position in words
    UBYTE  past255;   // 1 once a wait beyond beam line 255 has been emitted
} CopperList;

// Common register offsets for CopSet (offsets from $DFF000)
#define COP_COLOR00  0x0180
#define COP_COLOR(n) (0x0180 + (n) * 2)
#define COP_BPLCON0  0x0100
#define COP_BPLCON1  0x0102
#define COP_BPL1PTH  0x00e0   // plane i: 0x00e0 + i*4
#define COP_SPR0PTH  0x0120   // sprite i: 0x0120 + i*4

// -------------------------------------------------------------------------
// CopperCreate / CopperFree
//   capacityWords includes room for the END marker (2 words are reserved,
//   so CopEnd always succeeds even when the builder guard kicked in).
// -------------------------------------------------------------------------
static CopperList *CopperCreate(UWORD capacityWords) {
    CopperList *cl = (CopperList*)AllocMem(sizeof(CopperList), MEMF_PUBLIC | MEMF_CLEAR);
    if (!cl) return NULL;
    cl->list = (UWORD*)AllocMem((ULONG)capacityWords * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
    if (!cl->list) { FreeMem(cl, sizeof(CopperList)); return NULL; }
    cl->capacity = capacityWords;
    cl->pos      = 0;
    return cl;
}

static void CopperFree(CopperList *cl) {
    if (!cl) return;
    if (cl->list) FreeMem(cl->list, (ULONG)cl->capacity * sizeof(UWORD));
    FreeMem(cl, sizeof(CopperList));
}

// -------------------------------------------------------------------------
// Builder — writes are dropped when the list is full (2 words stay reserved
// for the END marker so a truncated list is still terminated).
// -------------------------------------------------------------------------
static inline UBYTE _CopRoom(CopperList *cl) {
    return cl->pos + 2 <= cl->capacity - 2;
}

static inline void CopRestart(CopperList *cl) {
    cl->pos     = 0;
    cl->past255 = 0;
}

// MOVE instruction: write val to custom register at offset reg
static inline void CopSet(CopperList *cl, UWORD reg, UWORD val) {
    if (!_CopRoom(cl)) return;
    cl->list[cl->pos++] = reg & 0x01fe;
    cl->list[cl->pos++] = val;
}

// MOVE pair for a 32-bit pointer register (BPLxPT, SPRxPT, AUDxLC...)
// regH is the PTH offset; PTL is regH+2.
static inline void CopSetPtr(CopperList *cl, UWORD regH, ULONG addr) {
    CopSet(cl, regH,     (UWORD)(addr >> 16));
    CopSet(cl, regH + 2, (UWORD)addr);
}

// MOVEs that load colours first..first+count-1 from 24-bit $00RRGGBB
// values (an ILBM CMAP) for screen s: one COLOR move per colour on OCS; on
// an AGA screen, per bank of 32 a BPLCON3 bank select, the high nibbles,
// BPLCON3 with LOCT and the low nibbles, and BPLCON3 back to bank 0 at the
// end — 2 moves per colour plus 2 per bank. Indices past the screen's
// colours are skipped.
static void CopSetPalette(CopperList *cl, const Screen *s, UWORD first, UWORD count,
                          const ULONG *rgb) {
    if (first + count > s->numColors) count = first < s->numColors ? (UWORD)(s->numColors - first) : 0;
    if (!s->aga) {
        for (UWORD i = 0; i < count; i++) CopSet(cl, COP_COLOR(first + i), RGB444(rgb[i]));
        return;
    }
    for (UWORD i = 0; i < count; ) {
        UWORD idx = (UWORD)(first + i), bank = (UWORD)(idx >> 5);
        UWORD n = (UWORD)(32 - (idx & 31));                    // to the bank's end
        if (n > count - i) n = (UWORD)(count - i);
        CopSet(cl, 0x0106, (UWORD)(_SADE_BPLCON3 | (bank << 13)));
        for (UWORD k = 0; k < n; k++) CopSet(cl, COP_COLOR((idx + k) & 31), RGB444(rgb[i + k]));
        CopSet(cl, 0x0106, (UWORD)(_SADE_BPLCON3 | (bank << 13) | 0x0200));
        for (UWORD k = 0; k < n; k++) {
            ULONG c = rgb[i + k];
            CopSet(cl, COP_COLOR((idx + k) & 31),
                   (UWORD)(((c >> 8) & 0x0f00) | ((c >> 4) & 0x00f0) | (c & 0x000f)));
        }
        i = (UWORD)(i + n);
    }
    CopSet(cl, 0x0106, _SADE_BPLCON3);
}

// WAIT instruction: wait for beam position >= (x, y).
// The WAIT compare only sees 8 bits of the line number, so waits beyond
// line 255 need the classic wait-for-end-of-line-255 first — emitted once
// per list (waits must be issued in increasing line order).
static void CopWait(CopperList *cl, UWORD x, UWORD y) {
    if (y > 255) {
        if (!cl->past255) {
            if (!_CopRoom(cl)) return;
            cl->list[cl->pos++] = 0xffdf;  // wait for last position of line 255
            cl->list[cl->pos++] = 0xfffe;
            cl->past255 = 1;
        }
        y -= 256;
    }
    if (!_CopRoom(cl)) return;
    cl->list[cl->pos++] = (UWORD)((y << 8) | (x & 0x00fe) | 1);
    cl->list[cl->pos++] = 0xfffe;      // compare all beam bits, ignore blitter
}

// WAIT for blitter finished: beam compare (0,0) is already true, so with
// BFD=0 the copper only waits until the blitter goes idle.
static inline void CopWaitBlt(CopperList *cl) {
    if (!_CopRoom(cl)) return;
    cl->list[cl->pos++] = 0x0001;
    cl->list[cl->pos++] = 0x7ffe;
}

// END marker — the copper halts here until the next vertical blank.
// Always fits: 2 words are reserved by the builder guard.
static inline void CopEnd(CopperList *cl) {
    cl->list[cl->pos++] = 0xffff;
    cl->list[cl->pos++] = 0xfffe;
}

// -------------------------------------------------------------------------
// CopScreenLine — beam line of a screen row (display window top + row)
// -------------------------------------------------------------------------
static inline UWORD CopScreenLine(UWORD row) {
    return (UWORD)((_sade_isPAL ? 0x2c : 0x18) + row);
}

// -------------------------------------------------------------------------
// CopperAttach / CopperDetach
//   Chain a finished dynamic list after a screen's copper list.
//   Attach again each frame to swap in a newly built list (parallax etc.) —
//   COP2LC is only read when the strobe executes, at the top of the frame.
//   The dynamic list MUST be terminated with CopEnd before attaching.
// -------------------------------------------------------------------------
static inline UWORD _CopScreenEndOffset(Screen *s) {
    return s->copEnd;
}

static void CopperAttach(Screen *s, CopperList *cl) {
    UWORD endOff = _CopScreenEndOffset(s);
    custom->cop2lc = (ULONG)cl->list;
    s->copper[endOff]     = 0x008a;    // MOVE to COPJMP2: jump to dynamic list
    s->copper[endOff + 1] = 0x7fff;
}

static void CopperDetach(Screen *s) {
    UWORD endOff = _CopScreenEndOffset(s);
    s->copper[endOff]     = 0xffff;    // restore END marker
    s->copper[endOff + 1] = 0xfffe;
}
