// =========================================================================
// The end of the game, as the original's (DAVE.EXE file offset 0x3DC4):
// after the last intermission the screen clears to a border of trophies
// round the playfield with the congratulations inside, and a sparkle runs
// round the border until Space. Then the high score check, with the table
// drawn over it ("WON" for the level).
// =========================================================================
#include "end.h"
#include "level.h"
#include "scores.h"
#include "../libs/sade/sade_system.h"
#include "../libs/sade/sade_input.h"
#include "../libs/sade/sade_keyboard.h"

#define OY        (LEVEL_PLAY_Y - 16)     // the original's screen y + OY
#define TROPHY    10                      // tiles 10-13: the trophy's sparkle
#define CUT_Y     (LEVEL_PLAY_Y + 150)    // the bottom row shows 6 lines, as the original's
#define TEXT_X    24
#define INK        2
#define TICKS_PER_SEC 70                  // the original: 4 border cells a retrace
#define CELLS_PER_TICK 4

// The original's border walk: the top row left to right, the right column
// down, the bottom row right to left, the left column up — 55 cells, its
// last left-column cell writing over the bottom-left corner.
#define PERIM 55
static UBYTE g_px[PERIM], g_py[PERIM];

static void BuildPerimeter(void) {
    for (UBYTE i = 0; i < 20; i++) { g_px[i] = i;              g_py[i] = 0; }
    for (UBYTE i = 1; i < 9; i++)  { g_px[19 + i] = 19;        g_py[19 + i] = i; }
    for (UBYTE i = 0; i < 20; i++) { g_px[28 + i] = (UBYTE)(19 - i); g_py[28 + i] = 9; }
    for (UBYTE i = 1; i < 9; i++)  { g_px[46 + i] = 0;         g_py[46 + i] = (UBYTE)(9 - i); }
}

static void BorderCell(Screen *s, UBYTE i, UWORD tile) {
    short x = (short)(g_px[i] * 16), y = (short)(LEVEL_PLAY_Y + g_py[i] * 16);
    TileBlit(LevelTiles(), s, tile, x, y);
    if (g_py[i] == 9) GfxFillBox(s, x, CUT_Y, (short)(x + 15), (short)(y + 15), 0);
}

static const struct { UBYTE y; const char *text; } kLines[] = {
    {  40, "         CONGRATULATIONS!" },
    {  64, "YOU MADE IT THROUGH ALL THE PERIL-" },
    {  72, "OUS AREAS IN CLYDE'S HIDEOUT!" },
    {  88, "VERY GOOD WORK! DID YOU FIND" },
    {  96, "THE 4 WARP ZONES? THEY ARE LOCATED" },
    { 104, "ON LEVELS 5,8,9 AND 10. JUST JUMP" },
    { 112, "OFF THE TOP OF THE SCREEN AT THE" },
    { 120, "EXTREME LEFT OR RIGHT EDGE OF THE" },
    { 128, "WORLD AND VOILA! YOU'RE THERE!" },
    { 152, "           PRESS SPACE" },
};

static void DrawEnding(Screen *s) {
    GfxClear(s);
    for (UBYTE i = 0; i < PERIM; i++) BorderCell(s, i, TROPHY);
    TileBlit(LevelTiles(), s, TROPHY, 0, LEVEL_PLAY_Y + 9 * 16);   // the corner the walk skips
    GfxFillBox(s, 0, CUT_Y, 15, LEVEL_PLAY_Y + 9 * 16 + 15, 0);
    for (UBYTE i = 0; i < sizeof kLines / sizeof kLines[0]; i++)
        TextDraw(LevelFont(), s, TEXT_X, (short)(kLines[i].y + OY), kLines[i].text, INK, 0);
}

void EndingRun(Screen *scr, const GameState *g) {
    for (UWORD i = 0; i < scr->numColors; i++) ScreenSetColor(scr, i, 0x000);   // black while drawn
    BuildPerimeter();

    // Drawn into both buffers and animated in both. One flip makes the
    // displayed one known (backPlanes) for the table and the name entry.
    UBYTE nbuf = scr->doubleBuffer ? 2 : 1;
    Screen buf[2];
    ScreenBuffer(scr, 0, &buf[0]);
    ScreenBuffer(scr, 1, &buf[1]);
    for (UBYTE b = 0; b < nbuf; b++) DrawEnding(&buf[b]);
    ScreenFlip(scr);
    const ULONG *pal = LevelPalette();
    for (UWORD i = 0; i < scr->numColors; i++) ScreenSetRGB(scr, i, pal[i]);
    ScreenBuffer(scr, 0, &buf[0]);
    ScreenBuffer(scr, 1, &buf[1]);
    Screen *shown = &buf[nbuf - 1];

    SadeClock clk;
    SadeClockStart(&clk, TICKS_PER_SEC);
    UBYTE pos = 0, frame = 0;
    short fireWas = 1;
    KeyboardUpdate();
    KeyHitAny();
    for (;;) {
        SadeWaitVbl();
        KeyboardUpdate();
        short fire = Joy1Fire();
        if (KeyHitAny() == RAWKEY_SPACE || (fire && !fireWas)) break;
        fireWas = fire;
        for (UBYTE n = SadeClockTicks(&clk); n; n--)
            for (UBYTE c = 0; c < CELLS_PER_TICK; c++) {
                for (UBYTE b = 0; b < nbuf; b++) BorderCell(&buf[b], pos, (UWORD)(TROPHY + frame));
                frame = (UBYTE)((frame + 1) & 3);
                if (++pos == PERIM) pos = 0;
            }
    }

    int rank = ScoresRank(g->score);
    if (rank < 0) return;
    ScoresInsert(rank, g->score, SCORE_LEVEL_WON);
    BottomMessage(scr, 80, "YOU GOT A HIGH SCORE!");
    ScoresShow(shown, SCORES_X, SCORES_Y + OY);   // no flips from here on
    ScoresEnterName(shown, SCORES_X, SCORES_Y + OY, rank);
}
