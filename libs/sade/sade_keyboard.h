#pragma once

#include "sade.h"

// SADE Keyboard — bare-metal CIA-A keyboard reading (AMOS: Key State).
//
// The keyboard talks to CIA-A's serial port: each keypress/release lands
// one byte in SDR and raises the SP flag in the ICR. The keyboard then
// WAITS for a handshake (KDAT held low >= 85 us) before sending the next
// byte — which is why simple once-per-frame polling is lossless.
//
//   KeyboardUpdate();            // once per frame, after SadeWaitVbl
//   if (KeyDown(RAWKEY_ESC))  ...    // level: held right now
//   if (KeyHit(RAWKEY_SPACE)) ...    // edge: once per press
//
// Codes are Amiga RAW key codes (position-based, layout-independent);
// every key is defined below as RAWKEY_*. No interrupt is installed — this works
// alongside ptplayer's CIA-B interrupt and the SADE VBL stub.
//
// Note: reading the ICR clears all its flags. SADE doesn't use CIA-A for
// anything else, so that is fine — just route all CIA-A access through
// this header.

// -------------------------------------------------------------------------
// RAW key codes — every key of the Amiga keyboard (A500/A600/A1200/A2000/
// A3000/A4000), from the Hardware Reference Manual's keyboard map. Codes
// are positions: names follow the US layout, other layouts print other
// symbols on some keys (QWERTZ swaps Y and Z, see RAWKEY_Y).
// -------------------------------------------------------------------------

// --- top row ---
#define RAWKEY_BACKQUOTE  0x00   // ` ~
#define RAWKEY_1          0x01
#define RAWKEY_2          0x02
#define RAWKEY_3          0x03
#define RAWKEY_4          0x04
#define RAWKEY_5          0x05
#define RAWKEY_6          0x06
#define RAWKEY_7          0x07
#define RAWKEY_8          0x08
#define RAWKEY_9          0x09
#define RAWKEY_0          0x0A
#define RAWKEY_MINUS      0x0B   // - _
#define RAWKEY_EQUALS     0x0C   // = +
#define RAWKEY_BACKSLASH  0x0D   // \ |
#define RAWKEY_BACKSPACE  0x41

// --- Q row ---
#define RAWKEY_TAB        0x42
#define RAWKEY_Q          0x10
#define RAWKEY_W          0x11
#define RAWKEY_E          0x12
#define RAWKEY_R          0x13
#define RAWKEY_T          0x14
#define RAWKEY_Y          0x15   // the key labelled Z on QWERTZ keyboards
#define RAWKEY_U          0x16
#define RAWKEY_I          0x17
#define RAWKEY_O          0x18
#define RAWKEY_P          0x19
#define RAWKEY_LBRACKET   0x1A   // [ {
#define RAWKEY_RBRACKET   0x1B   // ] }
#define RAWKEY_RETURN     0x44

// --- A row ---
#define RAWKEY_CTRL       0x63
#define RAWKEY_CAPSLOCK   0x62   // sends down when its LED goes on, up when off
#define RAWKEY_A          0x20
#define RAWKEY_S          0x21
#define RAWKEY_D          0x22
#define RAWKEY_F          0x23
#define RAWKEY_G          0x24
#define RAWKEY_H          0x25
#define RAWKEY_J          0x26
#define RAWKEY_K          0x27
#define RAWKEY_L          0x28
#define RAWKEY_SEMICOLON  0x29   // ; :
#define RAWKEY_QUOTE      0x2A   // ' "
#define RAWKEY_INTL1      0x2B   // international keyboards only: left of Return

// --- Z row ---
#define RAWKEY_LSHIFT     0x60
#define RAWKEY_INTL2      0x30   // international keyboards only: right of left Shift
#define RAWKEY_Z          0x31   // the key labelled Y on QWERTZ keyboards
#define RAWKEY_X          0x32
#define RAWKEY_C          0x33
#define RAWKEY_V          0x34
#define RAWKEY_B          0x35
#define RAWKEY_N          0x36
#define RAWKEY_M          0x37
#define RAWKEY_COMMA      0x38   // , <
#define RAWKEY_PERIOD     0x39   // . >
#define RAWKEY_SLASH      0x3A   // / ?
#define RAWKEY_RSHIFT     0x61

// --- bottom row ---
#define RAWKEY_LALT       0x64
#define RAWKEY_LAMIGA     0x66
#define RAWKEY_SPACE      0x40
#define RAWKEY_RAMIGA     0x67
#define RAWKEY_RALT       0x65

