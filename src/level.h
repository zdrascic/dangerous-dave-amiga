#pragma once
#include "../libs/sade/sade_graphics.h"
#include "../libs/sade/sade_bob.h"
#include "../libs/sade/sade_tilemap.h"
#include "../libs/sade/sade_text.h"
#include "dave_assets.h"

// Asset bytes loaded (FileLoad) before SadeInit; they must stay alive while
// the game runs. LevelRun copies the map of the level it plays (pickups are
// removed from the copy), so the loaded maps stay pristine.
typedef struct {
    const UBYTE *tilesIff; ULONG tilesSize;   // assets/game/tiles.iff
    const UBYTE *daveIff;  ULONG daveSize;    // assets/game/dave.iff
    const UBYTE *hudIff;   ULONG hudSize;     // assets/game/hud.iff
    const UBYTE *fontIff;  ULONG fontSize;    // assets/fonts/font8.iff
    const UBYTE *monIff;   ULONG monSize;     // assets/game/monsters.iff
    const UBYTE *mapData[NUM_LEVELS];         // assets/game/levels/levelNN.map
    ULONG        mapSize[NUM_LEVELS];
    const UBYTE *demoData; ULONG demoSize;    // assets/game/demos.bin (NULL: no demos)
} LevelAssets;

// Carried across levels.
typedef struct {
    ULONG score;
    UBYTE lives;             // spare Daves shown in the status bar
    UBYTE level;             // 0-based
    UBYTE zone;              // warp zone being played: the 1-based level whose
                             // map holds it, 0 = none (see LevelRun)
} GameState;

enum { LEVEL_QUIT, LEVEL_DONE, LEVEL_GAME_OVER, LEVEL_WARP_ZONE };

// Screen row of the playfield's top (map row 0); the original's is 16.
#define LEVEL_PLAY_Y 23

// LEVEL_GAME_OVER comes back after the original's end of a game: the high
// score table with the name entry when the score makes it (scores.h), else
// "GAME OVER" over the status bar while the level animates on, until a key
// or ~14 s.

// Debug keys during a level, off until "123555" is typed on the title
// (title.c): J = full jetpack, G = the gun, H = invulnerable on/off (kept
// until the program quits), L = finish the level (as if through the door),
// W = into this level's warp zone (levels 5, 8, 9, 10).
extern UBYTE g_debugKeys;

// Decode the shared graphics (tiles, HUD, font, Dave) once, after SadeInit.
// Returns 0 on failure. LevelShutdown frees them.
// progress (may be NULL) is called LEVEL_INIT_STEPS times in total, for a
// loading bar.
#define LEVEL_INIT_STEPS (6 + DAVE_FRAMES + MON_FRAMES + SHOT_FRAMES)
int  LevelInit(const LevelAssets *a, void (*progress)(void));
void LevelShutdown(void);

// Runs one level (g->level) on a SADE_DOUBLE 320x200x5 screen, updating
// g->score / g->lives. Returns LEVEL_DONE when Dave leaves through the door
// with the trophy, LEVEL_GAME_OVER when he dies with no spare lives,
// LEVEL_QUIT when the player answers Y to the ESC/RMB "QUIT?" box.
//
// Warp zones (levels 5, 8, 9, 10, as in the original): walking off the left
// or right edge of the map (over the top of the screen) returns
// LEVEL_WARP_ZONE with g->zone set. The caller shows WarpTubeRun, then calls
// LevelRun again, which plays that zone: a hidden area at the right of
// another level's map, entered falling down a shaft (and restarted there
// after a death). Its door returns LEVEL_DONE with g->zone still set; the
// caller clears it and replays g->level (the original returns Dave to the
// level he warped from, not the next one).
int LevelRun(Screen *scr, GameState *g, const LevelAssets *a);

// Between-levels intermission after g->level: Dave walks out of the door
// along a corridor under the original's message — "GOOD WORK! ONLY N MORE
// TO GO!", "THIS IS THE LAST LEVEL!!!" after level 9, "YES! YOU FINISHED
// THE GAME!" after level 10 — or none (message = 0: back from a warp
// zone, as the original). Returns LEVEL_DONE when he is through,
// LEVEL_QUIT on RMB/ESC.
int WarpRun(Screen *scr, GameState *g, const LevelAssets *a, UBYTE message);

// Warp zone cut-scene: Dave falls down a brick tube between "WARP" and
// "ZONE". Returns LEVEL_DONE when he is out of the bottom, LEVEL_QUIT on
// RMB/ESC.
int WarpTubeRun(Screen *scr, GameState *g, const LevelAssets *a);

// Attract mode: replays the original's recorded demo n (0 .. DemoCount-1)
// on its level, from a fresh game, while the title is idle. Returns
// LEVEL_QUIT if a key or button cut it short.
int DemoRun(Screen *scr, const LevelAssets *a, UBYTE n);
static inline UBYTE DemoCount(const LevelAssets *a) {
    return (a->demoData && a->demoSize >= 4) ? a->demoData[0] : 0;
}

// The graphics LevelInit decoded, shared with the title screen: map tiles
// (and their gameplay palette, $00RRGGBB: 32 entries OCS, 256 AGA), font8, Dave's frames
// (DAVE_* indices).
const TileSet *LevelTiles(void);
const ULONG   *LevelPalette(void);
const Font    *LevelFont(void);
BobImage      *LevelDaveImage(UWORD frame);
