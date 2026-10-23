"""Generate runtime/game_addr_v12.h: the 1.2 title update's addresses for what the hand-written runtime names.

    py -3.11 tools/gen_addr.py <1.0 headcoach.basefile> <1.2 basefile> <addrmap.txt> [--out12 out-v12]

The runtime is written against 1.0 (docs/HANDOFF_M10_5.md step 4). In the 1.2 build (CMake HC09_GAME_VERSION=1.2)
the header is force-included into the runtime's sources (never the recompiled code):
- every guest function name `sub_8XXXXXXX` (and `__imp__sub_...`) the kept sources use is #defined to its 1.2
  function, so hooks override the right function; names with no 1.2 function are #defined to an undefined name;
- every `GameAddr(0x8XXXXXXX)` literal (runtime/game_addr.h) gets its 1.2 value: code by the function map, data by
  the instructions that build the address (lis + addi/load/store in exactly mapped code), section ends by section.

Function names map by tools/v12config.py's Mapper (offset in a matched function, identical gap code, or a unique
window of masked instructions) and are checked against a vote of their callers: the `bl` sites in exactly mapped
1.0 code, read at the same place in 1.2. A window match or a function the Mapper cannot place (an empty function
with identical copies) needs the callers to agree unanimously. Every result must be a function of --out12's
ppc_func_mapping.cpp. Fails (exit 1) on anything it cannot map or verify. Addresses only: no game code or data.
"""
import argparse
import bisect
import collections
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v12config as V

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = V.BASE
# Left out of the 1.2 build (CMakeLists.txt): gameplay tools until their addresses are mapped, generated diagnostics.
EXCLUDED = {'hooks_gameplay.cpp', 'hooks_simlog.cpp', 'hooks_autowin.cpp', 'hooks_sim.cpp', 'hooks_music.cpp',
            'hooks_d3d.cpp', 'imports_gen.cpp', 'game_addr_v12.h'}


def sections(img):
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    opt = struct.unpack_from('<H', img, pe + 20)[0]
    out = []
    for i in range(struct.unpack_from('<H', img, pe + 6)[0]):
        o = pe + 24 + opt + 40 * i
        vsz, va = struct.unpack_from('<II', img, o + 8)
        out.append((img[o:o + 8].rstrip(b'\0').decode(), BASE + va, vsz,
                    bool(struct.unpack_from('<I', img, o + 36)[0] & 0x20)))
    return out


def simm(w):
    return struct.unpack('>h', struct.pack('>H', w & 0xFFFF))[0]


