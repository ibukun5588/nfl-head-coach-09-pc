#!/usr/bin/env python3
"""Static cross-references over the recompiled code (out/ppc_recomp.*.cpp keeps each guest
instruction as a `// insn` comment) and the image (headcoach.basefile, based at 0x82000000).
Run from the repo root. The index is cached in build-run/ppcxref.pkl (rebuilt when out/ is newer).

  py -3.11 tools/ppcxref.py dis 82874D28              disassembly of sub_82874D28 (comments + labels)
  py -3.11 tools/ppcxref.py refs 831D8AD4 ...         code that forms the address (lis+addi/ld/st chains)
  py -3.11 tools/ppcxref.py range 83025B00 83025B40   code references into [lo, hi)
  py -3.11 tools/ppcxref.py callers 8285B700          direct callers (bl/b); virtual calls do not show
  py -3.11 tools/ppcxref.py callees 8285B700
  py -3.11 tools/ppcxref.py dptr 8285B700             image words equal to the value (vtables, tables), and who refs them
  py -3.11 tools/ppcxref.py strs 828661E0             strings the function references
  py -3.11 tools/ppcxref.py strings "Sim|Music"       image strings matching a regex
  py -3.11 tools/ppcxref.py grep 'lwz r\\d+,112\\(' 'lwz r11,16\\(r11\\)'    functions matching every regex
  py -3.11 tools/ppcxref.py mgrep 'lwz (r\\d+),-19580\\(r\\d+\\)\\n(?:[^\\n]*\\n){0,12}?stb r\\d+,1\\(\\1\\)'
                                                      one regex over the whole disassembly (multi-line)
Value tracking is linear per function (lis, addi/addic/ori chains, mr; cleared on calls for r3-r12),
so a reference whose base register crosses a branch from elsewhere can be missed or, rarely, wrong.
Words of jump tables show up as odd instructions in `dis`; read them from the image instead.
Strings in the image: queries of the game's database are stored with reversed names ('MAET' = TEAM).
"""
import glob, os, pickle, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'out')
CACHE = os.path.join(ROOT, 'build-run', 'ppcxref.pkl')
BASE = 0x82000000
FRE = re.compile(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)')
IRE = re.compile(r'^\s*// (\S+)\s*(.*)$')
NOWRITE = ('st', 'cmp', 'b', 'tw', 'td', 'mt', 'dcb', 'sync', 'lwsync', 'eieio', 'isync', 'nop')


def sources():
    return glob.glob(os.path.join(OUT, 'ppc_recomp.*.cpp'))


def build():
    refs, calls, callees = {}, {}, {}
    for fn in sources():
        cur, v = None, {}
        for line in open(fn, encoding='utf-8', errors='replace'):
            m = FRE.search(line)
            if m:
                cur, v = int(m.group(1), 16), {}
                continue
            if cur is None:
                continue
            m = IRE.match(line)
            if not m:
                continue
            op, args = m.group(1), m.group(2)
            a = [x.strip() for x in args.split(',')]
            if op == 'lis':
                v[a[0]] = (int(a[1]) << 16) & 0xFFFFFFFF
                continue
            if op in ('bl', 'b') and a[0].startswith('0x'):
                t = int(a[0], 16)
                calls.setdefault(t, set()).add(cur)
                callees.setdefault(cur, set()).add(t)
                if op == 'bl':
                    for r in ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12'):
                        v.pop(r, None)
                continue
            if op in ('addi', 'addic') and len(a) == 3 and a[1] in v:
                addr = (v[a[1]] + int(a[2])) & 0xFFFFFFFF
                refs.setdefault(addr, []).append((cur, op + ' ' + args))
                v[a[0]] = addr
                continue
            if op == 'ori' and len(a) == 3 and a[1] in v:
                addr = v[a[1]] | int(a[2])
                refs.setdefault(addr, []).append((cur, op + ' ' + args))
                v[a[0]] = addr
                continue
            if op == 'mr' and len(a) == 2 and a[1] in v:
                v[a[0]] = v[a[1]]
                continue
            mm = re.match(r'(-?\d+)\((r\d+)\)', a[-1]) if a else None
            if mm and mm.group(2) in v:
                refs.setdefault((v[mm.group(2)] + int(mm.group(1))) & 0xFFFFFFFF, []).append((cur, op + ' ' + args))
            if a and not op.startswith(NOWRITE):
                v.pop(a[0], None)
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    pickle.dump((refs, calls, callees), open(CACHE, 'wb'))
    return refs, calls, callees


