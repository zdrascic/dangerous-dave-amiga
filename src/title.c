// =========================================================================
// Title screen, laid out like the original's (DAVE.EXE, file offset 0x13FF
// draws it, 0x9DE runs it): the logo at the top flashing through its four
// frames, the credits under it, a small dirt maze with two crowns and two
// fires, and "PRESS THE F1 KEY FOR HELP" at the bottom (and, in this port,
// the F2 line under it).
//
// Every 6 ticks of a 70 Hz clock (the original's six VGA retraces) the logo
// and the fires step one frame. After 200 steps (~17 s) with no key the
// caller plays a demo. F1 shows the original's three help pages; F2 (not
// in the original) the high score table, as after a demo. Typing 123555
// (not in the original) turns on the debug keys (level.h) and lists them
// in the help box; digits never start a game.
//
// The logo has its own colours; a copper split under it switches to the
// gameplay palette for the maze, the text and the help box. (The AGA logo
// is on the gameplay palette, so there the split loads nothing.)
// =========================================================================
#include "title.h"
#include "level.h"
#include "scores.h"
#include "sound.h"
#include "../libs/sade/sade_system.h"
#include "../libs/sade/sade_input.h"
#include "../libs/sade/sade_keyboard.h"
#include "../libs/sade/sade_ilbm.h"
#include "../libs/sade/sade_copper.h"

#define LOGO_X       104                  // the original's logo position
#define LOGO_BLIT_X  (LOGO_X - LOGO_ART_X) // word-aligned cell: art at +8
#define SPLIT_Y      LOGO_H               // gameplay palette from this row
#define HELP_SPLIT_Y 9                    // ...from the help box's top edge
#define SCORES_SPLIT_Y (SCORES_Y - 7)     // ...from the score box's top edge

#define TICKS_PER_SEC 70                  // the original's VGA retrace rate
#define STEP_TICKS     6                  // retraces per logo/fire frame
#define IDLE_STEPS   200                  // steps before the demo

#define DEBUG_CODE "123555"                // typed on the title: debug keys on

#define INK    2                          // near-white in the gameplay palette
#define PAPER  0
#define TEXT_X 64                         // the original prints from x=64

// The maze: 10x7 tiles from DAVE.EXE (DS:07D4), drawn from playfield
// column 5, row 3, i.e. screen x=80, y=64 (the playfield starts at y=16).
#define MAP_W  10
#define MAP_H   7
#define MAP_X  80
#define MAP_Y  64
static const UBYTE kTitleMap[MAP_H][MAP_W] = {
    { 18,  0, 18, 18, 18, 18,  0, 18,  0, 18 },
    { 18,  0, 18, 50,  0,  0,  0,  0,  0, 18 },
    { 18,  0, 18,  0,  0, 18, 18, 18,  0, 18 },
    { 18,  0, 18,  0,  0,  0,  0,  0, 50, 18 },
    { 18,  0, 18,  6, 18, 18, 18,  0,  0, 18 },
    { 18,  0,  0,  0,  0,  0,  0,  0,  0, 18 },
    { 18, 18, 18, 18, 18, 18, 18, 18,  6, 18 },
};
#define FIRE_TILE  6                      // tiles 6-9: the fire's four frames
#define FIRE1_X  (MAP_X + 3 * 16)
#define FIRE1_Y  (MAP_Y + 4 * 16)
#define FIRE2_X  (MAP_X + 8 * 16)
#define FIRE2_Y  (MAP_Y + 6 * 16)
#define PROMPT_Y 168                      // over the maze's bottom row, which
#define CLEAR_Y  166                      // the original shows to y=165
#define PROMPT2_Y 176                     // under the maze

static const char kPrompt[] = "PRESS THE F1 KEY FOR HELP";
static const char kPrompt2[] = "PRESS THE F2 KEY FOR SCORES";
// Centred on the screen. TextDraw x is a multiple of 8, so odd-length
// strings sit 4 px left of the exact centre (both lines share it).
#define CENTRE_X(s) ((short)((320 - (short)(sizeof(s) - 1) * 8) / 2) & ~7)

static TileSet *g_logoTs;

// One step of the animation: logo frame, both fires, and the prompt row
// the lower fire reaches into. Everything it touches is opaque, so it
// fully repaints a stale buffer.
static void DrawStep(Screen *scr, UWORD step) {
    const TileSet *ts = LevelTiles();
    TileBlit(g_logoTs, scr, (UWORD)(step & 3), LOGO_BLIT_X, 0);
    UWORD fire = (UWORD)(FIRE_TILE + ((step + 1) & 3));   // the original's order
    TileBlit(ts, scr, fire, FIRE1_X, FIRE1_Y);
    TileBlit(ts, scr, fire, FIRE2_X, FIRE2_Y);
    GfxFillBox(scr, MAP_X, CLEAR_Y, MAP_X + MAP_W * 16 - 1, PROMPT_Y - 1, PAPER);
    TextDraw(LevelFont(), scr, CENTRE_X(kPrompt), PROMPT_Y, kPrompt, INK, PAPER);
}

