#pragma once

// SADE System — owns TakeSystem/FreeSystem, library lifecycle, VBR, interrupts.
// Include this instead of managing SysBase/GfxBase/custom manually.
// Call SadeInit() at the top of main, SadeShutdown() before exit.

#include "sade.h"
#include "sade_clock.h"

// -------------------------------------------------------------------------
// Private state
// -------------------------------------------------------------------------
static UWORD  _sade_SystemInts;
static UWORD  _sade_SystemDMA;
static UWORD  _sade_SystemADKCON;
__attribute__((weak)) APTR _sade_VBR = 0;     // weak: shared by all .c files
static APTR   _sade_SystemIrq;          // saved VBL vector (VBR+0x6c)
static struct View *_sade_ActiView;
__attribute__((weak)) UBYTE _sade_sysTaken = 0; // 1 while the OS is suspended
                                        // (sade_file.h refuses disk I/O then)

// -------------------------------------------------------------------------
// VBR
// -------------------------------------------------------------------------
static __attribute__((noinline)) APTR _SadeGetVBR(void) {
    UWORD getvbr[] = { 0x4e7a, 0x0801, 0x4e73 }; // MOVEC VBR,D0 / RTE
    if (SysBase->AttnFlags & AFF_68010) {
        // the routine was just written as data: on a 68020+ the instruction
        // cache may hold stale code for these stack addresses (V37+)
        if (SysBase->LibNode.lib_Version >= 37) CacheClearU();
        return (APTR)Supervisor((ULONG (*)())getvbr);
    }
    return 0;
}

// Read/write the level-3 autovector directly (VBR+0x6c)
static inline APTR  _SadeGetVec(void) { return *(volatile APTR*)(((UBYTE*)_sade_VBR)+0x6c); }
static inline void  _SadeSetVec(APTR h){ *(volatile APTR*)(((UBYTE*)_sade_VBR)+0x6c) = h; }

// Safe VBL stub: counts the frame (_sade_vblCount, the clock behind
// SadeClock) and clears the VBL request. Level 3 is shared with the copper
// and blitter interrupts; only a VERTB request is counted.
static __attribute__((used)) __attribute__((interrupt)) void _sade_SafeVblStub(void) {
    if (custom->intreqr & INTF_VERTB) _sade_vblCount++;
    custom->intreq = INTF_VERTB;
    custom->intreq = INTF_VERTB; // twice for A4000 bug
}

// -------------------------------------------------------------------------
// Public: install a custom VBL handler (replaces the safe stub)
// -------------------------------------------------------------------------
static inline void SadeSetInterruptHandler(APTR handler) {
    _SadeSetVec(handler);
}

// -------------------------------------------------------------------------
// TakeSystem
// -------------------------------------------------------------------------
static void TakeSystem(void) {
    _sade_ActiView = GfxBase->ActiView;
    OwnBlitter(); WaitBlit();

    // save hardware state before touching anything
    _sade_SystemADKCON = custom->adkconr;
    _sade_SystemInts   = custom->intenar;
    _sade_SystemDMA    = custom->dmaconr;

    // resolve VBR while OS is still running
    _sade_VBR       = _SadeGetVBR();
    _sade_SystemIrq = _SadeGetVec();

    // blank the display before installing our stub — WaitTOF uses Exec TOF signal
    LoadView(0); WaitTOF(); WaitTOF();

    // now install stub — after this, use SadeWaitVbl() not WaitTOF()
    _SadeSetVec((APTR)_sade_SafeVblStub);
    SadeWaitVbl(); SadeWaitVbl();

    // now take the hardware: kill all DMA and interrupts
    Disable();
    custom->intena = 0x7fff;
    custom->intreq = 0x7fff;
    custom->dmacon = 0x7fff;
    for (int i = 0; i < 32; i++) custom->color[i] = 0;
    // leave Disable() in effect — demo takes over from here
    _sade_sysTaken = 1;
}

// -------------------------------------------------------------------------
// FreeSystem
// -------------------------------------------------------------------------
static void FreeSystem(void) {
    SadeWaitVbl(); SadeWaitBlt();

    // kill all DMA and interrupts
    custom->intena = 0x7fff;
    custom->intreq = 0x7fff;
    custom->dmacon = 0x7fff;

    // point copper at OS lists before re-enabling DMA
    custom->cop1lc  = (ULONG)GfxBase->copinit;
    custom->cop2lc  = (ULONG)GfxBase->LOFlist;
    custom->copjmp1 = 0x7fff;

    // restore original VBL vector before Enable() so Exec TOF signal works again
    _SadeSetVec(_sade_SystemIrq);
    Enable();
    custom->intena = INTF_SETCLR | INTF_INTEN | INTF_VERTB;

    // restore view — WaitTOF now works because original vector is back
    LoadView(_sade_ActiView); WaitTOF(); WaitTOF();

    // restore hardware state
    custom->intreq = 0x7fff;
    custom->intena = _sade_SystemInts   | 0x8000;
    custom->dmacon = _sade_SystemDMA    | 0x8000;
    custom->adkcon = _sade_SystemADKCON | 0x8000;

    WaitBlit(); DisownBlitter();
    _sade_sysTaken = 0;
}

