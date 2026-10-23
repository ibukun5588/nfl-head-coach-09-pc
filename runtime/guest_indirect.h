#pragma once
// Force-included into every recompiled TU (see CMakeLists.txt). Overrides the
// recompiler's indirect-call macro so a call through a null or garbage
// function pointer (typically a virtual call on a null object) produces a
// diagnostic naming the target and the guest caller, instead of a SIGSEGV
// deep inside the lookup table.
#include <cstdint>

// The recompiler turns `mftb` (the PowerPC time base) into __rdtsc(): the host
// CPU's cycle counter, several GHz. The Xenon time base ticks at 50 MHz
// (KeQueryPerformanceFrequency), and the game reads it inline wherever it
// would call QueryPerformanceCounter, so every high-resolution interval it
// measured ran ~60-80x fast (the menus' auto-repeat fired within one 150 ms
// press: five entries per tap). Redirect it to a 50 MHz guest time base
// (runtime.cpp). Only in the recompiled TUs (HC09_GUEST_TU, CMakeLists.txt):
// runtime files include <windows.h>, which declares __rdtsc itself. The
// intrinsic headers are included first so the macro never meets the
// intrinsic's own declaration.
uint64_t GuestTimeBase();
#if defined(HC09_GUEST_TU)
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif
#define __rdtsc() GuestTimeBase()
#endif

struct PPCContext;
[[noreturn]] void GuestBadIndirect(PPCContext& ctx, uint32_t target);

#define PPC_CALL_INDIRECT_FUNC(x)                                              \
    do {                                                                       \
        uint32_t _t = (uint32_t)(x);                                           \
        if ((uint32_t)(_t - PPC_CODE_BASE) < (uint32_t)PPC_CODE_SIZE) {        \
            PPCFunc* _f = PPC_LOOKUP_FUNC(base, _t);                           \
            if (_f) { _f(ctx, base); break; }                                  \
        }                                                                      \
        GuestBadIndirect(ctx, _t);                                             \
    } while (0)
