// =========================================================================
// High scores, as the original's (DAVE.EXE: the check at file offset
// 0x4FC, the table at 0x88E, the name entry at 0x7EB).
// =========================================================================
#include "scores.h"
#include "level.h"
#include "../libs/sade/sade_system.h"
#include "../libs/sade/sade_input.h"
#include "../libs/sade/sade_keyboard.h"

#define INK    0                          // gameplay palette: black
#define WHITE  2                          //   near-white
#define EDGE  11                          //   mid grey
#define DOT   25                          //   magenta: the name cursor

typedef struct {
    ULONG score;
    UBYTE level;                          // 1-10, SCORE_LEVEL_WON
    char  name[3];
} ScoreEntry;

static ScoreEntry g_tab[SCORE_ENTRIES];
static UBYTE      g_changed;

// The five digits of v (0-99999), most significant first; by subtraction,
// as the 68000 has no 32-bit divide.
static void Digits5(ULONG v, UBYTE out[5]) {
    static const ULONG kPow[5] = { 10000, 1000, 100, 10, 1 };
    for (UBYTE k = 0; k < 5; k++) {
        UBYTE n = 0;
        while (v >= kPow[k]) { v -= kPow[k]; n++; }
        out[k] = n;
    }
}

void ScoresLoad(const UBYTE *d, ULONG size) {
    for (UBYTE i = 0; i < SCORE_ENTRIES; i++) {
        ScoreEntry *e = &g_tab[i];
        e->score = 0;                     // the original's defaults
        e->level = 1;
        e->name[0] = 'J'; e->name[1] = 'O'; e->name[2] = 'N';
        if (!d || size < SCORE_FILE_SIZE) continue;
        const UBYTE *p = d + i * 9;
        UBYTE ok = p[0] >= 1 && p[0] <= SCORE_LEVEL_WON;
        ULONG v = 0;
        for (UBYTE k = 1; k <= 5; k++) {
            if (p[k] > 9) ok = 0;
            v = (v << 3) + (v << 1) + p[k];   // v * 10, no __mulsi3
        }
        if (!ok) continue;
        e->score = v;
        e->level = p[0];
        for (UBYTE k = 0; k < 3; k++) {
            char c = (char)p[6 + k];
            e->name[k] = (c >= ' ' && c <= 'Z') ? c : ' ';
        }
    }
    g_changed = 0;
}

int ScoresRank(ULONG score) {
    if (score > 99999) score = 99999;     // five digits, as on the HUD
    for (UBYTE i = 0; i < SCORE_ENTRIES; i++)
        if (score > g_tab[i].score) return i;
    return -1;
}

void ScoresInsert(int rank, ULONG score, UBYTE level) {
    if (rank < 0 || rank >= SCORE_ENTRIES) return;
    for (int i = SCORE_ENTRIES - 1; i > rank; i--) g_tab[i] = g_tab[i - 1];
    ScoreEntry *e = &g_tab[rank];
    e->score = score > 99999 ? 99999 : score;
    e->level = level;
    e->name[0] = e->name[1] = e->name[2] = ' ';
    g_changed = 1;
}

UBYTE ScoresChanged(void) { return g_changed; }

void ScoresEncode(UBYTE out[SCORE_FILE_SIZE]) {
    for (UBYTE i = 0; i < SCORE_ENTRIES; i++) {
        const ScoreEntry *e = &g_tab[i];
        UBYTE *p = out + i * 9;
        p[0] = e->level;
        Digits5(e->score, p + 1);
        for (UBYTE k = 0; k < 3; k++) p[6 + k] = (UBYTE)e->name[k];
    }
}

// ---- drawing -------------------------------------------------------------

void ScreenBuffer(const Screen *s, UBYTE back, Screen *out) {
    *out = *s;
    if (back && s->doubleBuffer)
        for (UBYTE p = 0; p < s->depth; p++) out->planes[p] = s->backPlanes[p];
    out->doubleBuffer = 0;
}

