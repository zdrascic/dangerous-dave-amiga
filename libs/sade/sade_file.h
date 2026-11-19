#pragma once

#include "sade.h"
#include "sade_system.h"
#include <dos/dos.h>
#include <proto/dos.h>

// SADE File — load assets from disk instead of #embed-ing them.
//
// Bare-metal rule: dos.library is part of the OS, and after SadeInit() the
// OS is suspended — so ALL disk access happens in a load phase BEFORE
// SadeInit, exactly like classic games did: load everything, then take the
// machine. After TakeSystem every function here fails safely (returns
// NULL/-1) instead of hanging on a dead filesystem.
//
// Programs using this header define a fourth global next to SysBase & co:
//
//   struct DosLibrary *DOSBase;
//
//   int main(void) {
//       if (!FileInit()) return 0;              // OS still alive here
//       ULONG picSize, modSize;
//       void *pic = FileLoad("gfx/title.iff", MEMF_PUBLIC, &picSize);
//       void *mod = FileLoadChip("music/song.mod", &modSize);
//       FileShutdown();                         // close dos.library
//       if (!pic || !mod) return 0;
//       if (!SadeInit()) return 0;              // NOW take over
//       ...
//   }
//
// Memory placement:
//   - Data the custom chips read via DMA (modules, samples, raw sprite
//     data) must be in chip RAM — use FileLoadChip.
//   - File bytes that only the CPU parses (IFF pictures: IlbmLoad copies
//     the planes to chip RAM itself) can go in any RAM — use MEMF_PUBLIC,
//     and FileUnload the buffer right after parsing.
//
// Paths are resolved from the program's start directory. The amiga-debug
// launcher mounts the project root as dh1: and CDs into it before running
// the exe, so relative paths like "assets/audio/song.mod" work both in the
// emulator and from a real shell.
//
// Side effect worth knowing: FileInit sets SysBase, so KPrintF is safe from
// that point on (normally it only becomes safe after SadeInit).

// -------------------------------------------------------------------------
// FileInit / FileShutdown — dos.library lifecycle.
// Call FileInit before SadeInit; call FileShutdown before SadeInit too
// (CloseLibrary is an OS call — it must not run after the takeover).
// -------------------------------------------------------------------------
static int FileInit(void) {
    if (_sade_sysTaken) return 0;               // too late — the OS is gone
    if (!SysBase) SysBase = *((struct ExecBase**)4UL);
    if (!DOSBase)
        DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    return DOSBase != NULL;
}

static void FileShutdown(void) {
    if (DOSBase && !_sade_sysTaken)
        CloseLibrary((struct Library*)DOSBase);
    DOSBase = NULL;
}

// -------------------------------------------------------------------------
// FileSize — byte size of a file, or -1 if it can't be opened.
// -------------------------------------------------------------------------
static LONG FileSize(const char *path) {
    if (!DOSBase || _sade_sysTaken) return -1;
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) return -1;
    Seek(fh, 0, OFFSET_END);
    LONG size = Seek(fh, 0, OFFSET_BEGINNING);  // returns the end position
    Close(fh);
    return size;
}

// -------------------------------------------------------------------------
// FileLoad — read a whole file into a freshly AllocMem'd buffer.
//   memFlags: MEMF_PUBLIC (any RAM) or MEMF_CHIP (custom-chip visible).
//   sizeOut:  receives the byte count (may be NULL).
// Returns NULL on any failure (missing file, out of memory, short read).
// Free the buffer with FileUnload(data, size).
// -------------------------------------------------------------------------
static void *FileLoad(const char *path, ULONG memFlags, ULONG *sizeOut) {
    if (sizeOut) *sizeOut = 0;
    if (!DOSBase || _sade_sysTaken) return NULL;

    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        KPrintF("FileLoad: can't open '%s'\n", (ULONG)path);
        return NULL;
    }

    Seek(fh, 0, OFFSET_END);
    LONG size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size <= 0) { Close(fh); return NULL; }

    void *buf = AllocMem((ULONG)size, memFlags);
    if (!buf) {
        KPrintF("FileLoad: no memory for '%s' (%ld bytes)\n",
                (ULONG)path, (ULONG)size);
        Close(fh);
        return NULL;
    }

    LONG got = Read(fh, buf, size);
    Close(fh);
    if (got != size) {
        KPrintF("FileLoad: short read on '%s'\n", (ULONG)path);
        FreeMem(buf, (ULONG)size);
        return NULL;
    }

    if (sizeOut) *sizeOut = (ULONG)size;
    KPrintF("FileLoad: '%s' -> %ld bytes%s\n", (ULONG)path, (ULONG)size,
            (ULONG)((memFlags & MEMF_CHIP) ? " (chip)" : ""));
    return buf;
}

// -------------------------------------------------------------------------
// FileSave — write size bytes to path (replacing it). OS alive only: before
// SadeInit, or after SadeShutdown with a fresh FileInit (e.g. a high score
// table saved on exit). Returns 1 on success.
// -------------------------------------------------------------------------
static int FileSave(const char *path, const void *data, ULONG size) {
    if (!DOSBase || _sade_sysTaken) return 0;
    BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        KPrintF("FileSave: can't create '%s'\n", (ULONG)path);
        return 0;
    }
    LONG put = Write(fh, (APTR)data, (LONG)size);
    Close(fh);
    if (put != (LONG)size) {
        KPrintF("FileSave: short write on '%s'\n", (ULONG)path);
        return 0;
    }
    return 1;
}

// Convenience: load straight into chip RAM (modules, samples, sprite data).
static void *FileLoadChip(const char *path, ULONG *sizeOut) {
    return FileLoad(path, MEMF_CHIP, sizeOut);
}

// -------------------------------------------------------------------------
// FileUnload — free a FileLoad buffer. Safe after takeover (FreeMem is one
// of the two Exec calls that keep working) and NULL-safe.
// -------------------------------------------------------------------------
static void FileUnload(void *data, ULONG size) {
    if (data && size) FreeMem(data, size);
}
