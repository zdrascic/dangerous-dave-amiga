#pragma once

// SADE Input — mouse and joystick polling.
// Call InputUpdate() once per frame after VBL to refresh mouse position.
// Then read mouseX/mouseY and the inline button/direction functions.
//
// Port assignment (standard Amiga):
//   Port 1 (right, DB9) — mouse.  JOY0DAT $DFF00A, buttons via CIA $BFE001.
//   Port 2 (left,  DB9) — joystick player 1.  JOY1DAT $DFF00C.
//
// Joystick direction decode (Amiga Hardware Reference Manual, JOYxDAT):
//   A digital stick feeds the mouse quadrature decoder, so directions are
//   recovered from the counter bits:
//     Left  = bit9  (Y1)
//     Right = bit1  (X1)
//     Up    = bit9 XOR bit8  (Y1 ^ Y0)
//     Down  = bit1 XOR bit0  (X1 ^ X0)
//   (asm equivalent: d1 = dat ^ (dat >> 1); up = btst #8, down = btst #0)

#include "sade.h"

// -------------------------------------------------------------------------
// Mouse absolute position — updated by InputUpdate()
// Defined here with __attribute__((weak)) so multiple translation units can
// include this header without a "multiple definition" link error, while a
// plain `short mouseX;` still works from a single-TU program.
// -------------------------------------------------------------------------
__attribute__((weak)) short mouseX = 0;
__attribute__((weak)) short mouseY = 0;

static UBYTE _prevMouseRawX = 0;
static UBYTE _prevMouseRawY = 0;

// Call once per frame (after VBL) to accumulate mouse delta into mouseX/Y.
// screenW/H are used to clamp to screen bounds.
static void InputUpdate(short screenW, short screenH) {
    UWORD joy0dat = *(volatile UWORD*)0xDFF00A;
    UBYTE rawX    = (UBYTE)(joy0dat & 0xff);
    UBYTE rawY    = (UBYTE)(joy0dat >> 8);

    // counters are free-running — cast delta to signed BYTE to get -128..+127
    mouseX += (BYTE)(rawX - _prevMouseRawX);
    mouseY += (BYTE)(rawY - _prevMouseRawY);
    _prevMouseRawX = rawX;
    _prevMouseRawY = rawY;

    if (mouseX < 0)        mouseX = 0;
    if (mouseY < 0)        mouseY = 0;
    if (mouseX >= screenW) mouseX = screenW - 1;
    if (mouseY >= screenH) mouseY = screenH - 1;
}

// -------------------------------------------------------------------------
// Mouse pointer sprite — uses hardware sprite 0.
// Call MouseShow(scr) after ScreenOpen to display an arrow cursor.
// Call MouseHide() to blank it.
// Call MouseUpdateSprite(scr) once per frame after InputUpdate().
//
// Sprite 0 uses palette entries COLOR17 (body) and COLOR18 (outline).
// SPRxPOS/SPRxCTL encode VSTART, VSTOP, HSTART each frame.
// -------------------------------------------------------------------------

#define _SADE_SPRITE_LINES 16

// Sprite image: 16 lines, dataa=white body, datab=black outline.
// pixel=00 transparent, 01=COLOR17 (white), 10=COLOR18 (black), 11=both
// Cross/crosshair: vertical bar in centre column 7, horizontal bar on row 7.
// Centre gap of 2 pixels keeps the hotspot clear.
// bit15=col0 ... bit0=col15
static const UWORD _sade_spriteImage[_SADE_SPRITE_LINES * 2] = {
    // dataa (white),  datab (black outline)
    0x0100, 0x0380, // col 7 only                 ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0000, 0x0000, // gap row                     (transparent)
    0x7f7e, 0xff7f, // horizontal bar, skip col 7  XXXXXXX . XXXXXXX
    0x0000, 0x0000, // gap row                     (transparent)
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0100, 0x0380, //                             ........ X .......
    0x0000, 0x0000, // blank
};

// Chip RAM buffer: pos word + ctl word + 16*(dataa+datab) + end marker (2 words)
// Total = 2 + 32 + 2 = 36 words = 72 bytes.
#define _SADE_SPRITE_BUF_WORDS  (2 + _SADE_SPRITE_LINES*2 + 2)

static UWORD *_sade_spriteBuf  = 0; // allocated in chip RAM by MouseShow
static UWORD  _sade_nullSprite[4] = { 0, 0, 0, 0 }; // blank end-marker for sprites 1-7
static UBYTE  _sade_spriteVisible = 0;