static void DrawTitle(Screen *scr, UWORD step) {
    const TileSet *ts = LevelTiles();
    const Font *font = LevelFont();
    GfxClear(scr);
    for (UBYTE y = 0; y < MAP_H; y++)
        for (UBYTE x = 0; x < MAP_W; x++)
            if (kTitleMap[y][x])
                TileBlit(ts, scr, kTitleMap[y][x], MAP_X + x * 16, MAP_Y + y * 16);
    TextDraw(font, scr, TEXT_X, 48, "     BY JOHN ROMERO", INK, PAPER);
    TextDraw(font, scr, TEXT_X, 56, " (C) 1990 SOFTDISK, INC.", INK, PAPER);
    TextDraw(font, scr, CENTRE_X(kPrompt2), PROMPT2_Y, kPrompt2, INK, PAPER);
    DrawStep(scr, step);
}

// Both buffers, so a flip shows the same picture.
static void DrawTitleBoth(Screen *scr, UWORD step) {
    DrawTitle(scr, step);
    ScreenFlip(scr);
    DrawTitle(scr, step);
    ScreenFlip(scr);
}

// ---- help: the original's three pages, in its box over the title -------
// Page 1 lists the original's DOS keys; here it lists this port's.
typedef struct { UBYTE y; const char *text; } HelpLine;
typedef struct {
    short textX;                          // left edge of the text
    UBYTE daveL, daveR;                   // Dave frames left and right of the header
    UBYTE bar;                            // the riveted bar under the header
    const HelpLine *lines;
} HelpPage;

static const HelpLine kHelp1[] = {
    {  24, "     DANGEROUS DAVE" },
    {  64, "YOU USE THE KEYBOARD OR" },
    {  72, "A JOYSTICK TO CONTROL" },
    {  80, "DAVE." },
    {  96, "THESE ARE THE KEYS THAT" },
    { 104, "ARE AVAILABLE:" },
    { 120, "F1 = HELP" },
    { 128, "F2 = HIGH SCORES" },
    { 136, "ARROWS = MOVE DAVE" },
    { 144, "CTRL = FIRE THE GUN" },
    { 152, "SHIFT = JETPACK ON/OFF" },
    { 160, "ESC OR RMB = QUIT GAME" },
    { 176, "   PRESS SPACE OR ESC" },
    {   0, NULL },
};
static const HelpLine kHelp2[] = {
    {  24, "       DANGEROUS DAVE" },
    {  40, "THE OBJECT OF THE GAME IS" },
    {  48, "TO GUIDE DAVE THROUGH 10" },
    {  56, "PERILOUS AREAS IN CLYDE'S" },
    {  64, "HIDEOUT. THERE ARE GUNS &" },
    {  72, "JETPACKS TO AID YOU." },
    {  88, "MOVE DAVE WITH THE ARROW" },
    {  96, "KEYS OR THE JOYSTICK. ITS" },
    { 104, "FIRE BUTTON WILL SHOOT AND" },
    { 112, "THE SECOND BUTTON WILL" },
    { 120, "TURN ON/OFF THE JETPACK." },
    { 176, "   PRESS SPACE OR ESC" },
    {   0, NULL },
};
static const HelpLine kHelp3[] = {
    {  24, "       DANGEROUS DAVE" },
    {  40, "WATCH OUT FOR FIRE, WATER" },
    {  48, "AND WEIRDWEEDS! THESE WILL" },
    {  56, "TOAST DAVE VERY BLACKLY!" },
    {  72, "MERELY GRAB THE TROPHY AND" },
    {  80, "WALK THROUGH THE DOOR. IT'S" },
    {  88, "THAT SIMPLE! OF COURSE, YOU" },
    {  96, "MIGHT RUN INTO A FEW (?)" },
    { 104, "MONSTERS..." },
    { 120, "ALSO ... THERE ARE A FEW" },
    { 128, "PLACES DAVE CAN GO BY TRY-" },
    { 136, "ING SOMETHING STRANGE AND" },
    { 144, "DANGEROUS..." },
    { 168, "        PRESS SPACE" },
    {   0, NULL },
};
static const HelpPage kHelpPages[] = {
    { 64, DAVE_WALK_R0, DAVE_WALK_L0, 1, kHelp1 },
    { 56, DAVE_JET_R0,  DAVE_JET_L0,  0, kHelp2 },
    { 56, DAVE_CLIMB0,  DAVE_CLIMB2,  0, kHelp3 },
};
#define HELP_PAGES 3

