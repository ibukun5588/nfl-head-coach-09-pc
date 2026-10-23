#!/usr/bin/env python3
"""The player's build of hc09.exe in the split build (docs/HANDOFF_M11.md step 2, runtime/split_abi.h): compiles the
recompiled code and runtime/split_glue.cpp with clang-cl against toolchain/include only (no Visual Studio, no Windows
SDK) and links them with lld-link against hc09rt.dll's import library and a ucrtbase import library made here; no
MSVC library is used (/NODEFAULTLIB). Prototype of the installer's build step.

    python tools/player_build.py --rt <dir with hc09rt.dll, hc09rt.lib> --obj <object dir> --out <dir> [--gen out]
        [--llvm "C:/Program Files/LLVM/bin"] [--jobs N]

Objects newer than their source are kept. hc09.exe is written to --out; copy it beside hc09rt.dll to run.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The flags of the CMake build for the recompiled code (CMakeLists.txt), with /X: no system include paths.
GUEST_FLAGS = ['/c', '/X', '/DWIN32', '/D_WINDOWS', '/EHsc', '/O2', '/Ob2', '/DNDEBUG', '-std:c++20', '-MD',
               '/clang:-fno-strict-aliasing', '/w', '/clang:-march=x86-64-v3', '/clang:-ffp-contract=off',
               '-DHC09_GUEST_TU']
GLUE_FLAGS = ['/c', '/X', '/O2', '-std:c++20', '-MD', '/clang:-fno-strict-aliasing', '/w']

# The C runtime functions the recompiled code calls (llvm-nm of the objects), all in Windows' own ucrtbase.dll.
UCRT = ['sqrt', 'sqrtf', 'exp2f', 'log2f']


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, env={k: v for k, v in os.environ.items()
                                                                 if k not in ('INCLUDE', 'LIB', 'CL', '_CL_')})
    return r.returncode, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rt', required=True)
    ap.add_argument('--obj', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--game-version', choices=['1.0', '1.2'], default='1.0')
    ap.add_argument('--gen', help='the recompiled code (default out/, or out-v12/ for 1.2)')
    ap.add_argument('--simde', default=os.path.join(ROOT, '..', 'XenonRecomp-hc09', 'thirdparty', 'simde'))
    ap.add_argument('--llvm', default=r'C:\Program Files\LLVM\bin')
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    v12 = a.game_version == '1.2'
    a.gen = a.gen or os.path.join(ROOT, 'out-v12' if v12 else 'out')
    clang = os.path.join(a.llvm, 'clang-cl.exe')
    os.makedirs(a.obj, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    inc = ['-I' + os.path.join(ROOT, 'toolchain', 'include'), '-I' + a.gen, '-I' + os.path.join(ROOT, 'runtime'),
           '-I' + a.simde]

    srcs = sorted(f for f in os.listdir(a.gen) if f.startswith('ppc_recomp.') and f.endswith('.cpp'))
    srcs.append('ppc_func_mapping.cpp')
    jobs = [([clang] + GUEST_FLAGS + ['/FI' + os.path.join(ROOT, 'runtime', 'guest_indirect.h')] + inc,
             os.path.join(a.gen, s), os.path.join(a.obj, s[:-4] + '.obj')) for s in srcs]
    # 1.2: the glue names the hooked functions as the runtime does, through game_addr_v12.h (1.0 name -> 1.2's).
    glue = GLUE_FLAGS + (['/FI' + os.path.join(ROOT, 'runtime', 'game_addr_v12.h')] if v12 else [])
    jobs.append(([clang] + glue + inc, os.path.join(ROOT, 'runtime', 'split_glue.cpp'),
                 os.path.join(a.obj, 'split_glue.obj')))
    todo = [j for j in jobs if not os.path.exists(j[2]) or os.path.getmtime(j[2]) < os.path.getmtime(j[1])]
    t0 = time.time()
    print(f'compile: {len(todo)} of {len(jobs)} files ({a.jobs} at a time)', flush=True)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for (cmd, src, obj), (rc, log) in zip(todo, ex.map(lambda j: run(j[0] + [j[1], '/Fo' + j[2]]), todo)):
            if rc:
                failed.append(os.path.basename(src))
                print(f'  FAILED {src}\n{log}', flush=True)
    if failed:
        sys.exit(f'{len(failed)} file(s) failed')
    print(f'compile: {time.time() - t0:.0f} s', flush=True)

    # ucrtbase.dll's import library, from a list of four names (made here: no Windows SDK needed).
    deff = os.path.join(a.obj, 'ucrtbase.def')
    with open(deff, 'w') as f:
        f.write('LIBRARY ucrtbase.dll\nEXPORTS\n' + ''.join(f'    {n}\n' for n in UCRT))
    rc, log = run([os.path.join(a.llvm, 'llvm-lib.exe'), '/machine:x64', '/def:' + deff,
                   '/out:' + os.path.join(a.obj, 'ucrtbase.lib')])
    if rc:
        sys.exit(f'llvm-lib: {log}')

    t1 = time.time()
    exe = os.path.join(a.out, 'hc09.exe')
    rsp = os.path.join(a.obj, 'link.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"' + j[2] + '"' for j in jobs) + '\n')
    rc, log = run([os.path.join(a.llvm, 'lld-link.exe'), '/OUT:' + exe, '/ENTRY:Hc09Entry', '/SUBSYSTEM:CONSOLE',
                   '/STACK:8388608', '/MACHINE:X64', '/NODEFAULTLIB', '/INCREMENTAL:NO', '/Brepro', '@' + rsp,
                   os.path.join(a.rt, 'hc09rt.lib'), os.path.join(a.obj, 'ucrtbase.lib')])
    if rc:
        sys.exit(f'lld-link failed:\n{log}')
    if log:
        print(log)
    print(f'link: {time.time() - t1:.0f} s -> {exe} ({os.path.getsize(exe):,} bytes)')


if __name__ == '__main__':
    main()
