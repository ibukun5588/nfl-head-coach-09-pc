// The runtime side of the split build (runtime/split_abi.h): hc09rt.dll's entry and what the runtime calls in the
// recompiled code, reached through the table hc09.exe passes in.
#include "runtime.h"
#include "split_abi.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace {
const Hc09GameTable* g_game = nullptr;

enum : uint32_t
{
#define HC09_HOOK(name) kHook_##name,
#include "split_hooks.inc"
#undef HC09_HOOK
    kHookCount
};
}  // namespace

// runtime.cpp's function table and entry point
const PPCFuncMapping* GameFuncMappings() { return g_game->mappings; }
PPC_FUNC(_xstart) { g_game->xstart(ctx, base); }

// The generated bodies the hooks call (they live in hc09.exe).
#define HC09_HOOK(name) PPC_FUNC_IMPL(__imp__##name) { g_game->originals[kHook_##name](ctx, base); }
#include "split_hooks.inc"
#undef HC09_HOOK

int RuntimeMain(int argc, char** argv);   // runtime.cpp

extern "C" __declspec(dllexport) int Hc09RtRun(const Hc09GameTable* game)
{
    if (!game || game->abi != kHc09SplitAbi || game->hookCount != kHookCount)
    {
        char msg[256];
        snprintf(msg, sizeof msg, "hc09.exe and hc09rt.dll do not match (ABI %u/%u, hooks %u/%u): rebuild the game.",
                 game ? game->abi : 0, kHc09SplitAbi, game ? game->hookCount : 0, uint32_t(kHookCount));
        fprintf(stderr, "[runtime] *** %s\n", msg);
        MessageBoxA(nullptr, msg, "hc09", MB_OK | MB_ICONERROR);
        exit(1);
    }
    g_game = game;
    // The command line as the CRT's narrow argv would have it.
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args(size_t(argc > 0 ? argc : 0));
    std::vector<char*> argv;
    for (int i = 0; i < argc; ++i)
    {
        int n = WideCharToMultiByte(CP_ACP, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        args[i].resize(size_t(n > 0 ? n - 1 : 0));
        if (n > 1) WideCharToMultiByte(CP_ACP, 0, wargv[i], -1, args[i].data(), n, nullptr, nullptr);
        argv.push_back(args[i].data());
    }
    argv.push_back(nullptr);
    LocalFree(wargv);
    exit(RuntimeMain(argc, argv.data()));
}
