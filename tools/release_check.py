#!/usr/bin/env python3
"""Check that a release hc09.exe has no development tools in it (docs/HANDOFF_M11.md step 1).

    python tools/release_check.py build-rel/hc09.exe [build-v12-rel/hc09.exe ...]

The release build (CMake HC09_DEV_TOOLS=OFF) compiles the tools out: the only HC09_* names left in the binary
are the settings of runtime/dev_env.h's kRelease list, and none of the left-out tools' log tags remain. Exit
status 1 when a binary fails, with what was found.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Log tags and names only the left-out sources print (hooks_gameplay, hooks_simlog, hooks_autowin, the generated
# hooks_sim/music/d3d, profiler.cpp), the virtual pad and the overlay's pipeline.
MARKERS = [b'[tweak]', b'[blocklog]', b'[sliders]', b'[simlog]', b'[autowin]', b'[profile]', b'[sim]', b'[music]',
           b'[d3d]', b'virtual pad', b'overlay.vert']


def release_names():
    src = open(os.path.join(ROOT, 'runtime', 'dev_env.h'), encoding='utf-8').read()
    body = re.search(r'kRelease\[\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
    return set(re.findall(r'"(HC09_[A-Z0-9_]+)"', body))


def check(path, allowed):
    data = open(path, 'rb').read()
    names = set(re.findall(rb'HC09_[A-Z0-9_]+', data))
    for w in re.findall(rb'H\x00C\x000\x009\x00_\x00(?:[A-Z0-9_]\x00)+', data):   # UTF-16 strings too
        names.add(w.decode('utf-16-le').encode())
    extra = sorted(n.decode() for n in names if n.decode() not in allowed)
    markers = [m.decode() for m in MARKERS if m in data]
    ok = not extra and not markers
    print(f'{path}: {"OK" if ok else "FAIL"} ({len(data):,} bytes; HC09_* names: {len(names)}, all release settings)'
          if ok else f'{path}: FAIL')
    if extra:
        print(f'  development switches still in the binary ({len(extra)}): {" ".join(extra)}')
    if markers:
        print(f'  left-out tools still in the binary: {" ".join(markers)}')
    return ok


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    allowed = release_names()
    results = [check(p, allowed) for p in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
