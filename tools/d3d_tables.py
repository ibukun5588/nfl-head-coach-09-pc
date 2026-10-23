#!/usr/bin/env python3
"""Dump the game's D3D render-state and sampler-state tables, with what each setter writes.

Usage: py -3.11 tools/d3d_tables.py   (run from the repo root; uses tools/ppcxref.py's index)

The statically linked Xbox D3D (0x82320000-0x8233DBB8) keeps a table of {getter, setter, default}
triplets per render state and per sampler state; device creation (sub_8232E958) applies the
defaults through it. Table index = the state's enum index (D3DRS_* / D3DSAMP_* order of the XDK).
Setters write fields of shadowed Xenos registers in the device (docs/NATIVE_RENDERER.md §2):
the device offset of each store is mapped to the register name from runtime/xenos/xenos.h.
"""
import os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppcxref  # noqa: E402

D3D_LO, D3D_HI = 0x82320000, 0x8233DBB8
ANCHOR_RS, ANCHOR_SS = 0x830A1130, 0x830A1188   # PRESENTINTERVAL's triplet; first sampler triplet
img = ppcxref.img()
rd = lambda a: struct.unpack_from('>I', img, a - 0x82000000)[0]
code = lambda w: D3D_LO <= w < D3D_HI

names = {}
for l in open(os.path.join(ppcxref.ROOT, 'runtime', 'xenos', 'xenos.h'), encoding='utf-8'):
    m = re.search(r'constexpr uint32_t (\w+)\s*=\s*0x([0-9A-Fa-f]+);', l)
    if m:
        names.setdefault(int(m.group(2), 16), m.group(1))
# device offset -> register, from the flush calls (docs/NATIVE_RENDERER.md §2)
BLOCKS = [(1920, 0x4000, 1024), (6016, 0x4400, 1024), (10112, 0x4900, 64), (10368, 0x2000, 19),
          (10444, 0x2100, 21), (10528, 0x2180, 5), (10548, 0x2200, 12), (10596, 0x2280, 21),
          (10680, 0x2300, 38), (10832, 0x2380, 16)]


def reg(off):
    if 1152 <= off < 1920:
        return 'FETCH+0x%X' % ((off - 1152) // 4)
    for base, r, n in BLOCKS:
        if base <= off < base + 4 * n:
            i = r + (off - base) // 4
            return names.get(i, 'reg0x%04X' % i)
    return None


def writes(f):
    body = ppcxref.function_body('%08X' % f) or []
    out = []
    for line in body:
        m = re.match(r'st\w*\s+r\d+,(-?\d+)\((r\d+)\)$', line)
        if m and m.group(2) != 'r1':
            r = reg(int(m.group(1)))
            if r is None:
                r = 'dev+%d' % int(m.group(1))
            if r not in out:
                out.append(r)
    return out


def table(anchor, sampler):
    # walk back to the first triplet, then forward while entries look like {code, code, value}
    lo = anchor
    while code(rd(lo - 12)) and code(rd(lo - 8)):
        lo -= 12
    hi = anchor
    while code(rd(hi + 12)) and code(rd(hi + 16)):
        hi += 12
    print('%s table 0x%08X..0x%08X: %d states' % ('sampler-state' if sampler else 'render-state', lo, hi + 12,
                                                  (hi + 12 - lo) // 12))
    for i, a in enumerate(range(lo, hi + 12, 12)):
        g, s, d = rd(a), rd(a + 4), rd(a + 8)
        f = struct.unpack('>f', struct.pack('>I', d))[0]
        dv = '0x%08X' % d if d > 0xFFFF and not (0.0009 < abs(f) < 1e6) else ('%g' % f if d > 0xFFFF else str(d))
        print('  %3d get sub_%08X set sub_%08X default %-10s writes %s' % (i, g, s, dv, ', '.join(writes(s)) or '-'))


table(ANCHOR_RS, False)
table(ANCHOR_SS, True)