// -------------------------------------------------------------------------
// Display refresh from the beam: the last line of the frame (PAL 311/312,
// NTSC 261/262 — long and short frames alternate in interlace), the highest
// over three whole frames. Anything else falls back to the Agnus PAL flag.
// -------------------------------------------------------------------------
__attribute__((weak)) UWORD _sade_frameLines = 0;   // last line seen, for SysInfo/tests

static UWORD _SadeMeasureLines(void) {
    UWORD maxLine = 0, last = SadeBeamLine(), l;
    for (UBYTE wraps = 0; wraps < 4; ) {          // first wrap starts frame 1
        l = SadeBeamLine();
        if (l < last) { if (wraps++ == 0) maxLine = 0; }
        else if (l > maxLine) maxLine = l;
        last = l;
    }
    return maxLine;
}

static UBYTE _SadeHzFromLines(UWORD lines) {
    if (lines >= 300 && lines <= 320) return 50;
    if (lines >= 255 && lines <= 270) return 60;
    return _sade_isPAL ? 50 : 60;
}

// SadeFrameHz — the display's real refresh rate (50 or 60), for pacing game
// logic in wall-clock time. Valid after SadeInit.
static inline UBYTE SadeFrameHz(void) { return _sade_frameHz; }

// SadeFrameCount — VBLs since SadeInit, counted by the level-3 interrupt.
static inline ULONG SadeFrameCount(void) { return _sade_vblCount; }

// SadeClock on the real display (see sade_clock.h). After SadeInit.
static inline void  SadeClockStart(SadeClock *c, UBYTE ticksPerSec) {
    SadeClockStartAt(c, ticksPerSec, _sade_frameHz, _sade_vblCount);
}
static inline UBYTE SadeClockTicks(SadeClock *c) { return SadeClockTicksAt(c, _sade_vblCount); }
static inline void  SadeClockSync(SadeClock *c)  { SadeClockSyncAt(c, _sade_vblCount); }

// -------------------------------------------------------------------------
// SadeInit — open libraries, grab hardware, take OS.
// Returns 0 on failure (library open failed).
// -------------------------------------------------------------------------
static int SadeInit(void) {
    SysBase = *((struct ExecBase**)4UL);
    custom  = (struct Custom*)0xdff000;

    _sade_isPAL = SadeIsPAL();
    _sade_isAGA = SadeIsAGA();
    KPrintF("SadeInit: %s%s\n", (ULONG)(_sade_isPAL ? "PAL" : "NTSC"),
            (ULONG)(_sade_isAGA ? ", AGA" : ""));

    GfxBase = (struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library", 0);
    if (!GfxBase) return 0;

    TakeSystem();

    // ECS/AGA can be left in another beam mode by Workbench (NTSC/60 Hz
    // forced on a PAL machine, Multiscan or DblPAL at 31 kHz ...). Put the
    // beam back to the machine's own standard 15 kHz timing; OCS has no
    // BEAMCON0 and is always standard.
    UWORD id = (UWORD)((*(volatile UWORD*)0xDFF004 >> 8) & 0x7F);
    if (id & 0x20) custom->beamcon0 = _sade_isPAL ? 0x0020 : 0x0000;
    _sade_frameLines = _SadeMeasureLines();
    _sade_frameHz = _SadeHzFromLines(_sade_frameLines);
    KPrintF("SadeInit: Agnus $%02lx, %ld lines, %ld Hz display\n",
            (ULONG)id, (ULONG)_sade_frameLines + 1, (ULONG)_sade_frameHz);

    // VBL interrupt on: the stub counts frames for SadeClock
    _sade_vblCount = 0;
    custom->intreq = INTF_VERTB;
    custom->intena = INTF_SETCLR | INTF_INTEN | INTF_VERTB;
    return 1;
}

// -------------------------------------------------------------------------
// SadeShutdown — restore OS, close libraries.
// Safe to call even if SadeInit failed (checks GfxBase).
// -------------------------------------------------------------------------
static void SadeShutdown(void) {
    FreeSystem();
    if (GfxBase) CloseLibrary((struct Library*)GfxBase);
}
