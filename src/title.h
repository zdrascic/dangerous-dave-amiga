#pragma once
#include "../libs/sade/sade_graphics.h"

// Title screen, as the original's: the flashing logo, the credits, the
// little maze with its fires, "PRESS THE F1 KEY FOR HELP". F1 shows the
// three help pages, F2 the high score table. Needs LevelInit's graphics (tiles, font, Dave).
//
// The logo's asset bytes must be loaded (FileLoad) during the pre-takeover
// load phase and handed in here; title.c does not touch disk itself.
//
// demos: how many attract-mode demos there are (0 = none; the title then
// waits for a key forever).
enum { TITLE_QUIT, TITLE_START, TITLE_DEMO };
int TitleScreenShow(Screen *scr, const UBYTE *logoIffData, ULONG logoIffSize, UBYTE demos);
