// =========================================================================
// Dangerous Dave (Amiga port) — entry point.
// Loads all assets, then loops: title screen (or its demos) -> levels until
// game over.
// =========================================================================
#include "../libs/sade/sade_system.h"
#include "../libs/sade/sade_input.h"
#include "../libs/sade/sade_graphics.h"
#include "../libs/sade/sade_file.h"
#include "../libs/sade/sade_package.h"
#include "../libs/sade/sade_bank.h"
#include "title.h"
#include "scores.h"
#include "end.h"
#include "level.h"
#include "sound.h"
#include "sysinfo.h"
#include "../libs/sade/sade_ilbm.h"
#include "../libs/sade/sade_text.h"
#include <dos/dosextens.h>
#include <workbench/startup.h>

#define SKIP_TITLE  0   // 1 = jump straight into the game (testing)
#define START_LEVEL 0   // 0-based, for testing later levels
#define SCORES_FILE "DSCORES.DAV"   // the high scores, saved on exit (the
                                    // original's name and format); the
                                    // package's copy is the first table
#define PACKAGE_OCS "dave.ocs"  // every asset below, packed by `make`
#define PACKAGE_AGA "dave.aga"  // (tools/make_package.py): the AGA one holds
                                // 8-plane graphics in the original's VGA
                                // colours, the OCS one 5-plane graphics.
                                // Without either the loose files under
                                // assets/ are loaded (the OCS set)

struct ExecBase        *SysBase;
volatile struct Custom *custom;
struct GfxBase         *GfxBase;
struct DosLibrary      *DOSBase;

// ---- boot loading bar: white frame, filled as LevelInit progresses ----
#define BAR_W 100
#define BAR_H   8
#define BAR_X ((320 - BAR_W) / 2)
// middle of the visible display, not of the 200-line screen at its top:
// 256 lines on PAL, the screen itself (200) on NTSC
#define BAR_Y ((_sade_isPAL ? 128 : 100) - BAR_H / 2)
#define LOAD_STEPS LEVEL_INIT_STEPS
#define LOAD_MIN_SECS 2   // the loading screen stays at least this long, so
                          // the machine line can be read on a fast machine
static Screen *g_loadScr;
static UWORD   g_loadDone;

static void LoadProgress(void) {
    if (g_loadDone < LOAD_STEPS) g_loadDone++;
    UWORD fill = divuw(muluw(g_loadDone, BAR_W - 4), LOAD_STEPS);
    if (fill)
        GfxFillBox(g_loadScr, BAR_X + 2, BAR_Y + 2, (short)(BAR_X + 1 + fill), BAR_Y + BAR_H - 3, 1);
}

// The machine's details along the bottom of the screen, centred (font8,
// capitals only): video standard and refresh, then CPU, memory and chipset.
#define INFO_Y (200 - 8 - 2)
#define VIDEO_Y (INFO_Y - 10)
static void DrawCentred(Font *font, Screen *scr, short y, const char *s) {
    UBYTE len = 0;
    while (s[len]) len++;
    TextDraw(font, scr, (short)((320 - len * 8) / 2), y, s, 1, 0);
}

static void LoadingInfo(Screen *scr, const UBYTE *fontIff, ULONG fontSize, const SysInfo *si) {
    IlbmImage *sheet = fontIff ? IlbmLoad(fontIff, fontSize) : NULL;
    Font *font = sheet ? FontCreate(sheet, 8, 8, ' ') : NULL;
    if (font) {
        char line[48];
        SysInfoVideoLine(si, line);
        DrawCentred(font, scr, VIDEO_Y, line);
        SysInfoLine(si, line);
        DrawCentred(font, scr, INFO_Y, line);
        FontFree(font);
    }
    if (sheet) IlbmFree(sheet);
}

static void LoadingBarShow(Screen *scr) {
    g_loadScr = scr;
    g_loadDone = 0;
    for (UWORD i = 0; i < scr->numColors; i++) ScreenSetColor(scr, i, 0x000);
    ScreenSetColor(scr, 1, 0xfff);            // white
    GfxClear(scr);                            // the displayed buffer (no flip yet)
    GfxDrawBox(scr, BAR_X, BAR_Y, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1, 1);
}

