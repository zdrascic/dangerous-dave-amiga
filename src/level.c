// =========================================================================
// Level: tile playfield + Dave.
// Screen layout (original game): 16px status bar, 20x10 tile playfield at
// y=16..175, bottom status bar below.
//
// Dave's movement is a port of skoperst/deadly-dave's state machine, which
// replicates the original's feel: 1px collision substeps, walking 2px every
// 3 ticks, a 95-step jump velocity table, falling that keeps drifting once
// steered, climbing 2px every other tick, pickups/hazards by collision box.
// Its tick is 14 ms (~71 Hz); here TICKS_PER_SEC sets the game speed in
// wall-clock time (SadeClock): the same on a 50 Hz PAL and a 60 Hz NTSC
// display, and kept when a frame overruns, with Dave drawn interpolated
// between ticks when they don't divide evenly.
// =========================================================================
#include "level.h"
#include "sound.h"
#include "scores.h"
#include "../libs/sade/sade_system.h"
#include "../libs/sade/sade_input.h"
#include "../libs/sade/sade_keyboard.h"
#include "../libs/sade/sade_ilbm.h"
#include "../libs/sade/sade_bank.h"
#include "../libs/sade/sade_tilemap.h"
#include "../libs/sade/sade_graphics.h"
#include "../libs/sade/sade_text.h"

#define TILE      16
#define VIEW_W    20                 // playfield width in tiles
#define BAND_Y    13                 // band's first screen row (under the top bar)
#define BAND_TOP  10                 // blank canvas rows above map row 0, so
                                     // Dave walking on row 0 shows his legs
#define PLAY_Y    (BAND_Y + BAND_TOP)  // map row 0 on screen
#define DOS_PLAY_Y 16                // the original's map row 0 on screen
_Static_assert(PLAY_Y == LEVEL_PLAY_Y, "level.h LEVEL_PLAY_Y must match");
#define GAME_OVER_TICKS (1000 * TICKS_PER_SEC / 70)   // the original: 1000 retraces
#define SCORES_WAIT    (1000 * 50 / 70)               // after a demo, in 50ths of a second
#define MAX_VIEW_PX ((LEVEL_W - VIEW_W) * TILE)
#define TICKS_PER_SEC 81             // game speed: 70 = original feel (deadly-dave ~71)
#define ANIM_TICKS  12               // ticks per animated-tile frame (~7 fps at 81 ticks/s)
#define UI_TICKS_PER_SEC 50          // dialog blink rate unit (tuned as PAL frames)
#define BLINK_MIN   10               // level start: ticks before a key releases Dave
#define BLINK_LOOP  11               // blink cycle: shown 11..20, hidden 21..31
#define BURN_TICKS 200               // explosion + pause before respawn
#define BURN_SHOWN 100               // explosion visible this long
#define MAX_LIVES   3
#define MAX_ANIM   400               // animated tiles in one level
#define SCROLL_PX    8               // view slide speed, px per tick
#define T_BLUE_BRICK 5
#define WALK_STEP_SOUND 2            // level: footstep click every Nth step
#define WARP_STEP_SOUND 3            // corridor: every Nth step (Dave never stops)
                                     // (every step = 27 clicks/s at 81 ticks/s)
#define TUBE_LABEL_X1 32              // warp zone tube: "WARP" / "ZONE" screen x
#define TUBE_LABEL_X2 200             // (deadly-dave), multiples of 8 for HudPut
#define TUBE_LABEL_Y  (76 + BAND_TOP) // canvas row: level with the middle
#define WARP_TEXT_X 48               // "GOOD WORK" position (original: 50,58)
#define WARP_TEXT_Y (58 - DOS_PLAY_Y - 4 + BAND_TOP)  // canvas row, 2px clear of the corridor roof
#define WARP_INK     2               // near-white in the gameplay palette
#define JET_FUEL   900               // jetpack ticks per pickup (deadly-dave)
#define MON_BURN   100               // monster explosion ticks
#define MAX_MON      5
#define MON_STEP_TICKS 6            // ticks per path waypoint (monster speed)
#define WALL_CLIMB   1               // original's wall climb (see OnGround); 0 = off

static const UBYTE startX[NUM_LEVELS] = { 2, 1, 2, 1, 2, 2, 1, 2, 6, 2 };
static const UBYTE startY[NUM_LEVELS] = { 8, 8, 5, 5, 8, 8, 2, 8, 1, 8 };

enum { ST_STANDING, ST_WALKING, ST_JUMPING, ST_CLIMBING, ST_FALLING,
       ST_BURNING, ST_DEAD, ST_JETPACK };
// FRONT: looking at the player; FRONTR/L: same pose, but a jump or fall
// will lean that way (original quirk).
enum { FACE_FRONT, FACE_FRONTR, FACE_FRONTL, FACE_LEFT, FACE_RIGHT };
// walk cooldown: a step moves 2px, then 2 idle ticks
enum { WALK_READY, WALK_CD1_R, WALK_CD1_L, WALK_CD2_R, WALK_CD2_L };
enum { CLIMB_READY, CLIMB_COOLDOWN, CLIMB_JUMP_R, CLIMB_JUMP_L };

typedef struct {
    short px, py;            // world pixels; py relative to the playfield top
    short prevX, prevY;      // position before the last tick (for drawing)
    UBYTE state, face;
    UBYTE walkState, climbState;
    UBYTE jumpState, jumpCooldown;
    UWORD steps;             // walk/climb step counter (animation)
    UWORD ticksInState;
    UBYTE onFire, onTree;    // set by the collision pass
    UBYTE keyLR;             // left/right held this tick (idle frame)
    short viewPx, prevViewPx;  // view left edge in world pixels
    short scrollTo;          // view target while a scroll slides
} Dave;

typedef struct {
    UBYTE trophy, gun;
    UWORD fuel;              // jetpack ticks left (0 = no jetpack)
    UBYTE barUnits;          // fuel units currently drawn in the status bar
    UBYTE mapDirty;          // a tile changed: re-render the view
    UBYTE hudDirty;
    UBYTE animPhase, animSub;  // tile animation advances every ANIM_TICKS
    UBYTE waiting, blink;      // level start / respawn: Dave blinks, game waits
    UBYTE over;                // GAME OVER box up: the level animates on, Dave is gone
    UWORD overTicks;
} Level;

typedef struct { UBYTE tx, ty, base, mod; } AnimCell;

static UBYTE       *g_cells;           // mutable copy of layer 0
static const UBYTE *g_flags;
static Level        g_lv;
static AnimCell     g_anim[MAX_ANIM];
static UWORD        g_numAnim;
UBYTE               g_debugKeys;       // see level.h
static UBYTE        g_invuln;          // debug H: nothing hurts Dave
static UBYTE        g_warp;            // a cut-scene map, no scroll, no monsters:
enum { WARP_NONE, WARP_CORRIDOR,           //   between levels: Dave walks right
       WARP_TUBE };                        //   into a warp zone: Dave falls
static UBYTE        g_mapLevel;        // 0-based level whose map is playing
static UBYTE        g_hudLevel;        // 0-based level number the HUD shows
static void MarkDirty(UBYTE tx, UBYTE ty);

static inline UBYTE *Cell(UWORD tx, UWORD ty) {
    return g_cells + muluw(ty, LEVEL_W) + tx;
}

// ---- collision against solid tiles (points; outside the map is open) ----
static UBYTE Solid(short x, short y) {
    if (x < 0 || y < 0) return 0;
    UWORD tx = (UWORD)x >> 4, ty = (UWORD)y >> 4;
    if (tx >= LEVEL_W || ty >= LEVEL_H) return 0;
    return g_flags[*Cell(tx, ty)] & TILE_SOLID;
}

static UBYTE HitRight(const Dave *d) { return Solid(d->px + 12, d->py + 2) || Solid(d->px + 12, d->py + 15); }
static UBYTE HitLeft(const Dave *d)  { return Solid(d->px + 1,  d->py + 2) || Solid(d->px + 1,  d->py + 15); }
static UBYTE HitTop(const Dave *d)   { return Solid(d->px + 4,  d->py + 1) || Solid(d->px + 9,  d->py + 1); }
// The original also tests x+10 under Dave's feet, two pixels right of the
// rest. Jumping up under a brick column's lower corner lets Dave rise with
// his body 2-4 px inside its left face (sideways checks only sample y+2 and
// y+15); x+10 then lands inside the wall and counts as ground, so he can
// stand on the face at any height and climb it by jumping (PC level 4).
// deadly-dave leaves x+10 out on purpose (dave_on_ground's comment calls it
// "climbing on right walls (original game's feature/bug)"); WALL_CLIMB
// keeps it, like the original.
static UBYTE OnGround(const Dave *d) {
    for (short k = 4; k <= (WALL_CLIMB ? 10 : 9); k++)
        if (Solid(d->px + k, d->py + 16)) return 1;
    return 0;
}

// ---- state transitions ----
static void Enter(Dave *d, UBYTE state) {
    d->state = state;
    d->ticksInState = 0;
}

static UBYTE g_keyUp;                      // up held this tick (walk sound)
static UBYTE g_keyJet;                     // jetpack toggle pressed this tick
static UBYTE g_keyFire;                    // gun button held this tick

// Dave's own sounds (the corridor walk included).
static void DaveSound(UBYTE id) { SoundPlay(id); }

// The footstep click of a walking step: every WALK_STEP_SOUND-th step in a
// level, every WARP_STEP_SOUND-th in the corridor, where nothing else plays
// and Dave never stops. g_stepSound counts steps since the last click; it
// is reset when Dave stands without left/right, so the first step of a walk
// always clicks.
static UBYTE g_stepSound;

static void StepSound(void) {
    UBYTE every = g_warp ? WARP_STEP_SOUND : WALK_STEP_SOUND;
    if (g_stepSound == 0) DaveSound(SND_WALKING);
    if (++g_stepSound >= every) g_stepSound = 0;
}

static void EnterJump(Dave *d) {
    DaveSound(SND_JUMPING);
    d->face = (d->face == FACE_FRONTL || d->face == FACE_LEFT) ? FACE_FRONTL : FACE_FRONTR;
    Enter(d, ST_JUMPING);
    d->walkState = WALK_READY;
    d->jumpState = 0;
}

static void EnterFall(Dave *d) {
    d->state = ST_FALLING;
    if (d->face == FACE_RIGHT) d->face = FACE_FRONTR;
    else if (d->face == FACE_LEFT) d->face = FACE_FRONTL;
}

static void EnterClimb(Dave *d, UBYTE left, UBYTE right, UBYTE up) {
    Enter(d, ST_CLIMBING);
    if (up && right)     d->climbState = CLIMB_JUMP_R;
    else if (up && left) d->climbState = CLIMB_JUMP_L;
    else                 d->climbState = CLIMB_READY;
}

