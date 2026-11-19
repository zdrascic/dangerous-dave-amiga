#pragma once

// SADE Package — load assets out of one ZIP-compatible package file (.ocs)
// instead of one file each. Format: docs/SADE_PACKAGE.md; built by
// tools/make_package.py (`make` packs dave.ocs).
//
// A package is a ZIP archive whose entries are all STORED (no compression),
// so an entry is its file's bytes at a known offset: no decompressor on the
// Amiga, and any unzip tool can list, test and extract it. Opening reads
// only the central directory; each PackageLoad then seeks to one entry and
// reads it, the same way FileLoad reads a file.
//
// Same rules as sade_file.h (which this builds on): disk access only in the
// load phase, before SadeInit; close the package before FileShutdown.
//
//   if (!FileInit()) return 0;
//   Package *pkg = PackageOpen("dave.ocs");          // NULL: no package
//   ULONG size;
//   void *tiles = AssetLoad(pkg, "assets/game/tiles.iff", MEMF_PUBLIC, &size);
//   ...
//   PackageClose(pkg);
//   FileShutdown();
//
// Buffers come from AllocMem, exactly like FileLoad's: free them with
// FileUnload(data, size). Entry names are the paths the package was built
// from ('/' separated), matched without regard to case like AmigaDOS.
//
// Every load checks the entry's CRC-32 against the directory (about 35
// cycles per byte on a 68000: ~0.4 s for Dave's 77 KB at 7 MHz). Define
// SADE_PACKAGE_NO_CRC before including to skip it.
//
// The parsing helpers (_Pkg*) touch no hardware and no DOS; with
// SADE_PACKAGE_HOST defined only they are compiled, for the host unit test
// (tests/package_test.c).

#ifndef SADE_PACKAGE_HOST
#include "sade_file.h"
#endif

// -------------------------------------------------------------------------
// ZIP structures (all little-endian)
// -------------------------------------------------------------------------
#define PKG_SIG_LOCAL    0x04034B50UL
#define PKG_SIG_CENTRAL  0x02014B50UL
#define PKG_SIG_EOCD     0x06054B50UL
#define PKG_LOCAL_SIZE   30           // local file header, before name + extra
#define PKG_CENTRAL_SIZE 46           // central directory header, before name...
#define PKG_EOCD_SIZE    22           // end of central directory, before comment
#define PKG_MAX_COMMENT  65535

// One directory entry. `name` points into the directory buffer and is not
// 0-terminated.
typedef struct {
    const char *name;
    ULONG offset;                     // of the entry's local header
    ULONG size;                       // stored = compressed = uncompressed
    ULONG crc;
    UWORD nameLen;
    UWORD _pad;
} PackageEntry;