// Plays from START_LEVEL until game over (LevelRun shows GAME OVER or the
// high scores), the last door (the ending), or a quit.
// Returns 0 if the player quit.
static int GameRun(Screen *scr, const LevelAssets *la) {
    GameState g = { 0, 3, START_LEVEL, 0 };
    for (;;) {
        int r = LevelRun(scr, &g, la);          // the level, or its warp zone
        if (r == LEVEL_QUIT) return 0;
        if (r == LEVEL_GAME_OVER) return 1;
        if (r == LEVEL_WARP_ZONE) {             // g.zone set: tube, then the zone
            if (WarpTubeRun(scr, &g, la) == LEVEL_QUIT) return 0;
            continue;
        }
        UBYTE fromZone = g.zone != 0;
        g.zone = 0;
        // the corridor: no message back from a zone, as the original
        if (WarpRun(scr, &g, la, !fromZone) == LEVEL_QUIT) return 0;
        if (!fromZone && g.level + 1 == NUM_LEVELS) {   // the last door: won
            EndingRun(scr, &g);
            return 1;
        }
        if (!fromZone) g.level++;               // a zone's door: back to its level
    }
}

// Back in the OS: write the high scores if a game changed them.
static void SaveScores(void) {
    if (!ScoresChanged() || !FileInit()) return;
    UBYTE buf[SCORE_FILE_SIZE];
    ScoresEncode(buf);
    FileSave(SCORES_FILE, buf, SCORE_FILE_SIZE);
    FileShutdown();
}

// A graphics file of the chosen set: dir + "ocs/" or "aga/" + name.
static void *GfxLoad(Package *pkg, const char *dir, UBYTE aga, const char *name, ULONG *size) {
    char path[40];
    char *d = path;
    for (const char *s = dir; *s; ) *d++ = *s++;
    for (const char *s = aga ? "aga/" : "ocs/"; *s; ) *d++ = *s++;
    for (const char *s = name; *s; ) *d++ = *s++;
    *d = 0;
    return AssetLoad(pkg, path, MEMF_PUBLIC, size);
}