// One walking step: 1px, plus a 2nd px if still clear.
static void EnterWalk(Dave *d, UBYTE left, UBYTE right) {
    d->state = ST_WALKING;
    if (left) {
        if (HitLeft(d)) { Enter(d, ST_STANDING); return; }
        d->px--;
        d->face = FACE_LEFT;
        d->walkState = WALK_CD2_L;
        d->steps++;
        if (!d->jumpCooldown && !g_keyUp) StepSound();
        if (!HitLeft(d)) d->px--;
    } else if (right) {
        if (HitRight(d)) { Enter(d, ST_STANDING); return; }
        d->px++;
        d->face = FACE_RIGHT;
        d->walkState = WALK_CD2_R;
        d->steps++;
        if (!d->jumpCooldown && !g_keyUp) StepSound();
        if (!HitRight(d)) d->px++;
    } else {
        Enter(d, ST_STANDING);
    }
}

static void EnterJetpack(Dave *d) {
    Enter(d, ST_JETPACK);
    SoundPlay(SND_TOJETPACK);
}

// Jetpack toggle, checked first by the states it can interrupt.
static UBYTE TryJetpack(Dave *d) {
    if (!g_keyJet || !g_lv.fuel) return 0;
    EnterJetpack(d);
    return 1;
}

// Walked off a ledge (not the end of a jump arc).
static void FallOff(Dave *d) {
    EnterFall(d);
    DaveSound(SND_FALLING);
}

static void Landed(Dave *d) {
    SoundStopIf(SND_FALLING);
    SoundStopIf(SND_JUMPING);
    Enter(d, ST_STANDING);
    d->jumpCooldown = 5;
    d->steps = 0;
}

// ---- per-state routines ----
static void Standing(Dave *d, UBYTE left, UBYTE right, UBYTE up) {
    if (d->onFire) { Enter(d, ST_BURNING); return; }
    if (TryJetpack(d)) return;
    if (!OnGround(d)) { FallOff(d); return; }
    if (d->jumpCooldown) d->jumpCooldown--;
    if (up) {
        if (d->onTree) { EnterClimb(d, left, right, up); return; }
        if (!d->jumpCooldown) { EnterJump(d); return; }
    }
    if (left || right) EnterWalk(d, left, right);
    else g_stepSound = 0;                     // next walk starts with a click
}

static void Walking(Dave *d, UBYTE up) {
    if (d->jumpCooldown) d->jumpCooldown--;
    if (d->onFire) { Enter(d, ST_BURNING); return; }
    if (TryJetpack(d)) return;
    if (!OnGround(d)) { FallOff(d); return; }
    if (up && !d->jumpCooldown) { EnterJump(d); return; }
    switch (d->walkState) {
        case WALK_CD2_R: d->walkState = WALK_CD1_R; break;
        case WALK_CD2_L: d->walkState = WALK_CD1_L; break;
        default:         d->walkState = WALK_READY; Enter(d, ST_STANDING); break;
    }
}

// px per tick: 40 ticks up, 14 hang, 40 down (alternating with 0s)
static const BYTE jumpVel[95] = {
    -1,  0, -2,  0, -3,  0, -2,  0, -2,  0,
    -2,  0, -2,  0, -2,  0, -2,  0, -2,  0,
    -2,  0, -1,  0, -2,  0, -1,  0, -1,  0,
    -1,  0, -1,  0, -1,  0, -1,  0, -1,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,
     0,  0,  1,  0,  1,  0,  1,  0,  1,  0,
     1,  0,  1,  0,  1,  0,  2,  0,  2,  0,
     2,  0,  2,  0,  2,  0,  2,  0,  2,  0,
     2,  0,  2,  0,  2,  0,  3,  0,  2,  0,
};

static void Jumping(Dave *d, UBYTE left, UBYTE right, UBYTE up) {
    if (d->onFire) { Enter(d, ST_BURNING); return; }
    if (TryJetpack(d)) return;
    if (d->jumpState >= 40 && OnGround(d)) { Landed(d); return; }

    if (d->onTree) {
        if (!up && (left || right)) EnterClimb(d, left, right, up);
        if (up && (left || right) && d->jumpState >= 3 && d->jumpState <= 76) {
            d->jumpCooldown = 1;
            EnterClimb(d, left, right, up);
            d->jumpState = 0;
            return;
        }
    }

    if (d->jumpState == 94) {                 // end of the arc
        d->py += 2;
        if (OnGround(d)) {
            EnterWalk(d, left, right);
            d->jumpCooldown = 5;
            d->steps = 0;
        } else {
            d->py -= 2;
            EnterFall(d);
        }
        return;
    }

    BYTE dy = jumpVel[d->jumpState++];
    while (dy > 0) {                          // way down: stop on ground
        d->py++; dy--;
        if (OnGround(d)) dy = 0;
    }
    while (dy < 0) {                          // way up: stop at a ceiling
        d->py--; dy++;
        // the original predicts the ceiling 2px ahead in the steered direction
        UBYTE hit = 0;
        if (HitTop(d)) {
            short sx = left ? -2 : right ? 2 : 0;
            d->px += sx;
            hit = HitTop(d);
            d->px -= sx;
        }
        if (hit) {
            d->walkState = WALK_READY;
            d->jumpState = 94;
            DaveSound(SND_HITWALL);
            return;
        }
    }

    // sideways: a 2px step, then the walk cooldown
    if (d->walkState == WALK_READY) {
        if (left) {
            d->face = FACE_LEFT;
            if (!HitLeft(d)) { d->px--; if (!HitLeft(d)) d->px--; }
            d->walkState = WALK_CD2_L;
        } else if (right) {
            d->face = FACE_RIGHT;
            if (!HitRight(d)) { d->px++; if (!HitRight(d)) d->px++; }
            d->walkState = WALK_CD2_R;
        }
        return;
    }
    d->walkState = WALK_READY;
}

static void Falling(Dave *d, UBYTE left, UBYTE right) {
    if (d->onFire) { Enter(d, ST_BURNING); return; }
    if (TryJetpack(d)) return;
    if (OnGround(d)) { Landed(d); return; }
    d->py++;
    if (left)  d->face = FACE_LEFT;
    else if (right) d->face = FACE_RIGHT;
    // once steered, the fall keeps drifting that way
    if (d->face == FACE_LEFT)  { if (!HitLeft(d))  d->px--; }
    else if (d->face == FACE_RIGHT) { if (!HitRight(d)) d->px++; }
}

static void Climbing(Dave *d, UBYTE left, UBYTE right, UBYTE up, UBYTE down) {
    if (!d->onTree) {
        if (up) EnterJump(d); else EnterFall(d);
        return;
    }
    if (d->onFire) { Enter(d, ST_BURNING); return; }

    // one 2px move every other tick
    UBYTE ready = d->climbState == CLIMB_READY;
    if (up && !left && !right && !down) {
        if (ready) { d->py -= 2; d->steps++; DaveSound(SND_WALKING); }
        d->climbState = ready ? CLIMB_COOLDOWN : CLIMB_READY;
    } else if (down && !left && !right && !up) {
        if (ready) {
            d->py++;
            if (!OnGround(d)) d->py++;
            d->steps++;
            DaveSound(SND_WALKING);
        }
        d->climbState = ready ? CLIMB_COOLDOWN : CLIMB_READY;
        if (OnGround(d)) { Enter(d, ST_STANDING); return; }
    } else if (left && !up && !right) {
        if (ready) { d->px -= 2; d->steps++; DaveSound(SND_WALKING); }
        d->climbState = ready ? CLIMB_COOLDOWN : CLIMB_READY;
    } else if (right && !up && !left) {
        if (ready) { d->px += 2; d->steps++; DaveSound(SND_WALKING); }
        d->climbState = ready ? CLIMB_COOLDOWN : CLIMB_READY;
    } else if ((left || right) && up) {       // jump off the tree
        if (!d->jumpCooldown) { d->py--; EnterJump(d); return; }
        d->jumpCooldown--;
    }
    d->ticksInState++;
}

// Flying: 1px per tick in one direction (left > right > up > down), fuel
// burns every tick; the toggle or an empty tank drops Dave (he falls).
static void Jetpacking(Dave *d, UBYTE left, UBYTE right, UBYTE up, UBYTE down) {
    if (g_keyJet) { Enter(d, ST_STANDING); SoundStop(); return; }
    if (d->onFire) { Enter(d, ST_BURNING); return; }
    if (left)       { if (!HitLeft(d))  d->px--; d->face = FACE_LEFT; }
    else if (right) { if (!HitRight(d)) d->px++; d->face = FACE_RIGHT; }
    else if (up)    { if (!HitTop(d))   d->py--; }
    else if (down)  { if (!OnGround(d)) d->py++; }
    d->ticksInState++;
    if (--g_lv.fuel == 0) {                   // empty: bar goes, full redraw
        Enter(d, ST_STANDING); SoundStop();
        g_lv.hudDirty = 1;
    }
    if (d->state == ST_JETPACK && !SoundIsPlaying()) SoundPlay(SND_FLYING);
}

static void DaveTick(Dave *d, UBYTE left, UBYTE right, UBYTE up, UBYTE down) {
    d->keyLR = left || right;
    g_keyUp = up;
    switch (d->state) {
        case ST_STANDING: Standing(d, left, right, up); break;
        case ST_WALKING:  Walking(d, up); break;
        case ST_JUMPING:  Jumping(d, left, right, up); break;
        case ST_CLIMBING: Climbing(d, left, right, up, down); break;
        case ST_FALLING:  Falling(d, left, right); break;
        case ST_JETPACK:  Jetpacking(d, left, right, up, down); break;
        case ST_BURNING:
            if (++d->ticksInState >= BURN_TICKS) d->state = ST_DEAD;
            break;
    }
    d->onTree = 0;
}

// ---- pickups, hazards, trees, door: collision boxes ----
static void AddScore(GameState *g, UWORD pts) {
    UWORD before = divuw(g->score, 20000);       // extra Dave every 20000
    g->score += pts;
    if (divuw(g->score, 20000) != before && g->lives < MAX_LIVES) g->lives++;
    g_lv.hudDirty = 1;
}

// A tile's collision box relative to its top-left (deadly-dave tile.c).
static void TileBox(UBYTE t, short *x, short *y, short *w, short *h) {
    *x = 0; *y = 0; *w = TILE; *h = TILE;
    if (t >= 6 && t <= 9)        { *x = 6; *w = 4; }            // fire
    else if (t >= 25 && t <= 28) { *x = 4; *w = 8; }            // vines
    else if (t >= 36 && t <= 40) { *x = 6; *y = 4; *w = 4; }    // water
    else if (t == 41)            { *x = 7; *y = 7; *w = 2; *h = 12; } // stars
    else if (t == 42)            { *y = 3; }                    // stars + moon
    else if (t == T_DOOR)        { *h = 20; }
}

static UWORD PickupScore(UBYTE t) {
    switch (t) {
        case 47: return 100;  case 48: return 50;   case 49: return 150;
        case 50: return 800;  case 51: return 550;  case 52: return 400;
        case T_GUN: return 100;
        case T_TROPHY: return 1000;
    }
    return 0;
}

