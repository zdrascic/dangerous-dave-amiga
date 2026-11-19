// =========================================================================
// SysInfo: CPU, memory and chipset, shown on the loading screen.
// =========================================================================
#include "sysinfo.h"
#include "../libs/sade/sade_system.h"
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>

// AGA: SadeIsAGA. Otherwise Agnus ID (VPOSR bits 14-8): 0x00/0x10 OCS,
// 0x20/0x30 ECS; Denise ID ($DFF07C): ECS Denise reads 0xFC; an OCS
// Denise has no such register (bus noise).
static UBYTE DetectChipset(void) {
    if (SadeIsAGA()) return 3;
    UBYTE agnus = (UBYTE)((*(volatile UWORD*)0xDFF004 >> 8) & 0x7f);
    UBYTE denise = (UBYTE)(*(volatile UWORD*)0xDFF07C & 0xff);
    UBYTE stable = 1;
    for (UBYTE i = 0; i < 8; i++)
        if ((UBYTE)(*(volatile UWORD*)0xDFF07C & 0xff) != denise) stable = 0;
    UBYTE ecsAgnus = agnus == 0x20 || agnus == 0x30;
    if (stable && denise == 0xfc) return 2;                          // ECS Denise
    return ecsAgnus ? 1 : 0;   // ECS Agnus + OCS Denise: a 1 MB-chip A500
}

// RTG: a graphics card's driver system loads its library at boot, so it is
// in Exec's library list (FindName: no disk access, nothing opened).
// Picasso96 first: it also brings a cybergraphics.library emulation.
// rtg.library is P96's core; Picasso96API.library loads only on demand.
// This finds the software, not the board: the game itself always shows on
// the native chipset, on the monitor at the native video port.
static UBYTE DetectRtg(void) {
    struct List *libs = &SysBase->LibList;
    UBYTE rtg = 0;
    Forbid();
    if (FindName(libs, (CONST_STRPTR)"rtg.library") ||
        FindName(libs, (CONST_STRPTR)"Picasso96API.library"))   rtg = 1;
    else if (FindName(libs, (CONST_STRPTR)"cybergraphics.library")) rtg = 2;
    Permit();
    return rtg;
}

static ULONG DetectCpu(void) {
    UWORD f = SysBase->AttnFlags;
    if (f & AFF_68060) return 68060;
    if (f & AFF_68040) return 68040;
    if (f & AFF_68030) return 68030;
    if (f & AFF_68020) return 68020;
    if (f & AFF_68010) return 68010;
    return 68000;
}

void SysInfoDetect(SysInfo *si) {
    si->cpu = DetectCpu();
    si->chipKB = si->fastKB = si->slowKB = 0;
    Forbid();
    for (struct Node *n = SysBase->MemList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct MemHeader *mh = (struct MemHeader*)n;
        ULONG lower = (ULONG)mh->mh_Lower, upper = (ULONG)mh->mh_Upper;
        // the header and first bytes of a block sit below mh_Lower: round
        // the size up to whole 64 KB, as RAM always comes in such sizes
        ULONG kb = ((upper - lower + 0xFFFF) & ~0xFFFFUL) >> 10;
        if (mh->mh_Attributes & MEMF_CHIP)                 si->chipKB += kb;
        else if (lower >= 0xC00000 && upper <= 0xDC0000)   si->slowKB += kb;
        else                                               si->fastKB += kb;
    }
    Permit();
    si->chipset = DetectChipset();
    si->agnusId = (UBYTE)((*(volatile UWORD*)0xDFF004 >> 8) & 0x7f);
    si->osHz = SysBase->VBlankFrequency;
    si->rtg = DetectRtg();
    si->pal = SadeIsPAL();                  // until SysInfoVideo confirms
    si->hz = si->pal ? 50 : 60;
    si->lines = si->pal ? 313 : 263;
}

void SysInfoVideo(SysInfo *si) {
    si->pal = _sade_isPAL;
    si->hz = SadeFrameHz();
    si->lines = (UWORD)(_sade_frameLines + 1);
}

// ---- tiny string helpers (no sprintf in a bare-metal build) ----
static char *Put(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

static char *PutNum(char *d, ULONG v) {
    char tmp[10]; UBYTE n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) *d++ = tmp[--n];
    *d = 0;
    return d;
}

static char *PutSize(char *d, ULONG kb) {
    if (kb >= 1024 && (kb & 1023) == 0) return Put(PutNum(d, kb >> 10), " MB");
    if (kb >= 1024 && (kb & 511) == 0) {               // 1.5 MB etc.
        d = PutNum(d, kb >> 10);
        return Put(d, ".5 MB");
    }
    return Put(PutNum(d, kb), " KB");
}

static char *PutSizeShort(char *d, ULONG kb) {
    if (kb >= 1024 && (kb & 1023) == 0) return Put(PutNum(d, kb >> 10), "MB");
    if (kb >= 1024 && (kb & 511) == 0) return Put(PutNum(d, kb >> 10), ".5MB");
    return Put(PutNum(d, kb), "KB");
}

void SysInfoLine(const SysInfo *si, char line[48]) {
    static const char *const kChip[4] = { "OCS", "ECS AGNUS", "ECS", "AGA" };
    char *d = Put(PutNum(line, si->cpu), ", ");
    d = Put(PutSizeShort(d, si->chipKB), " CHIP, ");
    if (si->fastKB)      d = Put(PutSizeShort(d, si->fastKB), " FAST, ");
    else if (si->slowKB) d = Put(PutSizeShort(d, si->slowKB), " SLOW, ");
    Put(d, kChip[si->chipset]);
}

void SysInfoVideoLine(const SysInfo *si, char line[48]) {
    char *d = Put(line, si->pal ? "PAL VIDEO, " : "NTSC VIDEO, ");
    d = Put(PutNum(d, si->hz), " HZ");
    if ((si->osHz == 50 || si->osHz == 60) && si->osHz != si->hz)
        d = Put(Put(PutNum(Put(d, " (OS "), si->osHz), " HZ"), ")");
    if (si->rtg) Put(d, si->rtg == 1 ? ", RTG P96" : ", RTG CGX");
}

UBYTE SysInfoLines(const SysInfo *si, char lines[4][24]) {
    static const char *const kChip[4] = {
        "OCS graphics", "OCS graphics, ECS Agnus", "ECS graphics", "AGA graphics" };
    UBYTE n = 0;
    Put(PutNum(lines[n++], si->cpu), " system");
    Put(PutSize(lines[n++], si->chipKB), " chip RAM");
    if (si->fastKB) Put(PutSize(lines[n++], si->fastKB), " fast RAM");
    else if (si->slowKB) Put(PutSize(lines[n++], si->slowKB), " slow RAM");
    Put(lines[n++], kChip[si->chipset]);
    return n;
}
