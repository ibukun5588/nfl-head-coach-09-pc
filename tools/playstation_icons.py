#!/usr/bin/env python3
"""PlayStation button icons for the port, from the 360 game files themselves.

The menus are APT movies inside the qkl_*.ast archives; a button prompt is a small DXT5 bitmap of a
movie (XPR2: tiled, 8-in-16 byte-swapped), followed by the shape that draws it (its bitmap fill takes
texels 1:1 from (0, 0), so the quad's size says which texels show). qkl_boot.ast entry 83 is the
button icon library and holds both sets, Xbox (B A Back D-pad LB LS... RT Start X Y) and PlayStation
(Circle Cross D-pad L1 L2 L3 L R1 R2 R3 R Select Square Start Triangle); the 360 build only ever shows
the Xbox ones. qkl_interface.ast 0x20-0x34 are standalone Xbox icon movies (the PS3 build has
PlayStation ones at 0x35-0x47; the 360 build lacks those, so the library's are used for them too).

This writes <hc09-cache>/glyphs/playstation.bin: per Xbox icon texture, the FNV-1a 64 hash of its DXT
blocks as they lie in memory (raster block order, each read at its tiled offset; runtime/glyphs.cpp
computes the same) and its replacement in RGBA at that texture's size: the PlayStation icon's visible
texels scaled to fit where the Xbox icon's visible texels are (inside its quad), centred. The native renderer swaps them while a PlayStation
pad is in use (hc09.ini [input] glyphs). Nothing of the game's is written.

  py -3.11 tools/playstation_icons.py [--root <360 game folder>] [--cache <hc09-cache>] [--sheet <png>]
"""
import argparse
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MAX_SIDE = 64          # runtime/glyphs.cpp GlyphCandidate

LIBRARY = ('qkl_boot.ast', 83, 44)    # archive, entry, textures in it
# Library texture order (k-th texture of the movie).
XBOX = {0: 'B', 1: 'A', 2: 'Back', 3: 'D-pad', 4: 'LB', 5: 'press LS', 6: 'LS', 7: 'LS', 8: 'LS left/right',
        9: 'LT', 10: 'RB', 11: 'press RS', 12: 'RS', 13: 'RS', 14: 'RS left/right', 15: 'RT', 16: 'Start',
        17: 'X', 18: 'Y'}
# 36-43 are light and dark chevrons (stick directions); left alone: the two kinds may be states, not
# platforms.
PS = {'B': 19, 'A': 20, 'Back': 32, 'D-pad': 21, 'LB': 22, 'LT': 23, 'press LS': 24, 'LS': 25,
      'LS left/right': 26, 'LS up/down': 25, 'RB': 27, 'RT': 28, 'press RS': 29, 'RS': 30, 'RS left/right': 31,
      'RS up/down': 30, 'Start': 34, 'X': 33, 'Y': 35}
# qkl_interface.ast standalone Xbox icon movies (names from an HC09 modding spreadsheet).
INTERFACE = {0x20: 'A', 0x21: 'B', 0x22: 'Back', 0x23: 'D-pad', 0x24: 'LB', 0x25: 'press LS', 0x26: 'LS',
             0x27: 'LS left/right', 0x28: 'LS', 0x29: 'LS up/down', 0x2A: 'LT', 0x2B: 'RB', 0x2C: 'press RS',
             0x2D: 'RS', 0x2E: 'RS left/right', 0x2F: 'RS', 0x30: 'RS up/down', 0x31: 'RT', 0x32: 'Start',
             0x33: 'X', 0x34: 'Y'}


# ---- BGFA1.05 (EA .ast) ----

def parse_toc(read):
    """[(flag, offset, size)] of an archive."""
    h = read(0, 0x30)
    if h[:8] != b'BGFA1.05':
        raise ValueError('not a BGFA archive')
    count = struct.unpack_from('<I', h, 0xC)[0]
    tocoff, tocsz = struct.unpack_from('<QQ', h, 0x10)
    _b0, wt, wh, wo, ws, wu, shift, _b7 = h[0x20:0x28]
    ntypes, namelen = struct.unpack_from('<II', h, 0x28)
    toc = read(tocoff, tocsz)
    p, esz, ents = ntypes * 4, 1 + wt + wh + wo + ws + wu + namelen, []
    for _ in range(count):
        e = toc[p:p + esz]; p += esz
        q = 1 + wt + wh
        off = int.from_bytes(e[q:q + wo], 'little') << shift; q += wo
        ents.append((e[0], off, int.from_bytes(e[q:q + ws], 'little')))
    return ents


def unpack(flag, raw):
    return zlib.decompress(raw) if flag & 1 else raw


def movie(path, index):
    """The decompressed entries of top-level movie `index`."""
    with open(path, 'rb') as f:
        def read(o, n):
            f.seek(o); return f.read(n)
        flag, off, size = parse_toc(read)[index]
        data = unpack(flag, read(off, size))
    return [unpack(fl, data[o:o + n]) for fl, o, n in parse_toc(lambda a, b: data[a:a + b])]


# ---- textures ----