// Returns 1 when Dave leaves through the door.
static UBYTE Touch(Dave *d, GameState *g) {
    short bx = d->px + 2, by = d->py + 2;        // Dave's box: 14x16
    short bw = 14, bh = 16;
    short tx0 = bx >> 4, tx1 = (short)((bx + bw - 1) >> 4);
    short ty0 = (short)((by - 4) >> 4), ty1 = (short)((by + bh - 1) >> 4);
    for (short ty = ty0; ty <= ty1; ty++) {
        if (ty < 0 || ty >= LEVEL_H) continue;
        for (short tx = tx0; tx <= tx1; tx++) {
            if (tx < 0 || tx >= LEVEL_W) continue;
            UBYTE *c = Cell((UWORD)tx, (UWORD)ty);
            UBYTE f = g_flags[*c];
            if (!(f & (TILE_PICKUP | TILE_HURT | TILE_LADDER | TILE_EXIT))) continue;
            short ox, oy, ow, oh;
            TileBox(*c, &ox, &oy, &ow, &oh);
            ox += tx * TILE; oy += ty * TILE;
            if (bx >= ox + ow || bx + bw <= ox || by >= oy + oh || by + bh <= oy)
                continue;
            if (f & TILE_PICKUP) {
                if (*c == T_TROPHY) { g_lv.trophy = 1; SoundPlay(SND_TROPHY); }
                else if (*c == T_GUN) { g_lv.gun = 1; SoundPlay(SND_GOTSOMETHING); }
                else if (*c == T_JETPACK) { g_lv.fuel = JET_FUEL; g_lv.hudDirty = 1; SoundPlay(SND_GOTSOMETHING); }
                else SoundPlay(SND_TREASURE);
                UWORD pts = PickupScore(*c);
                if (pts) AddScore(g, pts);
                *c = 0;
                MarkDirty((UBYTE)tx, (UBYTE)ty);
                g_lv.hudDirty = 1;
            } else if (f & TILE_EXIT) {
                if (g_lv.trophy) { SoundPlay(SND_NEXTLEVEL); return 1; }
            } else if ((f & TILE_HURT) && !g_invuln) {
                if (!d->onFire) { SoundStop(); SoundPlay(SND_OUCH); }
                d->onFire = 1;
            } else if (f & TILE_LADDER) {
                d->onTree = 1;
            }
        }
    }
    return 0;
}

// ---- monsters, their plasma shots, Dave's bullet ----
// The rules below are the original's, read from the unpacked DAVE.EXE
// (code 0x2B87 monsters, 0x2E1B shots, 0x35F5 / 0x386C hit tests; data
// segment at file offset 0x256D0). Kinds by level (3..10). Each level has one
// flight path (the 256 bytes before its grid): signed (dx, dy) waypoints,
// 0xEA = loop. The original jumps a monster one waypoint every 5 ticks (at
// ~71 ticks/s); here MON_STEP_TICKS 6 at 81 ticks/s, spread over the ticks
// for smooth motion, and everything else a monster does (firing) happens
// once per waypoint, as in the original. A shot scores (level + 1) * 100.
enum { MS_NONE, MS_ACTIVE, MS_BURNING, MS_DEAD };

typedef struct {
    UBYTE frame0;          // first frame in monsters.iff
    UBYTE pingPong;        // frames 0,1,2,3,2,1 (else 0,0,1,1,2,2,3,3)
    UBYTE artW, artH;      // sprite size (what drawing covers)
} MonKind;

static const MonKind kKinds[8] = {          // art only; sizes for logic below
    { MON_SPIDER0, 1, 28, 20 },
    { MON_SWIRL0,  1, 24, 20 },
    { MON_SUN0,    0, 24, 21 },
    { MON_BONES0,  0, 24, 22 },
    { MON_UFO0,    0, 18,  8 },
    { MON_GUARD0,  0, 16, 12 },
    { MON_ENVY0,   0, 16, 16 },
    { MON_PLATE0,  0, 16, 20 },
};

// Per level, the monsters' size for the hit tests and shot placement (DS
// 0x146 widths, 0x156 heights; 8-word tables that overlap, so levels 9-10
// read their width from the start of the heights, as in the original).
static const UBYTE kMonW[NUM_LEVELS] = { 32, 144, 28, 21, 22, 22, 14, 12, 16, 14 };
static const UBYTE kMonH[NUM_LEVELS] = { 16,  14, 15, 18, 22, 20,  5,  8, 15, 16 };

// Per level, up to 4 monsters (DS 0x496 + level * 80, copied to the monster
// arrays at level start): x and y in pixels (y here on the playfield, the
// original's minus the 16-row status bar), waypoints between shots, first
// waypoint byte. x 0 = none.
typedef struct { short x; UBYTE y, rate, path; } MonStart;
static const MonStart kMonStart[NUM_LEVELS][MAX_MON] = {
    { { 0 } }, { { 0 } },
    { {  951,  53, 20, 0 }, {  711,  53, 20, 0 } },
    { {  515,  30,  1, 0 } },
    { {  240,  44, 10, 0 }, {  520,  44, 10, 0 }, {  788,  50, 10, 0 }, { 1472,  52, 10, 0 } },
    { {  158, 126, 10, 0 }, {  457, 128, 10, 0 }, {  694, 126, 10, 0 }, {  662,  28, 10, 0 } },
    { {   76,  32, 25, 0 }, {  259,  15,  8, 0 }, {  743,  44,  8, 0 }, {  855,  44,  1, 0 } },
    { {  846,  84,  5, 0 }, { 1136,  34,  2, 0 }, { 1350,  24,  2, 0 } },
    { {  563, 127, 20, 0 }, {  709, 127, 10, 8 }, {  788, 127,  5, 0 }, { 1102, 127, 10, 8 } },
    { {  709, 128, 20, 0 }, { 1040,  48, 20, 0 }, { 1323,  80, 10, 0 }, {  819,  28, 10, 0 } },
};

typedef struct {
    UBYTE state, kind;
    short x, y, prevX, prevY;
    short baseX, baseY;        // position at the start of the waypoint
    BYTE  dx, dy;              // current waypoint
    UBYTE pathIdx, sub;        // next waypoint byte, tick within it
    UBYTE step;                // waypoints taken (animation)
    UWORD timer;               // ticks burning
    UWORD shootWait;           // waypoints left before the next shot
    UBYTE rate;                // waypoints between shots
    UBYTE shotOn;              // its plasma shot
    BYTE  shotDir;
    short shotX, shotY, shotPrevX;
    UWORD shotTicks;
} Monster;

typedef struct {
    UBYTE on;
    BYTE  dir;
    short x, y, prevX;
} Bullet;

static Monster      g_mon[MAX_MON];
static UBYTE        g_numMon;
static const BYTE  *g_path;          // NULL: no monsters on this map
static Bullet       g_bullet;

static void SpawnMonsters(UBYTE level) {
    g_numMon = 0;
    if (!g_path) return;
    for (UBYTE i = 0; i < MAX_MON; i++) {
        const MonStart *ms = &kMonStart[level][i];
        if (!ms->x) break;
        Monster *m = &g_mon[g_numMon++];
        Monster z = { 0 };
        *m = z;
        m->state = MS_ACTIVE;
        m->kind  = (UBYTE)(level - 2);
        m->x = m->prevX = m->baseX = ms->x;
        m->y = m->prevY = m->baseY = ms->y;
        m->pathIdx = ms->path;
        m->rate = (UBYTE)(m->shootWait = ms->rate);
    }
}

// Kill a monster: explosion, sound, the Ctrl-bullet scores.
static void BurnMonster(Monster *m) {
    m->state = MS_BURNING;
    m->timer = 0;
    SoundPlay(SND_EXPLOSION);
}

static void MonsterTick(Monster *m, const Dave *d) {
    m->prevX = m->x;
    m->prevY = m->y;
    if (m->state == MS_BURNING) {
        if (++m->timer >= MON_BURN) m->state = MS_DEAD;
    } else if (m->state == MS_ACTIVE) {
        if (m->sub == 0) {                        // next waypoint
            if ((UBYTE)g_path[m->pathIdx] == 0xEA && (UBYTE)g_path[m->pathIdx + 1] == 0xEA)
                m->pathIdx = 0;
            m->dx = g_path[m->pathIdx];
            m->dy = g_path[m->pathIdx + 1];
            m->pathIdx = (UBYTE)(m->pathIdx + 2);
            m->baseX = m->x; m->baseY = m->y;
            m->step++;
        }
        m->sub++;
        m->x = (short)(m->baseX + divsw(mulsw(m->dx, m->sub), MON_STEP_TICKS));
        m->y = (short)(m->baseY + divsw(mulsw(m->dy, m->sub), MON_STEP_TICKS));
        if (m->sub == MON_STEP_TICKS) {           // at the waypoint: the original's
            m->sub = 0;                           // move tick, when it may fire
            UBYTE lvl = (UBYTE)(m->kind + 2);
            short sx = (short)(m->x - d->viewPx);
            // one plasma at a time, only with the monster on screen; the
            // countdown runs in waypoints, and only while no shot of its own
            // flies; never at a dead or exploding Dave
            if (!m->shotOn && sx >= 0 && sx < 340 && --m->shootWait < 1 &&
                d->state != ST_BURNING && d->state != ST_DEAD) {
                m->shootWait = m->rate;
                BYTE dir = (BYTE)(m->x > d->px ? -1 : 1);
                short x0 = (short)(dir < 0 ? m->x - 16 : m->x + kMonW[lvl]);
                short y0 = (short)(m->y + (kMonH[lvl] >> 1) - 2);
                if (!Solid(x0, (short)(y0 + 1))) {    // not from inside a wall
                    m->shotOn = 1;
                    m->shotDir = dir;
                    m->shotX = m->shotPrevX = x0;
                    m->shotY = y0;
                    m->shotTicks = 0;
                }
            }
        }
    }
    // A shot keeps flying after its monster dies, off screen too, until the
    // tile just ahead of it (x+16 going right, x-2 going left) is solid.
    if (m->shotOn) {
        m->shotPrevX = m->shotX;
        m->shotX = (short)(m->shotX + m->shotDir * 2);
        m->shotTicks++;
        short fx = (short)(m->shotDir > 0 ? m->shotX + 16 : m->shotX - 2);
        if (Solid(fx, (short)(m->shotY + 1)) || fx < 0 || fx >= LEVEL_W * TILE)
            m->shotOn = 0;
    }
}

