#!/usr/bin/env python3
"""Crop the same rectangle out of several PNGs, scale it up and put the crops side by side
(dependency-free, uses tools/imgdiff.py's PNG reader). For comparing renderers by eye.

    py -3.11 tools/crop.py out.png x y w h scale a.png b.png [c.png ...]
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import imgdiff  # noqa: E402


def write_png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


def main():
    out, x, y, w, h, s = sys.argv[1], *map(int, sys.argv[2:7])
    files = sys.argv[7:]
    gap = 4
    W = len(files) * w * s + (len(files) - 1) * gap
    H = h * s
    img = bytearray(W * H * 4)
    for k, f in enumerate(files):
        iw, ih, px = imgdiff.read_png(f)
        ox = k * (w * s + gap)
        for yy in range(H):
            sy = min(ih - 1, y + yy // s)
            for xx in range(w * s):
                sx = min(iw - 1, x + xx // s)
                o = (sy * iw + sx) * 4
                d = (yy * W + ox + xx) * 4
                img[d:d + 4] = px[o:o + 4]
    write_png(out, W, H, img)


if __name__ == '__main__':
    main()
