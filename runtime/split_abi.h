#pragma once
// The split build (CMake HC09_SPLIT, docs/HANDOFF_M11.md step 2): the hand-written runtime is a DLL we build and ship
// (hc09rt.dll); the player's PC compiles only the recompiled code and links it with runtime/split_glue.cpp into
// hc09.exe, with clang-cl and lld-link alone (no MSVC libraries). This is everything that crosses between the two:
//   exe -> runtime: the exports of hc09rt.dll (the kernel imports __imp__X, GuestBadIndirect, GuestTimeBase, the
//                   hooks as hc09hook_sub_X; tools/split_def.py) and Hc09RtRun below;
//   runtime -> exe: this table (the function table, the entry point, the generated bodies of the hooked functions).
#include <cstdint>
#include "ppc_context.h"

constexpr uint32_t kHc09SplitAbi = 1;

struct Hc09GameTable
{
    uint32_t abi;                       // kHc09SplitAbi
    uint32_t hookCount;                 // entries in originals
    const PPCFuncMapping* mappings;     // the recompiled function table (ppc_func_mapping.cpp)
    PPCFunc* xstart;                    // the game's entry point (_xstart)
    PPCFunc* const* originals;          // __imp__sub_X of runtime/split_hooks.inc, in its order
};

// hc09rt.dll exports Hc09RtRun(const Hc09GameTable*): it runs the game (hc09.exe's command line) and exits the process
// (runtime/split_dll.cpp; hc09.exe calls it from its entry point, runtime/split_glue.cpp).