// Gun, monsters, shots, and their hits on Dave. After DaveTick.
static void ActorsTick(Dave *d, GameState *g) {
    // ---- Dave's gun: one bullet at a time, while the button is held ----
    if (!g_bullet.on && g_keyFire && g_lv.gun && d->state != ST_BURNING && d->state != ST_DEAD) {
        BYTE dir = 0;
        if (d->face == FACE_LEFT || d->face == FACE_FRONTL) dir = -1;
        else if (d->face == FACE_RIGHT || d->face == FACE_FRONTR) dir = 1;
        if (dir) {
            g_bullet.on = 1;
            g_bullet.dir = dir;
            g_bullet.x = g_bullet.prevX = (short)(dir > 0 ? d->px + 8 : d->px - 8);
            g_bullet.y = (short)(d->py + 8);
        }
    }
    if (g_bullet.on) {
        g_bullet.prevX = g_bullet.x;
        g_bullet.x = (short)(g_bullet.x + g_bullet.dir * 2);
        short fx = (short)(g_bullet.dir > 0 ? g_bullet.x + 10 : g_bullet.x - 2);
        if (Solid(fx, (short)(g_bullet.y + 1)) ||
            g_bullet.x < d->viewPx || g_bullet.x > d->viewPx + 320)
            g_bullet.on = 0;
    }

    // The original's hit tests (one point against a range each; w/h are the
    // level's monster size from kMonW/kMonH):
    //   Dave's bullet hits a monster: bullet - monster in x -5..w-1, y -2..h-1
    //   a monster touches Dave:       monster - Dave  in x -(w-1)..12, y -(h-1)..15
    //   a plasma hits Dave:           plasma - Dave   in x -15..12, y -2..15
    UBYTE daveAlive = d->state != ST_BURNING && d->state != ST_DEAD;
    UBYTE lvl = g_mapLevel;
    short w = kMonW[lvl], h = kMonH[lvl];
    for (UBYTE i = 0; i < g_numMon; i++) {
        Monster *m = &g_mon[i];
        MonsterTick(m, d);
        if (m->state == MS_ACTIVE && g_bullet.on && m->x - d->viewPx < 320) {
            short bx = (short)(g_bullet.x - m->x), by = (short)(g_bullet.y - m->y);
            if (bx > -6 && bx < w && by > -3 && by < h) {
                g_bullet.on = 0;
                BurnMonster(m);
                AddScore(g, (UWORD)muluw((UWORD)(lvl + 1), 100));
            }
        }
        if (!daveAlive) continue;
        short mx = (short)(m->x - d->px), my = (short)(m->y - d->py);
        if (m->state == MS_ACTIVE && mx > -w && mx <= 12 && my > -h && my <= 15) {
            BurnMonster(m);
            if (!g_invuln) d->onFire = 1;
        }
        short px = (short)(m->shotX - d->px), py = (short)(m->shotY - d->py);
        if (m->shotOn && px >= -15 && px <= 12 && py >= -2 && py <= 15) {
            m->shotOn = 0;                        // invulnerable: the shot still ends here
            if (!g_invuln) {
                if (!d->onFire) { SoundStop(); SoundPlay(SND_EXPLOSION); }
                d->onFire = 1;
            }
        }
    }
}

// Warp zones, from DAVE.EXE (unpacked; found by yo-yo-yo-jbo/dangerous_dave):
// per level, the 1-based level whose map holds its zone (0 = none; file
// offset 0x2583A), then the view's first tile and Dave's screen x in pixels
// (0x25862, two arrays of 10 words). Dave starts at y 16, falling, which
// puts him at the top of a shaft into an area at the right of that map.
static const UBYTE kZoneMap[NUM_LEVELS]   = { 0, 0, 0, 0,   2, 0, 0,  6,   7,   1 };
static const UBYTE kZoneView[NUM_LEVELS]  = { 0, 0, 0, 0,  42, 0, 0, 67,  71,  11 };
static const UBYTE kZoneDaveX[NUM_LEVELS] = { 0, 0, 0, 0, 144, 0, 0, 64, 144, 144 };
#define ZONE_START_Y 16

// A zone continues the tube cut-scene: the walls of its shaft (the solid
// tiles either side of Dave's column, down to where the shaft ends) become
// the tube's blue brick. Only in the zone's copy of the map; level 8's zone
// starts Dave in the open, with no walls to change.
static void BlueZoneShaft(UBYTE from) {
    UWORD col = (UWORD)((kZoneView[from] * TILE + kZoneDaveX[from]) >> 4);
    for (UBYTE y = 0; y < LEVEL_H; y++) {
        UBYTE *c = Cell(col, y);
        if (g_flags[*c] & TILE_SOLID) break;          // the bottom of the shaft
        if (g_flags[c[-1]] & TILE_SOLID) c[-1] = T_BLUE_BRICK;
        if (g_flags[c[1]] & TILE_SOLID)  c[1]  = T_BLUE_BRICK;
    }
}

// Dave at the top of warp zone `from`'s shaft (from = the level warped from).
static void SpawnZone(Dave *d, UBYTE from) {
    Dave z = { 0 };
    *d = z;
    g_stepSound = 0;
    d->viewPx = d->scrollTo = d->prevViewPx = (short)(kZoneView[from] * TILE);
    d->px = d->prevX = (short)(d->viewPx + kZoneDaveX[from]);
    d->py = d->prevY = ZONE_START_Y;
    d->face = FACE_FRONT;                     // DAVE_STAND; no sideways drift
    d->state = ST_FALLING;
}

// The warp zone tube: Dave above the top of the screen, between its walls.
#define TUBE_COL 9
static void SpawnTube(Dave *d) {
    Dave z = { 0 };
    *d = z;
    d->px = d->prevX = TUBE_COL * TILE;
    d->py = d->prevY = -TILE;
    d->face = FACE_FRONT;                     // DAVE_STAND, straight down
    d->state = ST_FALLING;
}

static void Spawn(Dave *d, UBYTE level) {
    Dave z = { 0 };
    *d = z;
    g_stepSound = 0;                          // first step clicks
    d->px = (short)((g_warp ? 1 : startX[level]) * TILE);
    d->py = (short)((g_warp ? 4 : startY[level]) * TILE);
    d->prevX = d->px;
    d->prevY = d->py;
    // facing the player: a new level (or a respawn) starts on DAVE_STAND
    d->face = FACE_FRONTR;
}

// Ticks don't land on every frame (81 per 50), so draw Dave between his last
// two tick positions, `frac` fifths of the way, for even motion.
// Big jumps (respawn, wrap at the bottom) are not interpolated.
static short Lerp(short prev, short cur, UBYTE frac, short limit) {
    short dv = (short)(cur - prev);
    if (dv > limit || dv < -limit) return cur;
    short n = (short)(dv * frac * 2);             // tenths of a pixel
    return (short)(prev + (n >= 0 ? divsw(n + 5, 10) : -divsw(5 - n, 10)));
}

typedef struct { UBYTE left, right, up, down, fire, jet; } Input;

// Fire: Ctrl or joystick button 1 (held = autofire once the bullet is gone).
// Jetpack on/off: Shift (either) or joystick button 2 (on press).
static Input ReadInput(void) {
    static UBYTE fire2Was;
    Input in;
    in.left  = Joy1Left()  || KeyDown(RAWKEY_LEFT);
    in.right = Joy1Right() || KeyDown(RAWKEY_RIGHT);
    in.up    = Joy1Up()    || KeyDown(RAWKEY_UP);
    in.down  = Joy1Down()  || KeyDown(RAWKEY_DOWN);
    in.fire  = Joy1Fire()  || KeyDown(RAWKEY_CTRL);
    UBYTE f2 = (UBYTE)Joy1Fire2();
    UBYTE shift = (UBYTE)(KeyHit(RAWKEY_LSHIFT) | KeyHit(RAWKEY_RSHIFT));   // both latches cleared
    in.jet   = (UBYTE)(shift || (f2 && !fire2Was));
    fire2Was = f2;
    return in;
}

static Input WarpInput(void) {
    Input in = { 0, 0, 0, 0, 0, 0 };          // tube: just fall
    in.right = g_warp == WARP_CORRIDOR;       // corridor: walk right
    return in;
}

// ---- attract-mode demo: the original's recorded input, one byte a tick
// (format in tools/extract_dave_exe.py). NULL while a player plays.
static const UBYTE *g_demo, *g_demoEnd;
static UBYTE g_demoIdle;                      // ticks left of a "0x20 n" run
static UBYTE g_demoJetWas;
static UBYTE g_demoDone;                      // reached the closing 0x30

static Input DemoInput(void) {
    // direction 0 = up, clockwise in 45 degree steps to 7 = up-left
    static const UBYTE kDir[8] = { 1, 1|2, 2, 2|4, 4, 4|8, 8, 8|1 };   // up right down left
    Input in = { 0, 0, 0, 0, 0, 0 };
    UBYTE b = 8;                              // no input
    if (g_demoIdle) g_demoIdle--;
    else if (g_demo + 1 >= g_demoEnd || *g_demo == 0x30) g_demoDone = 1;
    else if (*g_demo == 0x20) { g_demoIdle = g_demo[1]; g_demo += 2; }
    else b = *g_demo++;
    UBYTE dir = (UBYTE)(b & 15);
    if (dir < 8) {
        in.up    = kDir[dir] & 1;
        in.right = (kDir[dir] >> 1) & 1;
        in.down  = (kDir[dir] >> 2) & 1;
        in.left  = (kDir[dir] >> 3) & 1;
    }
    in.fire = (UBYTE)(b >> 7);                // button 1, held
    UBYTE jet = (b & 0xc0) == 0x40;           // button 2 (the original ignores
    in.jet = (UBYTE)(jet && !g_demoJetWas);   // it while button 1 is down)
    g_demoJetWas = jet;
    return in;
}

