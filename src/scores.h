#pragma once
#include "../libs/sade/sade_graphics.h"

// The original's high score table: 5 entries, kept in DSCORES.DAV. Loaded
// in the load phase (the saved table, else the default shipped in the
// package), changed in RAM while the game runs, saved on exit.
//
// DSCORES.DAV: 5 x 9 bytes, best first: UBYTE level (1-10, 11 = finished
// the game), 5 bytes score digits (0-9, most significant first), 3 bytes
// name (ASCII, space padded).
#define SCORE_ENTRIES   5
#define SCORE_FILE_SIZE (SCORE_ENTRIES * 9)
#define SCORE_LEVEL_WON 11

void  ScoresLoad(const UBYTE *data, ULONG size);  // NULL/short: all "JON", 0
int   ScoresRank(ULONG score);                    // its place 0-4, -1 = none
void  ScoresInsert(int rank, ULONG score, UBYTE level);   // name left blank
UBYTE ScoresChanged(void);                        // since ScoresLoad
void  ScoresEncode(UBYTE out[SCORE_FILE_SIZE]);

// Where the original shows the table, in its screen pixels (playfield at
// y=16): the score column and the "HIGH SCORES" row.
#define SCORES_X 96
#define SCORES_Y 48

// Drawing: tx is the score column (a multiple of 8), ty the title row.
// ScoresShow opens the box the way the original does and fills it in;
// ScoresEnterName then takes up to 3 keys for that row's name (Backspace
// or cursor left deletes, Return ends) — keyboard only.
void  ScoresShow(Screen *s, short tx, short ty);
void  ScoresEnterName(Screen *s, short tx, short ty, int rank);

// Shared with the end screens.
// The original's box: grey edge, white, a black line, white inside.
void  UiBox(Screen *s, short x0, short y0, short x1, short y1);
// A single-buffered view of one buffer (back = 1: backPlanes) of a screen.
void  ScreenBuffer(const Screen *s, UBYTE back, Screen *out);
// The original's message box across the bottom status bar ("GAME OVER",
// "YOU GOT A HIGH SCORE!"), text at x (a multiple of 8), both buffers.
void  BottomMessage(Screen *scr, short x, const char *text);
// Waits until a key, fire or a mouse button is pressed (fresh presses
// only), or `ticks` 50ths of a second have passed (0 = no limit).
// Returns 1 for a press.
int   UiWaitPress(UWORD ticks);
