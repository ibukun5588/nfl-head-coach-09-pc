"""Map the 1.0 game's functions to the 1.2 title update's (docs/HANDOFF_RELEASE.md section 1).

Functions come from each image's .pdata. Each is hashed with its address-dependent bits masked (branch
displacements; addi/addis/ori/oris immediates; load/store displacements), so moved but unchanged code hashes the
same; identical copies pair in address order. Writes <out> ("1.0 1.2" per matched function), <out>.changed (1.0
functions without a match: changed or removed) and <out>.new (1.2 functions without a match), and reports how the
runtime's hand-written hooks map.

  py -3.11 tools/addrmap.py headcoach.basefile <1.2 basefile> <out>
"""
import collections
import os
import hashlib
import re
import struct
import sys

BASE = 0x82000000


def load(path):
    img = open(path, 'rb').read()
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    nsec = struct.unpack_from('<H', img, pe + 6)[0]
    opt = struct.unpack_from('<H', img, pe + 20)[0]
    secs = {}
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        name = img[o:o + 8].rstrip(b'\0').decode()
        vsz, va = struct.unpack_from('<II', img, o + 8)
        secs.setdefault(name, (va, vsz))
    pva, psz = secs['.pdata']
    funcs = []
    for k in range(0, psz, 8):
        begin, info = struct.unpack_from('>II', img, pva + k)
        n = (info >> 8) & 0x3FFFFF
        if begin and n:
            funcs.append((begin, n))
    return img, funcs


def norm(img, begin, n):
    words = struct.unpack_from('>%dI' % n, img, begin - BASE)
    out = []
    for w in words:
        op = w >> 26
        if op == 18:
            w &= 0xFC000003            # b/bl: displacement
        elif op == 16:
            w &= 0xFFFF0003            # bc: displacement
        elif op in (14, 15, 24, 25):   # addi/addis(lis)/ori/oris: immediates (often address halves)
            w &= 0xFFFF0000
        elif 32 <= op <= 55 or op in (58, 62):  # loads/stores: displacement (address low halves, struct offsets)
            w &= 0xFFFF0000
        out.append(w)
    return hashlib.sha1(struct.pack('>%dI' % n, *out)).digest()


def exact(img, begin, n):
    return hashlib.sha1(img[begin - BASE:begin - BASE + 4 * n]).digest()


a_img, a_f = load(sys.argv[1])
b_img, b_f = load(sys.argv[2])
print('functions: 1.0 %d, 1.2 %d' % (len(a_f), len(b_f)))
bn = collections.defaultdict(list)
for f in b_f:
    bn[(norm(b_img, *f), f[1])].append(f[0])
an = collections.defaultdict(list)
for f in a_f:
    an[(norm(a_img, *f), f[1])].append(f[0])

mapping, ambiguous, changed = {}, 0, []
for key, alist in an.items():
    blist = bn.get(key, [])
    if len(alist) == 1 and len(blist) == 1:
        mapping[alist[0]] = blist[0]
    elif len(alist) == len(blist) and blist:
        for x, y in zip(sorted(alist), sorted(blist)):   # same code several times: pair in address order
            mapping[x] = y
        ambiguous += len(alist)
    else:
        changed += alist
matched_b = set(mapping.values())
new = [f for f in b_f if f[0] not in matched_b]
ad, bd = dict(a_f), dict(b_f)
exact_same = sum(1 for x, y in mapping.items() if exact(a_img, x, ad[x]) == exact(b_img, y, bd[y]))
shifts = collections.Counter(y - x for x, y in mapping.items())
print('matched %d (%.2f%%), of which %d paired in address order (identical copies), %d byte-identical'
      % (len(mapping), 100 * len(mapping) / len(a_f), ambiguous, exact_same))
print('1.0 functions without a match (changed or removed): %d; 1.2 functions without a match (changed or new): %d'
      % (len(changed), len(new)))
print('most common address shifts:', [(hex(s), c) for s, c in shifts.most_common(6)])

# The runtime's hand-written hooks: do their 1.0 addresses map?
import glob
want = set()
for f in ['hooks_native.cpp', 'hooks_pacing.cpp', 'interp.cpp', 'gpu_live.cpp', 'imports_video.cpp', 'imports_sys.cpp',
          'hooks_diag.cpp', 'hooks_gameplay.cpp', 'hooks_simlog.cpp', 'hooks_autowin.cpp', 'xenos/d3d_frontend.cpp',
          'xenos/d3d_frontend.h', 'xenos/d3d_state.cpp', 'xenos/d3d_state.h', 'xenos/d3d_stream.h']:
    src = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'runtime', f)
    for m in re.findall(r'sub_(8[0-9A-F]{7})', open(src, encoding='utf-8', errors='replace').read()):
        want.add((f, int(m, 16)))
starts = dict(a_f)
miss = [(f, a) for f, a in sorted(want) if a in starts and a not in mapping]
notfn = [(f, a) for f, a in sorted(want) if a not in starts]
print('hand-written hook addresses: %d; mapped %d; unmatched (changed in 1.2) %d; not a .pdata function start %d'
      % (len(want), len(want) - len(miss) - len(notfn), len(miss), len(notfn)))
for f, a in miss:
    print('   unmatched: %-26s sub_%08X' % (f, a))
open(sys.argv[3], 'w').write(''.join('%08X %08X\n' % (x, mapping[x]) for x in sorted(mapping)))
open(sys.argv[3] + '.changed', 'w').write(''.join('%08X %d\n' % (a, starts[a]) for a in sorted(changed)))
open(sys.argv[3] + '.new', 'w').write(''.join('%08X %d\n' % f for f in sorted(new)))
