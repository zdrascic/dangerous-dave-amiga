#pragma once

#include "sade.h"
#include "sade_bob.h"

// SADE Actor — frame animation for Bobs (AMOS: Anim / Anim On).
//
// An AnimDef is shared, immutable data: a list of BobImage* frames
// (typically fetched from a SpriteBank, or built in code) plus timing.
// An Anim is per-instance playback state. One AnimDef can drive any
// number of Anims, and one Anim can drive any number of Bobs.
//
//   static BobImage *coinFrames[2];
//   static const AnimDef coinSpin = {
//       .frames = coinFrames, .frameCount = 2,
//       .delays = NULL, .delay = 8,      // 8 VBL ticks per frame
//       .loop = 1,
//   };
//   Anim spin;  AnimStart(&spin, &coinSpin);
//   ...per frame:
//   bob->image = AnimTick(&spin);
//
// Timing is in game ticks — call AnimTick once per frame loop iteration
// (1 tick = 1 VBL = 20 ms PAL). `delays` may point to a per-frame tick
// array; when NULL every frame lasts `delay` ticks. One-shot animations
// (loop = 0) hold their last frame and set `done`.

typedef struct {
    BobImage   **frames;     // frameCount entries
    UBYTE        frameCount;
    const UBYTE *delays;     // per-frame ticks, or NULL to use `delay`
    UBYTE        delay;      // ticks per frame when delays == NULL
    UBYTE        loop;       // 1 = wrap around, 0 = one-shot
} AnimDef;

typedef struct {
    const AnimDef *def;
    UBYTE frame;             // current frame index
    UBYTE tick;              // ticks left on the current frame
    UBYTE done;              // 1 when a one-shot has finished
} Anim;

static inline UBYTE _AnimDelay(const AnimDef *d, UBYTE frame) {
    return d->delays ? d->delays[frame] : d->delay;
}

// Reset to frame 0. Safe to call again at any time (restart).
static inline void AnimStart(Anim *a, const AnimDef *def) {
    a->def   = def;
    a->frame = 0;
    a->tick  = _AnimDelay(def, 0);
    a->done  = 0;
}

// Advance one tick; returns the current frame's image (assign to
// bob->image). A finished one-shot keeps returning its last frame.
static BobImage *AnimTick(Anim *a) {
    const AnimDef *d = a->def;
    if (!a->done && a->tick && --a->tick == 0) {
        if (a->frame + 1 < d->frameCount) {
            a->frame++;
            a->tick = _AnimDelay(d, a->frame);
        } else if (d->loop) {
            a->frame = 0;
            a->tick  = _AnimDelay(d, 0);
        } else {
            a->done = 1;
        }
    }
    return d->frames[a->frame];
}

// Current frame's image without advancing (e.g. for a second Bob).
static inline BobImage *AnimImage(const Anim *a) {
    return a->def->frames[a->frame];
}

static inline UBYTE AnimDone(const Anim *a) {
    return a->done;
}
