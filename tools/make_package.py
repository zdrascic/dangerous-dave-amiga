#!/usr/bin/env python3
"""make_package — pack asset files into a SADE package (.ocs / .aga).

Usage:
  python -I tools/make_package.py -o dave.ocs FILE...
  python -I tools/make_package.py --list dave.ocs

A SADE package is a plain ZIP archive restricted to what a 68000 can read
cheaply (full spec: docs/SADE_PACKAGE.md):

  - every entry STORED (method 0): no compression, data is the file bytes
  - entry data starts on a 4-byte boundary (padding in the local header's
    extra field, ID 0xD935 as Android's zipalign writes it)
  - names are the paths given here, '/' separated, ASCII, case kept;
    no directory entries; no duplicates
  - entries in command-line order: that is the order the game loads them,
    so a floppy reads the package front to back
  - no ZIP64, no encryption, no data descriptors, single disk
  - fixed timestamp (1980-01-01 00:00) so the same inputs give the same
    bytes; archive comment "SADE package 1"

Any unzip tool lists, tests and extracts it (unzip -t dave.ocs).
"""
import os
import struct
import sys
import zlib

ALIGN = 4
ALIGN_EXTRA_ID = 0xD935
COMMENT = b'SADE package 1'
DOS_TIME, DOS_DATE = 0, (0 << 9) | (1 << 5) | 1      # 1980-01-01 00:00:00
VERSION = 20                                         # ZIP 2.0, stored


def build(out_path, files):
    names = [f.replace('\\', '/') for f in files]
    seen = set()
    for n in names:
        if not n or n.startswith('/') or '..' in n.split('/'):
            sys.exit('bad entry name: %r' % n)
        try:
            n.encode('ascii')
        except UnicodeEncodeError:
            sys.exit('entry name must be ASCII: %r' % n)
        if n.lower() in seen:                        # the loader ignores case
            sys.exit('duplicate entry: %s' % n)
        seen.add(n.lower())

    body = bytearray()
    central = bytearray()
    for path, name in zip(files, names):
        with open(path, 'rb') as f:
            data = f.read()
        if len(data) >= 0xFFFFFFFF:
            sys.exit('%s: too big for a non-ZIP64 archive' % path)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        bname = name.encode('ascii')
        offset = len(body)

        # extra field: ID, size, alignment (UWORD), zero padding so that the
        # data after the header lands on an ALIGN boundary
        base = offset + 30 + len(bname) + 6
        pad = (-base) % ALIGN
        extra = struct.pack('<HHH', ALIGN_EXTRA_ID, 2 + pad, ALIGN) + b'\0' * pad

        body += struct.pack('<IHHHHHIIIHH', 0x04034B50, VERSION, 0, 0,
                            DOS_TIME, DOS_DATE, crc, len(data), len(data),
                            len(bname), len(extra))
        body += bname + extra
        assert len(body) % ALIGN == 0
        body += data

        central += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014B50, VERSION, VERSION,
                               0, 0, DOS_TIME, DOS_DATE, crc, len(data), len(data),
                               len(bname), 0, 0, 0, 0, 0, offset)
        central += bname

    if len(names) > 0xFFFF or len(body) + len(central) >= 0xFFFFFFFF:
        sys.exit('too many / too big entries for a non-ZIP64 archive')
    eocd = struct.pack('<IHHHHIIH', 0x06054B50, 0, 0, len(names), len(names),
                       len(central), len(body), len(COMMENT)) + COMMENT

    tmp = out_path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(body + central + eocd)
    os.replace(tmp, out_path)                        # never a half-written package
    print('%s: %d entries, %d bytes' % (out_path, len(names),
                                        len(body) + len(central) + len(eocd)))


def list_package(path):
    import zipfile
    with zipfile.ZipFile(path) as z:
        for i in z.infolist():
            kind = 'stored' if i.compress_type == zipfile.ZIP_STORED else 'COMPRESSED'
            print('%8d  %08X  %-6s  %s' % (i.file_size, i.CRC, kind, i.filename))
        bad = z.testzip()
        print('CRC check: %s' % ('OK' if bad is None else 'FAILED at ' + bad))


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--list':
        list_package(args[1])
    elif len(args) >= 3 and args[0] == '-o':
        build(args[1], args[2:])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
