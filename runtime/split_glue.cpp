// hc09.exe's own code in the split build (runtime/split_abi.h), compiled on the player's PC with the recompiled code:
// no C runtime, no Windows headers (toolchain/include only). It forwards the hooked guest functions to hc09rt.dll,
// hands the runtime the table of the recompiled code, and provides the few C runtime symbols the compiler expects.
#include "ppc_recomp_shared.h"
#include "split_abi.h"

#include <intrin.h>

// The hooks: the recompiled code calls sub_X (a weak alias of its own __imp__sub_X); these strong definitions send it
// to the runtime's hook, exported by hc09rt.dll as hc09hook_sub_X.
#define HC09_HOOK(name)                                                                        \
    extern "C" __declspec(dllimport) void hc09hook_##name(PPCContext& __restrict__, uint8_t*); \
    PPC_FUNC(name) { hc09hook_##name(ctx, base); }
#include "split_hooks.inc"
#undef HC09_HOOK

// The generated bodies of the hooked functions, for the runtime's hooks to call.
#define HC09_HOOK(name) PPC_FUNC_IMPL(__imp__##name);
#include "split_hooks.inc"
#undef HC09_HOOK
static PPCFunc* const kOriginals[] = {
#define HC09_HOOK(name) &__imp__##name,
#include "split_hooks.inc"
#undef HC09_HOOK
};

PPC_EXTERN_FUNC(_xstart);
extern "C" __declspec(dllimport) int Hc09RtRun(const Hc09GameTable* game);

// What clang-cl's code expects from the C runtime: the floating-point marker and the /GS stack cookie.
extern "C" int _fltused = 0;
extern "C" uintptr_t __security_cookie = 0x00002B992DDFA232ull;
extern "C" void __security_check_cookie(uintptr_t cookie)
{
    if (cookie != __security_cookie) __fastfail(2);   // FAST_FAIL_STACK_COOKIE_CHECK_FAILURE
}

// The entry point (lld-link /ENTRY): nothing on the stack uses the cookie yet, so it can be randomized here.
extern "C" int Hc09Entry()
{
    __security_cookie ^= __rdtsc() & 0x0000FFFFFFFFFFFFull;
    static const Hc09GameTable game = { kHc09SplitAbi, uint32_t(sizeof kOriginals / sizeof kOriginals[0]),
                                        PPCFuncMappings, _xstart, kOriginals };
    return Hc09RtRun(&game);
}
