"""Make the 1.2 title update's recompiler config (config/v12/) from 1.0's (docs/HANDOFF_M10_5.md step 2).

Inputs: the two images, tools/addrmap.py's 1.0 -> 1.2 function map, and XenonAnalyse's switch tables for both
xex files (1.0's jt_final.toml = XenonAnalyse's 1,744 tables + 2 hand-resolved inline tables, M2 section 1).

  XenonAnalyse <1.0 default.xex> jt_analyse_v10.toml;  XenonAnalyse <1.2 default.xex> jt_analyse_v12.toml
  py -3.11 tools/addrmap.py headcoach.basefile <1.2 basefile> addrmap.txt
  py -3.11 tools/v12config.py <1.0 basefile> <1.2 basefile> addrmap.txt jt_analyse_v10.toml jt_analyse_v12.toml

Writes config/v12/hc09.toml (1.0's options; ABI helpers and forced function spans mapped; context_functions kept
from the existing file) and config/v12/jt_final.toml (1.2's XenonAnalyse output + the hand tables mapped and read
back from the 1.2 image), and lists every address it could not map or verify ("problems", 0 expected).
Addresses only: nothing of the game's code or data goes into the files.

Checks: ABI helpers byte-identical to their blr; 1.0's tables paired with 1.2's (labels mapped, or the same layout:
register, count, label offsets from the base); every forced span still holds its tables' labels; M1's missed-table
audit (every bctr no table covers in 1.2 either has a 1.0 counterpart or is a pointer tail call).

An address maps by its offset in the containing .pdata function when that function matched; code outside .pdata
(ABI helpers, gap functions) when the stretch between the matched functions around it has one shift and the same
masked instructions; otherwise by a unique window of masked instructions around it within the neighbours' shifts.
A span no address maps is placed through its switch table's 1.2 partner and ends, as M2 made them, at the first
function start after the table's highest label.

context_functions (registers kept in the context; without them the intro movie is blank): recompile with an empty
list into out-v12-noctx, then `py -3.11 tools/locals_scan.py out-v12-noctx --toml config/v12/hc09.toml --image <1.2
basefile>`, recompile with the list and audit (`locals_scan.py out-v12`: 0 live-ins). The scan of an output built
with the list finds only what the list misses. Done this way 1.0 gives its committed 123 functions, 1.2 gives 124.
"""
import bisect
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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
    funcs = {}
    for k in range(0, psz, 8):
        begin, info = struct.unpack_from('>II', img, pva + k)
        n = (info >> 8) & 0x3FFFFF
        if begin and n:
            funcs[begin] = 4 * n
    return img, funcs


def masked(w):
    """The instruction with its address-dependent bits cleared (as tools/addrmap.py hashes them)."""
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op == 16 or op in (14, 15, 24, 25) or 32 <= op <= 55 or op in (58, 62):
        return w & 0xFFFF0000
    return w


def word(img, a):
    return struct.unpack_from('>I', img, a - BASE)[0]


