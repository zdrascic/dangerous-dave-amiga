// =========================================================================
// Sound: the original PC-speaker effects on Paula.
//
// SPK bank (little-endian, see tools/extract_dave_sounds.py):
//   "SPK\0", ULONG size, then 16-byte entries from +16:
//   UWORD offset, UBYTE priority, UBYTE rate, char name[12]
// Sound data: UWORD PIT divisors, 0 = rest, 0xFFFF = end. While a sound
// plays the original reprograms the PC timer to rate * 18.2 Hz, so a step
// lasts 65536 / (rate * 1193180) s = 54925 / rate microseconds.
// =========================================================================
#include "sound.h"
#include "../libs/sade/sade_audio.h"

#define SND_VOLUME 64                // 0-64, Paula's full scale

static UWORD   *g_data;              // big-endian copy of the bank
static ULONG    g_dataWords;
static ToneTune g_tune[SND_COUNT];
static UBYTE    g_ok;
static UBYTE    g_current = 0xff;    // last started sound
static char     g_name[SND_COUNT][13];

static UWORD Le16(const UBYTE *p) { return (UWORD)(p[0] | (p[1] << 8)); }

int SoundInit(const UBYTE *spk, ULONG size) {
    g_ok = 0;
    if (!spk || size < 16 + 16 * SND_COUNT) return 0;
    if (spk[0] != 'S' || spk[1] != 'P' || spk[2] != 'K' || spk[3] != 0) return 0;

    g_dataWords = size >> 1;
    g_data = (UWORD*)AllocMem(g_dataWords * 2, MEMF_PUBLIC);
    if (!g_data) return 0;
    for (ULONG i = 0; i < g_dataWords; i++) g_data[i] = Le16(spk + i * 2);

    for (UBYTE n = 0; n < SND_COUNT; n++) {
        const UBYTE *e = spk + 16 + n * 16;
        UWORD off  = Le16(e);
        UBYTE rate = e[3];
        if ((off & 1) || off >= size || rate == 0) { SoundShutdown(); return 0; }
        g_tune[n].steps    = g_data + (off >> 1);
        g_tune[n].stepUs   = divuw(54925, rate);
        g_tune[n].priority = e[2];
        for (UBYTE k = 0; k < 12; k++) g_name[n][k] = (char)e[4 + k];
        g_name[n][12] = 0;
    }
    // both speakers: tone on 0 (left) + 1 (right), step timer on 3
    if (!ToneInitStereo(0, 1, 3)) { SoundShutdown(); return 0; }
    ToneSetVolume(SND_VOLUME);
    AudioSetFilter(0);               // the low-pass filter muffles the square wave
    g_ok = 1;
    return 1;
}

void SoundShutdown(void) {
    if (g_ok) { ToneShutdown(); AudioRestoreFilter(); }
    g_ok = 0;
    if (g_data) { FreeMem(g_data, g_dataWords * 2); g_data = NULL; }
}

void SoundPlay(UBYTE id) {
    if (!g_ok || id >= SND_COUNT) return;
    if (ToneIsPlaying() && g_tune[id].priority < g_tune[g_current].priority) return;
    TonePlay(&g_tune[id]);
    g_current = id;
}

const char *SoundName(UBYTE id) {
    return (g_ok && id < SND_COUNT) ? g_name[id] : "";
}

UBYTE SoundIsPlaying(void) { return g_ok && ToneIsPlaying(); }

void SoundStop(void) {
    if (g_ok) ToneStop();
}

void SoundStopIf(UBYTE id) {
    if (g_ok && g_current == id && ToneIsPlaying()) ToneStop();
}

