# SADE — Simple Amiga Development Environment
## API Reference

Target: Amiga 500 / ECS / 68000 / PAL (WinUAE). Bare-metal — no OS after `SadeInit`.

> This copy documents the version of SADE bundled in `libs/sade/` for the
> game. The `examples/` it mentions are in the SADE repository, not here.

---

## Table of Contents

1. [Globals](#1-globals)
2. [sade_system.h — Lifecycle](#2-sade_systemh--lifecycle)
3. [sade.h — Screen & Copper](#3-sadeh--screen--copper)
4. [sade_graphics.h — Drawing](#4-sade_graphicsh--drawing)
5. [sade_input.h — Input](#5-sade_inputh--input)
6. [sade_copper.h — Dynamic Copper Lists](#6-sade_copperh--dynamic-copper-lists)
7. [sade_audio.h — SFX & Music](#7-sade_audioh--sfx--music)
8. [sade_math.h — Fixed-Point Math](#8-sade_mathh--fixed-point-math)
9. [sade_bob.h — Blitter Objects](#9-sade_bobh--blitter-objects)
10. [sade_ilbm.h — IFF ILBM Loader](#10-sade_ilbmh--iff-ilbm-loader)
11. [sade_texture.h — Texture Mapping](#11-sade_textureh--texture-mapping)
12. [sade_bank.h — Sprite Banks](#12-sade_bankh--sprite-banks)
13. [Support utilities (gcc8_c_support.h)](#13-support-utilities)
14. [Known Constraints & Gotchas](#14-known-constraints--gotchas)
15. [sade_file.h — Disk Loading](#15-sade_fileh--disk-loading)
16. [sade_tilemap.h — Tile Maps](#16-sade_tilemaph--tile-maps)
17. [sade_text.h — Bitmap Fonts](#17-sade_texth--bitmap-fonts)
18. [sade_collision.h — Collision](#18-sade_collisionh--collision)
19. [sade_actor.h — Animation](#19-sade_actorh--animation)
20. [sade_keyboard.h — Keyboard](#20-sade_keyboardh--keyboard)
21. [sade_mem.h — Memory Pools](#21-sade_memh--memory-pools)
22. [sade_package.h — Asset Packages](#22-sade_packageh--asset-packages)

---

## 1. Globals

Defined in `main.c`, declared `extern` in `sade.h`. Must exist in exactly one translation unit.

```c
struct ExecBase        *SysBase;   // Exec base, set by SadeInit
volatile struct Custom *custom;    // $DFF000 hardware registers
struct GfxBase         *GfxBase;   // graphics.library base
```

---

## 2. sade_system.h — Lifecycle

Include order: `sade_system.h` → `sade_input.h` → `sade_graphics.h`.

### `int SadeInit(void)`

Opens `graphics.library`, detects PAL/NTSC, calls `TakeSystem`, puts an ECS/AGA beam back on the machine's own 15 kHz timing (`BEAMCON0`), measures the refresh with the beam and turns on the VBL interrupt that counts frames.

- **Returns** 1 on success, 0 if library open failed.
- **Side effects:** Sets `SysBase`, `custom`, `GfxBase`, `_sade_isPAL`, `_sade_frameHz` (50/60), `_sade_frameLines`, starts `_sade_vblCount`. After return the OS is suspended — no Exec calls except `AllocMem`/`FreeMem`.
- **CRITICAL:** Never `KPrintF` before `SadeInit()` returns. `SysBase` is not valid yet and the call will crash at address 0.

```c
if (!SadeInit()) return 0;
```

### `void SadeShutdown(void)`

Restores OS state, closes `graphics.library`. Safe to call even if `SadeInit` failed (checks `GfxBase` internally).

```c
SadeShutdown();
return 0;
```

### `void TakeSystem(void)` *(internal, called by SadeInit)*

Sequence (important — order matters):

1. Saves `ActiView`, blitter, ADKCON, INTENA, DMACON, VBR.
2. `LoadView(0)` + `WaitTOF()` × 2 — blanks display **before** touching the interrupt vector.
3. Installs `_sade_SafeVblStub` on level-3 autovector (VBR+`$6C`).
4. `SadeWaitVbl()` × 2 to let the stub settle.
5. `Disable()` + kills all DMA/interrupts.

### `void FreeSystem(void)` *(internal, called by SadeShutdown)*

Sequence:

1. Kills all DMA/interrupts.
2. Points copper at OS lists (`GfxBase->copinit`, `LOFlist`).
3. Restores original VBL vector **then** `Enable()` — WaitTOF works again.
4. `LoadView(ActiView)` + `WaitTOF()` × 2.
5. Restores INTENA, DMACON, ADKCON, releases blitter.

### `void SadeSetInterruptHandler(APTR handler)`

Replace the safe VBL stub with a custom level-3 interrupt handler.

- Handler must be a `__attribute__((interrupt))` function.
- Handler **must** write `custom->intreq = (1 << INTB_VERTB)` twice before `RTE` (twice for A4000 bug).
- Handler **must** do `_sade_vblCount++` on each VERTB, or `SadeClock` (and every game tick paced by it) stops.
- Do **not** call `WaitTOF()` after installing — use `SadeWaitVbl()` instead.

```c
static __attribute__((interrupt)) void MyVbl(void) {
    _sade_vblCount++;
    custom->intreq = INTF_VERTB;
    custom->intreq = INTF_VERTB;
    // your per-frame logic here
}
// after TakeSystem:
SadeSetInterruptHandler((APTR)MyVbl);
```

### `UBYTE SadeIsPAL(void)`

Reads the Agnus/Alice ID (VPOSR bits 14-8); bit 4 set = NTSC part (OCS $00/$10, ECS $20/$30, AGA $22-$23/$32-$33). Returns 1 on PAL hardware, 0 on NTSC. Can be called before or after `SadeInit`. This is the machine's crystal: it picks the Paula clock and display window, not the game speed.

### `UWORD SadeBeamLine(void)`

Current beam line (0..312 PAL, 0..262 NTSC). Re-reads when VPOSR changes between the two word reads, so crossing line 255 → 256 never reads as line 0.

### `void SadeWaitVbl(void)`

Beam-counter polling wait for VBL. Waits for raster line ≥ 256, safe on both PAL (313 lines) and NTSC (263 lines).

- **Use this, not `WaitTOF()`**, after `TakeSystem` is in effect.

### `UBYTE SadeFrameHz(void)` / `ULONG SadeFrameCount(void)`

The measured display refresh (50 or 60) and the VBLs counted since `SadeInit`.

### `SadeClock` — game ticks independent of PAL/NTSC (`sade_clock.h`)

Write game logic in ticks of a fixed rate; the clock hands out the ticks due each frame from the real VBL count, so the game runs at the same wall-clock speed at 50 and 60 Hz and catches up (at most `SADE_CLOCK_MAX_FRAMES` = 4 frames) when a frame overran.

| Call | |
|---|---|
| `SadeClockStart(&c, ticksPerSec)` | Start now, at the measured refresh |
| `SadeClockTicks(&c)` | Ticks due since the last call (0 if called twice in a frame) |
| `SadeClockFrac(&c, parts)` | Progress into the next tick, 0..parts-1, for interpolated drawing |
| `SadeClockSync(&c)` | Drop the time spent paused (dialog, load) |
| `...At(&c, ..., now)` variants | Same with an explicit VBL count — read `SadeFrameCount()` once when several clocks must agree |

```c
SadeClock clk;
SadeClockStart(&clk, 81);
for (;;) {
    SadeWaitVbl();
    for (UBYTE n = SadeClockTicks(&clk); n; n--) GameTick();
    Draw(SadeClockFrac(&clk, 5));
}
```

The arithmetic is host-testable: `make test` runs `tests/clock_test.c`. `examples/22_pal_ntsc_test.c` checks detection and timing on the machine.

### `void SadeWaitBlt(void)`

Polls `DMACONR` bit 14 until the blitter is idle. Always call before issuing a new blitter operation.

---

## 3. sade.h — Screen & Copper

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `SADE_LORES` | 0 | 320-px lores mode |
| `SADE_HIRES` | 1 | 640-px hires mode |
| `SADE_AGA` | 2 | OR into the mode: AGA screen (lores) at any depth; depths 6–8 imply it |
| `SADE_SINGLE` | 0 | Single buffer |
| `SADE_DOUBLE` | 1 | Double buffer |
| `SADE_MAX_PLANES` | 8 | Max bitplane depth (6–8 need AGA) |

### `Screen` struct

```c
typedef struct {
    UBYTE  *planes[SADE_MAX_PLANES];      // current draw buffer bitplanes
    UBYTE  *backPlanes[SADE_MAX_PLANES];  // back buffer (double-buffering only)
    UWORD  *copper;
    UWORD   width;
    UWORD   height;
    UBYTE   depth;
    UWORD   rowBytes;     // width / 8
    UWORD   numColors;    // 1 << depth (OCS: capped at 32)
    UBYTE   doubleBuffer;
    UBYTE   aga;          // 1: AGA screen (4x fetch, 24-bit colours)
    UWORD   copColors;    // copper word offset of the colour block
    UWORD   copEnd;       // copper word offset of the end marker
    UWORD   copWords;     // copper list size in words
} Screen;
```

All bitplane and copper memory is allocated in **chip RAM**.

### `Screen *ScreenOpen(UWORD width, UWORD height, UBYTE depth, UBYTE mode, UBYTE dblBuf)`

Allocates chip RAM for bitplanes and copper list, programs the copper with DIW/DDF/BPLCON0/color registers, activates DMA.

| Parameter | Type | Notes |
|---|---|---|
| `width` | UWORD | Pixels, must be multiple of 16 (AGA: of 64) |
| `height` | UWORD | Scanlines, ≤ 256 for standard PAL window |
| `depth` | UBYTE | 1–5 bitplanes (2ⁿ colors); AGA 1–8 |
| `mode` | UBYTE | `SADE_LORES` or `SADE_HIRES`, `\| SADE_AGA` for an AGA screen |
| `dblBuf` | UBYTE | `SADE_SINGLE` or `SADE_DOUBLE` |

- **Returns** `Screen*` or `NULL` on allocation failure, and for an AGA
  screen on a non-AGA machine, in hires, or with a width that is not a
  multiple of 64.
- Colors default to black; call `ScreenSetColor` after open.
- **AGA screens** (`depth > 5` or `SADE_AGA`): up to 256 colours with 8 bits
  per gun (`ScreenSetRGB`), planes fetched 64 bits at a time (FMODE 3,
  DDFSTRT `$38`, DDFSTOP `$A0` for 320 px) — 8 planes fetched OCS-style would
  leave the blitter and copper no cycles on display lines. Planes are
  8-byte aligned.
- Copper list layout (word offsets):
  - `[0..7]` DIWSTRT, DIWSTOP, DDFSTRT, DDFSTOP
  - `[8..13]` BPLCON0, BPLCON1, BPLCON2
  - `[14..17]` BPL1MOD, BPL2MOD
  - `[18..]` BPLxPTH/PTL pairs (`depth × 4` words)
  - FMODE, BPLCON3, BPLCON4 (6 words; written on every machine so a
    Workbench's AGA settings never leak in — harmless on OCS/ECS)
  - `[copColors..]` colours: OCS `numColors × 2` words; AGA per bank of 32
    a BPLCON3 bank select, 32 high-nibble moves, BPLCON3 with LOCT, 32
    low-nibble moves (132 words), then BPLCON3 back to bank 0
  - `[copEnd]` END marker `$FFFF/$FFFE`

### `UBYTE SadeIsAGA(void)`

1 on an AGA machine (Alice ID `$22`/`$23`/`$32`/`$33` in VPOSR). DENISEID
is not used: on OCS it is an open bus, and with the native display idle
(e.g. Workbench on an RTG card) its noise can read stable as Lisa's `$F8`. A register read, usable before `SadeInit`; `SadeInit` stores it
in `_sade_isAGA`, which `ScreenOpen` checks.

### `void ScreenClose(Screen *s)`

Frees all chip RAM (bitplanes × 1 or 2 + copper list + struct). NULL-safe.

### `void ScreenSetColor(Screen *s, UWORD index, UWORD rgb)`

Sets palette entry `index` (0-based). OCS: patches both the copper list and
`custom->color[]` immediately. AGA: patches the copper list only, shown from
the next frame (a 24-bit colour write needs BPLCON3 bank writes that could
interleave with the copper's own).

- `rgb` is 12-bit `$0RGB` format (e.g., `0x0f00` = red); on AGA each nibble
  is doubled (`$F` → `$FF`).
- No-op if `index >= s->numColors`.

```c
ScreenSetColor(scr, 0, 0x0000); // background black
ScreenSetColor(scr, 1, 0x0fff); // color 1 = white
ScreenSetColor(scr, 2, 0x0f00); // color 2 = red
```

### `void ScreenSetRGB(Screen *s, UWORD index, ULONG rgb)`

Like `ScreenSetColor` with a 24-bit `$00RRGGBB` colour (an ILBM CMAP
entry). An AGA screen keeps all 8 bits per gun, an OCS screen the top 4
(`RGB444(rgb)`).

### `UWORD ScreenGetColor(Screen *s, UWORD index)` / `ULONG ScreenGetRGB(Screen *s, UWORD index)`

Read palette entry `index` back from the copper list ($0RGB, or
$00RRGGBB). Return 0 if `index >= s->numColors`.

### `UWORD RGB444(ULONG rgb)`

`$00RRGGBB` → `$0RGB` (top 4 bits of each gun).

### `void ScreenFlip(Screen *s)`

Double-buffer swap. Waits for VBL, updates copper plane pointers to display the just-drawn frame, swaps `planes[]` ↔ `backPlanes[]` so next draw targets the hidden buffer.

- No-op on `SADE_SINGLE` screens.
- **Always draw to `s->planes[]`** — after `ScreenFlip` these automatically point to the back buffer.

```c
// per-frame loop:
GfxClear(scr);
// ... draw ...
ScreenFlip(scr); // waits VBL, swaps
```

---

## 4. sade_graphics.h — Drawing

All functions draw into `s->planes[]` (the current draw buffer). Coordinates are pixel-space, clipped to screen bounds. Color index maps to palette: bit 0 selects plane 0, bit 1 selects plane 1, etc.

### `void GfxClear(Screen *s)`

Zeroes all bitplanes using the blitter (DMA, fast). Waits for blitter idle before and after each plane.

### `void GfxDrawLine(Screen *s, short x0, short y0, short x1, short y1, UBYTE color)`

Bresenham line. CPU-drawn pixel by pixel. Clips each pixel individually.

### `void GfxDrawBox(Screen *s, short x0, short y0, short x1, short y1, UBYTE color)`

Hollow rectangle. Draws four lines with `GfxDrawLine`.

### `void GfxFillBox(Screen *s, short x0, short y0, short x1, short y1, UBYTE color)`

Solid filled rectangle using the blitter, pixel-exact on any x: the edge words are a masked read-modify-write (A = masked ones, C = D = the plane; `D = A|C` to set a bit, `D = ~A&C` to clear it), so pixels outside the box stay. Clips to screen. Blitter-based — fast.

(Until 2026-10 it filled whole 16-pixel words: a box ending mid-word overwrote its neighbours, e.g. an 8-pixel text cursor wiped the letter next to it.)

### `void GfxDrawCircle(Screen *s, short cx, short cy, short r, UBYTE color)`

Hollow circle. Bresenham midpoint algorithm, CPU-drawn.

### `void GfxFillCircle(Screen *s, short cx, short cy, short r, UBYTE color)`

Filled circle via horizontal span lines. Uses `GfxDrawLine` internally — CPU-drawn.

### `void _GfxSetPixel(Screen *s, short x, short y, UBYTE colorIndex)` *(internal)*

Sets a single pixel. Public enough to use directly (e.g., for paint tools or custom rasterizers). Clips to screen bounds. Updates all planes atomically for the given color index.

### `UBYTE *GfxPolyBufAlloc(Screen *s)` / `void GfxPolyBufFree(Screen *s, UBYTE *buf)`

Allocate / free the polygon edge buffer: one bitplane's worth of chip RAM (`rowBytes × height`), cleared. Allocate once and reuse across `GfxFillPoly` calls.

### `void GfxFillPoly(Screen *s, UBYTE *edgeBuf, const short *xs, const short *ys, UBYTE numVerts, UBYTE color)`

Filled convex polygon. The CPU rasterises the edges (one pixel per scanline per edge) into `edgeBuf`; the blitter then runs in descending inclusive-fill mode (BLTCON1 DESC|IFE) to flood the interior — one blit per set bitplane.

Constraints:

- Polygon must be convex (or at least simple / non-self-intersecting).
- 3–8 vertices (`GFXPOLY_MAX_VERTS`).
- `edgeBuf` must come from `GfxPolyBufAlloc` (chip RAM, plane-sized).
- Coordinates are clipped to screen bounds.

---

## 5. sade_input.h — Input

### Mouse

#### `short mouseX`, `short mouseY`

Current mouse position (global). Updated by `InputUpdate`. Clamped to `[0, screenW-1]` × `[0, screenH-1]`.

#### `void InputUpdate(short screenW, short screenH)`

Must be called once per frame. Reads JOY0DAT (`$DFF00A`) delta counters, accumulates signed delta into `mouseX`/`mouseY`, clamps to screen.

#### `short MouseLeft(void)`

Returns non-zero while left mouse button is held. Reads CIA-A PRA bit 6 (`$BFE001`).

#### `short MouseRight(void)`

Returns non-zero while right mouse button is held. Reads POTGOR bit 10 (`$DFF016`).

#### `void MouseShow(Screen *s)`

Allocates chip RAM sprite buffer, copies crosshair image, enables sprite DMA. Sets sprite palette: COLOR17 = white body, COLOR18 = black outline.

#### `void MouseHide(void)`

Points sprite 0 at null sprite data to blank it.

#### `void MouseUpdateSprite(Screen *s)`

Updates SPR0POS/SPR0CTL in the chip RAM buffer to follow `mouseX`/`mouseY`. Call once per frame after `InputUpdate`.

### Joystick — Port 2 (player 1, left DB9 port)

Reads JOY1DAT (`$DFF00C`). Standard quadrature decode per Amiga HRM p.195.

```c
short Joy1Up(void)
short Joy1Down(void)
short Joy1Left(void)
short Joy1Right(void)
short Joy1Fire(void)   // CIA-A PRA bit 7
```

### Joystick — Port 1 (right DB9 port, same port as mouse)

```c
short Joy0Up(void)
short Joy0Down(void)
short Joy0Left(void)
short Joy0Right(void)
short Joy0Fire(void)   // CIA-A PRA bit 6
```

> **Note:** Port 1 is physically shared with the mouse. Joy0 functions read the same JOY0DAT register as `InputUpdate` and will give garbage results if the mouse moves. Use Port 2 (Joy1) for player input when a mouse is connected.

---

## 6. sade_copper.h — Dynamic Copper Lists

Builder for runtime copper effects (raster bars, splits, sprite multiplexing).
The Screen's own copper list stays in charge of display setup; a dynamic
`CopperList` is chained after it via `COP2LC` — `CopperAttach` patches the
screen list's END marker into a `MOVE COPJMP2`, so the copper jumps into the
dynamic list every frame after the display registers are set.

### `CopperList` struct

```c
typedef struct {
    UWORD *list;      // chip RAM instruction buffer
    UWORD  capacity;  // total words allocated
    UWORD  pos;       // current write position in words
    UBYTE  past255;   // internal: crossed beam line 255
} CopperList;
```

### Lifecycle

| Function | Description |
|---|---|
| `CopperCreate(capacityWords)` | Allocate list in chip RAM (2 words always reserved for END) |
| `CopperFree(cl)` | Free list |
| `CopperAttach(scr, cl)` | Chain finished list after the screen's copper list; call again to swap lists |
| `CopperDetach(scr)` | Restore the screen list's END marker |

### Builder

| Function | Description |
|---|---|
| `CopRestart(cl)` | Reset write position to 0 |
| `CopSet(cl, reg, val)` | MOVE instruction (`reg` = offset from $DFF000, e.g. `COP_COLOR00`) |
| `CopSetPtr(cl, regH, addr)` | MOVE pair for a 32-bit pointer register (BPLxPT, SPRxPT) |
| `CopWait(cl, x, y)` | WAIT for beam >= (x, y); handles y > 255 automatically |
| `CopWaitBlt(cl)` | WAIT until blitter idle |
| `CopEnd(cl)` | END marker — required before `CopperAttach` |
| `CopScreenLine(row)` | Screen row → beam line (adds display window top: $2C PAL / $18 NTSC) |
| `CopSetPalette(cl, scr, first, count, rgb)` | MOVEs loading colours `first..first+count-1` from `$00RRGGBB` values for screen `scr`: one per colour on OCS; on AGA per bank of 32 a BPLCON3 bank select, the high nibbles, BPLCON3+LOCT, the low nibbles, then BPLCON3 back to bank 0 (2 moves per colour + 2 per bank) |

### Rules

- Terminate with `CopEnd` **before** calling `CopperAttach` — an unterminated list crashes the copper.
- Waits must be issued in increasing line order; the line-255 crossing is emitted only once per list.
- To rebuild the effect every frame, use **two** lists and alternate (`CopperAttach` swaps at the next frame top), so the copper never reads a half-built list. See `main.c` for a bouncing raster-bar example.
- Writes beyond capacity are silently dropped (the list stays terminated); size generously — one wait + one move = 4 words.

---

## 7. sade_audio.h — SFX & Music

Paula audio: raw sample playback on the 4 DMA channels, plus module music
through one of two replayers:

- **ptplayer 6.4** (Frank Wille, `libs/ptplayer/ptplayer.asm`) — plays raw
  ProTracker `.mod` files, driven by a CIA-B timer interrupt. **The default
  music path** — used by `main.c`.
- **The Player 6.1A** (`player610.6.no_cia.bin`, © 1992–95 Jarno Paananen) —
  VBL-timed replayer for `.p61`-converted modules. Legacy path.

### SFX

```c
typedef struct {
    UWORD *data;     // chip RAM, word-aligned
    UWORD  len;      // length in words
    UWORD  period;   // PAL clock 3546895 / sample rate (428 = C-2)
    UBYTE  volume;   // 0-64
    UBYTE  loop;     // 1 = loop the whole sample forever
} SfxSample;
```

| Function | Description |
|---|---|
| `AudioInit()` | Clear modulation, zero volumes, allocate silence buffer. Returns 0 on alloc failure |
| `AudioShutdown()` | Stop all channels, free silence buffer |
| `AudioPlay(ch, sfx)` | (Re)start channel 0–3 with a sample |
| `AudioStop(ch)` | Disable channel DMA, zero volume |
| `AudioStopAll()` | `AudioStop` on all four channels |
| `AudioSetVolume(ch, vol)` | Write AUDxVOL (0–64, clamped) |
| `AudioSetPeriod(ch, period)` | Change a channel's pitch on the fly (clamped to Paula's minimum 124) |
| `AudioIsPlaying(ch)` | 1 while a one-shot is still sounding (frame estimate from len × period, counted down by `AudioService`) or a loop hasn't been stopped |
| `AudioService()` | Call once per frame — retires finished one-shots to a silence loop, counts down `AudioIsPlaying` |
| `AudioClock()` | Paula clock in Hz (3546895 PAL / 3579545 NTSC) |
| `AudioSetFilter(on)` | Switch the ~3.3 kHz low-pass ("LED") filter; off = brighter sound, dimmer power LED. First call saves the state |
| `AudioRestoreFilter()` | Put the filter back as it was before the first `AudioSetFilter` — call before exit |
| `AudioRateToPeriod(hz)` | Sample rate → AUDxPER (55–28000 Hz) |
| `SfxLoad8SVX(iff, size, &sfx)` | IFF 8SVX (uncompressed) → `SfxSample` in chip RAM: one-shot part, or the repeat part looped; period from `samplesPerSec`, volume from VHDR. Returns 0 if unusable |
| `SfxFree(&sfx)` | Free a sample made by `SfxLoad8SVX` |

### Tone — PC-speaker sound emulation

Plays square-wave **tone sequences** the way a PC speaker does: a list of
PIT timer divisors (pitch = 1193180 / value, `TONE_REST` = silence,
`TONE_END` terminates), one per fixed-length step. That is exactly how DOS
games stored their speaker effects, so their tables can be played as-is.

One channel loops a 1-word square wave whose period is rewritten each step;
a second channel plays silence at a period chosen so that its **audio
interrupt (level 4) fires once per step** — clock-exact timing with no CIA
(CIA-A stays the keyboard's, CIA-B ptplayer's), independent of the frame
rate. Monophonic, like the speaker: a new tune replaces the playing one
unless the playing one has a higher priority.

```c
static const UWORD beepSteps[] = { TONE_HZ(880), TONE_REST, TONE_HZ(660), TONE_END };
static const ToneTune beep = { beepSteps, 20000, 0 };   // 20 ms per step

ToneInit(0, 3);          // after SadeInit: tone on channel 0, timer on 3
// or ToneInitStereo(0, 1, 3): tone on 0 (left) and 1 (right), timer on 3
TonePlay(&beep);         // returns at once; steps run from the interrupt
ToneShutdown();          // before SadeShutdown (restores the level-4 vector)
```

```c
typedef struct {
    const UWORD *steps;  // PIT divisors, TONE_REST, ..., TONE_END
    UWORD stepUs;        // step length in µs (300–36000)
    UBYTE priority;      // replaces a playing tune only if >= its priority
} ToneTune;
```

| Function | Description |
|---|---|
| `ToneInit(toneCh, timerCh)` | Claim two channels, install the level-4 handler. Returns 0 on failure. Paula channels 0/3 are left, 1/2 right: one tone channel sounds from one side only |
| `ToneInitStereo(toneCh, toneCh2, timerCh)` | Same with the tone on two channels (one per side), so it is centred. The pair is kept sample-locked: periods are written right after the tone channel's word start (its audio interrupt), and every sound starting from silence restarts both with one DMACON write — otherwise a 2-sample wave can slip half a cycle and play in opposite phase left/right |
| `TonePlay(&tune)` | Start a tune now (subject to priority) |
| `ToneStop()` | Silence now, regardless of priority |
| `ToneIsPlaying()` | 1 until the playing tune reaches `TONE_END` |
| `ToneSetVolume(vol)` | Loudness of sounding steps, 0–64 (default 40) |
| `ToneShutdown()` | Stop, release both channels, restore the old level-4 vector |
| `TONE_HZ(f)` | Frequency in Hz → PIT divisor |

Don't use Tone's two channels for anything else while it is initialised,
and don't run it alongside a module player that owns all four channels.

### Music — ProTracker modules (ptplayer, default)

Plays raw `.mod` files with no conversion. `libs/ptplayer/ptplayer.asm` is
assembled by vasm (see Makefile `VASM` rule) and linked in. Playback is
driven by a **CIA-B timer interrupt on the level 6 autovector** — no
per-frame service call, and tempo commands (`Fxx` > 32) work correctly,
unlike VBL-timed replayers. The module must be in **chip RAM**:

```c
EMBED_CHIP mod[] = { #embed "assets/audio/song.mod" };

ModInit(mod);                    // after SadeInit; installs CIA int on first call
while (...) { ... }              // nothing to call per frame
ModStop();                       // before SadeShutdown
```

| Function | Description |
|---|---|
| `ModInit(mod)` | Install CIA-B interrupt (first call only), init module, start playing |
| `ModPause()` / `ModResume()` | Toggle `mt_Enable` |
| `ModSetVolume(vol)` | Music master volume 0–64 (does not affect `ModPlaySfx`) |
| `ModPlaySfx(sfx)` | Play an `SfxSample` over the music — ptplayer picks the most idle channel and steals it back for music afterwards |
| `ModIsPlaying()` | 1 if a module is initialised and not paused |
| `ModSongEnded()` / `ModClearSongEnd()` | `mt_SongEnd` (song wrapped past its end; it keeps looping) / re-arm it |
| `ModStop()` | Stop music + SFX, remove the CIA interrupt, restore vectors |

Exposed ptplayer control variables:

```c
extern volatile UBYTE mt_Enable;   // 0 = paused
extern volatile UBYTE mt_SongEnd;  // set when the song wrapped around
```

ptplayer rules:

- While a mod is playing, use **`ModPlaySfx`, not `AudioPlay`** — ptplayer
  owns all 4 channels and would fight direct Paula writes.
- The **first word of a `ModPlaySfx` sample must be zero** (ptplayer uses it
  for channel idling). `loop` is ignored.
- `ModInit` sets the INTENA master enable bit (`mt_install_cia` enables EXTER
  but not INTEN, and `TakeSystem` left `Disable()` in effect).

### Music — P61 modules (legacy)

Modules must be **P61-converted** (`.p61`, via the P61 converter), not raw
ProTracker `.mod`, and must be in **chip RAM**:

```c
EMBED      player[] = { #embed "player610.6.no_cia.bin" };
EMBED_CHIP module[] = { #embed "song.p61" };

MusicInit(player, module);      // after SadeInit; 0 = success
while (...) { ...; ScreenFlip(scr); MusicVblService(); }
MusicStop();
```

| Function | Description |
|---|---|
| `MusicInit(player, module)` | Initialize replayer with a P61 module. Returns 0 on success |
| `MusicVblService()` | Advance one tick — call exactly once per frame |
| `MusicIsPlaying()` | 1 between `MusicInit` and `MusicStop` |
| `MusicStop()` | Halt playback, silence channels |

### Rules

- Both replayers drive **all 4 channels** — don't `AudioPlay` while music is
  running (with ptplayer, use `ModPlaySfx` instead).
- P61 only: `MusicVblService` must be called every frame, steadily; after
  `ScreenFlip` (which waits for VBL) is the natural spot. ptplayer needs no
  per-frame call.
- One-shot SFX shorter than 2 frames (~40 ms PAL) may loop once before `AudioService` retires them.
- Retriggering a channel in the same frame it was started may glitch — Paula needs a couple of scan lines to latch new pointers.

---

## 8. sade_math.h — Fixed-Point Math

8.8 fixed-point (value = integer / 256). No FPU on the 68000 — trig comes
from a 256-entry lookup table (`sade_sinTable`, values −256…+256), and
multiply/divide use the native MULS/DIVS opcodes via the `mulsw`/`divsw`
intrinsics.

| Function | Description |
|---|---|
| `SadeSin(a)` | sin of angle `a` (UBYTE, 256 units = full circle). Returns −256…+256 |
| `SadeCos(a)` | cos — same table, phase-shifted 64 entries |
| `FpMul(a, b)` | 8.8 × 8.8 multiply (`mulsw(a,b) >> 8`) |
| `FpDiv(n, d)` | 8.8 divide (`divsw(n << 8, d)`). **Caller must ensure `d != 0`** |

Angles are `UBYTE` — wraparound is free, no range checks needed.

---

## 9. sade_bob.h — Blitter Objects

Masked (cookie-cut) blitter objects with per-buffer erase tracking and full
screen-edge clipping. Minterm `0xCA` (`D = A·B + ~A·C`): A = shape mask,
B = image plane, C/D = screen. On planes where the Bob has no image data,
a hole-punch blit (`D = ~A·C`) zeroes the shape so background colors don't
bleed through.

### Data

```c
typedef struct {
    UBYTE *planeData[BOB_MAX_PLANES];
    UBYTE *maskData;    // single shape mask (1 wherever any pixel exists)
    UBYTE  planeMask;   // which planes have any image data
    UWORD  visWidth;    // visible pixel width (what the caller specifies)
    UWORD  width;       // allocated width = visWidth + 16 (blitter shift pad)
    UWORD  height;
    UBYTE  depth;
    UWORD  rowBytes, wordWidth;
} BobImage;

typedef struct {
    BobImage *image;
    short     x, y, vx, vy;
    UBYTE     visible;
    BobSlot   slot[2];  // saved erase rect, one per buffer
} Bob;
```

Image data is allocated **16 px wider** than requested so the blitter shift
never reads past the end of a row. All image/mask data in chip RAM.

### Functions

| Function | Description |
|---|---|
| `BobImageCreate(visWidth, h, depth)` | Allocate chip RAM planes + mask. Returns NULL on failure |
| `BobImageFree(img)` | Free image memory |
| `BobImageTrim(img)` | Blit only the rows/words that hold pixels (from the mask); Bob x/y still mean the cell's top-left. Cheaper draws/saves when the art is smaller than the cell |
| `BobImageSetPixel(img, x, y, color)` | Plot a pixel into image + mask (CPU; for building images) |
| `BobImageFillCircle(img, cx, cy, r, color)` | Filled circle helper |
| `BobCreate(img)` / `BobFree(bob)` | Allocate / free a Bob bound to an image |
| `BobDraw(bob, s, buf)` | Clipped masked blit; records erase rect in `slot[buf]` |
| `BobEraseAll(bobs, n, s, buf, bg)` | Restore saved rects from an `IlbmImage` background (or zero-fill if `bg` is NULL) |
| `BobDrawAll(bobs, n, s, buf)` | Draw all, back-to-front — index 0 appears on top |

### Frame loop

```c
// Double-buffered (buf alternates 0/1):
SadeWaitVbl();
BobEraseAll(bobs, n, s, buf, bg);  // erase this buffer's 2-frame-old rects
/* move bobs */
BobDrawAll(bobs, n, s, buf);
ScreenFlip(s);
buf ^= 1;
```

Single-buffer is the same without `ScreenFlip`, always `buf = 0`.

---

## 10. sade_ilbm.h — IFF ILBM Loader

Parses IFF ILBM image files from a memory buffer (e.g. `#embed`-ed file
data). Plane data lands in chip RAM.

| Function | Description |
|---|---|
| `IlbmLoad(data, size)` | Parse; returns `IlbmImage*` or NULL |
| `IlbmFree(img)` | Free all chip RAM planes |
| `IlbmApplyPalette(img, scr)` | Copy CMAP into the screen palette (clamped to both sizes; 8 bits per gun on an AGA screen) |
| `IlbmBlit(img, scr, dstX, dstY, srcX, srcY)` | Blitter copy (D = A) to screen; `srcX` is forced word-aligned |

```c
IlbmImage *img = IlbmLoad(picData, picSize);
if (img) {
    IlbmApplyPalette(img, scr);
    IlbmBlit(img, scr, 0, 0, 0, 0);
}
```

Constraints:

- Compression 0 (none) and 1 (PackBits) supported.
- Masking plane and CRNG/CAMG chunks are ignored.
- Depth ≤ 8 planes (more than 5 need an AGA screen), ≤ 256 colors.
- `img->palette[]` is the CMAP as `ULONG $00RRGGBB` (`numColors` entries,
  a `UWORD`); `RGB444(c)` gives the OCS value. The array is 1 KB — keep
  an `IlbmImage` off the 4 KB stack.
- An `IlbmImage` doubles as the background source for `BobEraseAll`.

---

## 11. sade_texture.h — Texture Mapping

Software texture-mapped triangles and quads (CPU per-pixel — for demo
effects, not hot game loops). Texture dimensions **must be powers of two**
(wrapping uses bit masks, not modulo).

### Render modes

| Mode | Technique |
|---|---|
| `TEX_AFFINE` | Linear UV interpolation — fastest, skews under perspective |
| `TEX_PERSPECTIVE` | Per-pixel 1/z correction — slowest, correct |
| `TEX_SUBDIVIDE` | Affine in 16-px sub-spans with per-span correction — good quality/cost |

### Functions

| Function | Description |
|---|---|
| `TexAlloc(w, h, depth)` / `TexFree(t)` | Allocate/free chip RAM texture planes |
| `TexCheckerboard(t, colA, colB, tileShift)` | Fill with a checker test pattern |
| `TexInitRecipTable()` | Build the 1/z lookup table — **call once before any perspective/subdivide draw** |
| `GfxTexTri(s, t, mode, x,y,z,u,v ×3)` | Textured triangle. `u`,`v` are 16.16 fixed-point (`(ULONG)texel << 16`) |
| `GfxTexQuad(s, t, mode, ...)` | Quad split into two triangles (v0 top-left, clockwise) |

`z` values are integer world-space depth, used only by the perspective modes.

---

## 12. sade_bank.h — Sprite Banks

AMOS-style sprite banks: cut N fixed-size sprites out of an IFF ILBM
spritesheet and store them as ready-to-blit `BobImage`s, indexed
`0..count-1`. The sheet's palette is kept in the bank (like AMOS `.abk`
banks carried their own palette).

### Data

```c
typedef struct {
    BobImage **images;                   // count entries, 0-based
    UWORD      count;
    ULONG      palette[ILBM_MAX_COLORS]; // sheet CMAP, $00RRGGBB
    UWORD      numColors;
} SpriteBank;
```

### Functions

| Function | Description |
|---|---|
| `CreateSpriteBankFromFile(iffData, size, startX, startY, sprW, sprH, count)` | Load the sheet (`#embed`-ed IFF bytes), cut `count` sprites, free the sheet, return the bank. NULL on failure |
| `SpriteBankFromIlbm(sheet, startX, startY, sprW, sprH, count)` | Same, from an already-loaded `IlbmImage` (sheet is not freed) |
| `SpriteBankImage(bank, n)` | Fetch sprite `n` (0-based). NULL if out of range |
| `SpriteBankTrim(bank)` | `BobImageTrim` every sprite in the bank |
| `SpriteBankApplyPalette(bank, scr)` | Load the sheet's CMAP into the screen palette |
| `SpriteBankFree(bank)` | Free every `BobImage` in the bank, then the bank. Free bound `Bob`s first |

### Usage

```c
EMBED sheet[] = { #embed "sprites.iff" };

SpriteBank *bank = CreateSpriteBankFromFile(sheet, sizeof(sheet),
                                            0, 0,    // start x,y on the sheet
                                            32, 32,  // sprite size
                                            4);      // sprites 0..3
SpriteBankApplyPalette(bank, scr);
Bob *player = BobCreate(SpriteBankImage(bank, 0));
...
BobFree(player);
SpriteBankFree(bank);
```

### Rules

- **Cutting order:** left-to-right from `(startX, startY)`; when the next
  sprite would pass the sheet's right edge, the cutter wraps back to
  `startX` on the next row (`startY + sprH`, …). Strips and grids both work.
- **`sprW` must be a multiple of 16** (blitter word constraint on
  `BobImage`); any `sprH`. Creation fails (NULL) otherwise.
- **Color 0 is transparent** (AMOS convention) — the Bob mask is set only
  where a pixel has a non-zero color index.
- Sprite images live in chip RAM; each costs
  `(sprW + 16) / 8 × sprH × (depth + 1)` bytes (planes + mask).
- Cutting is CPU per-pixel — do it at load time, not in the game loop.

---

## 13. Support Utilities

From `support/gcc8_c_support.h` (provided by amiga-debug extension).

### Embedded data

```c
EMBED name[] = { ... };              // embed data in .rodata (any RAM)
EMBED_CHIP name[] = { ... };         // embed data into chip RAM section
INCBIN_CHIP(name, "file.bin");       // link binary file directly into chip RAM
```

### Debug output (WinUAE only)

```c
KPrintF("fmt %ld\n", (ULONG)val);   // print to WinUAE debug console
debug_clear();
debug_text(x, y, "text", 0xffffff); // overlay text on WinUAE video
debug_rect(l, t, r, b, color);
debug_filled_rect(l, t, r, b, color);
```

Coordinates for debug overlay are PAL-based: (0,0)–(768,576).

### Performance

```c
warpmode(1);   // disable WinUAE speed limit (for load timing)
warpmode(0);
debug_start_idle();  // mark idle time for profiler
debug_stop_idle();
```

### 68000 fixed-point math

```c
unsigned int  muluw(unsigned short a, unsigned short b); // MULUW opcode
int           mulsw(short a, short b);                   // MULSW opcode
unsigned short divuw(unsigned int a, unsigned short b);  // DIVUW opcode
short          divsw(int a, short b);                    // DIVSW opcode
```

Use these instead of `*` / `/` on short values — the 68000 has native 16-bit mul/div instructions that are significantly faster than the compiler's software fallback on 32-bit operands.

---

## 14. Known Constraints & Gotchas

| # | Issue | Rule |
|---|---|---|
| 1 | **KPrintF before SadeInit** | Never. SysBase is 0 until SadeInit sets it; call crashes at address 0. |
| 2 | **WaitTOF after VBL stub** | After TakeSystem, WaitTOF sleeps forever (Exec TOF signal never fires). Use `SadeWaitVbl()`. |
| 3 | **Restore vector before Enable** | FreeSystem must restore original VBL vector before `Enable()` so WaitTOF works on shutdown. |
| 4 | **PAL-only line 311** | `SadeWaitVbl` waits for line ≥ 256, not 311 — works on both PAL and NTSC. |
| 4a | **Frame-counted timing** | Counting loop passes runs 20% faster on NTSC and slows down when frames drop. Pace logic with `SadeClock`. |
| 5 | **Copper list size** | Fixed 128 words. Maximum depth 5 + 32 colors + overhead = ~100 words. Safe up to 5 planes. |
| 6 | **AllocMem after TakeSystem** | `AllocMem`/`FreeMem` still work (Exec memory list is not touched). All other Exec/Intuition calls are unsafe. |
| 7 | **Sprite palette overlap** | `MouseShow` writes COLOR17–19. If your palette uses these entries, mouse sprite colors will be wrong. |
| 8 | **LTO reordering** | Build uses `-flto -fwhole-program`. Any function called before SadeInit must not transitively call KPrintF. |
| 9 | **Toolchain path spaces** | Junction `C:\amiga-gcc` → extension `opt/` required; Makefile uses `C:/amiga-gcc/bin/...`. |
| 10 | **Joy0 / mouse conflict** | JOY0DAT is read by both `InputUpdate` and `Joy0*` functions. Don't use Joy0 while mouse is active. |
| 11 | **ptplayer + INTEN** | `mt_install_cia` enables EXTER but not the INTENA master bit; `ModInit` sets it. Don't clear INTEN while a mod plays or music stops silently. |
| 12 | **ModPlaySfx first word** | The first word of a sample passed to `ModPlaySfx` must be zero — ptplayer points idle channels at it. |
| 13 | **AudioPlay vs ptplayer** | While a mod is playing, only `ModPlaySfx` may play SFX. Direct `AudioPlay` fights ptplayer for the channels. |
| 14 | **vasm required** | `libs/ptplayer/ptplayer.asm` is assembled with `vasmm68k_mot` (bundled with the amiga-debug extension; Makefile locates it per-platform). |
| 15 | **Disk dies with the takeover** | All `FileLoad` calls must happen before `SadeInit()`; afterwards they fail safely (NULL/−1). Load everything, then take the machine. |
| 16 | **CIA-A ICR reads clear it** | Reading the interrupt control register clears all its flags — route CIA-A access through `sade_keyboard.h`. |
| 17 | **No 32-bit mul/div library** | The build is `-nostdlib`: `ULONG * ULONG`, `/` and `%` on 32-bit values pull in `__mulsi3`/`__udivsi3` and fail to link. Use `muluw`/`divuw`/`divsw`, shifts (`(v<<3)+(v<<1)` = `v*10`), or subtraction. |
| 18 | **One LTO partition** | `CCFLAGS` has `-flto-partition=one`: once the program is big enough GCC splits LTO into partitions and marks symbols hidden, which this target warns about by the dozen. |
| 19 | **RAW key codes are positions** | `RAWKEY_Y` is the key labelled Z on QWERTZ keyboards (and vice versa). Accept both where it's a Y/N question. |

---

## 15. sade_file.h — Disk Loading

Load assets from disk instead of `#embed`-ing them. dos.library is part of
the OS, so **all disk access must happen before `SadeInit()`** — a classic
load phase. After the takeover every function fails safely (NULL/−1) via the
`_sade_sysTaken` guard set by `TakeSystem`/`FreeSystem`.

Programs using this header define a **fourth global**:

```c
struct DosLibrary *DOSBase;   // next to SysBase, custom, GfxBase

int main(void) {
    if (!FileInit()) return 0;               // OS still alive
    ULONG picSize, modSize;
    void *pic = FileLoad("gfx/title.iff", MEMF_PUBLIC, &picSize);
    void *mod = FileLoadChip("music/song.mod", &modSize);
    FileShutdown();                          // close dos.library
    if (!pic || !mod) return 0;
    if (!SadeInit()) return 0;               // NOW take over
    ...
}
```

| Function | Description |
|---|---|
| `FileInit()` | Sets `SysBase`, opens `dos.library`. Returns 0 after takeover or on failure |
| `FileShutdown()` | Close `dos.library` — must also run before `SadeInit` |
| `FileLoad(path, memFlags, &size)` | Whole file → fresh `AllocMem` buffer. NULL on missing file / OOM / short read |
| `FileLoadChip(path, &size)` | Shorthand for `MEMF_CHIP` |
| `FileSize(path)` | Byte size, or −1 |
| `FileUnload(data, size)` | Free a buffer. Works after takeover (`FreeMem` survives), NULL-safe |
| `FileSave(path, data, size)` | Write (replace) a file; 1 on success. OS alive only: before `SadeInit`, or after `SadeShutdown` with a fresh `FileInit` |

Rules:

- Chip RAM (`FileLoadChip`) for anything the chips read via DMA: modules,
  samples, sprite data. Any RAM for bytes the CPU merely parses — `IlbmLoad`
  copies planes to chip itself, so `FileUnload` the raw IFF right after.
- Paths resolve from the program's start directory. The amiga-debug launcher
  mounts the project root as `dh1:` and CDs into it, so relative paths like
  `"assets/audio/song.mod"` work.
- `FileInit` sets `SysBase`, so `KPrintF` is safe from then on (normally only
  after `SadeInit`).
- See `examples/15_load_file.c` — a complete program with no embedded data.
- Saving while the game runs is impossible for the same reason. Keep the
  data in RAM and write it on the way out (Dangerous Dave's high scores):

  ```c
  SadeShutdown();                      // the OS is back
  if (changed && FileInit()) {
      FileSave("DSCORES.DAV", buf, sizeof buf);
      FileShutdown();
  }
  ```

---

## 16. sade_tilemap.h — Tile Maps

Stage A: flip-screen rooms with per-tile collision flags. Maps are edited in
**Tiled** (mapeditor.org) and converted by `tools/tiled2sade.py` to a binary
`.map` the 68000 uses without parsing; the tileset is an IFF sheet
(`tools/png2iff.py --colors 14` keeps depth 4 with palette slots 14–15 free
for game sprites).

`.map` format (`"SMAP"`, big-endian): magic, `UWORD mapW/mapH` (tiles),
`UBYTE tileW/tileH/numLayers/reserved`, then one byte per cell per layer
(row-major, 0-based tileset indices), then a 256-entry per-tile-TYPE flag
table. Flags come from Tiled boolean tile properties `solid`/`ladder`/
`hurt`/`oneway` (bits `TILE_SOLID 0x01`, `TILE_LADDER 0x02`, `TILE_HURT
0x04`, `TILE_ONEWAY 0x08`) or an int property `flags`.

| Function | Description |
|---|---|
| `TileSetCreate(sheet, tileW, tileH)` | Wrap a loaded `IlbmImage` sheet. The sheet stays resident (don't `IlbmFree` while in use); tiles blit straight from it — no masks, no copies. `tileW` multiple of 16 |
| `TileSetFree(ts)` | Free the wrapper only |
| `TileMapLoad(data, size, ts)` | Zero-copy view over `.map` bytes — buffer must outlive the map |
| `TileMapFree(tm)` | Free the view only |
| `TileMapLayer(tm, n)` | Raw cells of extra layers (game data, not drawn) |
| `TileAt(tm, tx, ty)` | Tile index at a cell (out of bounds → 0) |
| `TileFlags(tm, tx, ty)` | Attribute bits (out of bounds → `TILE_SOLID` — the world edge is a wall) |
| `TileFlagsAtPixel(tm, wx, wy)` | Same by world pixel — the collision workhorse |
| `TileBlit(ts, s, n, x, y)` | One tile onto the draw buffer (`x` multiple of 16, no partial clip) |
| `TileMapCanvasCreate(ts, wTiles, hTiles)` | `IlbmImage`-compatible chip RAM room buffer; free with `IlbmFree` |
| `TileMapRenderRoom(tm, canvas, originTx, originTy)` | Blit a canvas-full of layer-0 tiles (~2–3 frames of blitter for 320×256 — do it on room entry) |

Rendering model: render the room into the canvas on room entry, `IlbmBlit`
it onto both screen buffers, and pass the canvas as the `bg` of
`BobEraseAll` — Bobs then restore the tiles behind themselves. Constraints:
single tileset, ≤ 256 tile types, ≤ 4 layers, no flipped/rotated tiles.
See `examples/16_tilemap.c` for a complete four-room platformer with
ladders (`TILE_LADDER`) and one-way ladder tops (`TILE_ONEWAY`).

### Stage B — hardware scrolling (`TileScroller`)

The whole map (layer 0) is pre-rendered once into a canvas as wide as the
map; scrolling is then zero blits per frame — pure copper-list patching
(bitplane pointers = coarse, BPLCON1 = fine, BPLxMOD = row skip, DDFSTRT
pulled one word early). Patches the Screen's own copper list, so palette
and `CopperAttach` effects keep working.

| Function | Description |
|---|---|
| `TileScrollerCreate(tm)` | Allocate wide canvas + pre-render the map (load-time) |
| `TileScrollerAttach(sc, s)` | Patch the copper, show x = 0. Returns 0 if incompatible |
| `TileScrollerSetX(sc, s, x)` | Show world pixel x (0..`maxScroll`) — once per frame after `SadeWaitVbl` |
| `TileScrollerDetach(sc, s)` / `TileScrollerFree(sc)` | Restore display / free |

Constraints: lores `SADE_SINGLE` screens; map height = screen height; the
screen's own bitplanes are unused (invisible) while attached. Chip cost
`(mapPx/8 + 4) × height × depth` (86 KB for 640×256×4). See
`examples/17_scrolling.c`.

**AGA screens** (4x fetch): the fetch unit is a 64-pixel block from an
8-byte aligned address, so the coarse step is 64 px, BPLCON1 delays 0–63 px
(delay bits 5–4 in BPLCON1 bits 11–10 / 15–14) and DDFSTRT is pulled one
block (`$20`) early. The canvas keeps canvas x = world x + 16; its rows are
`(mapPx/8 + 10)` rounded up to 8 bytes. `TileScrollerCreate` picks that
layout when the sheet has more than 5 planes; Attach refuses a screen of
the other kind. `TileBand` (the split-screen scroller in `sade_tilemap.h`)
works the same on an AGA screen, except that its 8 plane-pointer switches
don't fit a horizontal blank: bitplane DMA is off for one line at each
switch, so the band's first row and the first status row under it show
colour 0.

---

## 17. sade_text.h — Bitmap Fonts

Cell-aligned text (AMOS `Text`/`Print`/`Pen`/`Paper`). A `Font` wraps a
resident IFF glyph sheet — 8-px-wide glyphs in ASCII order; the standard
sheet is `assets/fonts/font8.iff` (8×8, ASCII 32–95, uppercase; lowercase
input maps to uppercase). Cells draw opaque (ink on paper) with the CPU —
redraw a HUD every frame and it just overwrites.

| Function | Description |
|---|---|
| `FontCreate(sheet, 8, glyphH, firstChar)` | Wrap a glyph sheet (plane 0 = ink mask). Sheet must outlive the Font |
| `FontFree(f)` | Free wrapper only |
| `TextDraw(f, s, x, y, txt, ink, paper)` | Draw string; `x` rounds down to multiple of 8 |
| `TextDrawNum(f, s, x, y, val, digits, ink, paper)` | Zero-padded unsigned number (16-bit DIVU only) |

---

## 18. sade_collision.h — Collision

Bob-vs-Bob, two tiers (AMOS `Col()`); both Bobs' coordinates must be in
the same space. Tile collision is `TileFlagsAtPixel` (section 16).

| Function | Description |
|---|---|
| `RectOverlap(a, b)` | `SadeRect {x,y,w,h}` intersection |
| `BobOverlap(a, b)` | Bounding boxes of two Bobs |
| `BobCollide(a, b)` | Pixel-perfect: bbox gate, then mask AND (CPU word windows, early-out). Safe to call unconditionally |

---

## 19. sade_actor.h — Animation

Frame animation for Bobs (AMOS `Anim`). `AnimDef` = shared immutable data
(BobImage* frames + VBL-tick timing, loop or one-shot); `Anim` = per
instance playback state.

| Function | Description |
|---|---|
| `AnimStart(&a, &def)` | (Re)start at frame 0 |
| `AnimTick(&a)` | Advance one tick, returns current frame — assign to `bob->image` |
| `AnimImage(&a)` | Current frame without advancing |
| `AnimDone(&a)` | 1 when a one-shot finished (it holds its last frame) |

`delays` may point at a per-frame tick array; NULL means every frame lasts
`delay` ticks.

---

## 20. sade_keyboard.h — Keyboard

Raw keyboard over CIA-A's serial port, polled (AMOS `Key State`). The
keyboard waits for a handshake after every byte, so once-per-frame polling
is lossless. Codes are Amiga RAW key codes; common ones are defined
(`RAWKEY_ESC`, `RAWKEY_SPACE`, `RAWKEY_UP`, `RAWKEY_A`…).

| Function | Description |
|---|---|
| `KeyboardUpdate()` | Once per frame: read pending scancode, handshake, update tables |
| `KeyDown(raw)` | Level-triggered: non-zero while held |
| `KeyHit(raw)` | Edge-triggered: 1 exactly once per press |
| `KeyHitAny()` | "Press any key": raw code of a key pressed since the last call, or −1. Clears **every** latch; Shift/Ctrl/Alt/Amiga/Caps Lock (0x60–0x67) don't count. Test its result instead of calling `KeyHit` for the same keys afterwards |

Gotcha: reading the CIA ICR clears **all** its flags — route CIA-A access
through this header (ptplayer lives on CIA-B, no conflict).

---

## 21. sade_mem.h — Memory Pools

Level-lifetime allocation (AMOS `Reserve As Work`). A `MemPool` is one big
`AllocMem` block carved by a bump allocator: `PoolReset()` reclaims the
whole level in O(1), no fragmentation, no per-object frees.

`PoolActivate` routes the asset headers' allocations (sade_ilbm, sade_bob,
sade_bank, sade_tilemap — via the `SadeAlloc`/`SadeFree` hooks in sade.h)
into a pool, one active pool per memory type. Allocations made before
activation (screens, fonts, the player) are untouched, and `Free` calls on
pool memory become safe no-ops.

| Function | Description |
|---|---|
| `PoolCreate(size, memFlags)` | One `AllocMem`; `MEMF_CHIP` or `MEMF_PUBLIC` |
| `PoolActivate(pool)` / `PoolDeactivate(pool)` | Start/stop capturing asset allocations |
| `PoolAlloc(pool, size)` | Manual bump allocation (8-byte aligned, not cleared) |
| `PoolReset(pool)` | Free everything in the pool, O(1) |
| `PoolAvail(pool)` | Remaining bytes |
| `PoolFree(pool)` | Release the pool itself |

The multi-level pattern (see `examples/18_levels.c`): stage all file bytes
in `MEMF_PUBLIC` before `SadeInit` (sade_file.h), then per level:
`ModStop()` → `PoolReset()` both pools → decode staged bytes → `memcpy` the
level's module into a `PoolAlloc`'d chip buffer → `ModInit`.

**Rule:** after `PoolReset`, every pointer that pool handed out is invalid —
stop the consumers first (`ModStop`, drop Bob image pointers, re-render).
Escape hatch for games bigger than RAM: `SadeShutdown` → load → `SadeInit`
between levels (the takeover is symmetric).

---

## 22. sade_package.h — Asset Packages

All of a game's assets in one ZIP-compatible file with stored (uncompressed)
entries, loaded in the same load phase as `sade_file.h`, which it builds on.
Full format: [SADE_PACKAGE.md](SADE_PACKAGE.md).

```c
if (!FileInit()) return 0;
Package *pkg = PackageOpen("dave.ocs");          // NULL: missing / unsupported
void *tiles = AssetLoad(pkg, "assets/game/tiles.iff", MEMF_PUBLIC, &size);
PackageClose(pkg);                               // before FileShutdown
FileShutdown();
```

| Function | Description |
|---|---|
| `PackageOpen(path)` | Read and validate the ZIP directory; file stays open |
| `PackageLoad(p, name, memFlags, &size)` | One entry → fresh `AllocMem` buffer, CRC-32 checked. The `FileLoad` contract: free with `FileUnload` |
| `PackageLoadChip(p, name, &size)` | Shorthand for `MEMF_CHIP` |
| `PackageFind(p, name)` / `PackageSize(p, name)` | Entry lookup (case-insensitive) / size or −1 |
| `AssetLoad(p, name, memFlags, &size)` | Package if `p` is open, else `FileLoad(name)` |
| `PackageClose(p)` | Close + free the directory. NULL-safe; must run before `FileShutdown` |

Rules:

- Entries must be stored (method 0). A compressed, encrypted or ZIP64
  archive is refused by `PackageOpen`.
- Name entries by their path relative to the project root, the same string
  you would pass to `FileLoad`.
- `SADE_PACKAGE_NO_CRC` skips the per-load CRC check (~35 cycles/byte).
- Build packages with `tools/make_package.py` (aligned, deterministic, in
  load order). `make test` checks the parser against it and other ZIP
  writers.
