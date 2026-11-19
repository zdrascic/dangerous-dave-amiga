#pragma once

#include "sade.h"

// Fixed-point format: 8.8  (value = integer / 256)
// Sin table: 256 entries covering 0..255 (full circle).
// Values are sin(angle) * 256, range -256..+256, stored as short.
// Cosine: phase-shift by 64 entries (quarter circle).

static const short sade_sinTable[256] = {
      0,   6,  12,  18,  25,  31,  37,  43,
     49,  56,  62,  68,  74,  80,  86,  92,
     97, 103, 109, 115, 120, 126, 131, 136,
    142, 147, 152, 157, 162, 167, 171, 176,
    180, 185, 189, 193, 197, 201, 205, 208,
    212, 215, 218, 221, 224, 227, 229, 232,
    234, 236, 238, 240, 242, 243, 245, 246,
    247, 248, 249, 250, 250, 251, 251, 251,
    252, 251, 251, 251, 250, 250, 249, 248,
    247, 246, 245, 243, 242, 240, 238, 236,
    234, 232, 229, 227, 224, 221, 218, 215,
    212, 208, 205, 201, 197, 193, 189, 185,
    180, 176, 171, 167, 162, 157, 152, 147,
    142, 136, 131, 126, 120, 115, 109, 103,
     97,  92,  86,  80,  74,  68,  62,  56,
     49,  43,  37,  31,  25,  18,  12,   6,
      0,  -6, -12, -18, -25, -31, -37, -43,
    -49, -56, -62, -68, -74, -80, -86, -92,
    -97,-103,-109,-115,-120,-126,-131,-136,
   -142,-147,-152,-157,-162,-167,-171,-176,
   -180,-185,-189,-193,-197,-201,-205,-208,
   -212,-215,-218,-221,-224,-227,-229,-232,
   -234,-236,-238,-240,-242,-243,-245,-246,
   -247,-248,-249,-250,-250,-251,-251,-251,
   -252,-251,-251,-251,-250,-250,-249,-248,
   -247,-246,-245,-243,-242,-240,-238,-236,
   -234,-232,-229,-227,-224,-221,-218,-215,
   -212,-208,-205,-201,-197,-193,-189,-185,
   -180,-176,-171,-167,-162,-157,-152,-147,
   -142,-136,-131,-126,-120,-115,-109,-103,
    -97, -92, -86, -80, -74, -68, -62, -56,
    -49, -43, -37, -31, -25, -18, -12,  -6,
};

static inline short SadeSin(UBYTE a) { return sade_sinTable[a]; }
static inline short SadeCos(UBYTE a) { return sade_sinTable[(UBYTE)(a + 64)]; }

// FpMul: fixed-point multiply in 8.8 format.
// Both operands are 8.8 values (integer part up to 256).
// Uses 68000 MULS instruction via mulsw() — single opcode, no software loop.
static inline short FpMul(short a, short b) {
    return (short)(mulsw(a, b) >> 8);
}

// FpDiv: fixed-point divide. Dividend is pre-shifted left 8 bits.
// Uses 68000 DIVS instruction via divsw().
// Avoid zero divisor — caller must ensure d != 0.
static inline short FpDiv(short n, short d) {
    return divsw((int)n << 8, d);
}