// Shown once DEBUG_CODE is typed; the keys are handled in LevelRun.
static const HelpLine kDebugHelp[] = {
    {  24, "     DANGEROUS DAVE" },
    {  64, "DEBUG MODE IS ON." },
    {  80, "KEYS DURING A LEVEL:" },
    {  96, "J = FULL JETPACK" },
    { 104, "G = GET GUN" },
    { 112, "H = INVULNERABLE" },
    { 120, "L = FINISH LEVEL" },
    { 128, "W = WARP ZONE" },
    { 176, "   PRESS SPACE OR ESC" },
    {   0, NULL },
};
static const HelpPage kDebugPage = { 64, DAVE_WALK_R0, DAVE_WALK_L0, 1, kDebugHelp };

// The box: x 41..286, y 9..190 — grey edge, white, a black line, white.
#define BOX_X0  41
#define BOX_Y0   9
#define BOX_X1 286
#define BOX_Y1 190
#define BOX_EDGE 11                       // mid grey (0x999)
#define BOX_INK   0
#define BOX_WHITE 2
#define BAR_TILE  3                       // riveted metal, 13 tiles at y=32
#define DAVE_Y   16

static void DrawHelpPage(Screen *scr, const HelpPage *pg, Bob *dave) {
    const Font *font = LevelFont();
    GfxFillBox(scr, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, BOX_EDGE);
    GfxFillBox(scr, BOX_X0 + 2, BOX_Y0 + 2, BOX_X1 - 2, BOX_Y1 - 2, BOX_WHITE);
    GfxDrawBox(scr, BOX_X0 + 3, BOX_Y0 + 3, BOX_X1 - 3, BOX_Y1 - 3, BOX_INK);
    if (pg->bar)
        for (short x = 64; x < 272; x += 16) TileBlit(LevelTiles(), scr, BAR_TILE, x, 32);
    for (const HelpLine *l = pg->lines; l->text; l++)
        TextDraw(font, scr, pg->textX, l->y, l->text, BOX_INK, BOX_WHITE);
    dave->y = DAVE_Y;                         // after the text: the header's
    dave->image = LevelDaveImage(pg->daveL);  // opaque leading spaces cover
    dave->x = 76;                             // the left one's lower half
    BobDraw(dave, scr, 0);
    dave->image = LevelDaveImage(pg->daveR);
    dave->x = 240;
    BobDraw(dave, scr, 0);
}

// Pages advance on Space (or fire); Esc leaves. Returns 0 on Esc/RMB.
static int HelpShow(Screen *scr, const HelpPage *pages, UBYTE count) {
    Bob *dave = BobCreate(LevelDaveImage(DAVE_STAND));
    if (!dave) return 1;
    short fireWas = 1;
    int r = 1;
    for (UBYTE p = 0; p < count && r; p++) {
        DrawHelpPage(scr, &pages[p], dave);
        ScreenFlip(scr);
        DrawHelpPage(scr, &pages[p], dave);
        ScreenFlip(scr);
        for (;;) {
            SadeWaitVbl();
            KeyboardUpdate();
            short k = KeyHitAny();
            short fire = Joy1Fire();
            if (k == RAWKEY_ESC || MouseRight()) { r = 0; break; }
            if (k == RAWKEY_SPACE || k == RAWKEY_RETURN || (fire && !fireWas)) break;
            fireWas = fire;
        }
        fireWas = 1;
    }
    while (MouseRight()) SadeWaitVbl();
    BobFree(dave);
    return r;
}

// Main-row or keypad digit as its character, else 0.
static char KeyDigit(short k) {
    static const UBYTE kPad[10] = {
        RAWKEY_KP_0, RAWKEY_KP_1, RAWKEY_KP_2, RAWKEY_KP_3, RAWKEY_KP_4,
        RAWKEY_KP_5, RAWKEY_KP_6, RAWKEY_KP_7, RAWKEY_KP_8, RAWKEY_KP_9,
    };
    if (k >= RAWKEY_1 && k <= RAWKEY_9) return (char)('1' + k - RAWKEY_1);
    if (k == RAWKEY_0) return '0';
    for (UBYTE i = 0; i < 10; i++)
        if (k == kPad[i]) return (char)('0' + i);
    return 0;
}

// ---- copper: gameplay palette from row `y` down ------------------------
// Only the colours that differ from the logo's: a whole AGA palette would
// take the copper some 10 lines to load.
static CopperList *SplitCreate(const Screen *scr, const IlbmImage *logo, UWORD y) {
    const ULONG *pal = LevelPalette();
    CopperList *cl = CopperCreate((UWORD)(8 + scr->numColors * 4 + 8 * 4));
    if (!cl) return NULL;
    CopRestart(cl);
    CopWait(cl, 0, CopScreenLine(y));
    for (UWORD i = 0; i < scr->numColors; ) {
        UWORD n = 0;
        while (i + n < scr->numColors && (i + n >= logo->numColors || pal[i + n] != logo->palette[i + n]))
            n++;
        if (n) CopSetPalette(cl, scr, i, n, pal + i);
        i = (UWORD)(i + n + 1);
    }
    CopEnd(cl);
    return cl;
}