// One game tick. Returns LEVEL_DONE / LEVEL_GAME_OVER, or -1 to go on.
static int Tick(Dave *d, Input in, GameState *g) {
    if (++g_lv.animSub == ANIM_TICKS) { g_lv.animSub = 0; g_lv.animPhase++; }
    d->prevX = d->px;
    d->prevY = d->py;
    d->prevViewPx = d->viewPx;
    if (g_lv.over) {                          // GAME OVER: monsters fly on
        g_keyJet = g_keyFire = 0;
        ActorsTick(d, g);
        return -1;
    }

    // Level start (and after a death): nothing moves, Dave blinks, until a
    // key or the stick is pressed — but not in the first BLINK_MIN ticks, so
    // a key still held from before doesn't start him at once (deadly-dave's
    // game_level_blinking; hidden 10, shown 10, hidden 11, repeat).
    if (g_lv.waiting) {
        if (++g_lv.blink >= 32) g_lv.blink = BLINK_LOOP;
        if (g_lv.blink >= BLINK_MIN &&
            (in.left || in.right || in.up || in.down || in.fire || in.jet)) {
            g_lv.waiting = 0;
            if (d->state == ST_FALLING) DaveSound(SND_FALLING);   // zone shaft
        }
        return -1;
    }

    // ---- view: slide 15 tiles near an edge; the game waits meanwhile ----
    if (d->viewPx < d->scrollTo) {
        d->viewPx = (short)(d->viewPx + SCROLL_PX > d->scrollTo ? d->scrollTo : d->viewPx + SCROLL_PX);
        return -1;
    }
    if (d->viewPx > d->scrollTo) {
        d->viewPx = (short)(d->viewPx - SCROLL_PX < d->scrollTo ? d->scrollTo : d->viewPx - SCROLL_PX);
        return -1;
    }
    short rel = (short)(d->px - d->viewPx);
    if (g_warp) rel = 100;                    // the corridor never scrolls
    if (rel > 320 - 40 && d->viewPx < MAX_VIEW_PX) {
        d->scrollTo = (short)(d->viewPx + 15 * TILE > MAX_VIEW_PX ? MAX_VIEW_PX : d->viewPx + 15 * TILE);
        return -1;
    }
    if (rel < 30 && d->viewPx > 0) {
        d->scrollTo = (short)(d->viewPx < 15 * TILE ? 0 : d->viewPx - 15 * TILE);
        return -1;
    }

    g_keyJet  = in.jet;
    g_keyFire = in.fire;
    DaveTick(d, in.left, in.right, in.up, in.down);
    ActorsTick(d, g);

    if (d->state == ST_DEAD) {
        if (!g->lives) return LEVEL_GAME_OVER;
        g->lives--;
        g_lv.hudDirty = 1;
        // view back to the start: the level's, or the warp zone's shaft
        if (g->zone) SpawnZone(d, g->level);
        else Spawn(d, g_mapLevel);
        g_lv.waiting = 1;                     // blink until a key, as at the start
        g_lv.blink = 0;
        g_bullet.on = 0;
        return -1;
    }

    if (Touch(d, g)) return LEVEL_DONE;
    if (g_warp == WARP_CORRIDOR && d->px > 300) return LEVEL_DONE;
    if (g_warp == WARP_TUBE && d->py >= LEVEL_H * TILE) return LEVEL_DONE;  // out the bottom

    if (d->px < 0 || d->px > (LEVEL_W - 1) * TILE) {
        // off the edge of the map (over the top of the screen): a warp zone,
        // if this level has one; never from inside a zone
        if (!g_warp && !g->zone && kZoneMap[g->level]) {
            g->zone = kZoneMap[g->level];
            SoundStop();
            return LEVEL_WARP_ZONE;
        }
        d->px = d->px < 0 ? 0 : (LEVEL_W - 1) * TILE;
    }
    // falling out of the bottom of the screen wraps to the top
    if (d->py > 200 - DOS_PLAY_Y) d->py = -20 - DOS_PLAY_Y;
    return -1;
}

// Returns -1 when Dave is not drawn (end of the explosion).
static short DaveFrame(const Dave *d) {
    if (g_lv.waiting && (g_lv.blink < BLINK_LOOP || g_lv.blink >= BLINK_LOOP + 10))
        return -1;                            // blinking, before the first key
    UBYTE walk = (UBYTE)(d->steps & 7);
    switch (d->state) {
        case ST_BURNING:
            if (d->ticksInState > BURN_SHOWN) return -1;
            return (short)(DAVE_DEAD0 + (divuw(d->ticksInState, 30) & 3));
        case ST_DEAD:
            return -1;
        case ST_FALLING:
            if (d->face == FACE_LEFT)   return DAVE_WALK_L0;
            if (d->face == FACE_RIGHT)  return DAVE_WALK_R0;
            if (d->face == FACE_FRONTR) return DAVE_JUMP_R;
            if (d->face == FACE_FRONTL) return DAVE_JUMP_L;
            return DAVE_STAND;
        case ST_JUMPING:
            return (d->face == FACE_LEFT || d->face == FACE_FRONTL) ? DAVE_JUMP_L : DAVE_JUMP_R;
        case ST_JETPACK: {
            UWORD t = d->ticksInState;
            UBYTE f = (UBYTE)(t - muluw(divuw(t, 3), 3));
            return (short)((d->face == FACE_LEFT ? DAVE_JET_L0 : DAVE_JET_R0) + f);
        }
        case ST_CLIMBING:
            if (d->climbState == CLIMB_JUMP_R) return DAVE_JUMP_R;
            if (d->climbState == CLIMB_JUMP_L) return DAVE_JUMP_L;
            if (walk < 2) return DAVE_CLIMB0;                 // hands up
            if (walk == 4 || walk == 5) return DAVE_CLIMB2;   // left hand
            return DAVE_CLIMB1;                               // right hand
    }
    // standing / walking: stand, stand, hands-free, hands-free,
    // stand, stand, serious, serious (by step); idle pose once stopped
    if (d->face == FACE_RIGHT || d->face == FACE_LEFT) {
        UBYTE base = d->face == FACE_RIGHT ? DAVE_WALK_R0 : DAVE_WALK_L0;
        if (d->state == ST_STANDING && !d->keyLR) return (short)(base + 1);
        if (walk == 2 || walk == 3) return base;              // hands-free
        if (walk >= 6) return (short)(base + 2);              // serious
        return (short)(base + 1);                             // stand
    }
    return DAVE_STAND;
}

// ---- animated tiles (fire, vines, water, trophy) ----
static UBYTE AnimMod(UBYTE tile) {
    switch (tile) {
        case 6: case 25: return 4;            // fire, vines
        case 10: case 36: return 5;           // trophy, water
    }
    return 1;
}

// frame = base + (map column + tick/ANIM_TICKS) % mod
static UBYTE TileFrame(UBYTE t, UWORD tx) {
    UBYTE mod = AnimMod(t);
    if (mod == 1) return t;
    UWORD v = (UWORD)(tx + g_lv.animPhase);
    return (UBYTE)(t + v - muluw(divuw(v, mod), mod));
}

// ---- the level canvas (TileBand): all of the map, Dave drawn into it ----
static TileBand *g_band;
static UBYTE     g_dirtyX[8], g_dirtyY[8], g_numDirty;   // changed cells

static void DrawCell(short tx, short ty) {
    if (tx < 0 || ty < 0 || tx >= LEVEL_W || ty >= LEVEL_H) return;
    TileBandSetTile(g_band, (UWORD)tx, (UWORD)ty, TileFrame(*Cell((UWORD)tx, (UWORD)ty), (UWORD)tx));
}

static void MarkDirty(UBYTE tx, UBYTE ty) {
    if (g_numDirty < 8) { g_dirtyX[g_numDirty] = tx; g_dirtyY[g_numDirty] = ty; g_numDirty++; }
    else g_lv.mapDirty = 1;                       // overflow: never in practice
}

static void CollectAnim(void) {
    g_numAnim = 0;
    for (UBYTE ty = 0; ty < LEVEL_H; ty++)
        for (UBYTE tx = 0; tx < LEVEL_W && g_numAnim < MAX_ANIM; tx++) {
            UBYTE t = *Cell(tx, ty);
            if (AnimMod(t) > 1) {
                AnimCell *a = &g_anim[g_numAnim++];
                a->tx = tx; a->ty = ty; a->base = t; a->mod = AnimMod(t);
            }
        }
}

// ---- everything drawn into the canvas this frame (Dave, monsters, shots,
// bullet). Each saves the background it covers (SADE BobBack); next frame
// restores them in reverse order before anything else touches the canvas.
//
// The canvas is on screen while this runs, so the work races the beam: a
// sprite redrawn after the beam passed its first line is missing for that
// frame (it flickered — level 7's two UFOs near the top). Sprites are
// queued and drawn top line first, each before the beam gets to it.
typedef struct {
    BobImage *img;
    short x, y;                // world position of the cell
    short top, bottom;         // rows the blit covers
    short x0, x1;              // canvas x the blit covers (whole words)
} Pending;
#define MAX_DRAWN 12
static BobBack g_back[MAX_DRAWN];       // allocated in LevelInit
static Pending g_pend[MAX_DRAWN];
static UBYTE   g_numPend, g_numDrawn;


static void RestoreActors(void) {
    for (UBYTE i = g_numDrawn; i-- > 0; ) BobRestore(&g_back[i]);
    g_numDrawn = 0;
    g_numPend = 0;                                // new frame, new queue
}

// Queue one frame of a bank at world (x, y); w is the art width, for the
// off-screen test. DrawQueued draws them.
static void DrawActor(SpriteBank *bank, UWORD frame, short x, short y,
                      UBYTE w, short viewPx) {
    if (x + w <= viewPx - 16 || x >= viewPx + 336) return;   // off screen
    if (g_numPend == MAX_DRAWN) return;
    BobImage *img = SpriteBankImage(bank, frame);
    short top = (short)(y + img->drawTop);
    UBYTE i = g_numPend++;
    for (; i && g_pend[i - 1].top > top; i--) g_pend[i] = g_pend[i - 1];
    Pending *p = &g_pend[i];
    p->img = img; p->x = x; p->y = y;
    p->top = top; p->bottom = (short)(top + _BobRows(img));
    p->x0 = (short)((x + 16) & ~15);                  // canvas has a 16px margin
    p->x1 = (short)(p->x0 + (_BobWords(img) << 4));
}

// Does a queued sprite's blit cover tile (tx, ty)?
static UBYTE UnderSprite(short tx, short ty) {
    short x0 = (short)((tx << 4) + 16), y0 = (short)(ty << 4);
    for (UBYTE i = 0; i < g_numPend; i++) {
        const Pending *p = &g_pend[i];
        if (p->x0 < x0 + 16 && x0 < p->x1 && p->top < y0 + 16 && y0 < p->bottom) return 1;
    }
    return 0;
}

// Draw the queue into the canvas, top first, saving what each covers.
static void DrawQueued(Bob *bob) {
    Screen *canvas = TileBandView(g_band);
    for (UBYTE i = 0; i < g_numPend; i++) {
        bob->image = g_pend[i].img;
        bob->x = (short)(g_pend[i].x + 16);           // canvas has a 16px margin
        bob->y = (short)(g_pend[i].y + BAND_TOP);     // and BAND_TOP rows above
        BobDrawSave(bob, canvas, &g_back[g_numDrawn]);
        g_numDrawn++;
    }
}

// Animated tiles near the view get their new frame a few per frame
// (a full screen of fire/water at once would overrun the frame). PickAnim
// chooses this frame's cells once the sprites are queued. A cell under a
// sprite is drawn before the sprites (DrawAnim(1)), so the saved copy holds
// the new frame — updated after the save, next frame's restore would put
// the old frame back (it froze when Dave stood just left of it). All other
// cells wait until the sprites are drawn (DrawAnim(0)): the sprites race
// the beam, the tiles don't need to.
//
// A pass always runs to the end of the list before the next one starts:
// restarting it on every phase change starved the cells late in the list
// when many were in view (level 2's bottom row of fire froze).
#define ANIM_PER_FRAME 4
static UWORD g_animPos = 0xffff;                  // next cell of this pass
static UBYTE g_animPassPhase;                     // animPhase this pass draws
static UBYTE g_animTx[ANIM_PER_FRAME], g_animTy[ANIM_PER_FRAME], g_numAnimNow;

