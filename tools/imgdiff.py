#!/usr/bin/env python3
"""Compare two PNG frames (8-bit RGB/RGBA, non-interlaced): pixel/channel error
statistics and an optional diff image. No dependencies beyond the standard library.

    python tools/imgdiff.py expected.png actual.png [diff.png] [--tol N]

Prints the number of differing pixels, max and mean absolute channel error,
the bounding box of the differences and the worst pixels. diff.png shows
differing pixels in red (brightness = error, x8) over a dimmed expected image.
Exit code 0 if every channel is within --tol (default 0), 1 otherwise.
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat, w = 8, [], None
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f'{path}: only 8-bit RGB/RGBA non-interlaced PNGs are supported')
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride): line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - bpp] if i >= bpp else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b, c = prev[i], prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if bpp == 4:
            out[y * w * 4:(y + 1) * w * 4] = line
        else:
            row = out[y * w * 4:(y + 1) * w * 4]
            row[0::4], row[1::4], row[2::4], row[3::4] = line[0::3], line[1::3], line[2::3], b'\xff' * w
            out[y * w * 4:(y + 1) * w * 4] = row
        prev = line
    return w, h, out


def write_png(path, w, h, rgba):
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--tol')]
    tol = 0
    for i, a in enumerate(sys.argv):
        if a.startswith('--tol'):
            tol = int(a.split('=', 1)[1]) if '=' in a else int(sys.argv[i + 1])
            if '=' not in a: args.remove(sys.argv[i + 1])
    if len(args) < 2:
        print(__doc__); return 2
    w, h, a = read_png(args[0])
    w2, h2, b = read_png(args[1])
    if (w, h) != (w2, h2):
        print(f'size mismatch: {w}x{h} vs {w2}x{h2}'); return 1
    npix, maxe, sume, worst = 0, 0, 0, []
    x0, y0, x1, y1 = w, h, -1, -1
    diff = bytearray(len(a)) if len(args) > 2 else None
    for i in range(0, len(a), 4):
        e = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]), abs(a[i + 3] - b[i + 3]))
        if diff is not None:
            if e:
                diff[i:i + 4] = bytes((min(255, 64 + e * 8), 0, 0, 255))
            else:
                diff[i:i + 4] = bytes((a[i] // 4, a[i + 1] // 4, a[i + 2] // 4, 255))
        if not e:
            continue
        npix += 1
        sume += sum(abs(a[i + k] - b[i + k]) for k in range(4))
        maxe = max(maxe, e)
        p = i // 4
        x, y = p % w, p // w
        x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        if len(worst) < 5 or e > worst[-1][0]:
            worst.append((e, x, y, tuple(a[i:i + 4]), tuple(b[i:i + 4])))
            worst.sort(key=lambda t: -t[0]); del worst[5:]
    total = w * h
    print(f'{w}x{h}: {npix} of {total} pixels differ ({100.0 * npix / total:.4f}%), max channel error {maxe}, '
          f'mean channel error {sume / (total * 4):.5f} (over differing pixels {sume / max(1, npix * 4):.3f})')
    if npix:
        print(f'  bounding box ({x0},{y0})-({x1},{y1})')
        for e, x, y, pa, pb in worst:
            print(f'  ({x},{y}) err {e}: expected {pa} actual {pb}')
    if diff is not None:
        write_png(args[2], w, h, diff)
    return 0 if maxe <= tol else 1


if __name__ == '__main__':
    sys.exit(main())
