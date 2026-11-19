#pragma once
#include "../libs/sade/sade.h"

// The original game's PC-speaker effects, from the SPK bank in DAVE.EXE
// (assets/game/sounds.spk, extracted by tools/extract_dave_sounds.py),
// played through SADE's Tone sequencer on Paula channels 0 + 1 (tone, left
// and right) and 3 (step timer), with the audio low-pass filter off.
enum {
    SND_HITWALL, SND_FALLING, SND_OUCH, SND_WALKING, SND_JUMPING,
    SND_TREASURE, SND_TROPHY, SND_EXPLOSION, SND_FLYING, SND_NEXTLEVEL,
    SND_TOJETPACK, SND_GOTSOMETHING,
    SND_COUNT
};

// After SadeInit. spk stays owned by the caller (it is copied). Returns 0 if
// the bank is unusable; the game then just runs silent.
int  SoundInit(const UBYTE *spk, ULONG size);
void SoundShutdown(void);

void SoundPlay(UBYTE id);        // obeys the bank's priorities
void SoundStop(void);
UBYTE SoundIsPlaying(void);
void SoundStopIf(UBYTE id);      // stop only if that sound is the one playing
const char *SoundName(UBYTE id); // name from the bank, e.g. "Walking" ("" if none)
