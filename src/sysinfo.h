#pragma once
#include "../libs/sade/sade.h"

// What the machine is, for the loading screen.
typedef struct {
    ULONG cpu;               // 68000, 68010, ..., 68060
    ULONG chipKB;            // chip RAM
    ULONG fastKB;            // real fast RAM (not on the chip bus)
    ULONG slowKB;            // "slow"/trapdoor RAM at $C00000 (A500 0.5 MB expansion)
    UBYTE chipset;           // 0 OCS, 1 OCS + ECS Agnus (late A500), 2 ECS, 3 AGA
    UBYTE agnusId;           // VPOSR bits 14-8 (bit 4 set = NTSC part)
    UBYTE osHz;              // Exec's VBlankFrequency (what the OS booted with)
    UBYTE rtg;               // RTG system running: 0 none, 1 Picasso96, 2 CyberGraphX
    // filled by SysInfoVideo, after SadeInit:
    UBYTE pal;               // 1 PAL machine (Agnus), 0 NTSC
    UBYTE hz;                // measured display refresh the game runs at: 50/60
    UWORD lines;             // measured lines per frame (313 PAL, 263 NTSC)
} SysInfo;

// Call while the OS is alive (walks Exec's memory list under Forbid).
void SysInfoDetect(SysInfo *si);

// Call after SadeInit: the video standard and the refresh SadeInit measured
// with the beam (back on the machine's own 15 kHz timing by then).
void SysInfoVideo(SysInfo *si);

// The video standard on one line, e.g. "PAL VIDEO, 50 HZ", or
// "NTSC VIDEO, 60 HZ". Notes when the OS had been running another mode,
// e.g. "PAL VIDEO, 50 HZ (OS 60 HZ)", and an RTG system, e.g.
// "PAL VIDEO, 50 HZ, RTG P96" (at most 37 characters).
void SysInfoVideoLine(const SysInfo *si, char line[48]);

// Up to 4 lines of text, e.g. "68000 system", "512 KB chip RAM",
// "512 KB slow RAM", "OCS graphics". Returns the number of lines.
UBYTE SysInfoLines(const SysInfo *si, char lines[4][24]);

// All of it on one line of at most 40 characters (a 320-pixel row of 8-pixel
// glyphs), e.g. "68000, 1MB CHIP, 512KB SLOW, ECS AGNUS".
void SysInfoLine(const SysInfo *si, char line[48]);