static void PickAnim(short viewPx) {
    short c0 = (short)((viewPx >> 4) - 1), c1 = (short)(c0 + VIEW_W + 2);
    if (g_animPos >= g_numAnim && g_animPassPhase != g_lv.animPhase) {
        g_animPos = 0;                            // start a new pass
        g_animPassPhase = g_lv.animPhase;
    }
    g_numAnimNow = 0;
    for (; g_animPos < g_numAnim && g_numAnimNow < ANIM_PER_FRAME; g_animPos++) {
        const AnimCell *a = &g_anim[g_animPos];
        short tx = a->tx, ty = a->ty;
        if (tx < c0 || tx > c1 || *Cell(a->tx, a->ty) != a->base) continue;
        g_animTx[g_numAnimNow] = (UBYTE)tx;
        g_animTy[g_numAnimNow] = (UBYTE)ty;
        g_numAnimNow++;
    }
}

static void DrawAnim(UBYTE underSprites) {
    for (UBYTE i = 0; i < g_numAnimNow; i++)
        if (UnderSprite(g_animTx[i], g_animTy[i]) == underSprites)
            DrawCell(g_animTx[i], g_animTy[i]);
}

// ---- status bars: byte-aligned CPU copies from hud.iff into the canvas ----
static const IlbmImage *g_hud;

static void HudPut(UWORD sx, UWORD sy, UWORD w, UWORD h,
                   IlbmImage *dst, UWORD dx, UWORD dy) {
    UWORD bytes = (UWORD)((w + 7) >> 3);
    for (UBYTE p = 0; p < dst->depth; p++) {
        const UBYTE *s = p < g_hud->depth
            ? g_hud->planes[p] + muluw(sy, g_hud->rowBytes) + (sx >> 3) : NULL;
        UBYTE *d = dst->planes[p] + muluw(dy, dst->rowBytes) + (dx >> 3);
        for (UWORD r = 0; r < h; r++) {
            for (UWORD b = 0; b < bytes; b++) d[b] = s ? s[b] : 0;
            if (s) s += g_hud->rowBytes;
            d += dst->rowBytes;
        }
    }
}

static void HudClear(IlbmImage *dst, UWORD dx, UWORD dy, UWORD w, UWORD h) {
    UWORD bytes = (UWORD)((w + 7) >> 3);
    for (UBYTE p = 0; p < dst->depth; p++) {
        UBYTE *d = dst->planes[p] + muluw(dy, dst->rowBytes) + (dx >> 3);
        for (UWORD r = 0; r < h; r++, d += dst->rowBytes)
            for (UWORD b = 0; b < bytes; b++) d[b] = 0;
    }
}

#define HUD_RECT_X(x, y, w, h) (x)
#define HUD_RECT_Y(x, y, w, h) (y)
#define HUD_RECT_W(x, y, w, h) (w)
#define HUD_RECT_H(x, y, w, h) (h)
#define HUD_X(r) HUD_RECT_X(r)
#define HUD_Y(r) HUD_RECT_Y(r)
#define HUD_W(r) HUD_RECT_W(r)
#define HUD_H(r) HUD_RECT_H(r)

static void HudNumber(IlbmImage *dst, UWORD x, UWORD y, ULONG v, UBYTE digits) {
    for (short i = (short)(digits - 1); i >= 0; i--) {
        UWORD q = divuw(v, 10);
        UWORD n = (UWORD)(v - muluw(q, 10));
        HudPut((UWORD)(HUD_X(HUD_DIGIT0) + n * 8), HUD_Y(HUD_DIGIT0),
               HUD_W(HUD_DIGIT0), HUD_H(HUD_DIGIT0), dst, (UWORD)(x + i * 8), y);
        v = q;
    }
}

#define BOTTOM_Y 186                 // under the band (ends at PLAY_Y + 160)

// Solid pixel rectangle in palette colour `color` (any x, any width).
static void HudFill(IlbmImage *dst, UWORD x, UWORD y, UWORD w, UWORD h, UBYTE color) {
    for (UWORD r = 0; r < h; r++) {
        ULONG row = muluw((UWORD)(y + r), dst->rowBytes);
        for (UWORD px = x; px < x + w; px++) {
            UBYTE bit = (UBYTE)(0x80 >> (px & 7));
            for (UBYTE p = 0; p < dst->depth; p++) {
                UBYTE *b = dst->planes[p] + row + (px >> 3);
                if (color & (1 << p)) *b |= bit; else *b &= (UBYTE)~bit;
            }
        }
    }
}

// Layout after lmdave: SCORE: 00000   LEVEL 01   DAVES: (faces)
static void DrawHud(IlbmImage *bg, const GameState *g) {
    HudPut(HUD_SCORE, bg, 0, 2);
    HudNumber(bg, 64, 2, g->score, 5);
    HudPut(HUD_LEVEL, bg, 120, 2);
    HudNumber(bg, 168, 2, (ULONG)g_hudLevel + 1, 2);
    HudPut(HUD_DAVES, bg, 200, 2);
    for (UBYTE i = 0; i < MAX_LIVES; i++) {
        UWORD x = (UWORD)(264 + i * 16);
        if (i < g->lives) HudPut(HUD_FACE, bg, x, 1);
        else              HudClear(bg, x, 1, HUD_W(HUD_FACE), HUD_H(HUD_FACE));
    }

    // bottom row: jetpack + fuel bar (original layout), else the door hint
    HudClear(bg, 0, BOTTOM_Y, 248, 14);
    if (g_lv.fuel) {
        HudPut(HUD_JETPACK, bg, 0, BOTTOM_Y);
        HudPut(HUD_JETBAR, bg, 72, BOTTOM_Y);
        UWORD units = divuw(g_lv.fuel, 15);              // 0..60, 2px each
        if (units) HudFill(bg, 76, BOTTOM_Y + 4, (UWORD)(units * 2), 4, HUD_JETBAR_COLOR);
        g_lv.barUnits = (UBYTE)units;
    } else if (g_lv.trophy) {
        HudPut(HUD_DOOR, bg, 72, BOTTOM_Y);
    }
    if (g_lv.gun)     HudPut(HUD_GUN, bg, 256, BOTTOM_Y);
}

// The status bars live in the Screen's own planes (both buffers, so it
// doesn't matter which one is displayed); the playfield rows show the band.
// Only the plane fields are set: an IlbmImage holds a 1 KB palette, so the
// callers keep theirs static rather than on the 4 KB stack.
static void ScreenImage(const Screen *scr, UBYTE back, IlbmImage *img) {
    img->width = scr->width; img->height = scr->height;
    img->depth = scr->depth; img->rowBytes = scr->rowBytes;
    for (UBYTE p = 0; p < scr->depth; p++)
        img->planes[p] = back ? scr->backPlanes[p] : scr->planes[p];
}

// Blitter-clear both screen buffers, so nothing of the title (or the
// previous map) shows while the next one is set up. A few ms.
static void ClearScreenBuffers(Screen *scr) {
    GfxClear(scr);                            // draw buffer
    if (!scr->doubleBuffer) return;
    for (UBYTE p = 0; p < scr->depth; p++) {  // and the displayed one
        UBYTE *t = scr->planes[p]; scr->planes[p] = scr->backPlanes[p]; scr->backPlanes[p] = t;
    }
    GfxClear(scr);
    for (UBYTE p = 0; p < scr->depth; p++) {
        UBYTE *t = scr->planes[p]; scr->planes[p] = scr->backPlanes[p]; scr->backPlanes[p] = t;
    }
}

// Fuel burning down: blank just the bar columns that emptied (2px per
// unit) instead of repainting the whole status bar — that took long enough
// to make flying stutter every ~0.2 s.
static void ShrinkFuelBar(Screen *scr, UBYTE units) {
    for (UBYTE back = 0; back <= (scr->doubleBuffer ? 1 : 0); back++) {
        static IlbmImage img;
        ScreenImage(scr, back, &img);
        HudFill(&img, (UWORD)(76 + units * 2), BOTTOM_Y + 4,
                (UWORD)((g_lv.barUnits - units) * 2), 4, 0);
    }
    g_lv.barUnits = units;
}

static void DrawHudAll(Screen *scr, const GameState *g) {
    for (UBYTE back = 0; back <= (scr->doubleBuffer ? 1 : 0); back++) {
        static IlbmImage img;
        ScreenImage(scr, back, &img);
        DrawHud(&img, g);
    }
}

// ---- resources shared by every level (decoding them per level took ~2 s) ----
static struct {
    IlbmImage  *sheet, *hud, *fontSheet;
    TileSet    *ts;
    SpriteBank *bank, *monBank, *shotBank;
    Bob        *daveBob;
    Font       *font;
} R;

void LevelShutdown(void) {
    for (UBYTE i = 0; i < MAX_DRAWN; i++) BobBackFree(&g_back[i]);
    if (R.daveBob)   BobFree(R.daveBob);
    if (R.bank)      SpriteBankFree(R.bank);
    if (R.monBank)   SpriteBankFree(R.monBank);
    if (R.shotBank)  SpriteBankFree(R.shotBank);
    if (R.font)      FontFree(R.font);
    if (R.fontSheet) IlbmFree(R.fontSheet);
    if (R.ts)        TileSetFree(R.ts);
    if (R.hud)       IlbmFree(R.hud);
    if (R.sheet)     IlbmFree(R.sheet);
    R.sheet = R.hud = R.fontSheet = NULL;
    R.ts = NULL; R.bank = R.monBank = R.shotBank = NULL; R.daveBob = NULL; R.font = NULL;
}

int LevelInit(const LevelAssets *a, void (*progress)(void)) {
    #define STEP() do { if (progress) progress(); } while (0)
    SpriteBankProgress = progress;            // one step per sprite cut
    R.sheet     = IlbmLoad(a->tilesIff, a->tilesSize);           STEP();
    R.hud       = IlbmLoad(a->hudIff, a->hudSize);               STEP();
    R.fontSheet = IlbmLoad(a->fontIff, a->fontSize);             STEP();
    R.ts        = R.sheet ? TileSetCreate(R.sheet, TILE, TILE) : NULL;
    R.font      = R.fontSheet ? FontCreate(R.fontSheet, 8, 8, ' ') : NULL;
    STEP();
    R.bank      = CreateSpriteBankFromFile(a->daveIff, a->daveSize,
                                           0, 0, DAVE_CELL_W, DAVE_CELL_H, DAVE_FRAMES);
    R.monBank   = CreateSpriteBankFromFile(a->monIff, a->monSize,
                                           0, 0, MON_CELL_W, MON_CELL_H, MON_FRAMES);
    R.shotBank  = CreateSpriteBankFromFile(a->monIff, a->monSize,
                                           0, SHOT_Y, MON_CELL_W, SHOT_CELL_H, SHOT_FRAMES);
    R.daveBob   = R.bank ? BobCreate(SpriteBankImage(R.bank, DAVE_STAND)) : NULL;
    SpriteBankTrim(R.bank);                   // blit the art, not the cell:
    SpriteBankTrim(R.monBank);                // a UFO is 18x8 in a 32x24 cell
    SpriteBankTrim(R.shotBank);
    STEP();
    UBYTE backs = 1;
    // largest sprite: 32x24 cells, at the screen's depth (the sheets'): a
    // save that doesn't fit is skipped and the sprite leaves a trail — on
    // AGA (8 planes) the jetpack Dave did with 5-plane buffers
    UBYTE depth = R.sheet ? R.sheet->depth : 5;
    for (UBYTE i = 0; i < MAX_DRAWN; i++)
        backs &= (UBYTE)BobBackAlloc(&g_back[i], MON_CELL_W, MON_CELL_H, depth);
    STEP();
    SpriteBankProgress = NULL;
    #undef STEP
    if (backs && R.ts && R.hud && R.font && R.daveBob && R.monBank && R.shotBank) return 1;
    LevelShutdown();
    return 0;
}