void UiBox(Screen *s, short x0, short y0, short x1, short y1) {
    GfxFillBox(s, x0, y0, x1, y1, EDGE);
    GfxFillBox(s, x0 + 2, y0 + 2, x1 - 2, y1 - 2, WHITE);
    GfxDrawBox(s, x0 + 3, y0 + 3, x1 - 3, y1 - 3, INK);
}

// The original's box covers the bottom three text rows; here the status
// bar under the playfield band is 16 rows, so the box is as tall as that.
// UiBox's frame would leave the text touching the black line, so this one
// is slimmer — a grey row, a black line — for 3 white rows above and below
// the glyphs (rows 0-6 of the 8x8 cell; row 7 is blank).
#define BOTTOM_Y0 (LEVEL_PLAY_Y + LEVEL_H * 16 + 1)
#define BOTTOM_Y1 (BOTTOM_Y0 + 15)
void BottomMessage(Screen *scr, short x, const char *text) {
    for (UBYTE back = 0; back <= (scr->doubleBuffer ? 1 : 0); back++) {
        Screen v;
        ScreenBuffer(scr, back, &v);
        GfxFillBox(&v, 2, BOTTOM_Y0, 317, BOTTOM_Y1, EDGE);
        GfxFillBox(&v, 4, BOTTOM_Y0 + 2, 315, BOTTOM_Y1 - 2, WHITE);
        GfxDrawBox(&v, 3, BOTTOM_Y0 + 1, 316, BOTTOM_Y1 - 1, INK);
        TextDraw(LevelFont(), &v, x, BOTTOM_Y0 + 5, text, INK, WHITE);
    }
}

// The table box around tx/ty (the original's: x 57..270, y 41..158).
#define BOX_X0(tx) ((tx) - 39)
#define BOX_Y0(ty) ((ty) - 7)
#define BOX_X1(tx) ((tx) + 174)
#define BOX_Y1(ty) ((ty) + 110)
#define ROW_Y(ty, i) ((ty) + 32 + (i) * 16)
#define NAME_X(tx) ((tx) + 56)

static void DrawRow(Screen *s, short tx, short ty, UBYTE i) {
    const Font *font = LevelFont();
    const ScoreEntry *e = &g_tab[i];
    UBYTE dig[5];
    char buf[6];
    Digits5(e->score, dig);
    for (UBYTE k = 0; k < 5; k++) buf[k] = (char)('0' + dig[k]);
    buf[5] = 0;
    short y = ROW_Y(ty, i);
    TextDraw(font, s, tx, y, buf, INK, WHITE);
    char name[4] = { e->name[0], e->name[1], e->name[2], 0 };
    TextDraw(font, s, NAME_X(tx), y, name, INK, WHITE);
    if (e->level == SCORE_LEVEL_WON) {
        TextDraw(font, s, tx + 112, y, "WON", INK, WHITE);
    } else {
        char lv[3] = { 0, 0, 0 };
        if (e->level >= 10) { lv[0] = '1'; lv[1] = (char)('0' + e->level - 10); }
        else lv[0] = (char)('0' + e->level);
        TextDraw(font, s, tx + 120, y, lv, INK, WHITE);
    }
}

void ScoresShow(Screen *s, short tx, short ty) {
    // the box opens from its middle in 11 steps, one a frame
    short cx = (short)((BOX_X0(tx) + BOX_X1(tx)) >> 1), cy = (short)((BOX_Y0(ty) + BOX_Y1(ty)) >> 1);
    short hw = (short)((BOX_X1(tx) - BOX_X0(tx)) >> 1), hh = (short)((BOX_Y1(ty) - BOX_Y0(ty)) >> 1);
    for (UBYTE k = 1; k <= 11; k++) {
        short w = (short)divsw((short)(hw * k), 11), h = (short)divsw((short)(hh * k), 11);
        if (w < 4) w = 4;
        if (h < 4) h = 4;
        SadeWaitVbl();
        UiBox(s, (short)(cx - w), (short)(cy - h), (short)(cx + w), (short)(cy + h));
    }
    const Font *font = LevelFont();
    TextDraw(font, s, tx + 32, ty, "HIGH SCORES", INK, WHITE);
    TextDraw(font, s, tx, ty + 16, "SCORE  NAME  LEVEL", INK, WHITE);
    TextDraw(font, s, tx, ty + 24, "-----  ----  -----", INK, WHITE);
    for (UBYTE i = 0; i < SCORE_ENTRIES; i++) DrawRow(s, tx, ty, i);
}