def tiled_offset_2d(x, y, pitch_blocks, bpp_log2):
    """Byte offset of block (x, y) of a tiled 2D surface (runtime/xenos/xenos_texture.cpp TiledOffset2D)."""
    outer = ((y >> 5) * (pitch_blocks >> 5) + (x >> 5)) << 6
    inner = (((y >> 1) & 7) << 3) | (x & 7)
    oi = (outer | inner) << bpp_log2
    bank = (y >> 4) & 1
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return (((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) |
            (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12))


def texture(xpr):
    """(w, h, blocks) of a small tiled DXT5 XPR2 texture: its blocks as stored (guest byte order), in raster
    block order; None for anything else."""
    if xpr[:4] != b'XPR2' or len(xpr) < 0x60:
        return None
    hdr = struct.unpack('>I', xpr[4:8])[0]
    res = struct.unpack('>I', xpr[0x14:0x18])[0] + 0xC
    fetch = struct.unpack('>6I', xpr[res + 0x1C:res + 0x34])
    fmt, w, h = fetch[1] & 0x3F, (fetch[2] & 0x1FFF) + 1, ((fetch[2] >> 13) & 0x1FFF) + 1
    dim, packed = (fetch[5] >> 9) & 3, (fetch[5] >> 11) & 1
    if fmt != 20 or w > MAX_SIDE or h > MAX_SIDE or not fetch[0] >> 31 or dim != 1 or packed:
        return None
    data = xpr[hdr + 0xC + (fetch[1] & 0xFFFFF000):]
    return w, h, b''.join(data[tiled_offset_2d(bx, by, 32, 4):][:16]
                          for by in range((h + 3) // 4) for bx in range((w + 3) // 4))


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def _rgb565(c):
    return (((c >> 11) * 255 + 15) // 31, (((c >> 5) & 63) * 255 + 31) // 63, ((c & 31) * 255 + 15) // 31)


def dxt5_block(b):
    """16 RGBA texels of a little-endian DXT5 block."""
    a0, a1 = b[0], b[1]
    al = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else
                     [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
    abits = int.from_bytes(b[2:8], 'little')
    c0, c1 = struct.unpack('<HH', b[8:12])
    p0, p1 = _rgb565(c0), _rgb565(c1)
    cl = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)), tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
    cbits = int.from_bytes(b[12:16], 'little')
    return [cl[(cbits >> (2 * i)) & 3] + (al[(abits >> (3 * i)) & 7],) for i in range(16)]


def rgba(w, h, blocks):
    """RGBA of a texture's blocks (guest 8-in-16 order)."""
    blocks = bytes(x for k in range(0, len(blocks), 2) for x in (blocks[k + 1], blocks[k]))
    px = bytearray(w * h * 4)
    bw = (w + 3) // 4
    for n in range(len(blocks) // 16):
        bx, by = n % bw, n // bw
        for i, c in enumerate(dxt5_block(blocks[n * 16:n * 16 + 16])):
            x, y = bx * 4 + i % 4, by * 4 + i // 4
            if x < w and y < h:
                o = (y * w + x) * 4
                px[o:o + 4] = bytes(c)
    return bytes(px)


def quad_size(shape, w, h):
    """Size of the quad a texture's shape draws (texels [0, qw) x [0, qh) show); the texture size if the
    entry is not such a shape."""
    if shape[:4] != b'\x00\x00\x01\x20' or len(shape) < 0xE8:
        return float(w), float(h)
    qw, qh = struct.unpack('>f', shape[0xA0:0xA4])[0], struct.unpack('>f', shape[0xE4:0xE8])[0]
    if not (0 < qw <= 4 * MAX_SIDE and 0 < qh <= 4 * MAX_SIDE):
        return float(w), float(h)
    return qw, qh


def icons(entries):
    """[(w, h, blocks, quad)] of a movie's icon textures, in order."""
    out = []
    for i, e in enumerate(entries):
        t = texture(e)
        if t:
            out.append(t + (quad_size(entries[i + 1] if i + 1 < len(entries) else b'', t[0], t[1]),))
    return out


def bbox(img, w, h, quad):
    """(x0, y0, x1, y1) of the visible texels (alpha >= 24) inside the quad; the quad if none are."""
    qw, qh = min(w, int(round(quad[0]))), min(h, int(round(quad[1])))
    xs = [x for y in range(qh) for x in range(qw) if img[(y * w + x) * 4 + 3] >= 24]
    ys = [y for y in range(qh) for x in range(qw) if img[(y * w + x) * 4 + 3] >= 24]
    if not xs:
        return 0.0, 0.0, float(qw), float(qh)
    return float(min(xs)), float(min(ys)), float(max(xs) + 1), float(max(ys) + 1)


def fit(src, sw, sh, sbox, dw, dh, dbox):
    """The texels in box sbox of RGBA image src (sw x sh) scaled uniformly to fit box dbox of a transparent
    dw x dh image, centred there (bilinear on premultiplied alpha). Boxes are (x0, y0, x1, y1)."""
    sq = (sbox[2] - sbox[0], sbox[3] - sbox[1])
    dq = (dbox[2] - dbox[0], dbox[3] - dbox[1])
    out = bytearray(dw * dh * 4)
    def texel(x, y):
        x, y = min(max(x, 0), sw - 1), min(max(y, 0), sh - 1)
        o = (y * sw + x) * 4
        a = src[o + 3]
        return (src[o] * a, src[o + 1] * a, src[o + 2] * a, a)
    scale = min(dq[0] / sq[0], dq[1] / sq[1])
    ox, oy = dbox[0] + (dq[0] - sq[0] * scale) / 2, dbox[1] + (dq[1] - sq[1] * scale) / 2
    for y in range(dh):
        fy = (y + 0.5 - oy) / scale - 0.5
        if fy < -0.5 or fy > sq[1] - 0.5:
            continue
        y0 = int(fy // 1); ty = fy - y0
        fy += sbox[1]; y0 += int(sbox[1])
        for x in range(dw):
            fx = (x + 0.5 - ox) / scale - 0.5
            if fx < -0.5 or fx > sq[0] - 0.5:
                continue
            fx += sbox[0]
            x0 = int(fx // 1); tx = fx - x0
            c = [0.0] * 4
            for (xx, yy, wgt) in ((x0, y0, (1 - tx) * (1 - ty)), (x0 + 1, y0, tx * (1 - ty)),
                                  (x0, y0 + 1, (1 - tx) * ty), (x0 + 1, y0 + 1, tx * ty)):
                t = texel(xx, yy)
                for k in range(4):
                    c[k] += t[k] * wgt
            a = c[3]
            o = (y * dw + x) * 4
            if a > 0.5:
                out[o:o + 4] = bytes((min(255, int(c[0] / a + 0.5)), min(255, int(c[1] / a + 0.5)),
                                      min(255, int(c[2] / a + 0.5)), min(255, int(a + 0.5))))
    return bytes(out)


def ini_root():
    try:
        for line in open(os.path.join(REPO, 'hc09.ini'), encoding='utf-8'):
            k, _, v = line.partition('=')
            if k.strip().lower() == 'root' and v.strip():
                return v.strip()
    except OSError:
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=ini_root(), help='the 360 game folder (default: hc09.ini [game] root)')
    ap.add_argument('--cache', default=os.path.join(REPO, 'hc09-cache'))
    ap.add_argument('--sheet', help='also write a PNG of every Xbox icon (top) and its replacement (needs Pillow)')
    a = ap.parse_args()
    if not a.root:
        sys.exit('give --root (the folder with the 360 qkl_*.ast)')

    arc, index, count = LIBRARY
    lib = icons(movie(os.path.join(a.root, arc), index))
    if len(lib) != count:
        sys.exit('%s entry %d has %d icon textures, expected %d (a different build?)' % (arc, index, len(lib), count))
    ps = {name: lib[k] for name, k in PS.items()}

    targets = [('%s %d' % (arc, index), XBOX[k], lib[k]) for k in sorted(XBOX)]
    for i, name in sorted(INTERFACE.items()):
        t = icons(movie(os.path.join(a.root, 'qkl_interface.ast'), i))
        if len(t) != 1:
            sys.exit('qkl_interface.ast entry 0x%X is not an icon movie (a different build?)' % i)
        targets.append(('qkl_interface.ast 0x%X' % i, name, t[0]))

    table, shown = {}, []
    for where, name, (w, h, blocks, quad) in targets:
        key = fnv1a64(blocks)
        if key in table:
            continue
        sw, sh, sblocks, squad = ps[name]
        simg, dimg = rgba(sw, sh, sblocks), rgba(w, h, blocks)
        # The visible PlayStation glyph fitted to where the Xbox glyph shows (the library's icons are drawn
        # larger, with more margin, than the standalone ones).
        rep = fit(simg, sw, sh, bbox(simg, sw, sh, squad), w, h, bbox(dimg, w, h, quad))
        table[key] = (w, h, rep)
        shown.append((w, h, dimg, rep))
        print('%-22s %-14s %016X  %2dx%-2d quad %4.1fx%-4.1f' % (where, name, key, w, h, quad[0], quad[1]))

    out = [b'HCGL', struct.pack('<II', 1, len(table))]
    for key, (w, h, rep) in table.items():
        out += [struct.pack('<QII', key, w, h), rep]
    dst = os.path.join(a.cache, 'glyphs', 'playstation.bin')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + '.tmp', 'wb') as f:
        f.write(b''.join(out))
    os.replace(dst + '.tmp', dst)
    print('wrote %s (%d icons)' % (dst, len(table)))

    if a.sheet:
        from PIL import Image
        sheet = Image.new('RGBA', (len(shown) * 68, 136), (48, 48, 48, 255))
        for n, (w, h, orig, rep) in enumerate(shown):
            sheet.alpha_composite(Image.frombytes('RGBA', (w, h), orig), (n * 68 + 2, 2))
            sheet.alpha_composite(Image.frombytes('RGBA', (w, h), rep), (n * 68 + 2, 70))
        sheet.save(a.sheet)
        print('wrote %s' % a.sheet)


if __name__ == '__main__':
    main()
