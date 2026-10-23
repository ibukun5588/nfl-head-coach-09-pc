#!/usr/bin/env python3
"""Registers-as-locals audit: recompiled functions that read a local register their caller set.

    py -3.11 tools/locals_scan.py out [--list N] [--toml config/hc09.toml] [--image headcoach.basefile]
    py -3.11 tools/locals_scan.py out-v12 --toml config/v12/hc09.toml --image <1.2 headcoach.basefile>   (title update)

With cr/ctr/non_volatile_as_local the recompiler makes r14-r31, f14-f31, v14-v31/v64-v127, cr0-cr7 and ctr
zero-initialised C++ locals of each function (except the functions it keeps "in the context",
seh_funclets_in_context). Correct ABI code never reads one of these before writing it in the same function, except
the prologue's saves to the stack (std r31,-16(r1); stvx128 v127,r1,r12; mfcr), whose values nobody reads back
into the caller. A function that does read one depends on its caller's registers and gets 0 instead:
- chunks the analysis split off a function and the function reaches with `b` (shared epilogues of functions with
  try/catch: `addi r1,r31,N` restores the stack from the parent's frame pointer);
- hand-written code with its own convention (a codec keeping constants in v64-v127 across calls);
- C++ exception funclets (reached only through exception dispatch, which the runtime does not support anyway).

Method: per function a control-flow graph over the generated C++ (labels, goto, if-goto, switch, return), a forward
"written on every path" analysis, and live-in registers propagated through direct calls (a call reads its callee's
live-ins) to a fixpoint. Code unreachable from the entry (jump tables decoded as instructions) is ignored.

--toml writes (into config/hc09.toml: replaces its list) the functions that must keep their registers in the context
for XenonRecomp-hc09's `context_functions` option: every function with live-ins, its direct callers, and the codec reached from the
functions that save/restore the caller's v64-v127 (its table-dispatched kernels read registers set elsewhere).
"""
import argparse
import collections
import os
import re
import struct