// Monsters (or their explosion), plasma shots and Dave's bullet.
static void DrawMonsters(UBYTE acc, short viewPx) {
    for (UBYTE i = 0; i < g_numMon; i++) {
        const Monster *m = &g_mon[i];
        const MonKind *k = &kKinds[m->kind];
        short x = Lerp(m->prevX, m->x, acc, 8), y = Lerp(m->prevY, m->y, acc, 8);
        if (m->state == MS_ACTIVE) {
            UWORD f;
            if (k->pingPong) {
                static const UBYTE seq[6] = { 0, 1, 2, 3, 2, 1 };
                f = seq[m->step - muluw(divuw(m->step, 6), 6)];
            } else {
                f = (UWORD)((m->step >> 1) & 3);
            }
            DrawActor(R.monBank, (UWORD)(k->frame0 + f), x, y, k->artW, viewPx);
        } else if (m->state == MS_BURNING) {
            UWORD f = (UWORD)((divuw(m->timer, 30) + 2) & 3);   // deadly-dave order
            DrawActor(R.bank, (UWORD)(DAVE_DEAD0 + f), x, y, 20, viewPx);
        }
        if (m->shotOn) {
            UWORD f = divuw((UWORD)(m->shotTicks >> 2), 3);
            f = (UWORD)((m->shotTicks >> 2) - muluw(f, 3));
            DrawActor(R.shotBank,
                      (UWORD)((m->shotDir > 0 ? SHOT_PLASMA_R0 : SHOT_PLASMA_L0) + f),
                      Lerp(m->shotPrevX, m->shotX, acc, 4), m->shotY, 20, viewPx);
        }
    }
    if (g_bullet.on)
        DrawActor(R.shotBank, g_bullet.dir > 0 ? SHOT_BULLET_R : SHOT_BULLET_L,
                  Lerp(g_bullet.prevX, g_bullet.x, acc, 4), g_bullet.y, 12, viewPx);
}

// Start of the frame's work: the line after the band (the canvas is off
// screen and its copper splits are done; the status rows below come from
// the screen buffers), 35 lines earlier than SadeWaitVbl.
static void WaitDisplayEnd(void) {
    UWORD end = (UWORD)(CopScreenLine(PLAY_Y + LEVEL_H * TILE));
    UWORD l;
    do { l = (UWORD)((*(volatile ULONG*)0xDFF004 >> 8) & 0x1ff); } while (l >= end);
    do { l = (UWORD)((*(volatile ULONG*)0xDFF004 >> 8) & 0x1ff); } while (l < end);
}

// ---- ESC: the original's "QUIT? (Y OR N):" box over the playfield ----
#define QUIT_W     168               // box size (screen x 80..247, y 82..119)
#define QUIT_H      38
#define QUIT_SX     80
#define QUIT_Y     (82 - BAND_Y)     // canvas row
#define QUIT_GREY  18               // dark grey frame
#define QUIT_WHITE  2
#define QUIT_INK    0
#define QUIT_DOT   25                // blinking magenta cursor

// Drawn into the canvas at the displayed view x; returns 1 for Y (quit),
// 0 for N / ESC (the covered cells are redrawn and the level carries on).
static int QuitDialog(UWORD viewX) {
    Screen *c = TileBandView(g_band);
    short x0 = (short)((viewX + 16 + QUIT_SX) & ~7), y0 = QUIT_Y;   // TextDraw: x % 8
    short x1 = (short)(x0 + QUIT_W - 1), y1 = (short)(y0 + QUIT_H - 1);

    RestoreActors();                              // canvas back to bare tiles
    GfxFillBox(c, x0, y0, x1, y1, QUIT_GREY);
    GfxFillBox(c, x0 + 4, y0 + 4, x1 - 4, y1 - 4, QUIT_WHITE);
    TextDraw(R.font, c, x0 + 16, y0 + 15, "QUIT? (Y OR N):", QUIT_INK, QUIT_WHITE);

    KeyHit(RAWKEY_Y); KeyHit(RAWKEY_Z); KeyHit(RAWKEY_N); KeyHit(RAWKEY_ESC);
    UBYTE rmbHeld = 1, blink = 0, shown = 2;
    SadeClock clk;                                // blink in time, not frames
    SadeClockStart(&clk, UI_TICKS_PER_SEC);
    int quit;
    for (;;) {
        WaitDisplayEnd();
        KeyboardUpdate();
        // Y by position and by label (QWERTZ swaps Y/Z); RMB again = yes
        if (KeyHit(RAWKEY_Y) || KeyHit(RAWKEY_Z)) { quit = 1; break; }
        if (KeyHit(RAWKEY_N) || KeyHit(RAWKEY_ESC)) { quit = 0; break; }
        if (!MouseRight()) rmbHeld = 0;
        else if (!rmbHeld) { quit = 1; break; }
        blink = (UBYTE)(blink + SadeClockTicks(&clk));
        UBYTE on = (UBYTE)(!(blink & 16));
        if (on != shown) {
            shown = on;
            GfxFillCircle(c, (short)(x0 + 16 + 15 * 8 + 9), (short)(y0 + 18), 4,
                          on ? QUIT_DOT : QUIT_WHITE);
        }
    }
    if (quit) return 1;

    for (short ty = (short)((y0 - BAND_TOP) >> 4); ty <= (short)((y1 - BAND_TOP) >> 4); ty++)
        for (short tx = (short)((x0 - 16) >> 4); tx <= (short)((x1 - 16) >> 4); tx++)
            DrawCell(tx, ty);
    while (MouseRight()) { WaitDisplayEnd(); KeyboardUpdate(); }
    return 0;
}

// The high score table over the playfield at the displayed view x, as the
// original draws it over the frozen level; then the name for row `rank`
// (-1: the table only).
static void ScoresOverPlayfield(UWORD viewX, int rank) {
    Screen *c = TileBandView(g_band);
    RestoreActors();                              // canvas back to bare tiles
    short tx = (short)((viewX + 16 + SCORES_X) & ~7);  // TextDraw: x % 8
    short ty = (short)(SCORES_Y + PLAY_Y - DOS_PLAY_Y - BAND_Y);   // canvas row
    ScoresShow(c, tx, ty);
    ScoresEnterName(c, tx, ty, rank);
}