def parse_switches(path):
    out = []
    for b in re.split(r'(?=^\[\[switch\]\])', open(path).read(), flags=re.M):
        m = re.search(r'base = (0x[0-9A-Fa-f]+)\s+r = (\d+)\s+default = (0x[0-9A-Fa-f]+)\s+labels = \[([^\]]*)\]', b)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2)), int(m.group(3), 16),
                        [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', m.group(4))]))
    return out


def fmt_switch(base, r, default, labels):
    return ('[[switch]]\nbase = 0x%08X\nr = %d\ndefault = 0x%X\nlabels = [\n' % (base, r, default) +
            ''.join('    0x%08X,\n' % l for l in labels) + ']\n')


class Mapper:
    def __init__(self, a_path, b_path, map_path):
        self.a, self.af = load(a_path)
        self.b, self.bf = load(b_path)
        self.m = {}
        for line in open(map_path):
            x, y = line.split()
            self.m[int(x, 16)] = int(y, 16)
        self.starts = sorted(self.af)
        self.mstarts = sorted(self.m)
        self.am, self.bm = self.masked_image(self.a), self.masked_image(self.b)
        self.how = {}

    @staticmethod
    def masked_image(img):
        n = len(img) // 4
        return struct.pack('>%dI' % n, *map(masked, struct.unpack_from('>%dI' % n, img)))

    def same(self, a0, b0, nbytes):
        return all(masked(word(self.a, a0 + i)) == masked(word(self.b, b0 + i)) for i in range(0, nbytes, 4))

    def shift_range(self, a):
        """Shifts code at 1.0 address a can have: the median shifts of the matched functions before and after it,
        widened by 0x400 (identical copies paired in address order make single shifts unreliable)."""
        j = bisect.bisect_right(self.mstarts, a)
        def med(fs):
            s = sorted(self.m[f] - f for f in fs)
            return s[len(s) // 2] if s else 0
        sp, sn = med(self.mstarts[max(0, j - 5):j]), med(self.mstarts[j:j + 5])
        return min(sp, sn) - 0x400, max(sp, sn) + 0x400

    def search(self, a, lo, hi, smin, smax):
        """Unique 1.2 place of the masked 1.0 words [a+lo, a+hi) at a shift in [smin, smax] (word-aligned)."""
        pat = self.am[a + lo - BASE:a + hi - BASE]
        start, end = a + lo + smin - BASE, a + hi + smax - BASE
        hits, k = [], self.bm.find(pat, start, end)
        while k >= 0 and len(hits) < 2:
            if k % 4 == 0:
                hits.append(k + BASE - lo)
            k = self.bm.find(pat, k + 1, end)
        return hits[0] if len(hits) == 1 else None

    def map(self, a):
        """1.2 address of 1.0 code address a, or None."""
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and a < self.starts[i] + self.af[self.starts[i]] and self.starts[i] in self.m:
            f = self.starts[i]
            self.how[a] = 'function'
            return self.m[f] + (a - f)
        j = bisect.bisect_right(self.mstarts, a)
        if 0 < j < len(self.mstarts):
            # outside .pdata: the matched functions before and after have one shift and the code between is the same
            p, n = self.mstarts[j - 1], self.mstarts[j]
            s = self.m[p] - p
            lo = p + self.af[p]
            if a >= lo and self.m[n] - n == s and self.same(lo, lo + s, n - lo):
                self.how[a] = 'gap'
                return a + s
        # changed function or a gap with changes in it: the code around a, found once near where it should be
        smin, smax = self.shift_range(a)
        for lo, hi in ((-24, 24), (0, 48), (-44, 4), (0, 24), (-20, 4)):
            b = self.search(a, lo, hi, smin, smax)
            if b is not None:
                self.how[a] = 'window'
                return b
        return None


def main():
    a_path, b_path, map_path, jt10_path, jt12_path = sys.argv[1:6]
    M = Mapper(a_path, b_path, map_path)
    cfg = open(os.path.join(ROOT, 'config', 'hc09.toml'), encoding='utf-8').read()
    problems = []

    # ---- ABI helpers: mapped and byte-identical (position-independent register save/restore runs)
    def abi(mo):
        key, a = mo.group(1), int(mo.group(2), 16)
        b = M.map(a)
        if b is None:
            problems.append('ABI helper %s 0x%08X: no mapping' % (key, a))
            return mo.group(0)
        n = 4                                     # the helper runs to its blr
        while word(M.a, a + n - 4) != 0x4E800020 and n < 0x400:
            n += 4
        ok = M.a[a - BASE:a - BASE + n] == M.b[b - BASE:b - BASE + n]
        print('  %-22s 0x%08X -> 0x%08X  %s' % (key, a, b, 'same %d bytes to the blr' % n if ok else 'BYTES DIFFER'))
        if not ok:
            problems.append('ABI helper %s: bytes differ' % key)
        return '%s = 0x%08X' % (key, b)
    print('ABI helpers:')
    cfg = re.sub(r'^(\w+_address) = (0x[0-9A-Fa-f]+)', abi, cfg, flags=re.M)

    # ---- switch tables
    final10 = parse_switches(os.path.join(ROOT, 'config', 'jt_final.toml'))
    an10, an12 = parse_switches(jt10_path), parse_switches(jt12_path)
    an10_bases = {s[0] for s in an10}
    hand = [s for s in final10 if s[0] not in an10_bases]
    print('switch tables: 1.0 jt_final %d = XenonAnalyse %d + hand %d; XenonAnalyse on 1.2: %d'
          % (len(final10), len(an10), len(hand), len(an12)))
    t12 = {s[0]: s for s in an12}
    # pair 1.0's tables with 1.2's: same register and labels, the labels either mapped one by one or at the same
    # offsets from the table base (the same switch where its blocks are too short to place each label)
    def layout(t10, t12s, b):
        base, r, d, labels = t10
        return t12s[1] == r and len(t12s[3]) == len(labels) and all(y - b == l - base for l, y in zip(labels, t12s[3]))
    pairs, by_labels, by_layout = {}, 0, 0
    for t in an10:
        b = M.map(t[0])
        if b in t12 and t12[b][1] == t[1] and t12[b][3] == [M.map(l) for l in t[3]]:
            pairs[t[0]] = b
            by_labels += 1
        elif b in t12 and layout(t, t12[b], b):
            pairs[t[0]] = b
            by_layout += 1
    # second pass: an unpaired 1.0 table whose base did not map, against the unpaired 1.2 tables in its shift range
    used = set(pairs.values())
    free = sorted(x for x in t12 if x not in used)
    by_near = 0
    for t in an10:
        if t[0] in pairs:
            continue
        smin, smax = M.shift_range(t[0])
        cand = [x for x in free[bisect.bisect_left(free, t[0] + smin):bisect.bisect_right(free, t[0] + smax)]
                if x not in used and layout(t, t12[x], x)]
        if len(cand) == 1:
            pairs[t[0]] = cand[0]
            used.add(cand[0])
            by_near += 1
    # last pass: a unique same-layout table anywhere among the unpaired 1.2 tables (code the update moved away)
    by_far = 0
    for t in an10:
        if t[0] in pairs:
            continue
        cand = [x for x in t12 if x not in used and layout(t, t12[x], x)]
        if len(cand) == 1:
            pairs[t[0]] = cand[0]
            used.add(cand[0])
            by_far += 1
    print('  1.0 tables paired with 1.2 tables: %d (labels mapped %d, same layout %d, same layout nearby %d, '
          'moved %d)' % (len(pairs), by_labels, by_layout, by_near, by_far))
    lost = [t for t in an10 if t[0] not in pairs]
    print('  1.0 tables without a 1.2 counterpart: %d' % len(lost))
    for base, r, d, labels in lost:
        print('    0x%08X r%d %d labels (1.0 function %s)' % (
            base, r, len(labels), 'changed' if M.map(base) is None or M.how.get(base) == 'window' else 'matched'))
    extra = [t for t in an12 if t[0] not in used]
    print('  1.2 tables without a 1.0 counterpart: %d' % len(extra))
    bstarts, bmatched = sorted(M.bf), set(M.m.values())
    for base, r, d, labels in extra:
        f = bstarts[bisect.bisect_right(bstarts, base) - 1]
        inside = base < f + M.bf[f]
        print('    0x%08X r%d %d labels, %s 1.2 function 0x%08X%s' % (
            base, r, len(labels), 'in' if inside else 'after', f, '' if f in bmatched else ' (changed or new)'))

    # ---- forced function spans (M2: switch regions without unwind records): start and end mapped
    spans_out, bad_spans, resized = [], 0, 0
    def span(mo):
        nonlocal bad_spans, resized
        a, size = int(mo.group(1), 16), int(mo.group(2), 16)
        b, e = M.map(a), M.map(a + size)
        if e is None:                                  # the span may end where the code after it changed
            e = M.map(a + size - 4)
            e = e + 4 if e is not None else None
        if b is None or e is None or e <= b:
            # placed through a switch table inside it that has a 1.2 partner (same layout), ended by M2's rule: at
            # the first 1.2 function start after the table's highest label
            inside = [t for t in an10 if a <= t[0] < a + size and t[0] in pairs]
            if inside:
                t = inside[0]
                b = pairs[t[0]] - (t[0] - a)
                hi = max(t12[pairs[t[0]]][3])
                e = bstarts[bisect.bisect_right(bstarts, hi)]
                print('  span 0x%08X size 0x%X -> 0x%08X size 0x%X (through its table 0x%08X -> 0x%08X)'
                      % (a, size, b, e - b, t[0], pairs[t[0]]))
                spans_out.append((a, size, b, e - b))
                return '{ address = 0x%08X, size = 0x%X }' % (b, e - b)
            bad_spans += 1
            problems.append('span 0x%08X size 0x%X: start %s end %s' % (
                a, size, b and hex(b), e and hex(e)))
            return '# 1.0 span not mapped: ' + mo.group(0)
        if e - b != size:
            resized += 1
            print('  span 0x%08X size 0x%X -> 0x%08X size 0x%X (code inside changed)' % (a, size, b, e - b))
        spans_out.append((a, size, b, e - b))
        return '{ address = 0x%08X, size = 0x%X }' % (b, e - b)
    cfg = re.sub(r'\{ address = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', span, cfg)
    print('forced function spans: %d mapped (%d with a new size), %d not' % (len(spans_out), resized, bad_spans))

    # each span must still hold its tables' labels (M2: a span runs to the first function after the highest label)
    t12_sorted = sorted(t12)
    for a, size, b, nsize in spans_out:
        for base in t12_sorted[bisect.bisect_left(t12_sorted, b):bisect.bisect_left(t12_sorted, b + nsize)]:
            out_of = [l for l in t12[base][3] if not b <= l < b + nsize]
            if out_of:
                problems.append('span 0x%08X (1.2 0x%08X size 0x%X): table 0x%08X has %d labels outside it'
                                % (a, b, nsize, base, len(out_of)))
    print('mapped addresses by kind:', dict(collections.Counter(M.how.values())))

    # ---- context_functions: kept from an existing config/v12/hc09.toml (tools/locals_scan.py wrote it); otherwise
    # 1.0's mapped as a first list. The scan must run on an output recompiled with an empty list: on one built with
    # the list, the functions in it keep their registers in the context and show no live-ins (it is then the audit,
    # 0 live-ins expected). 1.0's list is reproduced exactly that way (123 functions).
    m = re.search(r'context_functions = \[\n(.*?)\]', cfg, re.S)
    v12_path = os.path.join(ROOT, 'config', 'v12', 'hc09.toml')
    old = re.search(r'context_functions = \[\n(.*?)\]', open(v12_path, encoding='utf-8').read(), re.S) \
        if os.path.exists(v12_path) else None
    if old:
        mapped = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', old.group(1))]
        print('context_functions: %d kept from config/v12/hc09.toml (tools/locals_scan.py)' % len(mapped))
    else:
        ctx = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', m.group(1))]
        mapped = [b for b in map(M.map, ctx) if b is not None]
        print('context_functions: %d of %d mapped, a first list: rewrite it with tools/locals_scan.py' % (len(mapped), len(ctx)))
    cfg = cfg[:m.start(1)] + ''.join('    0x%08X,\n' % b for b in sorted(mapped)) + cfg[m.end(1):]
    cfg = cfg.replace('`py -3.11 tools/locals_scan.py out --toml config/hc09.toml` (needs XenonRecomp-hc09 a3f35e8).',
                      '`py -3.11 tools/locals_scan.py out-v12-noctx --toml config/v12/hc09.toml --image <1.2 basefile>`\n'
                      '# on an output recompiled with an empty list (docs/HANDOFF_M10_5.md); needs XenonRecomp-hc09 a3f35e8.')
    cfg = ('# The 1.2 title update (execution version 0.0.2.1): written by tools/v12config.py from config/hc09.toml\n'
           '# (addresses mapped with tools/addrmap.py); same options as 1.0.\n' + cfg)

    # ---- missed tables (M1's audit): every bctr no table covers; those in 1.2 without a 1.0 counterpart (by the
    # masked words before them) must be virtual or function-pointer tail calls (lwz rX,d(rY); mtctr rX; bctr)
    def code_bctrs(img):
        pe = struct.unpack_from('<I', img, 0x3C)[0]
        opt = struct.unpack_from('<H', img, pe + 20)[0]
        out = []
        for i in range(struct.unpack_from('<H', img, pe + 6)[0]):
            o = pe + 24 + opt + 40 * i
            vsz, va = struct.unpack_from('<II', img, o + 8)
            if struct.unpack_from('<I', img, o + 36)[0] & 0x20:          # code section
                out += [BASE + k for k in range(va, va + vsz, 4) if img[k:k + 4] == b'\x4E\x80\x04\x20']
        return out
    def uncovered(img, tables):
        bases = sorted(t[0] for t in tables)
        return [x for x in code_bctrs(img)
                if not (bisect.bisect_right(bases, x) and x - bases[bisect.bisect_right(bases, x) - 1] <= 0x60)]
    u10 = uncovered(M.a, final10)
    u12 = uncovered(M.b, an12 + [(b, 0, 0, []) for b in (M.map(t[0]) for t in hand) if b])
    before = collections.Counter(M.am[x - BASE - 48:x - BASE + 4] for x in u10)
    new = []
    for y in u12:
        k = M.bm[y - BASE - 48:y - BASE + 4]
        if before[k]:
            before[k] -= 1
        else:
            new.append(y)
    def pointer_call(y):
        """mtctr rX; bctr where rX last came from lwz rX,d(rY) (an indexed load would be a table)."""
        mt = word(M.b, y - 4)
        if mt >> 26 != 31 or (mt >> 1) & 0x3FF != 467:
            return False
        rx = (mt >> 21) & 31
        for k in range(8, 40, 4):
            w = word(M.b, y - k)
            if w >> 26 in (16, 18, 19) or (w >> 26 == 31 and (w >> 1) & 0x3FF == 467):
                continue                                # branches, mtctr: read rX at most
            if (w >> 21) & 31 == rx and (w >> 26 == 32 or w >> 26 == 31 or w >> 26 in (14, 15, 21)):
                return w >> 26 == 32                    # the first writer of rX: lwz (else lwzx/add/rlwinm/...)
        return False
    calls = [y for y in new if pointer_call(y)]
    print('bctr not covered by a table: 1.0 %d, 1.2 %d; 1.2 ones not in 1.0: %d, of them pointer tail calls: %d'
          % (len(u10), len(u12), len(new), len(calls)))
    for y in sorted(set(new) - set(calls)):
        problems.append('bctr at 0x%08X: not covered by a table, not a pointer tail call: review' % y)

    # hand tables: map, then read the inline table the lis/addi before the bctr points at in the 1.2 image
    hand12 = []
    for base, r, d, labels in hand:
        b = M.map(base)
        ml = [M.map(l) for l in labels]
        if b is None or None in ml:
            problems.append('hand table 0x%08X: not mapped' % base)
            continue
        hi, lo = word(M.b, b), word(M.b, b + 4)
        assert hi >> 26 == 15 and lo >> 26 == 14, 'hand table 0x%08X: no lis/addi at 0x%08X' % (base, b)
        tab = ((hi & 0xFFFF) << 16) + struct.unpack('>h', struct.pack('>H', lo & 0xFFFF))[0]
        img = [word(M.b, tab + 4 * k) for k in range(len(labels))]
        ok = img == ml
        print('  hand table 0x%08X -> 0x%08X r%d %d labels: table at 0x%08X in the 1.2 image %s'
              % (base, b, r, len(labels), tab, 'equals the mapped labels' if ok else 'DIFFERS'))
        if not ok:
            problems.append('hand table 0x%08X: 1.2 image table differs' % base)
        hand12.append((b, r, d, img))
    jt = open(jt12_path).read().rstrip('\n') + '\n\n# ---- HAND-RESOLVED (inline-in-text tables; 1.0\'s mapped by ' \
         'tools/v12config.py, read back from the 1.2 image) ----\n' + '\n'.join(fmt_switch(*s) for s in hand12)

    os.makedirs(os.path.join(ROOT, 'config', 'v12'), exist_ok=True)
    open(os.path.join(ROOT, 'config', 'v12', 'hc09.toml'), 'w', encoding='utf-8', newline='\n').write(cfg)
    open(os.path.join(ROOT, 'config', 'v12', 'jt_final.toml'), 'w', newline='\n').write(jt)
    print('wrote config/v12/hc09.toml, config/v12/jt_final.toml (%d tables)' % (len(an12) + len(hand12)))
    print('problems: %d' % len(problems))
    for p in problems:
        print('  ' + p)


if __name__ == '__main__':
    main()
