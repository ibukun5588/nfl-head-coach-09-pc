# Player-side toolchain (M11 step 2, docs/HANDOFF_M11.md)

`include/`: the C/C++ library headers the recompiled code (`out/`, `out-v12/`) needs, written for this project:
the declarations the generated code and SIMDe use, as the MSVC x64 UCRT declares them with /MD. With them the
generated code compiles with `clang-cl /X` (no Visual Studio, no Windows SDK headers) into code byte-identical to
today's build (all 339 files of 1.0: code and relocations compared, 2026-10-23):

    clang-cl /c /X ... -I toolchain/include -I out -I runtime -I <XenonRecomp>/thirdparty/simde out/ppc_recomp.N.cpp

Same flags as the CMake build otherwise (`/O2 -std:c++20 -MD /clang:-fno-strict-aliasing /FI runtime/guest_indirect.h
/clang:-march=x86-64-v3 /clang:-ffp-contract=off -DHC09_GUEST_TU`). The link side (a prebuilt runtime, no MSVC
libraries) is the next step.