def index():
    newest = max(os.path.getmtime(f) for f in sources())
    if os.path.exists(CACHE) and os.path.getmtime(CACHE) >= newest:
        return pickle.load(open(CACHE, 'rb'))
    print('(building the index, ~1 min)', file=sys.stderr)
    return build()


IMG = None


def img():
    global IMG
    if IMG is None:
        IMG = open(os.path.join(ROOT, 'headcoach.basefile'), 'rb').read()
    return IMG


def cstr(addr):
    d = img(); o = addr - BASE
    if not 0 <= o < len(d):
        return ''
    t = d[o:d.find(b'\0', o)][:100]
    return t.decode('latin1') if len(t) > 3 and all(32 <= c < 127 or 0x80 <= c < 0x90 for c in t) else ''


def function_body(name):
    pat = 'PPC_FUNC_IMPL(__imp__sub_%s)' % name
    for fn in sources():
        text = open(fn, encoding='utf-8', errors='replace').read()
        i = text.find(pat)
        if i < 0:
            continue
        j = text.find('PPC_FUNC_IMPL', i + len(pat))
        k = text.find('__attribute__', i + len(pat))
        end = min(x for x in (j, k, len(text)) if x >= 0)
        out = []
        for line in text[i:end].split('\n')[1:]:
            s = line.strip()
            if s.startswith('// '):
                out.append(s[3:])
            elif s.startswith('loc_'):
                out.append(s)
        return out
    return None


def funcs_matching(pats, multi=False):
    for fn in sources():
        cur, body = None, []

        def done():
            if cur is None:
                return
            if multi:
                m = pats[0].search('\n'.join(body))
                if m:
                    print('sub_' + cur, repr(m.group(0)[:160]))
            elif all(any(p.search(l) for l in body) for p in pats):
                print('sub_' + cur)
        for line in open(fn, encoding='utf-8', errors='replace'):
            m = FRE.search(line)
            if m:
                done(); cur, body = m.group(1), []
                continue
            s = line.strip()
            if s.startswith('// '):
                body.append(s[3:])
            elif s.startswith('loc_'):
                body.append(s)
        done()


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    cmd, args = sys.argv[1], sys.argv[2:]
    h = lambda s: int(s.replace('sub_', ''), 16)
    if cmd == 'dis':
        body = function_body(args[0].replace('sub_', '').upper())
        print('\n'.join(l if l.startswith('loc_') else '    ' + l for l in body) if body else 'not found')
    elif cmd == 'strings':
        p = re.compile(args[0])
        for m in re.finditer(rb'[\x20-\x7e\x80-\x8f]{4,}', img()):
            s = m.group().decode('latin1')
            if p.search(s):
                print('%08X %s' % (BASE + m.start(), s[:160].encode('ascii', 'replace').decode()))
    elif cmd == 'grep':
        funcs_matching([re.compile(a) for a in args])
    elif cmd == 'mgrep':
        funcs_matching([re.compile(args[0], re.S)], multi=True)
    else:
        refs, calls, callees = index()
        if cmd == 'refs':
            for a in args:
                for f, i in refs.get(h(a), []):
                    print('%08X  sub_%08X  %s' % (h(a), f, i))
        elif cmd == 'range':
            lo, hi = h(args[0]), h(args[1])
            for a in sorted(x for x in refs if lo <= x < hi):
                for f, i in refs[a]:
                    print('%08X  sub_%08X  %s' % (a, f, i))
        elif cmd in ('callers', 'callees'):
            src = calls if cmd == 'callers' else callees
            for a in args:
                print('%s %08X: %s' % (cmd, h(a), ' '.join('%08X' % c for c in sorted(src.get(h(a), [])))))
        elif cmd == 'dptr':
            d = img()
            for a in args:
                pat, o = struct.pack('>I', h(a)), 0
                while (o := d.find(pat, o)) >= 0:
                    if o % 4 == 0:
                        w = BASE + o
                        print('%08X at %08X  refs: %s' % (h(a), w, ', '.join('sub_%08X' % f for f, _ in refs.get(w, [])[:6])))
                    o += 4
        elif cmd == 'strs':
            f = h(args[0])
            for a in sorted(refs):
                if any(x == f for x, _ in refs[a]) and BASE <= a < 0x82300000 and cstr(a):
                    print('%08X %r' % (a, cstr(a)))
        else:
            print(__doc__)


if __name__ == '__main__':
    main()