static int DaveMain(void) {
    // ==== load phase: disk access only while the OS is alive ============
    // Loaded in the package's order, so a floppy reads it front to back.
    if (!FileInit()) return 0;
    UBYTE aga = 0;                              // AGA machine and dave.aga there
    Package *pkg = NULL;
    if (SadeIsAGA()) aga = (pkg = PackageOpen(PACKAGE_AGA)) != NULL;
    if (!pkg) pkg = PackageOpen(PACKAGE_OCS);   // NULL: fall back to loose files
    ULONG logoSize, fontSize;
    UBYTE *logoData = (UBYTE*)GfxLoad(pkg, "assets/title/", aga, "dave_logo.iff", &logoSize);
    UBYTE *fontData = (UBYTE*)AssetLoad(pkg, "assets/fonts/font8.iff", MEMF_PUBLIC, &fontSize);
    LevelAssets la;
    la.tilesIff = (UBYTE*)GfxLoad(pkg, "assets/game/", aga, "tiles.iff", &la.tilesSize);
    la.daveIff  = (UBYTE*)GfxLoad(pkg, "assets/game/", aga, "dave.iff", &la.daveSize);
    la.hudIff   = (UBYTE*)GfxLoad(pkg, "assets/game/", aga, "hud.iff", &la.hudSize);
    la.monIff   = (UBYTE*)GfxLoad(pkg, "assets/game/", aga, "monsters.iff", &la.monSize);
    la.fontIff  = fontData;
    la.fontSize = fontSize;
    ULONG spkSize = 0;                          // optional: silent without it
    UBYTE *spkData = (UBYTE*)AssetLoad(pkg, "assets/game/sounds.spk", MEMF_PUBLIC, &spkSize);
    la.demoSize = 0;                            // optional: no attract mode without it
    la.demoData = (UBYTE*)AssetLoad(pkg, "assets/game/demos.bin", MEMF_PUBLIC, &la.demoSize);
    ULONG hsSize = 0;                           // the saved table, else the package's
    UBYTE *hsData = (UBYTE*)FileLoad(SCORES_FILE, MEMF_PUBLIC, &hsSize);
    if (hsData) {
        ScoresLoad(hsData, hsSize);
        FileUnload(hsData, hsSize);
    } else {
        hsData = (UBYTE*)AssetLoad(pkg, "assets/game/dscores.dav", MEMF_PUBLIC, &hsSize);
        ScoresLoad(hsData, hsSize);             // NULL: the original's empty table
    }
    int ok = logoData && fontData && la.tilesIff && la.daveIff && la.hudIff && la.monIff;
    char path[] = "assets/game/levels/level00.map";
    for (UBYTE i = 0; i < NUM_LEVELS; i++) {
        path[24] = (char)('0' + (i + 1) / 10);
        path[25] = (char)('0' + (i + 1) % 10);
        la.mapData[i] = (UBYTE*)AssetLoad(pkg, path, MEMF_PUBLIC, &la.mapSize[i]);
        if (!la.mapData[i]) ok = 0;
    }
    PackageClose(pkg);                          // an OS call: before FileShutdown
    FileShutdown();
    if (!ok) return 0;
    SysInfo sys;
    SysInfoDetect(&sys);                        // while the OS is still running

    if (!SadeInit()) return 0;
    SysInfoVideo(&sys);                         // PAL/NTSC + refresh SadeInit measured

    Screen *scr = ScreenOpen(320, 200, aga ? 8 : 5, SADE_LORES, SADE_DOUBLE);
    if (!scr) { SadeShutdown(); return 0; }
    LoadingBarShow(scr);
    ULONG loadStart = SadeFrameCount();         // real VBLs: 2 s on 50 and 60 Hz
    LoadingInfo(scr, fontData, fontSize, &sys);
    if (!LevelInit(&la, LoadProgress)) { ScreenClose(scr); SadeShutdown(); return 0; }
    SoundInit(spkData, spkSize);
    // (not SadeClock: it catches up at most 4 frames per call)
    while (SadeFrameCount() - loadStart < muluw(SadeFrameHz(), LOAD_MIN_SECS))
        SadeWaitVbl();

    if (SKIP_TITLE) GameRun(scr, &la);
    else {
        UBYTE demo = (UBYTE)SadeFrameCount();   // first demo: any, like the original's rand()
        for (;;) {
            int t = TitleScreenShow(scr, logoData, logoSize, DemoCount(&la));
            if (t == TITLE_QUIT) break;
            if (t == TITLE_DEMO) {
                demo = (UBYTE)(demo + 1);
                while (demo >= DemoCount(&la)) demo = (UBYTE)(demo - DemoCount(&la));
                DemoRun(scr, &la, demo);
                continue;
            }
            if (!GameRun(scr, &la)) break;
        }
    }

    SoundShutdown();
    LevelShutdown();
    ScreenClose(scr);
    SadeShutdown();
    SaveScores();
    return 0;
}

// Started from Workbench (the icon): Workbench sends a startup message
// that must be taken before anything else and replied last, under
// Forbid(), or Workbench never unloads the program. The current directory
// is then the volume's root, not the icon's drawer, so change to the
// program's own directory (the message's first argument) for dave.ocs/.aga
// and DSCORES.DAV, and back before replying.
int main(void) {
    SysBase = *((struct ExecBase**)4UL);
    struct Process *me = (struct Process*)FindTask(NULL);
    struct WBStartup *wbMsg = NULL;
    BPTR oldDir = 0;
    if (!me->pr_CLI) {
        WaitPort(&me->pr_MsgPort);
        wbMsg = (struct WBStartup*)GetMsg(&me->pr_MsgPort);
        if (FileInit()) {
            oldDir = CurrentDir(wbMsg->sm_ArgList[0].wa_Lock);
            FileShutdown();
        }
    }
    int ret = DaveMain();
    if (wbMsg) {
        if (FileInit()) {
            CurrentDir(oldDir);
            FileShutdown();
        }
        Forbid();
        ReplyMsg(&wbMsg->sm_Message);
    }
    return ret;
}
