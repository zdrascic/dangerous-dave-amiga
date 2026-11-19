#pragma once
#include "level.h"

// After the last intermission ("YES! YOU FINISHED THE GAME!"): the
// original's congratulations inside a sparkling border of trophies, until
// Space (or fire); then, if g->score makes the high score table, the table
// with "WON" and the name entry (scores.h).
void EndingRun(Screen *scr, const GameState *g);
