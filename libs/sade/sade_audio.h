#pragma once

#include "sade.h"
#include "sade_system.h"   // _sade_VBR, _sade_isPAL for the MOD player

// Paula audio: raw SFX playback (+ IFF 8SVX loading), PC-speaker style tone
// sequences (Tone*), and module music via ptplayer (.mod) or The Player 6.1A.
//
// SFX: 4 DMA channels, each with chip RAM pointer, length (words), period
// (pitch) and volume (0-64). One-shot samples are retired to a looping
// silence buffer by AudioService — call it once per frame.
//
// Music: wraps the included player610.6.no_cia.bin (The Player 6.1A,
// © 1992-95 Jarno Paananen — VBL-timed build, no CIA, safe with SADE).
// Modules must be P61-converted (.p61), not raw ProTracker .mod, and must
// live in chip RAM (embed with EMBED_CHIP). The player drives all 4 Paula
// channels — don't AudioPlay while music is running.
//
// Usage:
//   EMBED       player[] = { #embed "player610.6.no_cia.bin" };
//   EMBED_CHIP  module[] = { #embed "song.p61" };
//   ...
//   MusicInit(player, module);      // after SadeInit
//   while (...) { ...; ScreenFlip(scr); MusicVblService(); }
//   MusicStop();

// -------------------------------------------------------------------------
// SFX
// -------------------------------------------------------------------------
typedef struct {
    UWORD *data;     // chip RAM, word-aligned
    UWORD  len;      // length in words
    UWORD  period;   // pitch: PAL clock 3546895 / sample rate (428 = C-2)
    UBYTE  volume;   // 0-64
    UBYTE  loop;     // 1 = loop the whole sample forever
} SfxSample;

static UWORD *_sade_audSilence = NULL;
static UBYTE  _sade_audOneShot[4];
static UWORD  _sade_audBusy[4];               // frames until a one-shot ends

// Paula clock (Hz) — the period unit for every channel.
static inline ULONG AudioClock(void) { return _sade_isPAL ? 3546895UL : 3579545UL; }

// -------------------------------------------------------------------------
// AudioSetFilter — the switchable audio low-pass filter (~3.3 kHz, A500
// rev 6+, A600, A1200, A2000, A3000, A4000), wired to the power LED: CIA-A
// PRA bit 1 low = LED bright = filter on. It muffles bright sounds (square
// waves, high samples); games usually turn it off. Off dims the power LED on
// most models. The first call saves the state for AudioRestoreFilter.
// -------------------------------------------------------------------------
static UBYTE _sade_filterSaved = 0xff;           // 0xff: nothing saved

static void AudioSetFilter(UBYTE on) {
    volatile UBYTE *pra = (volatile UBYTE*)0xBFE001;
    if (_sade_filterSaved == 0xff) _sade_filterSaved = (UBYTE)(*pra & 2);
    if (on) *pra &= (UBYTE)~2;
    else    *pra |= 2;
}

// AudioRestoreFilter — back to the state before the first AudioSetFilter.
static void AudioRestoreFilter(void) {
    if (_sade_filterSaved == 0xff) return;
    volatile UBYTE *pra = (volatile UBYTE*)0xBFE001;
    if (_sade_filterSaved) *pra |= 2;
    else                   *pra &= (UBYTE)~2;
    _sade_filterSaved = 0xff;
}

// AudioRateToPeriod — sample rate (Hz, 55..28000) to an AUDxPER value.
static UWORD AudioRateToPeriod(UWORD hz) {
    if (hz < 55) hz = 55;
    UWORD per = divuw(AudioClock(), hz);
    return per < 124 ? 124 : per;              // Paula's DMA limit
}

// AudioInit — clear audio modulation, zero volumes, allocate the silence
// loop buffer. Master DMA is already on (ScreenOpen); per-channel DMA is
// enabled by AudioPlay.
static int AudioInit(void) {
    _sade_audSilence = (UWORD*)AllocMem(4, MEMF_CHIP | MEMF_CLEAR);
    if (!_sade_audSilence) return 0;
    custom->adkcon = 0x00ff;                    // clear AM/FM modulation bits
    for (UBYTE ch = 0; ch < 4; ch++) {
        custom->aud[ch].ac_vol = 0;
        _sade_audOneShot[ch] = 0;
        _sade_audBusy[ch] = 0;
    }
    return 1;
}

