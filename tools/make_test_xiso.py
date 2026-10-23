#!/usr/bin/env python3
"""Development test: an XDVDFS disc image (the Xbox 360 game partition's file system) made from a folder of files, for
testing the installer's disc reader (tools/installer/setup.cpp) where no real disc image is at hand. The image stays on
the developer's PC: it holds the game files.

    python tools/make_test_xiso.py <folder> <out.iso> [--offset 0xFD90000] [--subdir name] [--replace name=path]

--offset: where the game partition starts (0: an extracted "XISO"; 0xFD90000: XGD2, as a full disc dump of a 360
game); the space before it is left sparse. --subdir: also put one file in a subfolder (tests directory recursion).
--replace name=path: the image's file `name` from another path (repeatable).
Layout per the XDVDFS format: sector 0x800; the volume descriptor at sector 32 ("MICROSOFT*XBOX*MEDIA", root
directory sector and size); a directory is a binary tree of entries {u16 left, u16 right (in 4-byte units), u32
sector, u32 size, u8 attributes, u8 name length, name}, 4-byte aligned, never across a sector (0xFF padding).
"""
import argparse
import os
import struct

SECTOR = 0x800


def dir_table(entries):
    """entries: [(name, sector, size, attr)] -> table bytes. A right-leaning tree: each entry's right child is the next."""
    entries = sorted(entries, key=lambda e: e[0].upper())
    raw, offs = bytearray(), []
    for i, (name, sector, size, attr) in enumerate(entries):
        n = name.encode('ascii')
        length = (14 + len(n) + 3) & ~3
        if len(raw) // SECTOR != (len(raw) + length - 1) // SECTOR:   # never across a sector
            raw += b'\xff' * (SECTOR - len(raw) % SECTOR)
        offs.append(len(raw))
        raw += b'\0' * length
    for i, (name, sector, size, attr) in enumerate(entries):
        n = name.encode('ascii')
        right = offs[i + 1] // 4 if i + 1 < len(entries) else 0
        e = struct.pack('<HHIIBB', 0, right, sector, size, attr, len(n)) + n
        raw[offs[i]:offs[i] + len(e)] = e
        pad = ((14 + len(n) + 3) & ~3) - len(e)
        raw[offs[i] + len(e):offs[i] + len(e) + pad] = b'\xff' * pad
    raw += b'\xff' * (-len(raw) % SECTOR)
    return bytes(raw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('out')
    ap.add_argument('--offset', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--subdir')
    ap.add_argument('--replace', action='append', default=[])
    a = ap.parse_args()
    src = {}
    for r in a.replace:
        name, path = r.split('=', 1)
        src[name] = path
    files = sorted(f for f in os.listdir(a.folder) if os.path.isfile(os.path.join(a.folder, f))
                   and f.lower().endswith(('.xex', '.ast')) and not f.lower().endswith('.orig'))
    # Sectors: 0..31 reserved, 32 the descriptor, then the root directory (and a subfolder's), then the files.
    next_sector = 34 + 4
    placed = []
    for f in files:
        size = os.path.getsize(src.get(f, os.path.join(a.folder, f)))
        placed.append((f, next_sector, size))
        next_sector += (size + SECTOR - 1) // SECTOR
    root_entries = [(f, s, n, 0x80) for f, s, n in placed]
    sub_sector = 34 + 2
    if a.subdir:   # an empty-ish subfolder with a copy of the smallest file's entry (same data)
        f, s, n = min(placed, key=lambda p: p[2])
        sub = dir_table([(f, s, n, 0x80)])
        root_entries.append((a.subdir, sub_sector, len(sub), 0x10))
    root = dir_table(root_entries)
    assert len(root) <= 2 * SECTOR, 'root directory too large for this test layout'
    with open(a.out, 'wb') as o:
        base = a.offset
        o.seek(base + 32 * SECTOR)
        o.write(b'MICROSOFT*XBOX*MEDIA' + struct.pack('<II', 34, len(root)) + b'\0' * 8 + b'MICROSOFT*XBOX*MEDIA')
        o.seek(base + 34 * SECTOR)
        o.write(root)
        if a.subdir:
            o.seek(base + sub_sector * SECTOR)
            o.write(sub)
        for f, s, n in placed:
            o.seek(base + s * SECTOR)
            with open(src.get(f, os.path.join(a.folder, f)), 'rb') as i:
                while True:
                    b = i.read(1 << 24)
                    if not b:
                        break
                    o.write(b)
        o.truncate(base + next_sector * SECTOR)
    print(f'{a.out}: {len(placed)} files, partition at {base:#x}, {os.path.getsize(a.out) / 2**30:.2f} GiB')


if __name__ == '__main__':
    main()