FUNC = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+)\) \{')
LOCAL = (r'(?<![\w.])(r(?:1[4-9]|2\d|3[01])|f(?:1[4-9]|2\d|3[01])|'
         r'v(?:1[4-9]|2\d|3[01]|6[4-9]|[7-9]\d|1[01]\d|12[0-7])|cr[0-7]|ctr)(?![\w])')
REG = re.compile(LOCAL)
WRITE = re.compile(r'^(r\d+|f\d+|v\d+|cr\d|ctr)((?:\.\w+(?:\[\d+\])?)*)\s*(=(?!=)|\.compare|\.setFromMask)')
VWRITE = re.compile(r'^(?:simde_)?_?mm_store\w*\(\(?(?:[\w ]+\*\))?(v\d+)\.\w+,(.*)$')
CALL = re.compile(r'^(sub_[0-9A-F]{8})\(ctx, base\);')
STACK = re.compile(r'(ctx\.r1\.u32|ctx\.r12\.u32)')


class Stmt:
    __slots__ = ('reads', 'writes', 'call', 'asm', 'save')

    def __init__(self, reads=(), writes=(), call=None, asm='', save=False):
        self.reads, self.writes, self.call, self.asm, self.save = set(reads), set(writes), call, asm, save


def parse(lines):
    """-> blocks [stmts, successor labels, falls through], label -> block index."""
    blocks = [[[], [], True]]
    labels = {}
    asm = ''
    in_switch = False

    def new_block():
        blocks.append([[], [], True])

    for line in lines:
        s = line.strip()
        if not s:
            continue
        if s.startswith('//'):
            asm = s[2:].strip()
            continue
        if re.match(r'^[\w:<>]+ \w+\{\};$', s) or s == 'PPC_FUNC_PROLOGUE();':
            continue
        m = re.match(r'^(loc_[0-9A-F]+):$', s)
        if m:
            if blocks[-1][0] or blocks[-1][1] or not blocks[-1][2]:
                new_block()
            labels[m.group(1)] = len(blocks) - 1
            continue
        cur = blocks[-1]
        if in_switch:
            m = re.match(r'^goto (loc_[0-9A-F]+);', s)
            if m:
                cur[1].append(m.group(1))
            elif s == '}':
                in_switch = False
                cur[2] = False
                new_block()
            continue
        m = re.match(r'^switch \((.*)\) \{$', s)
        if m:
            cur[0].append(Stmt(reads=REG.findall(m.group(1)), asm=asm))
            in_switch = True
            continue
        m = re.match(r'^if \((.*)\) (goto (loc_[0-9A-F]+)|return);$', s)
        if m:
            cur[0].append(Stmt(reads=REG.findall(m.group(1)), asm=asm))
            if m.group(3):
                cur[1].append(m.group(3))
            new_block()
            continue
        m = re.match(r'^goto (loc_[0-9A-F]+);', s)
        if m:
            cur[1].append(m.group(1))
            cur[2] = False
            new_block()
            continue
        if s.startswith('return;'):
            cur[2] = False
            new_block()
            continue
        m = CALL.match(s)
        if m:
            cur[0].append(Stmt(call=m.group(1), asm=asm))
            continue
        regs = REG.findall(s)
        if not regs:
            continue
        mnem = asm.split()[0] if asm else ''
        ops = asm.split(None, 1)[1].replace(' ', '').split(',') if ' ' in asm else []
        wreg, rhs = None, s
        vw = VWRITE.match(s)
        w = WRITE.match(s)
        if vw:
            wreg, rhs = vw.group(1), vw.group(2)
        elif w:
            # A field / element write counts as a write of the register (the recompiler writes all fields of a
            # CR field, all elements of a vector, in one instruction's statements).
            wreg = w.group(1)
            rhs = s[w.end():] if w.group(3) != '=' else s.split('=', 1)[1]
        reads = REG.findall(rhs)
        selfop = mnem in ('xor', 'subf', 'vxor', 'vsubuwm', 'vxor128') and len(ops) == 3 and ops[1] == ops[2]
        save = (mnem.startswith('st') and STACK.search(s) is not None) or mnem in ('mfcr', 'mfocrf') or selfop
        cur[0].append(Stmt(reads=reads, writes=[wreg] if wreg else [], asm=asm, save=save))
    return blocks, labels


def livein(blocks, labels, callee_in):
    """Registers read before being written on some path from the entry -> the first instruction reading each."""
    n = len(blocks)
    succ = []
    for i, (_, lab, ft) in enumerate(blocks):
        s = [labels[l] for l in lab if l in labels]
        if ft and i + 1 < n:
            s.append(i + 1)
        succ.append(s)
    gen = []
    for stmts, _, _ in blocks:
        g = set()
        for st in stmts:
            g |= st.writes
        gen.append(g)
    inn = [None] * n                # None: not reached (yet)
    inn[0] = frozenset()
    work = collections.deque([0])
    while work:
        i = work.popleft()
        out = inn[i] | gen[i]
        for j in succ[i]:
            new = out if inn[j] is None else (inn[j] & out)
            if inn[j] is None or new != inn[j]:
                inn[j] = frozenset(new)
                work.append(j)
    res = {}
    for i, (stmts, _, _) in enumerate(blocks):
        if inn[i] is None:
            continue
        d = set(inn[i])
        for st in stmts:
            rd = set(callee_in.get(st.call, ())) if st.call else (set() if st.save else st.reads)
            for r in rd - d:
                res.setdefault(r, st.asm or st.call)
            d |= st.writes
    return res


def load(outdir):
    funcs = {}
    for f in sorted(os.listdir(outdir)):
        if not (f.startswith('ppc_recomp.') and f.endswith('.cpp')):
            continue
        name, body = None, []
        for line in open(os.path.join(outdir, f), encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                name, body = m.group(1), []
                continue
            if name is None:
                continue
            if line.startswith('}'):
                funcs[name] = parse(body)
                name = None
                continue
            body.append(line)
    return funcs


def analyse(funcs):
    callers = collections.defaultdict(set)
    for name, (blocks, _) in funcs.items():
        for stmts, _, _ in blocks:
            for st in stmts:
                if st.call:
                    callers[st.call].add(name)
    lin = {}
    work = collections.deque(funcs)
    queued = set(funcs)
    while work:
        name = work.popleft()
        queued.discard(name)
        blocks, labels = funcs[name]
        r = livein(blocks, labels, lin)
        changed = r.keys() != lin.get(name, {}).keys()
        lin[name] = r
        if changed:
            for c in callers[name]:
                if c not in queued:
                    queued.add(c)
                    work.append(c)
    return {k: v for k, v in lin.items() if v}, callers


def data_words(path='headcoach.basefile'):
    img = open(path, 'rb').read()
    return collections.Counter(struct.unpack(f'>{len(img) // 4}I', img[:len(img) // 4 * 4]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--list', type=int, default=60)
    ap.add_argument('--toml', help='write the context_functions list here')
    ap.add_argument('--image', default='headcoach.basefile', help='the image out was recompiled from (data counts)')
    a = ap.parse_args()
    funcs = load(a.out)
    lin, callers = analyse(funcs)
    words = data_words(a.image)
    kinds = collections.Counter(re.sub(r'\d+', 'N', r) for v in lin.values() for r in v)
    direct = {k for k in lin if callers.get(k)}
    print(f'functions: {len(funcs)}; with live-in local registers: {len(lin)} (reached by direct calls: {len(direct)}); '
          f'registers by kind: {dict(kinds)}')
    for name in sorted(lin)[:a.list]:
        v = lin[name]
        cs = sorted(callers.get(name, ()))
        print(f'{name} [callers {len(cs)}{": " + " ".join(cs[:3]) if cs else ""}; data {words[int(name[4:], 16)]}]: '
              + '; '.join(f'{r} at "{asm}"' for r, asm in list(v.items())[:5]))
    if a.toml:
        keep = set(lin)
        for k in lin:
            keep |= callers.get(k, set())
        # The codec: the callers of the v64-v127 save helpers and what they reach directly that touches a vector
        # register kept as a local (its kernels are also called through a function table, invisible here).
        savers = [k for k, v in lin.items() if sum(1 for r in v if r.startswith('v')) >= 32]
        roots = set()
        for s in savers:
            roots |= callers.get(s, set())
        callees = collections.defaultdict(set)
        for c, cs in callers.items():
            for x in cs:
                callees[x].add(c)
        seen, stack = set(), list(roots)
        while stack:
            f = stack.pop()
            if f in seen or f not in funcs:
                continue
            seen.add(f)
            stack.extend(callees.get(f, ()))
        codec = {f for f in seen if any(r.startswith('v') for stmts, _, _ in funcs[f][0]
                                        for st in stmts for r in (st.reads | st.writes))}
        keep |= codec | roots
        print(f'context_functions: {len(keep)} (live-in {len(lin)}, + direct callers, + codec {len(codec | roots)} '
              f'from {len(roots)} entries via {len(savers)} save helpers)')
        arr = 'context_functions = [\n' + ''.join(f'    0x{k[4:]},\n' for k in sorted(keep)) + ']\n'
        cfg = open(a.toml, encoding='utf-8').read() if os.path.exists(a.toml) else ''
        if '[main]' in cfg:   # config/hc09.toml: replace its list
            assert re.search(r'^context_functions = \[.*?^\]\n', cfg, re.S | re.M), 'no context_functions list to replace'
            cfg = re.sub(r'^context_functions = \[.*?^\]\n', lambda m: arr, cfg, count=1, flags=re.S | re.M)
        else:
            cfg = '# Generated by tools/locals_scan.py (XenonRecomp-hc09 context_functions).\n' + arr
        open(a.toml, 'w', encoding='utf-8', newline='\n').write(cfg)


if __name__ == '__main__':
    main()
