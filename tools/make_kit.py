#!/usr/bin/env python3
"""Assemble the build kit the release ships (docs/HANDOFF_M11.md step 3): everything tools/installer/hc09_build.cpp
needs on the player's PC to build hc09.exe from their own default.xex. Our code and redistributable tools, plus one
game file when --tu is given: the 1.2 title update package, unchanged, so the installer builds 1.2 without the player
finding it (2026-10-23: taken down on request; the installer falls back to the player's own file).
Nothing compiled from a game file (the runtime DLLs are the hand-written runtime; the configs are addresses).

    python tools/make_kit.py --out <kit dir> --rt10 <split build 1.0> --rt12 <split build 1.2>
        [--hash10 <hc09.exe built from 1.0>] [--hash12 <...1.2>] [--tu <TU_12K2221_...G1>] [--llvm "C:/Program Files/LLVM"]

--rt10/--rt12: CMake build folders with HC09_SPLIT=ON (hc09rt.dll, hc09rt.lib, the FFmpeg/SDL3 DLLs beside it).
--hash10/--hash12: an hc09.exe built from the retail files with exactly this kit's tools (tools/player_build.py or a
first hc09-build run): its SHA-256 goes into kit.ini, and hc09-build checks every player's build against it.
--tu: Title Update #2 as Microsoft's package (hash-checked), into kit/title_update/ (never into git).
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XENON = os.path.join(ROOT, '..', 'XenonRecomp-hc09')
UCRT = ['sqrt', 'sqrtf', 'exp2f', 'log2f']   # tools/player_build.py: what the recompiled code calls in ucrtbase.dll
TU_NAME = 'TU_12K2221_0000004000000.00000000000G1'   # Title Update #2 (game version 1.2), docs/HANDOFF_RELEASE.md 1.1a
TU_SHA256 = '43B31C2B51E9797C6CAF02377D84203E963B977504B30B175382A4D1E17C44F2'
CRT_DLLS = ['msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']


def trim_headers(out):
    """Keep only the clang and SIMDe headers the build includes: compile probes of both game versions (a recompiled
    file, the function table, the glue) with the kit's clang-cl and /showIncludes, delete every other header. The
    generated files all start from the same ppc_recomp_shared.h, so the probes see what every file needs."""
    clang = os.path.join(out, 'toolchain', 'bin', 'clang-cl.exe')
    inc_dirs = [os.path.join(out, d) for d in ('include', 'src', 'simde')]
    used = set()
    env = {k: v for k, v in os.environ.items() if k not in ('INCLUDE', 'LIB', 'CL', '_CL_')}
    for gen, v12 in (('out', False), ('out-v12', True)):
        g = os.path.join(ROOT, gen)
        probes = [(os.path.join(g, 'ppc_recomp.0.cpp'), True), (os.path.join(g, 'ppc_func_mapping.cpp'), True),
                  (os.path.join(out, 'src', 'split_glue.cpp'), False)]
        for src, guest in probes:
            cmd = [clang, '/c', '/X', '/O2', '-std:c++20', '-MD', '/clang:-fno-strict-aliasing', '/w', '/showIncludes',
                   '/Fo' + os.path.join(out, 'probe.obj'), '-I' + g] + ['-I' + d for d in inc_dirs]
            if guest:
                cmd += ['/clang:-march=x86-64-v3', '/clang:-ffp-contract=off', '-DHC09_GUEST_TU',
                        '/FI' + os.path.join(out, 'src', 'guest_indirect.h')]
            elif v12:
                cmd += ['/FI' + os.path.join(out, 'src', 'game_addr_v12.h')]
            r = subprocess.run(cmd + [src], capture_output=True, text=True, env=env)
            if r.returncode:
                sys.exit(f'probe compile failed: {src}\n{r.stdout}{r.stderr}')
            for line in r.stdout.splitlines():
                if 'including file:' in line:
                    used.add(os.path.normcase(os.path.abspath(line.split('including file:', 1)[1].strip())))
    os.remove(os.path.join(out, 'probe.obj'))
    removed = 0
    for top in (os.path.join(out, 'toolchain', 'lib'), os.path.join(out, 'simde')):
        for d, _, ns in os.walk(top):
            for n in ns:
                p = os.path.join(d, n)
                if os.path.normcase(os.path.abspath(p)) not in used and not n.upper().startswith('COPYING'):
                    os.remove(p)
                    removed += 1
    for top in (os.path.join(out, 'toolchain', 'lib'), os.path.join(out, 'simde')):   # empty folders
        for d, ds, ns in sorted(os.walk(top), key=lambda w: -len(w[0])):
            if not os.listdir(d):
                os.rmdir(d)
    print(f'trimmed: {len(used)} headers kept, {removed} files removed')


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest().upper()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--rt10', required=True)
    ap.add_argument('--rt12', required=True)
    ap.add_argument('--hash10')
    ap.add_argument('--hash12')
    ap.add_argument('--tu', help='the title update package to include (TU_12K2221_...G1, checked against its hash)')
    ap.add_argument('--llvm', default=r'C:\Program Files\LLVM')
    ap.add_argument('--no-trim', action='store_true', help='keep all of clang\'s and SIMDe\'s headers')
    ap.add_argument('--zip', help='with --setup: also pack the release folder into this .zip (Windows\' tar.exe)')
    ap.add_argument('--setup', help='HC09Setup.exe: also lay out the release folder (the kit\'s parent): HC09Setup.exe, '
                                    'README.txt, THIRD-PARTY-NOTICES.txt')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)

    # The compiler and linker (LLVM, Apache-2.0 with LLVM exception) and clang's own headers.
    tc = os.path.join(out, 'toolchain')
    for exe in ('clang-cl.exe', 'lld-link.exe'):
        copy(os.path.join(a.llvm, 'bin', exe), os.path.join(tc, 'bin', exe))
    res = glob.glob(os.path.join(a.llvm, 'lib', 'clang', '*', 'include'))
    if len(res) != 1:
        sys.exit(f'clang resource headers: {res}')
    ver = os.path.basename(os.path.dirname(res[0]))
    shutil.copytree(res[0], os.path.join(tc, 'lib', 'clang', ver, 'include'))

    # Our C library headers, SIMDe (MIT), the sources and headers of our own that the build compiles.
    shutil.copytree(os.path.join(ROOT, 'toolchain', 'include'), os.path.join(out, 'include'))
    simde = os.path.join(XENON, 'thirdparty', 'simde')
    shutil.copytree(simde, os.path.join(out, 'simde'), ignore=shutil.ignore_patterns('.git*', 'test', 'docker', '*.md'))
    for f in ('guest_indirect.h', 'split_glue.cpp', 'split_abi.h', 'split_hooks.inc', 'game_addr_v12.h'):
        copy(os.path.join(ROOT, 'runtime', f), os.path.join(out, 'src', f))
    copy(os.path.join(XENON, 'XenonUtils', 'ppc_context.h'), os.path.join(out, 'src', 'ppc_context.h'))

    # The recompiler (our fork, MIT), the title update patcher, the build driver, the launcher: from the release build
    # folder (--rt10; the development build's launcher may carry a private icon, CMakeLists.txt res/hc09.ico).
    copy(os.path.join(XENON, 'build', 'XenonRecomp', 'XenonRecomp.exe'), os.path.join(out, 'XenonRecomp.exe'))
    for exe in ('hc09-xexpatch.exe', 'hc09-build.exe', 'hc09-launcher.exe'):
        copy(os.path.join(a.rt10, exe), os.path.join(out, exe))
    # Steam Deck / AMD on Linux: the LD_PRELOAD fix for the renderer's memory import (tools/steamdeck/userptr_shim.c),
    # a Linux library built here with our clang and lld; setup puts it beside the launcher.
    subprocess.run([os.path.join(a.llvm, 'bin', 'clang.exe'), '--target=x86_64-unknown-linux-gnu', '-O2', '-fPIC', '-shared',
                    '-nostdlib', '-fuse-ld=lld', os.path.join(ROOT, 'tools', 'steamdeck', 'userptr_shim.c'),
                    '-o', os.path.join(out, 'libhc09_userptr.so')], check=True)

    # ucrtbase.dll's import library (four names; made here with llvm-lib, no Windows SDK).
    os.makedirs(os.path.join(out, 'lib'))
    deff = os.path.join(out, 'lib', 'ucrtbase.def')
    with open(deff, 'w') as f:
        f.write('LIBRARY ucrtbase.dll\nEXPORTS\n' + ''.join(f'    {n}\n' for n in UCRT))
    subprocess.run([os.path.join(a.llvm, 'bin', 'llvm-lib.exe'), '/machine:x64', '/def:' + deff,
                    '/out:' + os.path.join(out, 'lib', 'ucrtbase.lib')], check=True)
    os.remove(deff)

    # Per version: the recompiler configs (addresses only) and the runtime DLL with its import library.
    for v, cfg, rt in (('1.0', 'config', a.rt10), ('1.2', os.path.join('config', 'v12'), a.rt12)):
        for f in ('hc09.toml', 'jt_final.toml'):
            copy(os.path.join(ROOT, cfg, f), os.path.join(out, 'config', v, f))
        for f in ('hc09rt.dll', 'hc09rt.lib'):
            copy(os.path.join(rt, f), os.path.join(out, 'rt', v, f))

    # DLLs beside hc09.exe: FFmpeg (LGPL, shared), SDL3 (zlib), the VC++ runtime (Microsoft, app-local redistributable).
    for f in glob.glob(os.path.join(a.rt10, '*.dll')):
        if os.path.basename(f).lower() != 'hc09rt.dll':
            copy(f, os.path.join(out, 'redist', os.path.basename(f)))
    crt = glob.glob(os.path.join(os.environ['ProgramFiles(x86)'], 'Microsoft Visual Studio', '2022', '*', 'VC', 'Redist',
                                 'MSVC', '*', 'x64', 'Microsoft.VC143.CRT'))
    if not crt:
        sys.exit('Microsoft.VC143.CRT (VS 2022 redistributable DLLs) not found')
    for f in CRT_DLLS:   # the ones hc09rt.dll imports (llvm-readobj --coff-imports); the UCRT is part of Windows 10+
        copy(os.path.join(sorted(crt)[-1], f), os.path.join(out, 'redist', f))

    # The player's documentation and the licenses (release/; the installer copies them into the install folder).
    for f in ('README.txt', 'THIRD-PARTY-NOTICES.txt'):
        copy(os.path.join(ROOT, 'release', f), os.path.join(out, f))
    lic = os.path.join(out, 'licenses')
    for src, name in ((os.path.join(ROOT, 'release', 'licenses', 'GPL-3.0.txt'), 'GPL-3.0.txt'),
                      (os.path.join(ROOT, 'thirdparty', 'ffmpeg', 'LICENSE.txt'), 'FFmpeg-LGPL-3.0.txt'),
                      (os.path.join(ROOT, 'thirdparty', 'SDL3', 'LICENSE.txt'), 'SDL3-LICENSE.txt'),
                      (os.path.join(ROOT, 'release', 'licenses', 'LLVM-LICENSE.txt'), 'LLVM-LICENSE.txt'),
                      (os.path.join(XENON, 'LICENSE.md'), 'XenonRecomp-LICENSE.txt'),
                      (os.path.join(simde, 'COPYING'), 'SIMDe-COPYING.txt')):
        if not os.path.exists(src):
            sys.exit(f'license file missing: {src}')
        copy(src, os.path.join(lic, name))
    # The title update the installer uses when the player gives none (setup.cpp kBundledTu).
    if a.tu:
        if sha256(a.tu) != TU_SHA256:
            sys.exit(f'{a.tu} is not Title Update #2 ({TU_NAME})')
        copy(a.tu, os.path.join(out, 'title_update', TU_NAME))
        print(f'title update included: {TU_NAME}')

    if a.setup:
        parent = os.path.dirname(out)
        copy(a.setup, os.path.join(parent, 'HC09Setup.exe'))
        for f in ('README.txt', 'THIRD-PARTY-NOTICES.txt'):
            copy(os.path.join(ROOT, 'release', f), os.path.join(parent, f))

    if not a.no_trim:
        trim_headers(out)

    with open(os.path.join(out, 'kit.ini'), 'w') as f:
        f.write(f'; hc09 build kit (tools/make_kit.py): clang {ver}\n')
        for v, p in (('1.0', a.hash10), ('1.2', a.hash12)):
            if p:
                f.write(f'hc09_{v}_sha256 = {sha256(p)}\n')
    if a.setup and a.zip:   # the download: the release folder as one zip
        parent = os.path.dirname(out)
        z = os.path.abspath(a.zip)
        if os.path.exists(z):
            os.remove(z)
        tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
        subprocess.run([tar, '-a', '-c', '-f', z, '-C', os.path.dirname(parent), os.path.basename(parent)], check=True)
        print(f'zip: {z} ({os.path.getsize(z) / 2**20:.1f} MB)')
    size = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
    print(f'kit: {out} ({size / 2**20:.0f} MB, clang {ver})')


if __name__ == '__main__':
    main()