int TitleScreenShow(Screen *scr, const UBYTE *logoIffData, ULONG logoIffSize, UBYTE demos) {
    IlbmImage *logo = IlbmLoad(logoIffData, logoIffSize);
    g_logoTs = logo ? TileSetCreate(logo, LOGO_CELL_W, LOGO_H) : NULL;
    CopperList *split = NULL, *helpSplit = NULL, *scoresSplit = NULL;
    int result = TITLE_QUIT;
    if (!g_logoTs) goto done;
    split = SplitCreate(scr, logo, SPLIT_Y);
    helpSplit = SplitCreate(scr, logo, HELP_SPLIT_Y);
    scoresSplit = SplitCreate(scr, logo, SCORES_SPLIT_Y);
    if (!split || !helpSplit || !scoresSplit) goto done;

    // black while both buffers are drawn, then the logo's colours above
    // the split and the gameplay palette below it
    for (UWORD i = 0; i < scr->numColors; i++) ScreenSetColor(scr, i, 0x000);
    UWORD step = 0;
    DrawTitleBoth(scr, step);
    IlbmApplyPalette(logo, scr);
    CopperAttach(scr, split);

    SadeClock clk;
    SadeClockStart(&clk, TICKS_PER_SEC);
    UWORD ticks = 0, idle = 0;
    short fireWas = 1, rmbWas = 1;            // fresh presses only
    UBYTE code = 0;                           // DEBUG_CODE digits typed so far
    KeyboardUpdate();
    KeyHitAny();                              // drop keys pressed before the title

    for (;;) {
        SadeWaitVbl();
        KeyboardUpdate();
        short k = KeyHitAny();
        short fire = Joy1Fire(), rmb = MouseRight();
        if (k == RAWKEY_ESC || (rmb && !rmbWas)) { result = TITLE_QUIT; break; }
        if (k == RAWKEY_F1) {
            CopperAttach(scr, helpSplit);
            int stay = HelpShow(scr, kHelpPages, HELP_PAGES);
            CopperAttach(scr, split);
            if (!stay) { result = TITLE_QUIT; break; }
            DrawTitleBoth(scr, step);
            SadeClockSync(&clk);
            idle = 0;
            fireWas = 1;
            continue;
        }
        if (k == RAWKEY_F2) {                 // the table opens in the shown buffer
            Screen shown;
            ScreenBuffer(scr, 1, &shown);
            CopperAttach(scr, scoresSplit);
            ScoresShow(&shown, SCORES_X, SCORES_Y);
            UiWaitPress(0);
            CopperAttach(scr, split);
            DrawTitleBoth(scr, step);
            SadeClockSync(&clk);
            idle = 0;
            fireWas = rmbWas = 1;
            continue;
        }
        char digit = KeyDigit(k);
        if (digit) {                          // digits feed the debug code only
            code = (UBYTE)(digit == DEBUG_CODE[code] ? code + 1 : digit == DEBUG_CODE[0]);
            idle = 0;
            if (code < sizeof(DEBUG_CODE) - 1) continue;
            code = 0;
            g_debugKeys = 1;
            SoundPlay(SND_GOTSOMETHING);
            CopperAttach(scr, helpSplit);
            HelpShow(scr, &kDebugPage, 1);    // Esc here only closes the box
            CopperAttach(scr, split);
            DrawTitleBoth(scr, step);
            SadeClockSync(&clk);
            fireWas = rmbWas = 1;
            continue;
        }
        if (k >= 0 || (fire && !fireWas)) { result = TITLE_START; break; }
        fireWas = fire;
        rmbWas = rmb;

        ticks += SadeClockTicks(&clk);
        if (ticks < STEP_TICKS) continue;
        while (ticks >= STEP_TICKS) { ticks -= STEP_TICKS; step++; idle++; }
        if (demos && idle >= IDLE_STEPS) { result = TITLE_DEMO; break; }
        DrawStep(scr, step);
        ScreenFlip(scr);
    }

    CopperDetach(scr);
    SadeWaitVbl();                            // the copper is out of the split lists
done:
    if (scoresSplit) CopperFree(scoresSplit);
    if (helpSplit) CopperFree(helpSplit);
    if (split) CopperFree(split);
    if (g_logoTs) TileSetFree(g_logoTs);
    g_logoTs = NULL;
    if (logo) IlbmFree(logo);
    return result;
}
