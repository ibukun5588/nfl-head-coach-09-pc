#!/usr/bin/env python3
"""hc09rt.dll's export list for the split build (CMake HC09_SPLIT, runtime/split_abi.h), from the runtime's objects.

    python tools/split_def.py <llvm-nm> <runtime/split_hooks.inc> <out.def> <object dir> [runtime/game_addr_v12.h]

Exports what the recompiled code calls in the runtime: the kernel imports (?__imp__X), GuestBadIndirect,
GuestTimeBase, GuestUnresolvedBranch, and each hook of split_hooks.inc as hc09hook_sub_X. Fails when the runtime hooks a guest function
that split_hooks.inc does not list (hc09.exe would then call the generated body and skip the hook), or the other way.
1.2: split_hooks.inc names 1.0's functions; game_addr_v12.h maps them to 1.2's (the export names stay the 1.0 ones,
as in runtime/split_glue.cpp, which is compiled with the same header).
"""
import os
import re
import subprocess
import sys

SIG = '@@YAXAEIAUPPCContext@@PEAE@Z'


def main():
    nm, inc, out, objdir = sys.argv[1:5]
    rename = {}
    if len(sys.argv) > 5:
        rename = dict(re.findall(r'^#define (sub_[0-9A-F]+) (sub_[0-9A-F]+)', open(sys.argv[5], encoding='utf-8').read(), re.M))
    objs = [os.path.join(d, f) for d, _, fs in os.walk(objdir) for f in fs if f.endswith('.obj')]
    text = subprocess.run([nm, '--defined-only', '--no-sort'] + objs, capture_output=True, text=True, check=True).stdout
    defined = {p[2] for p in (l.split() for l in text.splitlines()) if len(p) == 3 and p[1] in 'TtW'}
    names = re.findall(r'^HC09_HOOK\((sub_[0-9A-F]+)\)', open(inc, encoding='utf-8').read(), re.M)
    hooks = [rename.get(n, n) for n in names]   # the functions the runtime defines in this game version
    have = {s[1:-len(SIG)] for s in defined if re.fullmatch(r'\?sub_[0-9A-F]+' + re.escape(SIG), s)}
    missing, extra = sorted(set(hooks) - have), sorted(have - set(hooks))
    if missing or extra:
        sys.exit(f'split_def: {inc} does not match the runtime\'s hooks: missing {missing}, not listed {extra}')
    imports = sorted(s for s in defined if re.fullmatch(r'\?__imp__\w+' + re.escape(SIG), s))
    guest = sorted(s for s in defined if s.startswith(('?GuestBadIndirect@@', '?GuestTimeBase@@', '?GuestUnresolvedBranch@@')))
    if len(guest) != 3:
        sys.exit(f'split_def: GuestBadIndirect/GuestTimeBase/GuestUnresolvedBranch not found ({guest})')
    lines = ['LIBRARY hc09rt', 'EXPORTS'] + [f'    {s}' for s in imports + guest]
    lines += [f'    hc09hook_{n}=?{h}{SIG}' for n, h in zip(names, hooks)]
    new = '\n'.join(lines) + '\n'
    if not os.path.exists(out) or open(out).read() != new:
        open(out, 'w').write(new)
    print(f'split_def: {len(imports)} kernel imports, {len(guest)} guest helpers, {len(hooks)} hooks -> {out}')


if __name__ == '__main__':
    main()
