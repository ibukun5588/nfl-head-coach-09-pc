#!/usr/bin/env python3
"""Portable full build (the same pipeline as build.sh; works on Windows and Linux).

    python tools/build.py <path/to/default.xex> [--xenon ../XenonRecomp-hc09] [--skip-recomp]
    python tools/build.py <xex> --variant locals --recomp-option non_volatile_as_local=true ... [--guest-arch x86-64-v3]

Steps: build the recompiler -> decrypt the XEX -> recompile to out/ (338 TUs)
-> generate import stubs -> CMake/Ninja -> build-run/hc09[.exe].
Compiler: clang-cl on Windows (run from a shell where the MSVC environment is
set up, e.g. tools/winenv.cmd), clang/clang++ elsewhere. Requires cmake, ninja
and a Python with `cryptography`. --skip-recomp reuses an existing out/ (for
example one copied from another machine: it is identical input).
--variant <name>: an experiment beside the build: recompiles into out-<name>/ with the extra
config/hc09.toml [main] options given by --recomp-option key=value (repeatable) and builds
build-<name>/ (CMake HC09_OUT=out-<name>); out/ and build-run/ are not touched.
config/hc09.toml builds with guest registers as C++ locals (since 2026-10-09; needs XenonRecomp-hc09 a3f35e8);
--no-locals keeps them all in the context (as before), --recomp-option key=value overrides a key of the config.
--guest-arch: -march for the recompiled code (CMake HC09_GUEST_ARCH), default x86-64-v3 (AVX2/BMI2/MOVBE CPUs,
Intel Haswell 2013+ / AMD Zen); --guest-arch x86-64-v2 or x86-64 (or "") builds for older CPUs.

The 1.2 title update (docs/HANDOFF_M10_5.md), a build beside 1.0's:
    python tools/build.py <1.0 default.xex> --title-update <TU package or default.xexp>     (both versions)
    python tools/build.py <1.0 default.xex> --game-version 1.2 --title-update <TU package or default.xexp>
    python tools/build.py <1.2 default.xex> --game-version 1.2          (an already patched xex)
applies the update (tools/stfs.py, tools/xexpatch.cpp) into default-v12.xex, makes headcoach-v12.basefile,
recompiles with config/v12/ into out-v12/ and builds build-v12/hc09[.exe] (CMake HC09_GAME_VERSION=1.2).
default.xex, headcoach.basefile, out/ and build-run/ stay 1.0's and are not touched.

--release: the release build (docs/HANDOFF_M11.md step 1, CMake HC09_DEV_TOOLS=OFF): hc09.exe without the development
tools, in build-rel/ (1.0) and build-v12-rel/ (1.2) from the same out/ and out-v12/; the development builds are not
touched.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN = os.name == 'nt'


def run(cmd, **kw):
    print('    $', ' '.join(f'"{c}"' if ' ' in c else c for c in cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


VERSIONS = {'1.0': 0x00000001, '1.2': 0x00000201}   # the xex's execution id version (0.0.0.1, 0.0.2.1)


def xex_version(path):
    import struct
    d = open(path, 'rb').read(0x10000)
    u32 = lambda o: struct.unpack('>I', d[o:o + 4])[0]
    assert d[:4] == b'XEX2', f'{path}: not a XEX2 file'
    for i in range(u32(0x14)):
        if u32(0x18 + i * 8) == 0x40006:                  # execution info: media id, version, base version, ...
            return u32(u32(0x1C + i * 8) + 4)
    raise SystemExit(f'{path}: no execution info')


def make_v12_xex(base_xex, tu, xenon, cxx, exe):
    """default-v12.xex from the 1.0 xex and the title update (a LIVE package or its default.xexp)."""
    os.makedirs(os.path.join('builds', 'tools'), exist_ok=True)
    xexp = tu
    if open(tu, 'rb').read(4) in (b'LIVE', b'CON ', b'PIRS'):
        run([sys.executable, 'tools/stfs.py', tu, os.path.join('builds', 'v12')])
        xexp = os.path.join('builds', 'v12', 'default.xexp')
    patcher = os.path.join('builds', 'tools', 'xexpatch' + exe)
    if not os.path.exists(patcher):
        run(['ninja', '-C', os.path.join(xenon, 'build'), 'XenonUtils', 'disasm'], stdout=subprocess.DEVNULL)
        lib = lambda *p: os.path.join(xenon, 'build', *p)
        inc = [os.path.join(xenon, 'XenonUtils'), os.path.join(xenon, 'thirdparty', 'simde')]
        if WIN:
            run([cxx, '/nologo', '/std:c++20', '/O2', '/EHsc', '/MT'] + [f'/I{i}' for i in inc] +
                ['tools/xexpatch.cpp', f'/Fo{patcher[:-4]}.obj', f'/Fe:{patcher}', '/link',
                 lib('XenonUtils', 'XenonUtils.lib'), lib('thirdparty', 'disasm', 'disasm.lib')])
        else:
            run([cxx, '-std=c++20', '-O2'] + [f'-I{i}' for i in inc] + ['tools/xexpatch.cpp', '-o', patcher,
                lib('XenonUtils', 'libXenonUtils.a'), lib('thirdparty', 'disasm', 'libdisasm.a')])
    run([patcher, base_xex, xexp, 'default-v12.xex'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('xex')
    ap.add_argument('--xenon', default=os.path.join(ROOT, '..', 'XenonRecomp-hc09'))
    ap.add_argument('--skip-recomp', action='store_true')
    ap.add_argument('--variant')
    ap.add_argument('--recomp-option', action='append', default=[])
    ap.add_argument('--no-locals', action='store_true', help='guest registers in the context (no *_as_local options)')
    ap.add_argument('--guest-arch', default='x86-64-v3',
                    help='-march for the recompiled code (default x86-64-v3; x86-64-v2, x86-64 or "" for older CPUs)')
    ap.add_argument('--game-version', choices=sorted(VERSIONS) + ['both'],
                    help='1.0 (the disc), 1.2 (the Xbox 360 title update; out-v12/, build-v12/) or both; default: '
                         'both with --title-update (2026-10-22: both versions by default), else 1.0')
    ap.add_argument('--title-update', help='1.2: the title update (LIVE package or default.xexp) for a 1.0 xex')
    ap.add_argument('--build-dir', help='the build folder (default build-run/, build-v12/, or build-rel/ etc. with '
                                         '--release), e.g. on another drive')
    ap.add_argument('--release', action='store_true',
                    help='the release build without the development tools (HC09_DEV_TOOLS=OFF; build-rel/, build-v12-rel/)')
    a = ap.parse_args()
    if not a.game_version:
        a.game_version = 'both' if a.title_update else '1.0'
    if a.game_version == 'both':
        # 1.0 from the disc's xex, then 1.2 from the same xex and the title update: two full builds side by side
        if not a.title_update:
            raise SystemExit('--game-version both needs --title-update <TU package or default.xexp>')
        if a.build_dir:
            raise SystemExit('--build-dir is for one game version: give --game-version 1.0 or 1.2')
        args, skip = [], False
        for x in sys.argv[1:]:   # the command line without its --game-version
            if skip:
                skip = False
            elif x == '--game-version':
                skip = True
            elif not x.startswith('--game-version='):
                args.append(x)
        for v in ('1.0', '1.2'):
            print(f'==== game version {v} ====', flush=True)
            run([sys.executable, os.path.abspath(__file__)] + args + ['--game-version', v])
        return
    v12 = a.game_version == '1.2'
    tag = ('v12-' + a.variant if a.variant else 'v12') if v12 else a.variant
    out = f'out-{tag}' if tag else 'out'
    bdir = f'build-{tag}' if tag else 'build-run'
    if a.release:
        bdir = f'build-{tag}-rel' if tag else 'build-rel'
    if a.build_dir:
        bdir = os.path.abspath(a.build_dir)
    toml = f'hc09-{tag}.toml' if tag else 'hc09.toml'
    jt = 'jt_final-v12.toml' if v12 else 'jt_final.toml'
    staged_xex, image = ('default-v12.xex', 'headcoach-v12.basefile') if v12 else ('default.xex', 'headcoach.basefile')
    config = os.path.join('config', 'v12') if v12 else 'config'
    xex = os.path.abspath(a.xex)
    xenon = os.path.abspath(a.xenon)
    tu = os.path.abspath(a.title_update) if a.title_update else None
    os.chdir(ROOT)
    cc, cxx = ('clang-cl', 'clang-cl') if WIN else ('clang', 'clang++')
    exe = '.exe' if WIN else ''

    print(f'==> [1/5] stage {staged_xex} (game version {a.game_version}), decrypt image')
    have = xex_version(xex)
    if have == VERSIONS[a.game_version]:
        if os.path.abspath(staged_xex) != xex:
            shutil.copyfile(xex, staged_xex)
    elif v12 and have == VERSIONS['1.0']:
        if not tu:
            raise SystemExit(f'{a.xex} is the 1.0 xex: --title-update <TU package or default.xexp> applies 1.2 to it')
        make_v12_xex(xex, tu, xenon, cxx, exe)
    else:
        raise SystemExit(f'{a.xex}: execution version 0x{have:08X}, not {a.game_version} '
                         f'(0x{VERSIONS[a.game_version]:08X})')
    if xex_version(staged_xex) != VERSIONS[a.game_version]:
        raise SystemExit(f'{staged_xex}: not game version {a.game_version}')
    run([sys.executable, 'tools/xexdec.py', staged_xex, image])

    if not a.skip_recomp:
        print(f'==> [2/5] build the recompiler ({xenon})')
        run(['cmake', '-S', xenon, '-B', os.path.join(xenon, 'build'), '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_C_COMPILER={cc}', f'-DCMAKE_CXX_COMPILER={cxx}'],
            stdout=subprocess.DEVNULL)
        run(['ninja', '-C', os.path.join(xenon, 'build'), 'XenonRecomp'], stdout=subprocess.DEVNULL)

        print('==> [3/5] recompile game to C++')
        shutil.copyfile(os.path.join(config, 'jt_final.toml'), jt)
        cfg = open(os.path.join(config, 'hc09.toml'), encoding='utf-8').read()
        if tag:
            cfg = cfg.replace('out_directory_path = "out"', f'out_directory_path = "{out}"', 1)
        if v12:
            cfg = cfg.replace('file_path = "default.xex"', f'file_path = "{staged_xex}"', 1)
            cfg = cfg.replace('switch_table_file_path = "jt_final.toml"', f'switch_table_file_path = "{jt}"', 1)
        opts = [o.split('=', 1) for o in a.recomp_option]
        if a.no_locals:
            opts += [[k, 'false'] for k in ('cr_as_local', 'ctr_as_local', 'xer_as_local', 'reserved_as_local',
                                            'non_volatile_as_local', 'skip_msr', 'seh_funclets_in_context')]
        for k, v in opts:   # a key the config has is replaced (TOML allows no duplicates), others go after [main]
            k, v = k.strip(), v.strip()
            if re.search(rf'^{re.escape(k)} = ', cfg, re.M):
                cfg = re.sub(rf'^{re.escape(k)} = .*$', lambda m: f'{k} = {v}', cfg, count=1, flags=re.M)
            else:
                cfg = cfg.replace('[main]\n', f'[main]\n{k} = {v}\n', 1)
        open(toml, 'w', encoding='utf-8').write(cfg)
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        recomp = os.path.join(xenon, 'build', 'XenonRecomp', 'XenonRecomp' + exe)
        log_name = f'recomp-{tag}.log' if tag else 'recomp.log'
        with open(log_name, 'w') as log:
            subprocess.run([recomp, toml, os.path.join(xenon, 'XenonUtils', 'ppc_context.h')],
                           stdout=log, stderr=subprocess.STDOUT)
        text = open(log_name, errors='replace').read()
        tus = [f for f in os.listdir(out) if f.startswith('ppc_recomp.') and f.endswith('.cpp')]
        print(f"    switch errors: {text.count('ERROR: Switch')}")
        print(f"    unrecognized instructions: {text.count('Unrecognized')}")
        print(f'    translation units: {len(tus)}')
    else:
        print(f'==> [2-3/5] skipped (--skip-recomp): using the existing {out}/')

    if v12:
        print("==> [4/5] import stubs: generated by the build into build-v12/ (runtime/imports_gen.cpp stays 1.0's)")
    else:
        print('==> [4/5] generate import stubs')
        run([sys.executable, 'runtime/gen_imports.py', f'{out}/ppc_func_mapping.cpp', 'runtime/imports_manual.cpp'])

    print('==> [5/5] build host executable (338 TUs; minutes, scales with cores)')
    run(['cmake', '-S', '.', '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         f'-DCMAKE_CXX_COMPILER={cxx}', f'-DXENON_DIR={xenon}', f'-DPython3_EXECUTABLE={sys.executable}',
         f'-DHC09_OUT={out}', f'-DHC09_GUEST_ARCH={a.guest_arch}', f'-DHC09_GAME_VERSION={a.game_version}',
         f'-DHC09_XEX={os.path.join(ROOT, staged_xex)}', f'-DHC09_DEV_TOOLS={"OFF" if a.release else "ON"}'],
        stdout=subprocess.DEVNULL)
    run(['ninja', '-C', bdir] + (['hc09'] if tag else []))

    print('\nDone. Test boot:')
    print(f'    {os.path.join(bdir, "hc09" + exe)} {image} --root <folder with the .ast files>')


if __name__ == '__main__':
    main()