static void AudioShutdown(void) {
    custom->dmacon = DMAF_AUDIO;                // stop all channel DMA
    for (UBYTE ch = 0; ch < 4; ch++) custom->aud[ch].ac_vol = 0;
    if (_sade_audSilence) { FreeMem(_sade_audSilence, 4); _sade_audSilence = NULL; }
}

// AudioPlay — (re)start a channel with a sample. Retriggering a channel in
// the same frame it was started may glitch: Paula needs a couple of scan
// lines to latch the new pointers.
static void AudioPlay(UBYTE ch, const SfxSample *sfx) {
    if (ch > 3) return;
    custom->dmacon = (UWORD)(1 << ch);          // stop channel first
    custom->aud[ch].ac_ptr = sfx->data;
    custom->aud[ch].ac_len = sfx->len;
    custom->aud[ch].ac_per = sfx->period;
    custom->aud[ch].ac_vol = sfx->volume;
    custom->dmacon = (UWORD)(DMAF_SETCLR | (1 << ch));
    _sade_audOneShot[ch] = sfx->loop ? 0 : 2;   // retire after ~2 frames
    if (sfx->loop) {
        _sade_audBusy[ch] = 0xffff;             // until AudioStop
    } else {
        // len * 2 bytes * period clocks / clocks per frame, +1 to round up
        ULONG clocks = muluw(sfx->len, sfx->period);           // half the total
        UWORD perFrameHalf = _sade_isPAL ? 35469 : 35796;      // clock/50|60 / 2
        ULONG c = clocks >> 4;
        UWORD d = (UWORD)(perFrameHalf >> 4);
        _sade_audBusy[ch] = c >= muluw(d, 0xfffe) ? 0xfffe : (UWORD)(divuw(c, d) + 1);
    }
}

static void AudioStop(UBYTE ch) {
    if (ch > 3) return;
    custom->dmacon = (UWORD)(1 << ch);
    custom->aud[ch].ac_vol = 0;
    _sade_audOneShot[ch] = 0;
    _sade_audBusy[ch] = 0;
}

static void AudioStopAll(void) {
    for (UBYTE ch = 0; ch < 4; ch++) AudioStop(ch);
}

static void AudioSetVolume(UBYTE ch, UBYTE vol) {
    if (ch > 3) return;
    custom->aud[ch].ac_vol = vol > 64 ? 64 : vol;
}

// AudioSetPeriod — change a playing channel's pitch (takes effect at the
// next sample word). 124 is the fastest Paula can fetch.
static void AudioSetPeriod(UBYTE ch, UWORD period) {
    if (ch > 3) return;
    custom->aud[ch].ac_per = period < 124 ? 124 : period;
}

// AudioIsPlaying — 1 while a one-shot is still sounding (frame-accurate
// estimate from its length and period, kept by AudioService) or a looping
// sample has not been stopped.
static UBYTE AudioIsPlaying(UBYTE ch) {
    return ch < 4 && _sade_audBusy[ch] != 0;
}

// AudioService — call once per frame. Queues the silence loop on one-shot
// channels so Paula plays silence (instead of looping the sample) when the
// sample finishes. The new pointer/length only latch at the loop point, so
// the sample always plays to its end. Samples shorter than 2 frames may
// loop once before retiring.
static void AudioService(void) {
    for (UBYTE ch = 0; ch < 4; ch++) {
        if (_sade_audOneShot[ch] && --_sade_audOneShot[ch] == 0) {
            custom->aud[ch].ac_ptr = _sade_audSilence;
            custom->aud[ch].ac_len = 1;
        }
        if (_sade_audBusy[ch] && _sade_audBusy[ch] != 0xffff) _sade_audBusy[ch]--;
    }
}

