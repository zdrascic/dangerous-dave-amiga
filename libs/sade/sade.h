#pragma once

// SADE - Simple Amiga Development Environment
// Thin hardware abstraction for bare-metal Amiga demos.
// Handles boilerplate; exposes raw pointers so you keep full hardware control.

#include "../../support/gcc8_c_support.h"
#include <proto/exec.h>
#include <proto/graphics.h>
#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>

// -------------------------------------------------------------------------
// Globals — defined in main.c (or whichever TU includes sade_system.h)
// -------------------------------------------------------------------------
extern struct ExecBase        *SysBase;
extern volatile struct Custom *custom;
extern struct GfxBase         *GfxBase;

// -------------------------------------------------------------------------
// Central allocators — the asset headers (sade_ilbm/bob/bank/tilemap) route
// their alloc/free through these so that sade_mem.h pools can capture
// level-scoped data (PoolActivate). With no pool active they are exactly
// AllocMem/FreeMem. Program-lifetime allocations (screens, copper lists,
// audio buffers) stay on AllocMem directly.
// -------------------------------------------------------------------------
typedef APTR (*_SadeAllocHookFn)(ULONG size, ULONG flags);
typedef int  (*_SadeFreeHookFn)(APTR ptr, ULONG size);   // 1 = handled
__attribute__((weak)) _SadeAllocHookFn _sade_allocHook = NULL;  // weak: shared by all .c files
__attribute__((weak)) _SadeFreeHookFn  _sade_freeHook  = NULL;

static APTR SadeAlloc(ULONG size, ULONG flags) {
    if (_sade_allocHook) {
        APTR r = _sade_allocHook(size, flags);
        if (r) return r;                 // pool served it
    }
    return AllocMem(size, flags);        // fallback: the real thing
}

static void SadeFree(APTR ptr, ULONG size) {
    if (!ptr) return;
    if (_sade_freeHook && _sade_freeHook(ptr, size)) return;  // pool memory
    FreeMem(ptr, size);
}

// -------------------------------------------------------------------------
// PAL/NTSC detection
//   VPOSR ($DFF004) bit 5 of the high byte is set on PAL hardware.
//   Read before taking the system (while OS is still running is fine too).
// -------------------------------------------------------------------------
__attribute__((weak)) UBYTE _sade_isPAL = 1; // default PAL; set by SadeInit (weak: shared by all .c files)
__attribute__((weak)) UBYTE _sade_frameHz = 50; // measured display refresh, set by SadeInit
__attribute__((weak)) volatile ULONG _sade_vblCount = 0; // VBLs since SadeInit (level-3 IRQ)
__attribute__((weak)) UBYTE _sade_isAGA = 0; // AGA chipset; set by SadeInit

static inline UBYTE SadeIsPAL(void) {
    // Agnus/Alice ID lives in VPOSR bits 14-8; bit 4 (0x10) is set on NTSC
    // parts: OCS 0x00 PAL / 0x10 NTSC, ECS 8372/8375 0x20-0x21 PAL /
    // 0x30-0x31 NTSC, AGA Alice 0x22-0x23 PAL / 0x32-0x33 NTSC. (Testing
    // 0x20 instead called an NTSC ECS or AGA machine PAL.)
    UWORD id = (UWORD)((*(volatile UWORD*)0xDFF004 >> 8) & 0x7F);
    return (id & 0x10) ? 0 : 1;
}

// SadeIsAGA — 1 on an AGA machine (A1200/A4000/CD32): the Alice ID in
// VPOSR bits 14-8, 0x22/0x23 PAL or 0x32/0x33 NTSC. Lisa always comes with
// Alice, so DENISEID is not read: on an OCS Denise $DFF07C is an open bus,
// and with the native display idle (Workbench on an RTG card, no bitplane
// DMA) that noise reads back stable and could pass for Lisa's 0xF8.
// A plain register read: fine before SadeInit, while the OS still runs.
static inline UBYTE SadeIsAGA(void) {
    UBYTE agnus = (UBYTE)((*(volatile UWORD*)0xDFF004 >> 8) & 0x7f);
    return (agnus & 0x6e) == 0x22;               // 0x22/0x23/0x32/0x33
}