class Refs:
    """Branch-and-link sites and lis-based address references of one image's code."""
    def __init__(self, img, secs):
        self.calls = collections.defaultdict(list)    # target -> bl sites
        self.data = collections.defaultdict(list)     # address -> (lis site, user site)
        for name, va, vsz, code in secs:
            if not code:
                continue
            words = struct.unpack_from('>%dI' % (vsz // 4), img, va - BASE)
            for i, w in enumerate(words):
                op = w >> 26
                pc = va + 4 * i
                if op == 18 and w & 3 == 1:
                    li = w & 0x3FFFFFC
                    self.calls[(pc + (li - 0x4000000 if li & 0x2000000 else li)) & 0xFFFFFFFF].append(pc)
                elif op == 15 and (w >> 16) & 31 == 0:          # lis rT,hi
                    rt, hi = (w >> 21) & 31, (w & 0xFFFF) << 16
                    for k in range(1, 9):
                        if i + k >= len(words):
                            break
                        u = words[i + k]
                        uop, ut, ua = u >> 26, (u >> 21) & 31, (u >> 16) & 31
                        if ua == rt and (uop == 14 or 32 <= uop <= 55):   # addi / D-form load or store from rT
                            self.data[(hi + simm(u)) & 0xFFFFFFFF].append((pc, va + 4 * (i + k)))
                        if ut == rt and uop not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55):
                            break                                  # rT overwritten (stores only read it)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('map')
    ap.add_argument('--out12', default=os.path.join(ROOT, 'out-v12'))
    ap.add_argument('--header', default=os.path.join(ROOT, 'runtime', 'game_addr_v12.h'))
    o = ap.parse_args()
    M = V.Mapper(o.a, o.b, o.map)
    sa, sb = sections(M.a), sections(M.b)
    ra, rb = Refs(M.a, sa), Refs(M.b, sb)
    fm12 = set(int(x, 16) for x in re.findall(r'\bsub_(8[0-9A-F]{7})\b',
                                               open(os.path.join(o.out12, 'ppc_func_mapping.cpp')).read()))
    errors = []

    def exact(a):
        b = M.map(a)
        return b if M.how.get(a) in ('function', 'gap') else None

    def callers_vote(a):
        v = collections.Counter()
        for s in ra.calls.get(a, ()):
            t = exact(s)
            if t is None:
                continue
            w = V.word(M.b, t)
            if w >> 26 == 18 and w & 3 == 1:
                li = w & 0x3FFFFFC
                v[(t + (li - 0x4000000 if li & 0x2000000 else li)) & 0xFFFFFFFF] += 1
        return v

    def map_code(a, need_function):
        b, how = M.map(a), M.how.get(a)
        votes = callers_vote(a) if need_function else collections.Counter()
        top = votes.most_common(2)
        note = '%s%s' % (how or 'unmapped', ', callers %s' % dict((('%08X' % k), n) for k, n in top) if votes else '')
        if how in ('function', 'gap'):
            if votes and (len(votes) > 1 or top[0][0] != b):
                return None, note + ': callers disagree'
            return b, note
        if votes and len(votes) == 1 and top[0][1] >= 2 and (b is None or b == top[0][0]):
            return top[0][0], note
        return None, note + ': not verified'

    # ---- names
    files = [f for f in glob.glob(os.path.join(ROOT, 'runtime', '**', '*.*'), recursive=True)
             if f.endswith(('.cpp', '.h')) and os.path.basename(f) not in EXCLUDED]
    names = collections.defaultdict(set)
    lits = collections.defaultdict(set)
    for f in files:
        src = open(f, encoding='utf-8', errors='replace').read()
        rel = os.path.relpath(f, os.path.join(ROOT, 'runtime')).replace('\\', '/')
        for m in re.findall(r'\bsub_(8[0-9A-F]{7})\b', src):
            names[int(m, 16)].add(rel)
        for m in re.findall(r'\bGameAddr\(\s*0x([0-9A-Fa-f]{8})u?\s*\)', src):
            lits[int(m, 16)].add(rel)
    out_names = []
    for a in sorted(names):
        b, note = map_code(a, True)
        if b is not None and b not in fm12:
            note += ': 0x%08X is not a 1.2 function' % b
            b = None
        print('  sub_%08X -> %s  (%s; %s)' % (a, 'sub_%08X' % b if b else 'NONE', note, ', '.join(sorted(names[a]))))
        if b is None:
            errors.append('sub_%08X (%s): %s' % (a, ', '.join(sorted(names[a])), note))
        out_names.append((a, b))
    src_set = {a for a, b in out_names}
    chains = [a for a, b in out_names if b in src_set and b != a]
    if chains:
        errors.append('names whose 1.2 name is another 1.0 name the runtime uses (macro chain): %s' %
                      ', '.join('sub_%08X' % a for a in chains))

    # ---- literals
    def section_of(secs, a):
        for name, va, vsz, code in secs:
            if va <= a <= va + vsz:
                return name, va, vsz, code
        return None

    out_lits = []
    for a in sorted(lits):
        s = section_of(sa, a)
        b, note = None, ''
        if s is None:
            note = 'outside the image'
        elif a == s[1] + s[2]:                                   # a section end: the same section's in 1.2
            i = sa.index(s)
            b, note = sb[i][1] + sb[i][2], 'end of %s' % s[0]
        elif s[3]:
            b, note = map_code(a, a in M.af or a in fm12)
        else:
            v = collections.Counter()
            for lis, use in ra.data.get(a, ()):
                tl, tu = exact(lis), exact(use)
                if tl is None or tu is None:
                    continue
                wl, wu = V.word(M.b, tl), V.word(M.b, tu)
                if wl >> 26 == 15 and (wu >> 26 == 14 or 32 <= wu >> 26 <= 55):
                    v[(((wl & 0xFFFF) << 16) + simm(wu)) & 0xFFFFFFFF] += 1
            note = 'data in %s, references %s' % (s[0], dict(('%08X' % k, n) for k, n in v.most_common(3)))
            if len(v) == 1:
                b = next(iter(v))
                if s[0] == '.rdata' and M.a[a - BASE:a - BASE + 16] != M.b[b - BASE:b - BASE + 16]:
                    note += ': contents differ'
                    b = None
            elif not v and s[0] == '.rdata':        # constant data no mapped code builds: its bytes, found once
                pat = M.a[a - BASE - 16:a - BASE + 16]
                rd = sb[[x[0] for x in sb].index('.rdata')]
                hits, k = [], M.b.find(pat, rd[1] - BASE, rd[1] + rd[2] - BASE)
                while k >= 0 and len(hits) < 2:
                    hits.append(k + BASE + 16)
                    k = M.b.find(pat, k + 1, rd[1] + rd[2] - BASE)
                note += ', the 32 bytes around it found %s in 1.2' % ('once' if len(hits) == 1 else '%d times' % len(hits))
                b = hits[0] if len(hits) == 1 else None
            else:
                note += ': not unanimous' if v else ': no references in mapped code'
        print('  0x%08X -> %s  (%s; %s)' % (a, '0x%08X' % b if b else 'NONE', note, ', '.join(sorted(lits[a]))))
        if b is None:
            errors.append('GameAddr(0x%08X) (%s): %s' % (a, ', '.join(sorted(lits[a])), note))
        out_lits.append((a, b))

    if errors:
        print('\n%d errors:' % len(errors))
        for e in errors:
            print('  ' + e)
        sys.exit(1)
    h = ['// Generated by tools/gen_addr.py (addresses only) - do not edit. docs/HANDOFF_M10_5.md step 4.',
         '// The 1.2 title update: force-included into the runtime\'s sources in the 1.2 build (CMakeLists.txt).',
         '#pragma once',
         '#include <cstdint>',
         '',
         '// Guest functions the hand-written runtime names: 1.0 name -> the 1.2 function.']
    for a, b in out_names:
        h.append('#define sub_%08X sub_%08X' % (a, b))
        h.append('#define __imp__sub_%08X __imp__sub_%08X' % (a, b))
    h += ['', '// GameAddr(1.0 address) literals (runtime/game_addr.h).', 'namespace hc09_v12 {',
          'struct AddrPair { uint32_t v10, v12; };', 'inline constexpr AddrPair kAddr[] = {']
    h += ['    { 0x%08X, 0x%08X },' % p for p in out_lits]
    h += ['};', '}  // namespace hc09_v12', '']
    open(o.header, 'w', newline='\n').write('\n'.join(h))
    print('wrote %s: %d names, %d literals' % (os.path.relpath(o.header, ROOT), len(out_names), len(out_lits)))


if __name__ == '__main__':
    main()