// -------------------------------------------------------------------------
// SfxLoad8SVX — IFF 8SVX sound (uncompressed) into a chip RAM SfxSample.
//   Plays the one-shot part, or loops the whole sample if the file only has
//   a repeat part. Period from samplesPerSec, volume from VHDR (Fixed 1.0 =
//   64). The bytes are copied, so the file buffer may be freed afterwards.
//   Returns 0 if the file is not a usable 8SVX. Free with SfxFree.
// -------------------------------------------------------------------------
static ULONG _SfxU32(const UBYTE *p) {
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static int SfxLoad8SVX(const UBYTE *iff, ULONG size, SfxSample *out) {
    out->data = NULL;
    if (!iff || size < 12 || _SfxU32(iff) != 0x464F524DUL      // "FORM"
                          || _SfxU32(iff + 8) != 0x38535658UL) // "8SVX"
        return 0;
    ULONG oneShot = 0, repeat = 0, vol = 0x10000;
    UWORD rate = 8363;
    const UBYTE *body = NULL; ULONG bodyLen = 0;
    ULONG pos = 12;
    while (pos + 8 <= size) {
        ULONG id = _SfxU32(iff + pos), len = _SfxU32(iff + pos + 4);
        const UBYTE *ck = iff + pos + 8;
        if (pos + 8 + len > size) break;
        if (id == 0x56484452UL && len >= 20) {                 // "VHDR"
            oneShot = _SfxU32(ck);
            repeat  = _SfxU32(ck + 4);
            rate    = (UWORD)((ck[12] << 8) | ck[13]);
            if (ck[15] != 0) return 0;                          // compressed
            vol     = _SfxU32(ck + 16);
        } else if (id == 0x424F4459UL) {                        // "BODY"
            body = ck; bodyLen = len;
        }
        pos += 8 + len + (len & 1);
    }
    if (!body) return 0;
    UBYTE loop = oneShot == 0 && repeat != 0;
    ULONG n = loop ? repeat : (oneShot ? oneShot : bodyLen);
    if (n > bodyLen) n = bodyLen;
    n &= ~1UL;                                                   // whole words
    if (n < 2 || n > 131070UL) return 0;                         // AUDxLEN limit

    UBYTE *chip = (UBYTE*)AllocMem(n, MEMF_CHIP);
    if (!chip) return 0;
    for (ULONG i = 0; i < n; i++) chip[i] = body[i];
    out->data   = (UWORD*)chip;
    out->len    = (UWORD)(n >> 1);
    out->period = AudioRateToPeriod(rate);
    out->volume = (UBYTE)(vol >= 0x10000UL ? 64 : (vol >> 10));
    out->loop   = loop;
    return 1;
}

static void SfxFree(SfxSample *sfx) {
    if (sfx && sfx->data) {
        FreeMem(sfx->data, (ULONG)sfx->len * 2);
        sfx->data = NULL;
    }
}

// -------------------------------------------------------------------------
// Tone — PC-speaker sound emulation (AMOS-style "Play"/"Bell" beeps, and the
// speaker effects of DOS games, which are lists of PIT timer divisors).
//
// One channel plays a 1-word square wave whose period is rewritten for every
// step (two with ToneInitStereo: Paula wires channels 0/3 to the left output
// and 1/2 to the right, so one channel alone sounds from one side only); a
// further channel plays silence and serves as the step timer — Paula
// raises its audio interrupt (level 4) each time it restarts the 1-word
// sample, so steps are timed to the clock cycle with no CIA involved (CIA-A
// stays free for the keyboard, CIA-B for ptplayer). Monophonic like the
// speaker: a new tune replaces the current one unless the current one has a
// higher priority.
//
//   static const UWORD beepSteps[] = { TONE_HZ(880), 0, TONE_HZ(660), TONE_END };
//   static const ToneTune beep = { beepSteps, 20000, 0 };    // 20 ms steps
//   ToneInit(0, 3);           // after SadeInit (and AudioInit if used)
//   ToneInitStereo(0, 1, 3);  // or: both speakers (left 0, right 1, timer 3)
//   TonePlay(&beep);
//   ToneShutdown();           // before SadeShutdown
//
// Don't use the two channels for anything else while Tone is initialised,
// and don't run it alongside a module player that owns all four channels.
// -------------------------------------------------------------------------
#define TONE_PIT_CLOCK 1193180UL                  // PC timer input clock
#define TONE_HZ(f)     ((UWORD)(TONE_PIT_CLOCK / (f)))
#define TONE_REST      0x0000
#define TONE_END       0xffff

typedef struct {
    const UWORD *steps;    // PIT divisors (Hz = 1193180 / value), TONE_REST,
                           // terminated by TONE_END
    UWORD stepUs;          // step length in microseconds (300 .. 36000)
    UBYTE priority;        // replaces a playing tune only if >= its priority
} ToneTune;

static UWORD *_sade_toneWave = NULL;              // [0] square, [1] silence
static volatile const UWORD *_sade_tonePos = NULL;
static volatile UBYTE _sade_tonePri = 0;
static volatile UBYTE _sade_toneVol = 40;
static UBYTE _sade_toneCh = 0, _sade_toneTimerCh = 3;
static UBYTE _sade_toneCh2 = 0xff;                // second tone channel, 0xff = none
static UWORD _sade_toneDma;                       // DMA bits of the tone channel(s)
static volatile UWORD _sade_tonePendPer;          // next step's period, written at
static volatile UBYTE _sade_tonePend;             //   the tone channel's next word start
static volatile UBYTE _sade_toneOn;               // tone channel DMA running
static UWORD _sade_toneK;                         // period = div * (1 + K/65536)
static APTR  _sade_toneOldVec = NULL;

// PIT divisor -> Paula period of the 2-sample square wave:
//   per = clock / (2 * 1193180/div) = div * clock / 2386360 = div * 1.486 (PAL)
static inline UWORD _TonePeriod(UWORD div) {
    ULONG p = (ULONG)div + (muluw(div, _sade_toneK) >> 16);
    if (p < 124) p = 124;
    if (p > 65535) p = 65535;
    return (UWORD)p;
}

// Silence the tone channel(s), stop their DMA and drop a period not yet
// written. The next sound starts them again with one DMACON write, so a
// pair always starts sample-locked (see _sade_ToneIrq).
static inline void _ToneOff(void) {
    UWORD sbit = (UWORD)(1 << (INTB_AUD0 + _sade_toneCh));
    custom->intena = sbit;
    _sade_tonePend = 0;
    custom->aud[_sade_toneCh].ac_vol = 0;
    if (_sade_toneCh2 != 0xff) custom->aud[_sade_toneCh2].ac_vol = 0;
    custom->dmacon = _sade_toneDma;
    _sade_toneOn = 0;
}

// Silence the tone channel(s) (a rest: they keep running, in step) and drop
// a period not yet written.
static inline void _ToneMute(void) {
    UWORD sbit = (UWORD)(1 << (INTB_AUD0 + _sade_toneCh));
    custom->intena = sbit;
    _sade_tonePend = 0;
    custom->aud[_sade_toneCh].ac_vol = 0;
    if (_sade_toneCh2 != 0xff) custom->aud[_sade_toneCh2].ac_vol = 0;
}

// Level 4: the timer channel's block start = next step; the tone channel's
// block start (its 1-word wave reloading, enabled only while a step waits)
// = the moment to write that step's period.
//
// Why wait for it: with a second tone channel, the two must switch period at
// the same sample. Paula takes AUDxPER at each sample start, so two writes
// a few us apart straddle a sample boundary now and then; that channel then
// runs one sample (of a 2-sample wave: half a cycle) behind for good, and
// left and right play in opposite phase — the sound smears across the
// stereo field. Written right after a word start, the next sample boundary
// is a whole sample (>= 35 us) away, and both channels take the new period
// together. Rare slips (pitches near 20 kHz, where a sample is shorter
// than the interrupt's reaction) last only to the end of that sound: a
// sound starting from silence restarts both channels with one DMACON write.
static __attribute__((used)) __attribute__((interrupt)) void _sade_ToneIrq(void) {
    UWORD req  = custom->intreqr;
    UWORD sbit = (UWORD)(1 << (INTB_AUD0 + _sade_toneCh));
    UWORD tbit = (UWORD)(1 << (INTB_AUD0 + _sade_toneTimerCh));
    UBYTE c2 = _sade_toneCh2;

    if ((req & sbit) && _sade_tonePend) {          // tone word start: apply
        custom->intena = sbit;
        custom->intreq = sbit;
        custom->intreq = sbit;                     // twice for A4000
        UWORD per = _sade_tonePendPer;
        custom->aud[_sade_toneCh].ac_per = per;
        if (c2 != 0xff) custom->aud[c2].ac_per = per;
        custom->aud[_sade_toneCh].ac_vol = _sade_toneVol;
        if (c2 != 0xff) custom->aud[c2].ac_vol = _sade_toneVol;
        _sade_tonePend = 0;
    }

    if (!(req & tbit)) return;                     // not a step
    custom->intreq = tbit;
    custom->intreq = tbit;
    const UWORD *p = (const UWORD*)_sade_tonePos;
    if (!p) return;
    UWORD v = *p;
    if (v == TONE_END) {                           // done: silence, stop timer
        _ToneOff();
        custom->dmacon = (UWORD)(1 << _sade_toneTimerCh);
        _sade_tonePos = NULL;
        _sade_tonePri = 0;
        return;
    }
    _sade_tonePos = p + 1;
    if (v == TONE_REST) {
        _ToneMute();
    } else if (!_sade_toneOn) {                    // from silence: start both now
        UWORD per = _TonePeriod(v);
        custom->aud[_sade_toneCh].ac_per = per;
        custom->aud[_sade_toneCh].ac_vol = _sade_toneVol;
        if (c2 != 0xff) {
            custom->aud[c2].ac_per = per;
            custom->aud[c2].ac_vol = _sade_toneVol;
        }
        custom->dmacon = (UWORD)(DMAF_SETCLR | _sade_toneDma);   // in phase
        _sade_toneOn = 1;
    } else {                                       // at the next tone word start
        _sade_tonePendPer = _TonePeriod(v);
        _sade_tonePend = 1;
        custom->intreq = sbit;                     // forget earlier word starts
        custom->intreq = sbit;
        custom->intena = (UWORD)(INTF_SETCLR | sbit);
    }
}

static inline APTR *_ToneVecSlot(void) {
    return (APTR*)(((UBYTE*)_sade_VBR) + 0x70);   // level 4 autovector
}

// ToneInitStereo — claim three channels: the tone on toneCh and toneCh2 (one
// left, one right: centred on stereo), timerCh as the step timer. toneCh2 =
// 0xff plays the tone on toneCh only. Returns 0 on failure.
static int ToneInitStereo(UBYTE toneCh, UBYTE toneCh2, UBYTE timerCh) {
    if (toneCh > 3 || timerCh > 3 || toneCh == timerCh) return 0;
    if (toneCh2 != 0xff && (toneCh2 > 3 || toneCh2 == toneCh || toneCh2 == timerCh)) return 0;
    if (!_sade_toneWave)
        _sade_toneWave = (UWORD*)AllocMem(4, MEMF_CHIP | MEMF_CLEAR);
    if (!_sade_toneWave) return 0;
    _sade_toneWave[0] = 0x7f80;                    // +127, -128
    _sade_toneWave[1] = 0x0000;
    _sade_toneCh = toneCh;
    _sade_toneCh2 = toneCh2;
    _sade_toneTimerCh = timerCh;
    _sade_toneDma = (UWORD)((1 << toneCh) | (toneCh2 != 0xff ? 1 << toneCh2 : 0));
    // K = (clock / 2386360 - 1) * 65536
    _sade_toneK = _sade_isPAL ? 31871 : 32768;

    UWORD bit = (UWORD)(1 << (INTB_AUD0 + timerCh));
    custom->intena = bit;
    custom->dmacon = (UWORD)(_sade_toneDma | (1 << timerCh));
    if (!_sade_toneOldVec) {
        _sade_toneOldVec = *_ToneVecSlot();
        *_ToneVecSlot() = (APTR)_sade_ToneIrq;
    }

    // tone channel(s): the square wave, looping while DMA runs; a sound
    // starting from silence starts it (one DMACON write: in phase)
    for (UBYTE c = 0; c < 4; c++) {
        if (!(_sade_toneDma & (1 << c))) continue;
        custom->aud[c].ac_ptr = _sade_toneWave;
        custom->aud[c].ac_len = 1;
        custom->aud[c].ac_per = 1000;
        custom->aud[c].ac_vol = 0;
    }
    _sade_toneOn = 0;
    _sade_tonePend = 0;
    custom->intena = INTF_SETCLR | INTF_INTEN;
    return 1;
}

// ToneInit — claim two channels (tone, timer). Returns 0 on failure.
static int ToneInit(UBYTE toneCh, UBYTE timerCh) {
    return ToneInitStereo(toneCh, 0xff, timerCh);
}

// TonePlay — start a tune now (subject to priority).
static void TonePlay(const ToneTune *t) {
    if (!_sade_toneWave || !t || !t->steps) return;
    if (_sade_tonePos && t->priority < _sade_tonePri) return;
    UBYTE tc = _sade_toneTimerCh;
    UWORD bit = (UWORD)(1 << (INTB_AUD0 + tc));
    custom->intena = (UWORD)(bit | (1 << (INTB_AUD0 + _sade_toneCh)));  // no step
    _sade_tonePend = 0;                            // nor last tune's period
    custom->dmacon = (UWORD)(1 << tc);
    _sade_tonePos = t->steps;
    _sade_tonePri = t->priority;

    // timer period: one 2-byte block = one step -> per = stepUs * clock / 2e6
    UWORD us = t->stepUs < 300 ? 300 : (t->stepUs > 36000 ? 36000 : t->stepUs);
    UWORD per = divuw(muluw(us, _sade_isPAL ? 35469 : 35795), 20000);
    custom->aud[tc].ac_ptr = _sade_toneWave + 1;
    custom->aud[tc].ac_len = 1;
    custom->aud[tc].ac_per = per;
    custom->aud[tc].ac_vol = 0;
    custom->intreq = bit;
    custom->intena = (UWORD)(INTF_SETCLR | bit);
    custom->dmacon = (UWORD)(DMAF_SETCLR | (1 << tc));  // first irq = first step
}

// ToneStop — silence now (any priority).
static void ToneStop(void) {
    if (!_sade_toneWave) return;
    UWORD bit = (UWORD)(1 << (INTB_AUD0 + _sade_toneTimerCh));
    custom->intena = bit;
    custom->dmacon = (UWORD)(1 << _sade_toneTimerCh);
    _ToneOff();
    _sade_tonePos = NULL;
    _sade_tonePri = 0;
}

static UBYTE ToneIsPlaying(void) { return _sade_tonePos != NULL; }

// ToneSetVolume — loudness of sounding steps, 0-64 (default 40).
static void ToneSetVolume(UBYTE vol) { _sade_toneVol = vol > 64 ? 64 : vol; }

// ToneShutdown — stop, release both channels, restore the level-4 vector.
static void ToneShutdown(void) {
    if (!_sade_toneWave) return;
    ToneStop();
    custom->dmacon = _sade_toneDma;
    custom->intreq = (UWORD)((1 << (INTB_AUD0 + _sade_toneTimerCh)) |
                             (1 << (INTB_AUD0 + _sade_toneCh)));
    if (_sade_toneOldVec) { *_ToneVecSlot() = _sade_toneOldVec; _sade_toneOldVec = NULL; }
    FreeMem(_sade_toneWave, 4);
    _sade_toneWave = NULL;
    _sade_toneCh2 = 0xff;
}

// -------------------------------------------------------------------------
// Music — The Player 6.1A wrapper
//   Jump table of the no_cia build: +0 Init, +4 Music (per-frame tick),
//   +8 End. Register usage per the amiga-debug reference project.
// -------------------------------------------------------------------------
static const void *_sade_p61Player = NULL;

// MusicInit — point the player at a P61 module (chip RAM!).
// Returns 0 on success, non-zero otherwise.
static int MusicInit(const void *player, const void *module) {
    _sade_p61Player = player;
    register volatile const void *_a0 ASM("a0") = module;
    register volatile const void *_a1 ASM("a1") = NULL;  // samples inside module
    register volatile const void *_a2 ASM("a2") = NULL;  // no external buffer
    register volatile const void *_a3 ASM("a3") = player;
    register                int   _d0 ASM("d0");
    __asm volatile (
        "movem.l %%d1-%%d7/%%a4-%%a6,-(%%sp)\n"
        "jsr 0(%%a3)\n"
        "movem.l (%%sp)+,%%d1-%%d7/%%a4-%%a6"
    : "=r" (_d0), "+rf"(_a0), "+rf"(_a1), "+rf"(_a2), "+rf"(_a3)
    :
    : "cc", "memory");
    return _d0;
}

// MusicVblService — advance the tune one tick. Call exactly once per frame
// (right after ScreenFlip / SadeWaitVbl is the natural spot).
static void MusicVblService(void) {
    if (!_sade_p61Player) return;
    register volatile const void *_a3 ASM("a3") = _sade_p61Player;
    register volatile const void *_a6 ASM("a6") = (void*)0xdff000;
    __asm volatile (
        "movem.l %%d0-%%d7/%%a0-%%a2/%%a4-%%a5,-(%%sp)\n"
        "jsr 4(%%a3)\n"
        "movem.l (%%sp)+,%%d0-%%d7/%%a0-%%a2/%%a4-%%a5"
    : "+rf"(_a3), "+rf"(_a6)
    :
    : "cc", "memory");
}

// MusicIsPlaying — a P61 module was started with MusicInit and not stopped.
static UBYTE MusicIsPlaying(void) { return _sade_p61Player != NULL; }

// MusicStop — halt playback and silence the channels.
static void MusicStop(void) {
    if (!_sade_p61Player) return;
    register volatile const void *_a3 ASM("a3") = _sade_p61Player;
    register volatile const void *_a6 ASM("a6") = (void*)0xdff000;
    __asm volatile (
        "movem.l %%d0-%%d1/%%a0-%%a1,-(%%sp)\n"
        "jsr 8(%%a3)\n"
        "movem.l (%%sp)+,%%d0-%%d1/%%a0-%%a1"
    : "+rf"(_a3), "+rf"(_a6)
    :
    : "cc", "memory");
    _sade_p61Player = NULL;
}

// -------------------------------------------------------------------------
// MOD music — ptplayer 6.4 by Frank Wille (public domain), plays raw
// ProTracker .mod files with no conversion. libs/ptplayer/ptplayer.asm is
// assembled by vasm (see Makefile) and linked in.
//
// Playback is driven by a CIA-B timer interrupt on the level 6 autovector
// — no per-frame service call needed, and tempo commands (Fxx > 32) work
// correctly, unlike VBL-timed replayers.
//
// The module must be in chip RAM (EMBED_CHIP). While a mod is playing,
// use ModPlaySfx for sound effects (ptplayer picks the most idle channel
// and steals it back for music afterwards) — not AudioPlay.
// -------------------------------------------------------------------------
static UBYTE _sade_modInstalled = 0;

// ptplayer control variables (defined in ptplayer.asm)
extern volatile UBYTE mt_Enable  ASM("_mt_Enable");   // 0 = paused
extern volatile UBYTE mt_SongEnd ASM("_mt_SongEnd");  // set when song wrapped

// ModInit — install the CIA-B interrupt (first call only), initialize the
// module (samples stored after patterns, start at position 0), start it.
static void ModInit(const void *mod) {
    if (!_sade_modInstalled) {
        register volatile const void *_a0 ASM("a0") = _sade_VBR;
        register volatile const void *_a6 ASM("a6") = (void*)0xdff000;
        register volatile ULONG       _d0 ASM("d0") = _sade_isPAL;
        __asm volatile (
            "movem.l %%d1-%%d7/%%a1-%%a5,-(%%sp)\n"
            "jsr _mt_install_cia\n"
            "movem.l (%%sp)+,%%d1-%%d7/%%a1-%%a5"
        : "+rf"(_a0), "+rf"(_a6), "+rf"(_d0)
        :
        : "cc", "memory");
        // mt_install_cia enables EXTER but not the master bit; TakeSystem
        // left Disable() in effect, so turn INTEN on or nothing ever fires.
        custom->intena = INTF_SETCLR | INTF_INTEN;
        _sade_modInstalled = 1;
    }
    {
        register volatile const void *_a0 ASM("a0") = mod;
        register volatile const void *_a1 ASM("a1") = NULL;  // samples in module
        register volatile const void *_a6 ASM("a6") = (void*)0xdff000;
        register volatile ULONG       _d0 ASM("d0") = 0;     // song position
        __asm volatile (
            "movem.l %%d1-%%d7/%%a2-%%a5,-(%%sp)\n"
            "jsr _mt_init\n"
            "movem.l (%%sp)+,%%d1-%%d7/%%a2-%%a5"
        : "+rf"(_a0), "+rf"(_a1), "+rf"(_a6), "+rf"(_d0)
        :
        : "cc", "memory");
    }
    mt_SongEnd = 0;
    mt_Enable  = 1;
}

static void ModPause(void)  { mt_Enable = 0; }
static void ModResume(void) { mt_Enable = 1; }

// ModIsPlaying — a module is initialised and not paused.
static UBYTE ModIsPlaying(void) { return _sade_modInstalled && mt_Enable; }

// ModSongEnded — 1 once the song has wrapped past its last position
// (it keeps looping); ModClearSongEnd re-arms the flag.
static UBYTE ModSongEnded(void)    { return mt_SongEnd != 0; }
static void  ModClearSongEnd(void) { mt_SongEnd = 0; }

// ModSetVolume — music master volume 0-64 (does not affect ModPlaySfx).
static void ModSetVolume(UWORD vol) {
    register volatile const void *_a6 ASM("a6") = (void*)0xdff000;
    register volatile ULONG       _d0 ASM("d0") = vol;
    __asm volatile (
        "movem.l %%d1-%%d7/%%a0-%%a5,-(%%sp)\n"
        "jsr _mt_mastervol\n"
        "movem.l (%%sp)+,%%d1-%%d7/%%a0-%%a5"
    : "+rf"(_a6), "+rf"(_d0)
    :
    : "cc", "memory");
}

// ModPlaySfx — play a sample over the music on the most idle channel.
// The first word of the sample data MUST be zero (used for idling).
// loop is ignored; volume 0-64, len in words, chip RAM.
static void ModPlaySfx(const SfxSample *sfx) {
    register volatile const void *_a0 ASM("a0") = sfx->data;
    register volatile const void *_a6 ASM("a6") = (void*)0xdff000;
    register volatile ULONG       _d0 ASM("d0") = sfx->len;
    register volatile ULONG       _d1 ASM("d1") = sfx->period;
    register volatile ULONG       _d2 ASM("d2") = sfx->volume;
    __asm volatile (
        "movem.l %%d3-%%d7/%%a1-%%a5,-(%%sp)\n"
        "jsr _mt_soundfx\n"
        "movem.l (%%sp)+,%%d3-%%d7/%%a1-%%a5"
    : "+rf"(_a0), "+rf"(_a6), "+rf"(_d0), "+rf"(_d1), "+rf"(_d2)
    :
    : "cc", "memory");
}

// ModStop — stop music and sound effects, remove the CIA interrupt.
static void _sade_ModCall(const void *fn) {
    register volatile const void *_a3 ASM("a3") = fn;
    register volatile const void *_a6 ASM("a6") = (void*)0xdff000;
    __asm volatile (
        "movem.l %%d0-%%d7/%%a0-%%a2/%%a4-%%a5,-(%%sp)\n"
        "jsr (%%a3)\n"
        "movem.l (%%sp)+,%%d0-%%d7/%%a0-%%a2/%%a4-%%a5"
    : "+rf"(_a3), "+rf"(_a6)
    :
    : "cc", "memory");
}

extern const UBYTE _mt_end_ref[]        ASM("_mt_end");
extern const UBYTE _mt_remove_cia_ref[] ASM("_mt_remove_cia");

static void ModStop(void) {
    if (!_sade_modInstalled) return;
    mt_Enable = 0;
    _sade_ModCall(_mt_end_ref);         // stop music + sfx, silence Paula
    _sade_ModCall(_mt_remove_cia_ref);  // restore level 6 vector + CIA timers
    _sade_modInstalled = 0;
}
