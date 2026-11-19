#pragma once

// SADE Clock — fixed-rate game ticks on a 50 Hz (PAL) or 60 Hz (NTSC)
// display. Game logic written in ticks then runs at the same wall-clock
// speed on both, and catches up when a frame's work overran into the next
// VBL instead of slowing down.
//
//   SadeClock clk;
//   SadeClockStart(&clk, 81);              // 81 ticks per second
//   for (;;) {
//       wait for the frame;
//       for (UBYTE n = SadeClockTicks(&clk); n; n--) GameTick();
//       draw, interpolated by SadeClockFrac(&clk, 5) fifths of a tick;
//   }
//
// After a pause (a dialog, a level load) call SadeClockSync so the time
// spent there is not caught up in one burst.
//
// The arithmetic below has no hardware access, so it builds on the host too
// (tests/clock_test.c); SadeClockStart/Ticks/Sync in sade_system.h feed it
// the real refresh rate and VBL counter.

#define SADE_CLOCK_MAX_FRAMES 4   // frames caught up at most per call

typedef struct {
    ULONG last;    // VBL count at the previous call
    UWORD acc;     // 0..hz-1: progress into the next tick, in 1/hz ticks
    UBYTE rate;    // ticks per second
    UBYTE hz;      // display frames per second
} SadeClock;

static inline void SadeClockStartAt(SadeClock *c, UBYTE ticksPerSec, UBYTE hz, ULONG now) {
    c->last = now;
    c->acc  = 0;
    c->rate = ticksPerSec;
    c->hz   = hz ? hz : 50;
}

// Ticks due at VBL count `now` (wraps safely). Repeated calls within one
// frame return 0.
static inline UBYTE SadeClockTicksAt(SadeClock *c, ULONG now) {
    ULONG frames = now - c->last;
    c->last = now;
    if (frames > SADE_CLOCK_MAX_FRAMES) frames = SADE_CLOCK_MAX_FRAMES;
    UWORD acc = c->acc;
    for (; frames; frames--) acc = (UWORD)(acc + c->rate);   // <= 59 + 4*255
    UBYTE n = 0;
    while (acc >= c->hz) { acc = (UWORD)(acc - c->hz); n++; }
    c->acc = acc;
    return n;
}

static inline void SadeClockSyncAt(SadeClock *c, ULONG now) {
    c->last = now;
    c->acc  = 0;
}

// How far into the next tick we are, in 1/parts (0..parts-1), for drawing
// actors between their last two tick positions.
static inline UBYTE SadeClockFrac(const SadeClock *c, UBYTE parts) {
    UWORD x = 0;
    for (UBYTE i = 0; i < parts; i++) x = (UWORD)(x + c->acc);
    UBYTE f = 0;
    while (x >= c->hz) { x = (UWORD)(x - c->hz); f++; }
    return f;
}
