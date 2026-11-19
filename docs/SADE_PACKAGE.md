# SADE package format (`.ocs`) — version 1

One file holding all of a game's assets — pictures, sprite sheets, fonts,
levels, sounds, music — loaded at start-up instead of one file per asset.
Dangerous Dave ships as `dave.exe` + `dave.ocs`.

A SADE package **is a ZIP archive**. It uses a strict subset of ZIP (PKWARE
APPNOTE 6.3) that a 68000 reads with a few dozen lines of C and no
decompressor. Any ZIP tool (unzip, 7-Zip, Windows Explorer, Python's
`zipfile`) can list, test and extract it, so you can inspect a package
without special tools.

| | |
|---|---|
| Writer | `tools/make_package.py` (run by `make`) |
| Reader | `libs/sade/sade_package.h` |
| Tests | `make test` → `tests/package_test.c`, `tests/make_test_zips.py` |
| Extension | `.ocs` by convention (the chipset it targets); `.zip` works too |

## Why ZIP, why stored

- **No compression.** A stored entry's bytes are the file's bytes at a known
  offset, so loading one is a seek and a read straight into its final
  buffer, chip or fast RAM. Decompressing on a 7 MHz 68000 would cost more
  load time than reading the extra bytes from disk.
- **ZIP compatible.** You don't need a custom viewer or extractor. CRC-32s
  are real ZIP CRCs, so `unzip -t` checks a package and SADE checks each
  entry as it loads it.
- **One file.** One file on the disk to open and close instead of 17, and
  the entries are in load order, so a floppy reads the package front to
  back without seeking back and forth.

## Layout

```
offset 0   local header 0 | name 0 | extra 0 (alignment) | data 0   <- 4-aligned
           local header 1 | name 1 | extra 1 (alignment) | data 1   <- 4-aligned
           ...
           central directory: one header per entry, same order
           end of central directory record (EOCD) + comment
```

All numbers are little-endian (ZIP), and the reader byte-swaps them.

### Local file header (30 bytes + name + extra)

| Off | Size | Field | SADE value |
|---|---|---|---|
| 0 | 4 | signature | `50 4B 03 04` |
| 4 | 2 | version needed | 20 |
| 6 | 2 | flags | 0 |
| 8 | 2 | method | **0 (stored)** |
| 10 | 2 | mod time | 0 |
| 12 | 2 | mod date | `0x0021` (1980-01-01) |
| 14 | 4 | CRC-32 | of the data |
| 18 | 4 | compressed size | = size |
| 22 | 4 | uncompressed size | = size |
| 26 | 2 | name length | n |
| 28 | 2 | extra length | m |
| 30 | n | name | e.g. `assets/game/tiles.iff` |
| 30+n | m | extra | alignment field, below |

### Alignment extra field

Entry data starts on a **4-byte boundary** within the file, so a loader that
reads a whole package into memory can use entries in place as `UWORD`/`ULONG`
data. The padding is an extra field in the format Android's `zipalign`
writes, which ZIP readers skip as an unknown field:

| Off | Size | Field |
|---|---|---|
| 0 | 2 | ID `0xD935` |
| 2 | 2 | size of the rest (2 + pad) |
| 4 | 2 | alignment (4) |
| 6 | pad | zeros, 0–3 bytes |

The central directory entries carry no extra field.

### Central directory header (46 bytes + name)

Standard ZIP. The fields SADE reads and checks:

| Off | Size | Field | Must be |
|---|---|---|---|
| 0 | 4 | signature | `50 4B 01 02` |
| 8 | 2 | flags | bit 0 (encrypted) clear |
| 10 | 2 | method | 0 |
| 16 | 4 | CRC-32 | checked on load |
| 20 | 4 | compressed size | = uncompressed size, not `FFFFFFFF` |
| 24 | 4 | uncompressed size | |
| 28/30/32 | 2+2+2 | name / extra / comment lengths | |
| 34 | 2 | disk number start | 0 |
| 42 | 4 | local header offset | not `FFFFFFFF` |

### End of central directory (22 bytes + comment)

