#!/usr/bin/env bash
# One-shot build for the hc09-recomp test boot. Run from the repo root.
#
#   ./build.sh /path/to/default.xex [/path/to/XenonRecomp-hc09]
#
# Produces build-run/hc09 and headcoach.basefile, then prints the run command.
# Requires: clang++ (18+), cmake (3.20+), ninja, python3 + `cryptography`.
set -euo pipefail

XEX="${1:?usage: ./build.sh default.xex [xenon-dir]}"
XENON="${2:-../XenonRecomp-hc09}"
XENON="$(cd "$XENON" && pwd)"
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "==> [1/5] build the recompiler ($XENON)"
cmake -S "$XENON" -B "$XENON/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
ninja -C "$XENON/build" XenonRecomp >/dev/null

echo "==> [2/5] decrypt image + stage config"
cp -f "$XEX" default.xex
python3 tools/xexdec.py default.xex headcoach.basefile
cp -f config/hc09.toml config/jt_final.toml .
rm -rf out && mkdir out

echo "==> [3/5] recompile game to C++ (~10 s)"
"$XENON/build/XenonRecomp/XenonRecomp" hc09.toml "$XENON/XenonUtils/ppc_context.h" \
    > recomp.log 2>&1 || true
echo "    switch errors: $(grep -c 'ERROR: Switch' recomp.log || true)"
echo "    unrecognized instructions: $(grep -c 'Unrecognized' recomp.log || true)"
echo "    translation units: $(ls out/ppc_recomp.*.cpp | wc -l)"

echo "==> [4/5] generate import stubs"
python3 runtime/gen_imports.py out/ppc_func_mapping.cpp runtime/imports_manual.cpp

echo "==> [5/5] build host executable (338 TUs; minutes, scales with cores)"
cmake -S . -B build-run -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++ -DXENON_DIR="$XENON" >/dev/null
ninja -C build-run

echo
echo "Done. Test boot:"
echo "    ./build-run/hc09 headcoach.basefile --root /path/to/game/files   # folder with the .ast files"