// -------------------------------------------------------------------------
// Hardware wait helpers — declared early, used throughout
// -------------------------------------------------------------------------
// SadeBeamLine — the beam's current line (V8-V0, 0..312 PAL / 0..262 NTSC).
// VPOSR (V8) and VHPOSR (V7-V0) are two word reads: crossing line 255 -> 256 between them
// reads 0 (V8 still clear, V7-V0 already wrapped), a fake end of frame. Read
// VPOSR again and retry when it moved.
static inline UWORD SadeBeamLine(void) {
    volatile UWORD *vposr = (volatile UWORD*)0xDFF004, *vhposr = (volatile UWORD*)0xDFF006;
    UWORD hi, lo;
    do { hi = *vposr; lo = *vhposr; } while ((hi ^ *vposr) & 1);
    return (UWORD)(((hi & 1) << 8) | (lo >> 8));
}

static inline void SadeWaitVbl(void) {
    // Use a safe vblank line: PAL ends ~312, NTSC ends ~262.
    // Waiting for line >= 256 is in vblank on both.
    while (SadeBeamLine() >= 256) {}
    while (SadeBeamLine() < 256) {}
}

static inline void SadeWaitBlt(void) {
    UWORD tst = *(volatile UWORD*)&custom->dmaconr; (void)tst; // A1000 compatibility
    while (*(volatile UWORD*)&custom->dmaconr & (1 << 14)) {}  // wait while busy
}

// -------------------------------------------------------------------------
// ScreenOpen flags
// -------------------------------------------------------------------------
#define SADE_LORES   0
#define SADE_HIRES   1
#define SADE_AGA     2   // OR into the mode: AGA screen at any depth (depth 6-8 imply it)

#define SADE_SINGLE  0
#define SADE_DOUBLE  1

// -------------------------------------------------------------------------
// Screen descriptor
// -------------------------------------------------------------------------
#define SADE_MAX_PLANES 8   // 6-8 need AGA

// Copper list word layout (fixed for all screens):
//   [0..7]   DIWSTRT, DIWSTOP, DDFSTRT, DDFSTOP  (4 reg/val pairs = 8 words)
//   [8..13]  BPLCON0, BPLCON1, BPLCON2            (3 pairs = 6 words)
//   [14..17] BPL1MOD, BPL2MOD                     (2 pairs = 4 words)
//   [18..]   BPLxPTH/PTL pairs, depth*4 words
//   then     FMODE, BPLCON3, BPLCON4              (3 pairs = 6 words)
//   [copColors..] colours: OCS numColors*2 words; AGA per bank of 32 a
//            BPLCON3 bank select, 32 high nibbles, BPLCON3 LOCT, 32 low
//            nibbles (132 words), then BPLCON3 back to bank 0
//   [copEnd] end marker (CopperAttach turns it into a jump)
// FMODE/BPLCON3/BPLCON4 are written on every machine: Workbench on an AGA
// machine may leave 4x fetch or another colour bank behind, and on OCS/ECS
// the writes go nowhere (ECS BPLCON3 ignores the bits set).
#define _SADE_COP_PLANES_OFFSET  18
#define _SADE_COP_BANK_WORDS    132  // AGA: one bank of 32 colours
#define _SADE_BPLCON3            0x0c00   // reset value: bank 0, PF2OF 3
#define _SADE_BPLCON4            0x0011   // reset value: sprites use colours 16-31

typedef struct {
    UBYTE  *planes[SADE_MAX_PLANES];     // current draw buffer bitplanes
    UBYTE  *backPlanes[SADE_MAX_PLANES]; // back buffer (double-buffering only)
    UWORD  *copper;
    UWORD   width;
    UWORD   height;
    UBYTE   depth;
    UWORD   rowBytes;
    UWORD   numColors;
    UBYTE   doubleBuffer;                // 1 if double-buffered
    UBYTE   aga;                         // 1: AGA screen (4x fetch, 24-bit colours)
    UWORD   copColors;                   // copper word offset of the colour block
    UWORD   copEnd;                      // copper word offset of the end marker
    UWORD   copWords;                    // copper list size in words
} Screen;

// -------------------------------------------------------------------------
// Internal: patch copper plane pointers to point at planes[]
// -------------------------------------------------------------------------
static void _ScreenUpdateCopperPlanes(Screen *s) {
    UWORD base = _SADE_COP_PLANES_OFFSET;
    for (UBYTE i = 0; i < s->depth; i++) {
        ULONG addr = (ULONG)s->planes[i];
        s->copper[base + i*4 + 1] = (UWORD)(addr >> 16); // PTH value
        s->copper[base + i*4 + 3] = (UWORD)(addr);       // PTL value
    }
}