// Plays one map: a level, or (warp) the between-levels corridor.
static int RunMap(Screen *scr, GameState *g, const LevelAssets *a,
                  const UBYTE *mapData, ULONG mapSize, const char *message) {
    (void)a;
    int result = LEVEL_QUIT;
    UBYTE lvl = g_mapLevel;
    IlbmImage  *sheet = R.sheet;
    SpriteBank *bank  = R.bank;
    Bob        *daveBob = R.daveBob;
    Font       *font  = R.font;

    UBYTE      *mapCopy = (UBYTE*)SadeAlloc(mapSize, MEMF_PUBLIC);
    TileMap    *tm    = NULL;
    if (mapCopy) {
        for (ULONG i = 0; i < mapSize; i++) mapCopy[i] = mapData[i];
        tm = TileMapLoad(mapCopy, mapSize, R.ts);
    }
    g_band = NULL;
    if (!tm) goto done;

    g_cells = (UBYTE*)mapCopy + 12;           // == tm->cells, but writable
    g_flags = tm->tileFlags;
    if (g->zone && !g_warp) BlueZoneShaft(g->level);   // before the band renders
    g_hud   = R.hud;
    Level z = { 0 };
    g_lv = z;
    g_numDirty = 0;
    g_path = (!g_warp && mapSize >= 12 + LEVEL_W * LEVEL_H + 256 + LEVEL_PATH_BYTES)
           ? (const BYTE*)(mapCopy + 12 + LEVEL_W * LEVEL_H + 256) : NULL;
    SpawnMonsters(lvl);
    g_bullet.on = 0;
    g_numDrawn = 0;                           // fresh canvas: nothing to restore

    Dave d;
    if (g_warp == WARP_TUBE) SpawnTube(&d);
    else if (g->zone && !g_warp) SpawnZone(&d, g->level);
    else Spawn(&d, lvl);
    g_lv.waiting = g_warp == WARP_NONE;      // play waits for a key; cut-scenes run
    if (g_warp == WARP_TUBE) DaveSound(SND_FALLING);

    IlbmApplyPalette(sheet, scr);
    ClearScreenBuffers(scr);
    DrawHudAll(scr, g);
    g_band = TileBandCreateTop(tm, scr, BAND_Y, BAND_TOP);  // renders the whole level
    if (!g_band) goto done;
    CollectAnim();

    Screen *canvas = TileBandView(g_band);
    if (message)                              // canvas x = screen x + 16
        TextDraw(font, canvas, WARP_TEXT_X + 16, WARP_TEXT_Y, message, WARP_INK, 0);
    if (g_warp == WARP_TUBE) {                // "WARP" left, "ZONE" right of the tube
        static IlbmImage img;
        ScreenImage(canvas, 0, &img);
        HudPut(HUD_X(HUD_WARP), HUD_Y(HUD_WARP), HUD_W(HUD_WARP), HUD_H(HUD_WARP),
               &img, TUBE_LABEL_X1 + 16, TUBE_LABEL_Y);
        HudPut(HUD_X(HUD_ZONE), HUD_Y(HUD_ZONE), HUD_W(HUD_ZONE), HUD_H(HUD_ZONE),
               &img, TUBE_LABEL_X2 + 16, TUBE_LABEL_Y);
    }
    // Ticks per second stay the same on a 50 Hz and a 60 Hz display, and
    // frames lost to an overrun are caught up: SadeClock counts real VBLs.
    SadeClock clk;
    SadeClockStart(&clk, TICKS_PER_SEC);     // after the level render above
    UWORD shownX = 0;                         // view x the band shows
    short overFireWas = 1, overMouseWas = 1;  // GAME OVER: fresh presses only

    for (;;) {
        // Everything below lands before the beam reaches the band again.
        WaitDisplayEnd();
        KeyboardUpdate();
        if (g_demo) {                             // any key or button ends a demo
            if (KeyHitAny() >= 0 || MouseRight() || MouseLeft() || Joy1Fire()) break;
        } else if (g_lv.over) {                   // GAME OVER: a key, a button, or time
            short fire = Joy1Fire(), mb = (short)(MouseLeft() || MouseRight());
            if (KeyHitAny() >= 0 || (fire && !overFireWas) || (mb && !overMouseWas) ||
                g_lv.overTicks >= GAME_OVER_TICKS) {
                result = LEVEL_GAME_OVER;
                break;
            }
            overFireWas = fire;
            overMouseWas = mb;
        } else if (KeyHit(RAWKEY_ESC) || MouseRight()) {
            if (QuitDialog(shownX)) break;
            SadeClockSync(&clk);                  // no catch-up burst after the pause
            continue;
        }
        if (g_debugKeys) {
            if (KeyHit(RAWKEY_J) && !g_warp) {            // debug: full jetpack
                g_lv.fuel = JET_FUEL;
                g_lv.hudDirty = 1;
                SoundPlay(SND_GOTSOMETHING);
            }
            if (KeyHit(RAWKEY_G) && !g_warp) {            // debug: the gun
                g_lv.gun = 1;
                g_lv.hudDirty = 1;
                SoundPlay(SND_GOTSOMETHING);
            }
            if (KeyHit(RAWKEY_H)) {                       // debug: invulnerable on/off
                g_invuln = (UBYTE)!g_invuln;
                SoundPlay(g_invuln ? SND_GOTSOMETHING : SND_HITWALL);
            }
            if (KeyHit(RAWKEY_W) && !g_warp && !g->zone && kZoneMap[g->level]) {
                g->zone = kZoneMap[g->level];             // debug: into the warp zone
                SoundStop();
                result = LEVEL_WARP_ZONE;
                break;
            }
            if (KeyHit(RAWKEY_L) && !g_warp) {            // debug: finish the level
                SoundPlay(SND_NEXTLEVEL);
                result = LEVEL_DONE;
                break;
            }
        }

        int r = -1;
        UBYTE ticks = SadeClockTicks(&clk);
        for (UBYTE n = ticks; n && r < 0; n--)
            r = Tick(&d, g_warp ? WarpInput() : g_demo ? DemoInput() : ReadInput(), g);
        if (g_lv.over) g_lv.overTicks = (UWORD)(g_lv.overTicks + ticks);
        if (g_demo && (g_demoDone || r >= 0)) {   // played out: the high scores, as the original
            ScoresOverPlayfield(shownX, -1);
            UiWaitPress(SCORES_WAIT);
            result = LEVEL_DONE;
            break;
        }
        if (r == LEVEL_GAME_OVER) {
            int rank = ScoresRank(g->score);
            if (rank >= 0) {                      // a high score: the table, no GAME OVER
                ScoresInsert(rank, g->score, (UBYTE)(g->level + 1));
                BottomMessage(scr, 80, "YOU GOT A HIGH SCORE!");
                ScoresOverPlayfield(shownX, rank);
                result = r;
                break;
            }
            BottomMessage(scr, 120, "GAME OVER"); // over the status bar; the level runs on
            g_lv.over = 1;
            g_lv.overTicks = 0;
            KeyHitAny();
            r = -1;
        }
        if (r >= 0) { result = r; break; }
        UBYTE frac = SadeClockFrac(&clk, 5);     // 0..4: fifths of the next tick

        shownX = (UWORD)Lerp(d.prevViewPx, d.viewPx, frac, SCROLL_PX);
        TileBandSetX(g_band, scr, shownX);

        RestoreActors();                          // last frame's sprites off
        if (g_lv.mapDirty) {                      // dirty-cell overflow
            g_lv.mapDirty = 0;
            for (short ty = 0; ty < LEVEL_H; ty++)
                for (short tx = (short)((d.viewPx >> 4) - 1); tx <= (short)((d.viewPx >> 4) + VIEW_W); tx++)
                    DrawCell(tx, ty);
        }
        for (UBYTE i = 0; i < g_numDirty; i++) DrawCell(g_dirtyX[i], g_dirtyY[i]);
        g_numDirty = 0;
        // Sprites, top of the band first (see DrawQueued); animated tiles
        // under them before, the rest after (see PickAnim)
        short frame = DaveFrame(&d);
        if (frame >= 0)
            DrawActor(bank, (UWORD)frame, Lerp(d.prevX, d.px, frac, 4),
                      Lerp(d.prevY, d.py, frac, 4), DAVE_ART_W, d.viewPx);
        DrawMonsters(frac, d.viewPx);
        PickAnim(d.viewPx);
        DrawAnim(1);
        DrawQueued(daveBob);
        DrawAnim(0);

        if (g_lv.over) {
            // the GAME OVER box covers the status bar
        } else if (g_lv.hudDirty) {
            g_lv.hudDirty = 0;
            DrawHudAll(scr, g);
        } else if (g_lv.fuel && divuw(g_lv.fuel, 15) < g_lv.barUnits) {
            ShrinkFuelBar(scr, (UBYTE)divuw(g_lv.fuel, 15));   // cheap, while flying
        }
    }

done:
    if (g_band)  TileBandFree(g_band, scr);
    g_band = NULL;
    if (tm)      TileMapFree(tm);
    if (mapCopy) SadeFree(mapCopy, mapSize);
    return result;
}

int LevelRun(Screen *scr, GameState *g, const LevelAssets *a) {
    g_warp = 0;
    g_mapLevel = g->zone ? (UBYTE)(g->zone - 1) : g->level;
    g_hudLevel = g->level;                    // a zone shows the level warped from
    return RunMap(scr, g, a, a->mapData[g_mapLevel], a->mapSize[g_mapLevel], NULL);
}

// The original's intermission (deadly-dave res/levels/warp_right.ddt): a
// blue-brick corridor, the door behind Dave, and Dave walking right on his
// own under "GOOD WORK! ONLY N MORE TO GO!" until he leaves the screen.
int WarpRun(Screen *scr, GameState *g, const LevelAssets *a, UBYTE message) {
    const ULONG cellsSize = (ULONG)LEVEL_W * LEVEL_H;
    const ULONG size = 12 + cellsSize + 256;
    UBYTE *map = (UBYTE*)SadeAlloc(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!map) return LEVEL_DONE;
    static const UBYTE hdr[12] = { 'S','M','A','P', 0, LEVEL_W, 0, LEVEL_H, TILE, TILE, 1, 0 };
    for (UBYTE i = 0; i < 12; i++) map[i] = hdr[i];
    UBYTE *cells = map + 12;
    for (UBYTE x = 0; x < 29; x++) {
        cells[3 * LEVEL_W + x] = T_BLUE_BRICK;
        cells[5 * LEVEL_W + x] = T_BLUE_BRICK;
    }
    cells[4 * LEVEL_W + 0] = T_DOOR;
    const UBYTE *flags = a->mapData[0] + 12 + cellsSize;     // same tile rules
    for (UWORD i = 0; i < 256; i++) map[12 + cellsSize + i] = flags[i];

    char msg[] = "GOOD WORK! ONLY 0 MORE TO GO!";
    msg[16] = (char)('0' + (NUM_LEVELS - 1 - g->level));
    const char *text = msg;                   // the original's three messages
    if (g->level == NUM_LEVELS - 2) text = "  THIS IS THE LAST LEVEL!!!";
    if (g->level == NUM_LEVELS - 1) text = " YES! YOU FINISHED THE GAME!";

    g_warp = WARP_CORRIDOR;
    g_hudLevel = g->level;
    int r = RunMap(scr, g, a, map, size, message ? text : NULL);
    g_warp = WARP_NONE;
    SadeFree(map, size);
    return r;
}

// The original's warp zone cut-scene (deadly-dave res/levels/warp_down.ddt):
// a one-screen tube of blue brick, columns 8 and 10, with "WARP" and "ZONE"
// (original sprites 139/140) either side, and Dave dropping through it from
// above the top until he is out of the bottom.
int WarpTubeRun(Screen *scr, GameState *g, const LevelAssets *a) {
    const ULONG cellsSize = (ULONG)LEVEL_W * LEVEL_H;
    const ULONG size = 12 + cellsSize + 256;
    UBYTE *map = (UBYTE*)SadeAlloc(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!map) return LEVEL_DONE;
    static const UBYTE hdr[12] = { 'S','M','A','P', 0, LEVEL_W, 0, LEVEL_H, TILE, TILE, 1, 0 };
    for (UBYTE i = 0; i < 12; i++) map[i] = hdr[i];
    UBYTE *cells = map + 12;
    const UBYTE *flags = a->mapData[0] + 12 + cellsSize;     // same tile rules
    for (UWORD i = 0; i < 256; i++) map[12 + cellsSize + i] = flags[i];

    for (UBYTE y = 0; y < LEVEL_H; y++) {
        cells[y * LEVEL_W + TUBE_COL - 1] = T_BLUE_BRICK;
        cells[y * LEVEL_W + TUBE_COL + 1] = T_BLUE_BRICK;
    }

    g_warp = WARP_TUBE;
    g_hudLevel = g->level;
    int r = RunMap(scr, g, a, map, size, NULL);
    g_warp = WARP_NONE;
    SadeFree(map, size);
    return r;
}

// The original's attract mode: demo n of assets/game/demos.bin replayed
// on its level, from a fresh game. Returns LEVEL_QUIT when a key or button
// cut it short, otherwise when it ended (its input ran out, the door, or
// game over).
int DemoRun(Screen *scr, const LevelAssets *a, UBYTE n) {
    const UBYTE *d = a->demoData;
    ULONG size = a->demoSize;
    if (!d || size < 4 || n >= d[0] || 4 + (ULONG)n * 4 + 2 > size) return LEVEL_DONE;
    UWORD off = (UWORD)(d[4 + n * 4] | (d[5 + n * 4] << 8));   // little-endian
    if ((ULONG)off + 2 > size || d[off] >= NUM_LEVELS) return LEVEL_DONE;

    GameState g = { 0, 3, d[off], 0 };
    g_demo = d + off + 1;
    g_demoEnd = d + size;
    g_demoIdle = g_demoJetWas = g_demoDone = 0;
    UBYTE invuln = g_invuln;                  // a recording replays as recorded
    g_invuln = 0;
    int r = LevelRun(scr, &g, a);
    g_invuln = invuln;
    g_demo = NULL;
    SoundStop();
    return r;
}

// The shared graphics, for the title screen (valid between LevelInit and
// LevelShutdown).
const TileSet *LevelTiles(void)            { return R.ts; }
const ULONG   *LevelPalette(void)          { return R.sheet->palette; }
const Font    *LevelFont(void)             { return R.font; }
BobImage      *LevelDaveImage(UWORD frame) { return SpriteBankImage(R.bank, frame); }