// Raw key -> the character it types (the original takes ' '..'Z').
static char RawChar(short k) {
    static const char kRow1[] = "1234567890-=";          // 0x01-0x0C
    static const char kRow2[] = "QWERTYUIOP";            // 0x10-0x19
    static const char kRow3[] = "ASDFGHJKL;'";           // 0x20-0x2A
    static const char kRow4[] = "ZXCVBNM,./";            // 0x31-0x3A
    if (k >= 0x01 && k <= 0x0C) return kRow1[k - 0x01];
    if (k >= 0x10 && k <= 0x19) return kRow2[k - 0x10];
    if (k >= 0x20 && k <= 0x2A) return kRow3[k - 0x20];
    if (k >= 0x31 && k <= 0x3A) return kRow4[k - 0x31];
    if (k == RAWKEY_SPACE) return ' ';
    return 0;
}

static void Cursor(Screen *s, short x, short y, UBYTE on) {
    GfxFillBox(s, x, y, x + 7, y + 7, WHITE);
    if (on) GfxFillCircle(s, (short)(x + 3), (short)(y + 3), 2, DOT);
}

void ScoresEnterName(Screen *s, short tx, short ty, int rank) {
    if (rank < 0 || rank >= SCORE_ENTRIES) return;
    ScoreEntry *e = &g_tab[rank];
    const Font *font = LevelFont();
    short x = NAME_X(tx), y = ROW_Y(ty, rank);
    UBYTE len = 0, blink = 0, shown = 2;
    SadeClock clk;
    SadeClockStart(&clk, 50);
    KeyboardUpdate();
    KeyHitAny();                              // drop keys pressed before
    for (;;) {
        SadeWaitVbl();
        KeyboardUpdate();
        short k = KeyHitAny();
        if (k == RAWKEY_RETURN || k == RAWKEY_KP_ENTER) break;
        if ((k == RAWKEY_BACKSPACE || k == RAWKEY_LEFT || k == RAWKEY_DEL) && len) {
            if (len < 3) Cursor(s, (short)(x + len * 8), y, 0);
            len--;
            e->name[len] = ' ';
            TextDraw(font, s, (short)(x + len * 8), y, " ", INK, WHITE);
            shown = 2;
        } else if (len < 3 && RawChar(k)) {
            char c[2] = { RawChar(k), 0 };
            e->name[len] = c[0];
            TextDraw(font, s, (short)(x + len * 8), y, c, INK, WHITE);
            len++;
            shown = 2;
        }
        blink = (UBYTE)(blink + SadeClockTicks(&clk));
        UBYTE on = (UBYTE)(!(blink & 16));
        if (len < 3 && on != shown) {
            shown = on;
            Cursor(s, (short)(x + len * 8), y, on);
        }
    }
    if (len < 3) Cursor(s, (short)(x + len * 8), y, 0);
    g_changed = 1;
}

int UiWaitPress(UWORD ticks) {
    SadeClock clk;
    SadeClockStart(&clk, 50);
    UWORD t = 0;
    short fireWas = 1, lmbWas = 1, rmbWas = 1;
    KeyboardUpdate();
    KeyHitAny();
    for (;;) {
        SadeWaitVbl();
        KeyboardUpdate();
        short fire = Joy1Fire(), lmb = MouseLeft(), rmb = MouseRight();
        if (KeyHitAny() >= 0 || (fire && !fireWas) || (lmb && !lmbWas) || (rmb && !rmbWas))
            return 1;
        fireWas = fire; lmbWas = lmb; rmbWas = rmb;
        t = (UWORD)(t + SadeClockTicks(&clk));
        if (ticks && t >= ticks) return 0;
    }
}