// --- function and editing keys ---
#define RAWKEY_ESC        0x45
#define RAWKEY_F1         0x50
#define RAWKEY_F2         0x51
#define RAWKEY_F3         0x52
#define RAWKEY_F4         0x53
#define RAWKEY_F5         0x54
#define RAWKEY_F6         0x55
#define RAWKEY_F7         0x56
#define RAWKEY_F8         0x57
#define RAWKEY_F9         0x58
#define RAWKEY_F10        0x59
#define RAWKEY_DEL        0x46
#define RAWKEY_HELP       0x5F

// --- cursor keys ---
#define RAWKEY_UP         0x4C
#define RAWKEY_DOWN       0x4D
#define RAWKEY_RIGHT      0x4E
#define RAWKEY_LEFT       0x4F

// --- numeric keypad ---
#define RAWKEY_KP_LPAREN  0x5A   // (
#define RAWKEY_KP_RPAREN  0x5B   // )
#define RAWKEY_KP_SLASH   0x5C   // /
#define RAWKEY_KP_STAR    0x5D   // *
#define RAWKEY_KP_7       0x3D
#define RAWKEY_KP_8       0x3E
#define RAWKEY_KP_9       0x3F
#define RAWKEY_KP_MINUS   0x4A   // -
#define RAWKEY_KP_4       0x2D
#define RAWKEY_KP_5       0x2E
#define RAWKEY_KP_6       0x2F
#define RAWKEY_KP_PLUS    0x5E   // +
#define RAWKEY_KP_1       0x1D
#define RAWKEY_KP_2       0x1E
#define RAWKEY_KP_3       0x1F
#define RAWKEY_KP_ENTER   0x43
#define RAWKEY_KP_0       0x0F
#define RAWKEY_KP_PERIOD  0x3C   // .

// Codes 0x78 and up are keyboard messages, not keys: 0x78 reset warning
// (Ctrl + both Amiga keys, A2000/A3000), 0xF9 last key code bad, 0xFA
// buffer overflow, 0xFC self-test failed, 0xFD / 0xFE start / end of the
// keys held at power-up.

// -------------------------------------------------------------------------
// State
// -------------------------------------------------------------------------
static UBYTE _sade_keyState[128];   // 1 while held
static UBYTE _sade_keyHit[128];     // latched on press, cleared by KeyHit

#define _SADE_CIAA_SDR  (*(volatile UBYTE*)0xBFEC01)
#define _SADE_CIAA_ICR  (*(volatile UBYTE*)0xBFED01)
#define _SADE_CIAA_CRA  (*(volatile UBYTE*)0xBFEE01)

// -------------------------------------------------------------------------
// KeyboardUpdate — call once per frame. Reads a pending scancode (if any),
// acknowledges it with the KDAT handshake, updates the key state tables.
// -------------------------------------------------------------------------
static void KeyboardUpdate(void) {
    if (!(_SADE_CIAA_ICR & 0x08)) return;      // SP flag: a byte is waiting

    UBYTE sdr = _SADE_CIAA_SDR;

    // handshake: drive KDAT low for >= 85 us by switching the serial port
    // to output mode, then release. ~200 us busy-wait — only on key events.
    _SADE_CIAA_CRA |= 0x40;
    for (volatile UWORD i = 0; i < 200; i++) {}
    _SADE_CIAA_CRA &= (UBYTE)~0x40;

    // wire format: complement of the code rotated right by one;
    // bit 7 of the decoded byte = 1 for key release
    UBYTE code = (UBYTE)~((sdr >> 1) | (sdr << 7));
    UBYTE raw  = code & 0x7F;
    if (code & 0x80) {
        _sade_keyState[raw] = 0;
    } else {
        if (!_sade_keyState[raw]) _sade_keyHit[raw] = 1;
        _sade_keyState[raw] = 1;
    }
}

// Level-triggered: non-zero while the key is held.
static inline short KeyDown(UBYTE raw) {
    return _sade_keyState[raw & 0x7F];
}

// Edge-triggered: returns 1 exactly once per press.
static inline short KeyHit(UBYTE raw) {
    UBYTE r = _sade_keyHit[raw & 0x7F];
    _sade_keyHit[raw & 0x7F] = 0;
    return r;
}

// "Press any key": the raw code of a key pressed since the last call, or -1.
// Clears every latch. The qualifiers (Shift, Ctrl, Alt, Amiga, Caps Lock,
// 0x60-0x67) don't count.
static short KeyHitAny(void) {
    short hit = -1;
    for (UBYTE i = 0; i < 128; i++) {
        if (_sade_keyHit[i] && hit < 0 && (i < 0x60 || i > 0x67)) hit = i;
        _sade_keyHit[i] = 0;
    }
    return hit;
}
