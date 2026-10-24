#!/usr/bin/env python3
"""Function-table audit: guest code addresses the game uses as functions that have no entry in the recompiled
function table (ppc_func_mapping.cpp). An indirect call to such an address stops the game with "indirect call to an
invalid guest address" (GitHub issue #1: 1.2, 0x82610FA0); a direct call to one was recompiled as `// ERROR <addr>`
and does nothing.

  py -3.11 tools/funcscan.py --out out --image headcoach.basefile --jt config/jt_final.toml
  py -3.11 tools/funcscan.py --out out-v12 --image headcoach-v12.basefile --jt config/v12/jt_final.toml

Sources of addresses, as the recompiler's discovery passes see them:
  data   aligned words in the data sections (vtables, callback tables);
  lisaddi  lis rX,hi + addi/ori rY,rX,lo within 8 instructions anywhere in the code;
  error  branches the recompiler could not resolve (`// ERROR` in the output: a bl/b/bc to a non-start).
Each address inside the code that is not a function start is classified:
  FUNCTION   the word before it is blr/bctr/zero padding and nothing in the containing function branches to it and no
             switch table lists it: unreachable from the function around it, so it starts a function of its own;
  label      a branch or switch-table target inside the containing function (an ordinary basic block);
  table      the address of an inline jump table (its words are code addresses) or of zero words;
  other      anything else (mid-block: usually a data value that happens to look like a code address).
Exit status 1 when a FUNCTION or an error address is left.
"""
import argparse, os, re, struct, sys

BASE = 0x82000000
BLR, BCTR = 0x4E800020, 0x4E800420


def sections(img):
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    n = struct.unpack_from('<H', img, pe + 6)[0]
    opt = struct.unpack_from('<H', img, pe + 20)[0]
    s = pe + 24 + opt
    out = []
    for i in range(n):
        name = img[s + i * 40:s + i * 40 + 8].rstrip(b'\0').decode('latin-1')
        vs, va = struct.unpack_from('<II', img, s + i * 40 + 8)
        ch = struct.unpack_from('<I', img, s + i * 40 + 36)[0]
        out.append((name, BASE + va, BASE + va + vs, bool(ch & 0x20)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--jt', required=True)
    ap.add_argument('--all', action='store_true', help='also list labels and other addresses')
    a = ap.parse_args()

    img = open(a.image, 'rb').read()
    secs = sections(img)
    code = [(lo, hi) for _, lo, hi, c in secs if c]
    data = [(lo, hi) for name, lo, hi, c in secs if not c and lo - BASE < len(img)]

    def word(addr):
        return struct.unpack_from('>I', img, addr - BASE)[0]

    def in_code(x):
        return (x & 3) == 0 and any(lo <= x < hi for lo, hi in code)

    mapping = open(os.path.join(a.out, 'ppc_func_mapping.cpp'), encoding='utf-8').read()
    bases = sorted(int(m, 16) for m in re.findall(r'\{ 0x([0-9A-F]+), \w+ \}', mapping))
    baseset = set(bases)
    import bisect

    def containing(x):
        i = bisect.bisect_right(bases, x) - 1
        if i < 0:
            return None, None
        return bases[i], bases[i + 1] if i + 1 < len(bases) else x + 4

    switch_labels = set()
    for m in re.finditer(r'labels = \[(.*?)\]', open(a.jt, encoding='utf-8').read(), re.S):
        switch_labels.update(int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', m.group(1)))

    def branch_targets(lo, hi):
        t = set()
        for p in range(lo, hi, 4):
            w = word(p)
            op = w >> 26
            if op == 18 and not (w & 2):                      # b / bl
                d = w & 0x03FFFFFC
                if d & 0x02000000: d -= 0x04000000
                t.add((p + d) & 0xFFFFFFFF)
            elif op == 16 and not (w & 2):                    # bc
                d = w & 0xFFFC
                if d & 0x8000: d -= 0x10000
                t.add((p + d) & 0xFFFFFFFF)
        return t

    refs = {}   # address -> set of where it came from

    def note(x, src):
        if in_code(x) and x not in baseset:
            refs.setdefault(x, set()).add(src)

    for lo, hi in data:
        end = min(hi, BASE + len(img)) & ~3
        for p in range(lo, end, 4):
            note(word(p), 'data@%08X' % p)

    for lo, hi in code:
        lisv, lisage = [0] * 32, [-1] * 32
        for p in range(lo, hi & ~3, 4):
            w = word(p)
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            for r in range(32):
                if lisage[r] >= 0: lisage[r] += 1
            if op == 15 and ra == 0:
                lisv[rd], lisage[rd] = (imm << 16) & 0xFFFFFFFF, 0
            elif op == 14 and 0 <= lisage[ra] <= 8:
                note((lisv[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF, 'lisaddi@%08X' % p)
            elif op == 24 and 0 <= lisage[ra] <= 8:
                note(lisv[ra] | imm, 'lisori@%08X' % p)

    errors = {}
    fre = re.compile(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)')
    for fn in sorted(os.listdir(a.out)):
        if not fn.startswith('ppc_recomp.') or not fn.endswith('.cpp'):
            continue
        cur, last = None, None
        for line in open(os.path.join(a.out, fn), encoding='utf-8'):
            m = fre.search(line)
            if m:
                cur = m.group(1)
                continue
            s = line.strip()
            if s.startswith('// ERROR '):
                x = int(s[9:], 16)
                errors.setdefault(x, set()).add('sub_%s: %s' % (cur, last))
            elif s.startswith('// ') and s[3:4].islower():
                last = s[3:]

    def classify(x):
        lo, hi = containing(x)
        prev = word(x - 4)
        boundary = prev in (BLR, BCTR, 0)
        if lo is not None and x in branch_targets(lo, hi) or x in switch_labels:
            return 'label', lo
        w0, w1 = word(x), word(x + 4)
        if w0 == 0 or (w0 >> 26) == 0 or (in_code(w0) and in_code(w1)):
            return 'table', lo          # an inline jump table (code addresses) or padding, not code
        return ('FUNCTION' if boundary else 'other'), lo

    found = 0
    print('%d function table entries, %d switch labels' % (len(bases), len(switch_labels)))
    for x in sorted(refs):
        kind, lo = classify(x)
        if kind == 'FUNCTION' or a.all:
            found += kind == 'FUNCTION'
            src = sorted(refs[x])
            print('%-8s %08X in sub_%08X  word before %08X  from %s%s' % (kind, x, lo or 0, word(x - 4),
                  ', '.join(src[:3]), ' (+%d)' % (len(src) - 3) if len(src) > 3 else ''))
    for x in sorted(errors):
        kind, lo = classify(x)
        src = sorted(errors[x])
        print('error    %08X in sub_%08X  word before %08X  (%s)  %s%s' % (x, lo or 0, word(x - 4), kind,
              '; '.join(src[:2]), ' (+%d)' % (len(src) - 2) if len(src) > 2 else ''))
    print('%d addresses look like missed functions, %d unresolved branch targets' % (found, len(errors)))
    return 1 if found or errors else 0


if __name__ == '__main__':
    sys.exit(main())