// Patch pos/ctl words in chip RAM buffer and update sprpt — call each frame.
static void MouseUpdateSprite(Screen *s) {
    if (!_sade_spriteVisible || !_sade_spriteBuf) return;

    // HSTART: sprite hpos = display left edge + mouseX.
    // DIWSTRT horizontal = 0x81, sprite hpos unit = 1 color clock = 2 lores pixels.
    // So hstart = 0x81 + mouseX (lores column → color clock offset from left).
    UWORD hstart = (UWORD)(0x81 + mouseX);

    UWORD diwY   = _sade_isPAL ? 0x2c : 0x18;
    UWORD vstart = (UWORD)(diwY + mouseY);
    UWORD vstop  = vstart + _SADE_SPRITE_LINES;

    // SPRxPOS: [15:8]=VSTART[7:0], [7:0]=HSTART[8:1]
    // SPRxCTL: [15:8]=VSTOP[7:0],  [2]=VSTART[8], [1]=VSTOP[8], [0]=HSTART[0]
    _sade_spriteBuf[0] = (UWORD)(((vstart & 0xff) << 8) | ((hstart >> 1) & 0xff));
    _sade_spriteBuf[1] = (UWORD)(((vstop  & 0xff) << 8)
                       | (((vstart >> 8) & 1) << 2)
                       | (((vstop  >> 8) & 1) << 1)
                       | ( (hstart       & 1)      ));

    // re-arm sprite DMA pointer each frame (copper reloads it at top of frame)
    custom->sprpt[0] = (APTR)_sade_spriteBuf;
    (void)s;
}

static void MouseShow(Screen *s) {
    if (!_sade_spriteBuf) {
        _sade_spriteBuf = (UWORD*)AllocMem(_SADE_SPRITE_BUF_WORDS * 2, MEMF_CHIP | MEMF_CLEAR);
        if (!_sade_spriteBuf) return;

        // copy image data into chip RAM buffer (words 2..33), leave end marker zero
        for (int i = 0; i < _SADE_SPRITE_LINES * 2; i++)
            _sade_spriteBuf[2 + i] = _sade_spriteImage[i];
        // end marker already zero (MEMF_CLEAR)
    }

    _sade_spriteVisible = 1;

    // sprite 0 palette: COLOR17=white body, COLOR18=black outline
    custom->color[17] = 0x0fff;
    custom->color[18] = 0x0000;
    custom->color[19] = 0x0888;

    // point sprites 1-7 at a blank null sprite so they don't show garbage
    for (int i = 1; i < 8; i++)
        custom->sprpt[i] = (APTR)_sade_nullSprite;

    custom->dmacon = DMAF_SETCLR | DMAF_SPRITE;

    MouseUpdateSprite(s);
}

static void MouseHide(void) {
    _sade_spriteVisible = 0;
    // point sprite 0 at null sprite — DMA will load blank pos/ctl and stop
    custom->sprpt[0] = (APTR)_sade_nullSprite;
}

// -------------------------------------------------------------------------
// Mouse buttons  (port 1, right port)
// CIAAPRA $BFE001: bit6 = /LMB, bit7 = /RMB
// -------------------------------------------------------------------------
static inline short MouseLeft()  { return !((*(volatile UBYTE*)0xBFE001) & (1 << 6)); }
static inline short MouseRight() { return !((*(volatile UWORD*)0xDFF016) & (1 << 10)); }

// -------------------------------------------------------------------------
// Joystick port 2  (left port, player 1 joystick)
// JOY1DAT $DFF00C, fire via CIAAPRA bit 7
// -------------------------------------------------------------------------
static inline short Joy1Up(void)    { UWORD d=*(volatile UWORD*)0xDFF00C; return  (UWORD)((d>>9)^(d>>8)) & 1; }
static inline short Joy1Down(void)  { UWORD d=*(volatile UWORD*)0xDFF00C; return  (UWORD)((d>>1)^(d>>0)) & 1; }
static inline short Joy1Left(void)  { UWORD d=*(volatile UWORD*)0xDFF00C; return  (d >> 9) & 1; }
static inline short Joy1Right(void) { UWORD d=*(volatile UWORD*)0xDFF00C; return  (d >> 1) & 1; }
static inline short Joy1Fire(void)  { return !((*(volatile UBYTE*)0xBFE001) & (1 << 7)); }
// Second button (port 2 pin 9, POTINP DATRY): the pin must be pulled high
// as an output first; the button then pulls it low.
static inline short Joy1Fire2(void) { custom->potgo = 0xff00; return !((*(volatile UWORD*)0xDFF016) & (1 << 14)); }

// -------------------------------------------------------------------------
// Joystick port 1  (right port, second joystick — rarely used with mouse)
// JOY0DAT $DFF00A, fire via CIAAPRA bit 6
// -------------------------------------------------------------------------
static inline short Joy0Up(void)    { UWORD d=*(volatile UWORD*)0xDFF00A; return  (UWORD)((d>>9)^(d>>8)) & 1; }
static inline short Joy0Down(void)  { UWORD d=*(volatile UWORD*)0xDFF00A; return  (UWORD)((d>>1)^(d>>0)) & 1; }
static inline short Joy0Left(void)  { UWORD d=*(volatile UWORD*)0xDFF00A; return  (d >> 9) & 1; }
static inline short Joy0Right(void) { UWORD d=*(volatile UWORD*)0xDFF00A; return  (d >> 1) & 1; }
static inline short Joy0Fire(void)  { return !((*(volatile UBYTE*)0xBFE001) & (1 << 6)); }