// -------------------------------------------------------------------------
// Bitplane allocation with a one-word guard in front: BobDraw's left-edge
// clip starts its blit one word left of column 0, which on row 0 is the
// word before the plane. The blit writes that word back unchanged (mask=0).
// -------------------------------------------------------------------------
// AGA screens fetch 64 bits at a time (FMODE 3), so their planes must be
// 8-byte aligned: AllocMem gives that, and the guard is 8 bytes there.
#define _SADE_PLANE_GUARD 2
#define _ScreenGuard(s) ((s)->aga ? 8 : _SADE_PLANE_GUARD)

static UBYTE *_ScreenAllocPlane(ULONG planeSize, UWORD guard) {
    UBYTE *p = (UBYTE*)AllocMem(planeSize + guard, MEMF_CHIP | MEMF_CLEAR);
    return p ? p + guard : NULL;
}

static void _ScreenFreePlane(UBYTE *plane, ULONG planeSize, UWORD guard) {
    FreeMem(plane - guard, planeSize + guard);
}

// -------------------------------------------------------------------------
// ScreenOpen
//   width      : pixels (multiple of 16; AGA: multiple of 64)
//   height     : scan lines (<= 256 PAL)
//   depth      : bitplanes 1-5, AGA 1-8
//   mode       : SADE_LORES or SADE_HIRES, | SADE_AGA for an AGA screen
//                (depth 6-8 always make one)
//   dblBuf     : SADE_SINGLE or SADE_DOUBLE
//
// An AGA screen (lores only) has up to 256 colours with 8 bits per gun
// (ScreenSetRGB) and fetches its planes 64 bits at a time (FMODE 3): a
// quarter of the chip bus an OCS-style fetch of 8 planes would take, which
// would leave the blitter and the copper no cycles on display lines.
// NULL on a non-AGA machine (SadeInit sets _sade_isAGA).
// -------------------------------------------------------------------------
static Screen *ScreenOpen(UWORD width, UWORD height, UBYTE depth, UBYTE mode, UBYTE dblBuf) {
    UBYTE hires = mode & SADE_HIRES;
    UBYTE aga   = (mode & SADE_AGA) || depth > 5;
    if (depth < 1 || depth > SADE_MAX_PLANES) return NULL;
    if (aga && (!_sade_isAGA || hires || (width & 63))) return NULL;

    Screen *s = (Screen*)AllocMem(sizeof(Screen), MEMF_PUBLIC | MEMF_CLEAR);
    if (!s) return NULL;

    s->width        = width;
    s->height       = height;
    s->depth        = depth;
    s->rowBytes     = width / 8;
    s->aga          = aga;
    s->numColors    = aga ? (UWORD)(1 << depth) : ((1 << depth) > 32 ? 32 : (UWORD)(1 << depth));
    s->doubleBuffer = dblBuf;

    UWORD banks = (UWORD)((s->numColors + 31) >> 5);
    s->copColors = (UWORD)(_SADE_COP_PLANES_OFFSET + depth * 4 + 6);
    s->copEnd    = aga ? (UWORD)(s->copColors + banks * _SADE_COP_BANK_WORDS + 2)
                       : (UWORD)(s->copColors + s->numColors * 2);
    s->copWords  = (UWORD)(s->copEnd + 2);

    ULONG planeSize = (ULONG)s->rowBytes * height;
    UWORD guard = _ScreenGuard(s);

    // allocate front (and optionally back) bitplanes
    for (UBYTE i = 0; i < depth; i++) {
        s->planes[i] = _ScreenAllocPlane(planeSize, guard);
        if (!s->planes[i]) goto fail_planes;

        if (dblBuf) {
            s->backPlanes[i] = _ScreenAllocPlane(planeSize, guard);
            if (!s->backPlanes[i]) {
                _ScreenFreePlane(s->planes[i], planeSize, guard);
                s->planes[i] = NULL;
                goto fail_planes;
            }
        }
    }

    s->copper = (UWORD*)AllocMem((ULONG)s->copWords * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
    if (!s->copper) goto fail_planes;

    // --- build copper list ---
    {
        UWORD *cp = s->copper;

        const UWORD diwX = 0x81;
        const UWORD diwY = _sade_isPAL ? 0x2c : 0x18;                         // PAL=44, NTSC=24
        *cp++ = 0x008e; *cp++ = (diwY << 8) | diwX;                           // DIWSTRT
        *cp++ = 0x0090;
        *cp++ = ((UWORD)((diwY + height) & 0xff) << 8)
              | ((UWORD)((diwX + width - 256) & 0xff));                        // DIWSTOP

        // 4x fetch: blocks of 64 pixels (32 colour clocks). The fetch runs
        // on to the end of the block DDFSTOP falls in, plus one more block,
        // so DDFSTOP sits 8 clocks into the last-but-one block (320 px:
        // $38/$A0, 5 blocks).
        UWORD ddfstrt = hires ? 0x003c : 0x0038;
        UWORD ddfstop = hires ? (ddfstrt + (width / 8) - 8)
                      : aga   ? (ddfstrt + ((width / 64 - 2) * 32) + 8)
                              : (ddfstrt + ((width / 16 - 1) * 8));
        *cp++ = 0x0092; *cp++ = ddfstrt;                                       // DDFSTRT
        *cp++ = 0x0094; *cp++ = ddfstop;                                       // DDFSTOP

        // BPU2-0 in bits 14-12, BPU3 (AGA, 8 planes) in bit 4
        UWORD bplcon0 = (UWORD)((depth & 7) << 12) | (1 << 9) | ((depth & 8) << 1);
        if (hires) bplcon0 |= (1 << 15);
        *cp++ = 0x0100; *cp++ = bplcon0;                                       // BPLCON0
        *cp++ = 0x0102; *cp++ = 0x0000;                                        // BPLCON1
        *cp++ = 0x0104; *cp++ = 0x0000;                                        // BPLCON2

        *cp++ = 0x0108; *cp++ = 0x0000;                                        // BPL1MOD
        *cp++ = 0x010a; *cp++ = 0x0000;                                        // BPL2MOD

        // bitplane pointer slots — register fields written now, values patched below
        for (UBYTE i = 0; i < depth; i++) {
            *cp++ = (UWORD)(0x00e0 + i * 4); *cp++ = 0x0000;                  // BPLxPTH
            *cp++ = (UWORD)(0x00e2 + i * 4); *cp++ = 0x0000;                  // BPLxPTL
        }

        *cp++ = 0x01fc; *cp++ = aga ? 0x0003 : 0x0000;                        // FMODE
        *cp++ = 0x0106; *cp++ = _SADE_BPLCON3;                                 // BPLCON3
        *cp++ = 0x010c; *cp++ = _SADE_BPLCON4;                                 // BPLCON4

        // colors — all black by default
        if (aga) {
            for (UWORD b = 0; b < banks; b++)
                for (UWORD loct = 0; loct <= 0x0200; loct += 0x0200) {
                    *cp++ = 0x0106; *cp++ = (UWORD)(_SADE_BPLCON3 | (b << 13) | loct);
                    for (UWORD i = 0; i < 32; i++) { *cp++ = (UWORD)(0x0180 + i * 2); *cp++ = 0; }
                }
            *cp++ = 0x0106; *cp++ = _SADE_BPLCON3;
        } else {
            for (UWORD i = 0; i < s->numColors; i++) {
                *cp++ = (UWORD)(0x0180 + i * 2);
                *cp++ = 0x0000;
            }
        }

        *cp++ = 0xffff; *cp++ = 0xfffe;                                        // END
    }

    // fill in actual plane addresses
    _ScreenUpdateCopperPlanes(s);

    custom->cop1lc  = (ULONG)s->copper;
    custom->copjmp1 = 0x7fff;
    custom->dmacon  = DMAF_SETCLR | DMAF_MASTER | DMAF_RASTER | DMAF_COPPER | DMAF_BLITTER;

    return s;

fail_planes:
    for (UBYTE i = 0; i < depth; i++) {
        if (s->planes[i])     _ScreenFreePlane(s->planes[i],     planeSize, guard);
        if (s->backPlanes[i]) _ScreenFreePlane(s->backPlanes[i], planeSize, guard);
    }
    FreeMem(s, sizeof(Screen));
    return NULL;
}

// -------------------------------------------------------------------------
// ScreenFlip — double-buffer swap.
// Waits for VBL, points copper at the just-drawn planes (now display),
// swaps planes[] <-> backPlanes[] so next draw goes to the hidden buffer.
// No-op on single-buffered screens.
// -------------------------------------------------------------------------
static void ScreenFlip(Screen *s) {
    if (!s->doubleBuffer) return;

    SadeWaitVbl();

    // swap plane pointers
    for (UBYTE i = 0; i < s->depth; i++) {
        UBYTE *tmp      = s->planes[i];
        s->planes[i]    = s->backPlanes[i];
        s->backPlanes[i] = tmp;
    }

    // copper now points at backPlanes[] (the just-finished frame = display buffer)
    // but after the swap above, backPlanes[] holds the old planes[] (what we just drew)
    // We want copper to show that — so patch copper to backPlanes[] post-swap:
    UWORD base = _SADE_COP_PLANES_OFFSET;
    for (UBYTE i = 0; i < s->depth; i++) {
        ULONG addr = (ULONG)s->backPlanes[i];
        s->copper[base + i*4 + 1] = (UWORD)(addr >> 16);
        s->copper[base + i*4 + 3] = (UWORD)(addr);
    }
}

// -------------------------------------------------------------------------
// ScreenClose
// -------------------------------------------------------------------------
static void ScreenClose(Screen *s) {
    if (!s) return;
    ULONG planeSize = (ULONG)s->rowBytes * s->height;
    UWORD guard = _ScreenGuard(s);
    for (UBYTE i = 0; i < s->depth; i++) {
        if (s->planes[i])     _ScreenFreePlane(s->planes[i],     planeSize, guard);
        if (s->backPlanes[i]) _ScreenFreePlane(s->backPlanes[i], planeSize, guard);
    }
    if (s->copper) FreeMem(s->copper, (ULONG)s->copWords * sizeof(UWORD));
    FreeMem(s, sizeof(Screen));
}

// -------------------------------------------------------------------------
// Colours. ScreenSetColor takes Amiga 12-bit RGB444 ($0RGB), ScreenSetRGB
// 24-bit $00RRGGBB (an ILBM CMAP entry); an OCS screen keeps the top four
// bits of each gun, an AGA screen all eight.
//
// OCS: patches the copper list and the colour register, so it shows at
// once. AGA: patches the copper list only, shown from the next frame — an
// AGA colour write is three writes with BPLCON3 between them, which could
// interleave with the copper's own palette writes.
// -------------------------------------------------------------------------
// copper word index of colour `index`'s value (AGA: its high nibbles; the
// low nibbles are 66 words further)
static inline UWORD _ScreenColorPos(const Screen *s, UWORD index) {
    if (!s->aga) return (UWORD)(s->copColors + index * 2 + 1);
    return (UWORD)(s->copColors + (index >> 5) * _SADE_COP_BANK_WORDS + 2 + (index & 31) * 2 + 1);
}

static inline UWORD RGB444(ULONG rgb) {
    return (UWORD)(((rgb >> 12) & 0x0f00) | ((rgb >> 8) & 0x00f0) | ((rgb >> 4) & 0x000f));
}

static void ScreenSetRGB(Screen *s, UWORD index, ULONG rgb) {
    if (index >= s->numColors) return;
    UWORD pos = _ScreenColorPos(s, index);
    UWORD hi  = RGB444(rgb);
    s->copper[pos] = hi;
    if (s->aga)
        s->copper[pos + 66] = (UWORD)(((rgb >> 8) & 0x0f00) | ((rgb >> 4) & 0x00f0) | (rgb & 0x000f));
    else
        custom->color[index] = hi;
}

static void ScreenSetColor(Screen *s, UWORD index, UWORD rgb) {
    if (index >= s->numColors) return;
    UWORD pos = _ScreenColorPos(s, index);
    s->copper[pos] = rgb;
    if (s->aga) s->copper[pos + 66] = rgb;    // low nibbles = high: $0RGB -> $RRGGBB
    else        custom->color[index] = rgb;
}

// -------------------------------------------------------------------------
// ScreenGetColor / ScreenGetRGB — read a colour back from the copper list
// -------------------------------------------------------------------------
static UWORD ScreenGetColor(Screen *s, UWORD index) {
    if (index >= s->numColors) return 0;
    return s->copper[_ScreenColorPos(s, index)];
}

static ULONG ScreenGetRGB(Screen *s, UWORD index) {
    if (index >= s->numColors) return 0;
    UWORD pos = _ScreenColorPos(s, index);
    UWORD hi = s->copper[pos], lo = s->aga ? s->copper[pos + 66] : hi;
    return ((ULONG)(hi & 0x0f00) << 12) | ((ULONG)(lo & 0x0f00) << 8)
         | ((ULONG)(hi & 0x00f0) << 8)  | ((ULONG)(lo & 0x00f0) << 4)
         | ((ULONG)(hi & 0x000f) << 4)  | (lo & 0x000f);
}