| Off | Size | Field | SADE value |
|---|---|---|---|
| 0 | 4 | signature | `50 4B 05 06` |
| 4 | 2 | this disk | 0 |
| 6 | 2 | disk with the directory | 0 |
| 8 | 2 | entries on this disk | = total |
| 10 | 2 | total entries | 1..65534 |
| 12 | 4 | directory size | |
| 16 | 4 | directory offset | |
| 20 | 2 | comment length | 14 |
| 22 | 14 | comment | `SADE package 1` |

## Rules

1. **Stored only.** Every entry has method 0 and compressed size equal to
   uncompressed size. The loader refuses the whole package otherwise.
2. **No ZIP64, no encryption, single disk.** A package stays under 4 GB with
   at most 65 534 entries, which is far beyond any Amiga game.
3. **Names** are ASCII paths with `/` separators, relative to the project
   root, exactly as the game asks for them (`assets/game/levels/level01.map`).
   There are no directory entries, no leading `/` and no `..`. The loader
   matches names without regard to case, like AmigaDOS, so two names that
   differ only in case are not allowed in one package.
4. **Order = load order.** The writer keeps the order it is given; give it
   the order the game loads assets in.
5. **Deterministic.** The writer uses a fixed timestamp and fixed headers,
   so the same inputs give the same bytes.
6. **Version.** The comment `SADE package 1` names this version. Readers do
   not require it, so any stored-only ZIP works, which is handy while
   developing.

What the reader accepts is slightly wider than what the writer produces:
any stored ZIP from any tool loads (unaligned data, other extra fields,
comments of any length, data-descriptor flag), as `make test` checks with
Python's `zipfile` and Info-ZIP's `zip -0`.

## Loading (sade_package.h)

```c
if (!FileInit()) return 0;                      // OS alive: load phase
Package *pkg = PackageOpen("dave.ocs");         // reads EOCD + directory only
ULONG size;
void *tiles = PackageLoad(pkg, "assets/game/tiles.iff", MEMF_PUBLIC, &size);
void *song  = PackageLoadChip(pkg, "assets/audio/song.mod", &songSize);
PackageClose(pkg);                              // before FileShutdown
FileShutdown();
if (!SadeInit()) return 0;                      // now take the machine
...
FileUnload(tiles, size);                        // same as FileLoad buffers
```

| Call | |
|---|---|
| `PackageOpen(path)` | Open, find the EOCD (a 278-byte tail probe, then the ZIP maximum), read and validate the directory. `NULL` if missing or unsupported. |
| `PackageLoad(p, name, memFlags, &size)` | Read the local header, seek to the data, `AllocMem` + `Read`, verify the CRC-32. Same contract as `FileLoad`. |
| `PackageLoadChip(p, name, &size)` | `PackageLoad` into chip RAM (modules, samples). |
| `PackageFind(p, name)` / `PackageSize(p, name)` | Look up an entry / its size (-1 if absent). |
| `AssetLoad(p, name, memFlags, &size)` | `PackageLoad` when `p` is open, otherwise `FileLoad(name)`, so a game also runs from loose files. |
| `PackageClose(p)` | Close the file and free the directory. NULL-safe. |

Memory: the directory (about 70 bytes per entry) plus 20 bytes per entry,
held until `PackageClose`. Opening also allocates the tail probe for a
moment.

CRC checking costs about 35 cycles per byte on a 68000 (~0.4 s for Dave's
77 KB). Define `SADE_PACKAGE_NO_CRC` before including the header to skip it.

`sade_file.h` (`FileLoad` & co.) is unchanged and still works for single
files; `sade_package.h` builds on it.

## Building a package

```sh
make                                   # game build: packs dave.ocs too
make package                           # only the package
python -I tools/make_package.py -o my.ocs assets/a.iff assets/b.mod ...
python -I tools/make_package.py --list dave.ocs    # entries, CRCs, test
unzip -l dave.ocs                      # or any ZIP tool
```

The Makefile's `PACKAGE_FILES` lists what goes into `dave.ocs`. When you
add an asset to the game, add its path there, in load order.