static inline UWORD _PkgLe16(const UBYTE *p) { return (UWORD)(p[0] | (p[1] << 8)); }
static inline ULONG _PkgLe32(const UBYTE *p) {
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

// The end-of-central-directory record in the last `len` bytes of the file:
// its index in `tail`, or -1. Searched from the end, so a comment that
// happens to contain the signature does not fool it.
static LONG _PkgFindEocd(const UBYTE *tail, ULONG len) {
    if (len < PKG_EOCD_SIZE) return -1;
    for (LONG i = (LONG)(len - PKG_EOCD_SIZE); i >= 0; i--) {
        if (_PkgLe32(tail + i) != PKG_SIG_EOCD) continue;
        if ((ULONG)i + PKG_EOCD_SIZE + _PkgLe16(tail + i + 20) <= len) return i;
    }
    return -1;
}

// Fields of the EOCD record. 0 for what SADE does not read: multi-disk
// archives and ZIP64 (whose EOCD fields are all 0xFF).
static int _PkgParseEocd(const UBYTE *e, UWORD *count, ULONG *dirSize, ULONG *dirOffset) {
    if (_PkgLe16(e + 4) != 0 || _PkgLe16(e + 6) != 0) return 0;          // disk numbers
    if (_PkgLe16(e + 8) != _PkgLe16(e + 10)) return 0;                    // entries on this disk
    *count     = _PkgLe16(e + 10);
    *dirSize   = _PkgLe32(e + 12);
    *dirOffset = _PkgLe32(e + 16);
    if (*count == 0xFFFF || *dirSize == 0xFFFFFFFFUL || *dirOffset == 0xFFFFFFFFUL) return 0;
    return 1;
}

// Central directory -> `count` entries. 0 on a malformed directory or an
// entry SADE cannot load in place: compressed, encrypted, ZIP64-sized, or
// on another disk.
static int _PkgParseDir(const UBYTE *dir, ULONG dirSize, UWORD count, PackageEntry *out) {
    ULONG at = 0;
    for (UWORD i = 0; i < count; i++) {
        if (at + PKG_CENTRAL_SIZE > dirSize) return 0;
        const UBYTE *h = dir + at;
        if (_PkgLe32(h) != PKG_SIG_CENTRAL) return 0;
        UWORD flags  = _PkgLe16(h + 8);
        UWORD method = _PkgLe16(h + 10);
        ULONG csize  = _PkgLe32(h + 20), usize = _PkgLe32(h + 24);
        UWORD nlen   = _PkgLe16(h + 28), xlen = _PkgLe16(h + 30), clen = _PkgLe16(h + 32);
        if (method != 0 || (flags & 1) || csize != usize) return 0;      // stored only
        if (usize == 0xFFFFFFFFUL || _PkgLe32(h + 42) == 0xFFFFFFFFUL) return 0;
        if (_PkgLe16(h + 34) != 0) return 0;                               // disk start
        ULONG next = at + PKG_CENTRAL_SIZE + nlen + xlen + clen;
        if (next > dirSize) return 0;
        out[i].name    = (const char*)(h + PKG_CENTRAL_SIZE);
        out[i].nameLen = nlen;
        out[i].offset  = _PkgLe32(h + 42);
        out[i].size    = usize;
        out[i].crc     = _PkgLe32(h + 16);
        out[i]._pad    = 0;
        at = next;
    }
    return 1;
}

// From the 30-byte local header at e->offset: how far past it the data
// starts (its own name and extra field can differ from the directory's).
// 0 if it is not a stored entry's local header.
static ULONG _PkgDataStart(const UBYTE *lh) {
    if (_PkgLe32(lh) != PKG_SIG_LOCAL || _PkgLe16(lh + 8) != 0) return 0;
    return PKG_LOCAL_SIZE + (ULONG)_PkgLe16(lh + 26) + _PkgLe16(lh + 28);
}

static int _PkgNameEq(const PackageEntry *e, const char *name) {
    for (UWORD i = 0; i < e->nameLen; i++) {
        UBYTE a = (UBYTE)e->name[i], b = (UBYTE)name[i];
        if (!b) return 0;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return name[e->nameLen] == 0;
}

static const PackageEntry *_PkgFind(const PackageEntry *entries, UWORD count, const char *name) {
    for (UWORD i = 0; i < count; i++)
        if (_PkgNameEq(&entries[i], name)) return &entries[i];
    return 0;
}

// CRC-32 (ZIP / IEEE 802.3, reflected 0xEDB88320). Start with crc = 0.
static ULONG _pkg_crcTable[256];
static ULONG _PkgCrc32(ULONG crc, const UBYTE *p, ULONG n) {
    if (!_pkg_crcTable[1]) {
        for (UWORD i = 0; i < 256; i++) {
            ULONG c = i;
            for (UBYTE k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
            _pkg_crcTable[i] = c;
        }
    }
    crc = ~crc;
    while (n--) crc = _pkg_crcTable[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

#ifndef SADE_PACKAGE_HOST
// -------------------------------------------------------------------------
// Package — an open package file and its directory.
// -------------------------------------------------------------------------
typedef struct {
    BPTR          fh;
    UBYTE        *dir;                // central directory bytes (names live here)
    ULONG         dirSize;
    PackageEntry *entries;
    UWORD         count;
} Package;

static void PackageClose(Package *p) {
    if (!p) return;
    if (p->fh && DOSBase && !_sade_sysTaken) Close(p->fh);
    if (p->entries) FreeMem(p->entries, muluw(p->count, sizeof(PackageEntry)));
    if (p->dir) FreeMem(p->dir, p->dirSize);
    FreeMem(p, sizeof(Package));
}

// Reads the last `want` bytes (at most the file) and looks for the EOCD.
static int _PkgReadEocd(BPTR fh, LONG fileSize, ULONG want, UBYTE *e) {
    ULONG len = (ULONG)fileSize < want ? (ULONG)fileSize : want;
    UBYTE *tail = (UBYTE*)AllocMem(len, MEMF_PUBLIC);
    if (!tail) return 0;
    int ok = 0;
    Seek(fh, fileSize - (LONG)len, OFFSET_BEGINNING);
    if (Read(fh, tail, (LONG)len) == (LONG)len) {
        LONG at = _PkgFindEocd(tail, len);
        if (at >= 0) {
            for (UBYTE i = 0; i < PKG_EOCD_SIZE; i++) e[i] = tail[at + i];
            ok = 1;
        }
    }
    FreeMem(tail, len);
    return ok;
}

// -------------------------------------------------------------------------
// PackageOpen — open a package and read its directory. NULL if the file is
// missing, is not a ZIP, or holds an entry SADE cannot load (compressed,
// encrypted, ZIP64). The file stays open until PackageClose.
// -------------------------------------------------------------------------
static Package *PackageOpen(const char *path) {
    if (!DOSBase || _sade_sysTaken) return NULL;
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        KPrintF("PackageOpen: can't open '%s'\n", (ULONG)path);
        return NULL;
    }
    Package *p = (Package*)AllocMem(sizeof(Package), MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) { Close(fh); return NULL; }
    p->fh = fh;

    Seek(fh, 0, OFFSET_END);
    LONG fileSize = Seek(fh, 0, OFFSET_BEGINNING);
    UBYTE eocd[PKG_EOCD_SIZE];
    UWORD count;
    ULONG dirOffset;
    // a short comment first (SADE writes 14 bytes), then the ZIP maximum
    if (fileSize < PKG_EOCD_SIZE ||
        !(_PkgReadEocd(fh, fileSize, PKG_EOCD_SIZE + 256, eocd) ||
          _PkgReadEocd(fh, fileSize, PKG_EOCD_SIZE + PKG_MAX_COMMENT, eocd)) ||
        !_PkgParseEocd(eocd, &count, &p->dirSize, &dirOffset) ||
        dirOffset + p->dirSize > (ULONG)fileSize || !count || !p->dirSize) {
        KPrintF("PackageOpen: '%s' is not a SADE package\n", (ULONG)path);
        PackageClose(p);
        return NULL;
    }

    p->dir = (UBYTE*)AllocMem(p->dirSize, MEMF_PUBLIC);
    p->entries = (PackageEntry*)AllocMem(muluw(count, sizeof(PackageEntry)), MEMF_PUBLIC);
    if (p->entries) p->count = count;            // PackageClose frees by count
    Seek(fh, (LONG)dirOffset, OFFSET_BEGINNING);
    if (!p->dir || !p->entries ||
        Read(fh, p->dir, (LONG)p->dirSize) != (LONG)p->dirSize ||
        !_PkgParseDir(p->dir, p->dirSize, count, p->entries)) {
        KPrintF("PackageOpen: bad or unsupported directory in '%s'\n", (ULONG)path);
        PackageClose(p);
        return NULL;
    }
    KPrintF("PackageOpen: '%s', %ld entries\n", (ULONG)path, (ULONG)count);
    return p;
}

// -------------------------------------------------------------------------
// PackageFind / PackageSize — look an entry up by name.
// -------------------------------------------------------------------------
static const PackageEntry *PackageFind(const Package *p, const char *name) {
    return p ? _PkgFind(p->entries, p->count, name) : NULL;
}

static LONG PackageSize(const Package *p, const char *name) {
    const PackageEntry *e = PackageFind(p, name);
    return e ? (LONG)e->size : -1;
}

// -------------------------------------------------------------------------
// PackageLoad — one entry into a freshly AllocMem'd buffer (the FileLoad
// contract: memFlags MEMF_PUBLIC or MEMF_CHIP, sizeOut may be NULL, NULL on
// any failure, free with FileUnload).
// -------------------------------------------------------------------------
static void *PackageLoad(Package *p, const char *name, ULONG memFlags, ULONG *sizeOut) {
    if (sizeOut) *sizeOut = 0;
    if (!p || !DOSBase || _sade_sysTaken) return NULL;
    const PackageEntry *e = PackageFind(p, name);
    if (!e) {
        KPrintF("PackageLoad: no '%s' in the package\n", (ULONG)name);
        return NULL;
    }
    if (!e->size) return NULL;                   // as FileLoad: nothing to load

    UBYTE lh[PKG_LOCAL_SIZE];
    ULONG start = 0;
    Seek(p->fh, (LONG)e->offset, OFFSET_BEGINNING);
    if (Read(p->fh, lh, PKG_LOCAL_SIZE) == PKG_LOCAL_SIZE) start = _PkgDataStart(lh);
    if (!start) {
        KPrintF("PackageLoad: bad local header for '%s'\n", (ULONG)name);
        return NULL;
    }
    Seek(p->fh, (LONG)(start - PKG_LOCAL_SIZE), OFFSET_CURRENT);

    void *buf = AllocMem(e->size, memFlags);
    if (!buf) {
        KPrintF("PackageLoad: no memory for '%s' (%ld bytes)\n", (ULONG)name, e->size);
        return NULL;
    }
    if (Read(p->fh, buf, (LONG)e->size) != (LONG)e->size) {
        KPrintF("PackageLoad: short read on '%s'\n", (ULONG)name);
        FreeMem(buf, e->size);
        return NULL;
    }
#ifndef SADE_PACKAGE_NO_CRC
    if (_PkgCrc32(0, (const UBYTE*)buf, e->size) != e->crc) {
        KPrintF("PackageLoad: CRC mismatch on '%s'\n", (ULONG)name);
        FreeMem(buf, e->size);
        return NULL;
    }
#endif
    if (sizeOut) *sizeOut = e->size;
    KPrintF("PackageLoad: '%s' -> %ld bytes%s\n", (ULONG)name, e->size,
            (ULONG)((memFlags & MEMF_CHIP) ? " (chip)" : ""));
    return buf;
}

static void *PackageLoadChip(Package *p, const char *name, ULONG *sizeOut) {
    return PackageLoad(p, name, MEMF_CHIP, sizeOut);
}

// -------------------------------------------------------------------------
// AssetLoad — from the package when there is one, else the loose file of
// the same path (FileLoad). Lets a game run from its package and still
// start from the asset tree while the package is not built.
// -------------------------------------------------------------------------
static void *AssetLoad(Package *p, const char *name, ULONG memFlags, ULONG *sizeOut) {
    return p ? PackageLoad(p, name, memFlags, sizeOut) : FileLoad(name, memFlags, sizeOut);
}
#endif
