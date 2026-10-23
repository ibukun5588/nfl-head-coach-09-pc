// HC09_PROFILE=<threads>: a sampling profiler of the game's threads by guest
// function (docs/REPORT_GAME_CPU.md). Off by default.
//
//   HC09_PROFILE=1 (or "on")   the render loop (FGGameRender/LoopRender), the simulation
//                              (FGGameSim/LoopSimulation) and the GPU command processor
//   HC09_PROFILE=LoopRender,25,native   thread names (case-insensitive substrings of the
//                              name the game gave the thread, or of a host thread's
//                              name: "GPU command processor", "native renderer"), guest
//                              tids, or "all"
//   HC09_PROFILE_HZ=1000       samples per second per thread
//   HC09_PROFILE_EVERY=10      seconds per report window
//   HC09_PROFILE_OUT=captures/profile.txt   the full counts of every window (appended;
//                              tools/profile_report.py aggregates and names them)
//
// A sampler thread suspends each selected thread in turn (SuspendThread /
// GetThreadContext), walks its host stack with the modules' own unwind tables
// (.pdata, RtlVirtualUnwind: no loader locks, no allocation while the thread is
// suspended) and resumes it; the frames are classified afterwards:
//   - recompiled guest code: every guest function is a host function, so the frames
//     are the guest call chain (minus tail calls). Functions the runtime hooks
//     (hooks_*.cpp wrappers) map to their guest address too: the wrapper by the
//     function table, the original body (__imp__sub_X) found by scanning the
//     wrapper's code for the call to it;
//   - imports (the kernel/XAM layer, by guest thunk address) and other runtime code,
//     attributed to the nearest guest-level caller (D3D hooks = the native front end);
//   - other modules by kind: OS (ntdll, kernel32, kernelbase; "wait" when the thread
//     sits in a system call), CRT, the GPU driver, other DLLs; named by their nearest
//     export;
//   - with runtime symbols (a build with -DHC09_RUNTIME_PDB=ON: hc09.pdb beside hc09.exe)
//     the runtime's own functions are nodes too ("rt:Name", DbgHelp), in self, inclusive
//     and caller edges: what the native front end and the imports spend their time in.
// Samples are wall-clock: a blocked thread is sampled where it waits.
// Per window and thread: samples by category, self samples per function (the leaf),
// inclusive samples (each guest-level function on the stack, once per sample; and
// the same counting only samples whose leaf is the game's own code, not D3D/runtime/OS),
// caller edges, and the time the threads spent suspended (the overhead).
#include "dev_env.h"
#include "profiler.h"
#include "runtime.h"
#include "kernel.h"
#include "game_addr.h"
#include "gpu.h"
#include "xex_imports.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace {

bool Enabled()
{
    static const bool on = [] { const char* e = Hc09Env("HC09_PROFILE"); return e && *e && strcmp(e, "0") != 0; }();
    return on;
}

// ---- threads -------------------------------------------------------------------
struct Thread
{
    uint64_t tid = 0;
    std::string hostName;          // host-only threads (tid 0)
    HANDLE handle = nullptr;
    uintptr_t stackLo = 0, stackHi = 0;
};
std::mutex g_threadMu;
std::vector<Thread> g_threads;

// ---- modules (own .pdata lookup: RtlLookupFunctionEntry may take loader locks) ----
enum ModKind : uint8_t { kModExe, kModOs, kModCrt, kModDriver, kModOther };
struct Module
{
    uintptr_t base = 0, end = 0;
    std::string name;              // lower case, file name only
    ModKind kind = kModOther;
    const RUNTIME_FUNCTION* pdata = nullptr;
    uint32_t npdata = 0;
    bool exportsLoaded = false;
    std::vector<std::pair<uint32_t, const char*>> exports;   // sorted by rva
};
std::vector<Module> g_mods;        // append-only (indices are stable), sorted copy below
std::vector<int> g_modOrder;       // indices sorted by base

ModKind KindOf(const std::string& n)
{
    if (n == "ntdll.dll" || n == "kernel32.dll" || n == "kernelbase.dll" || n == "win32u.dll" || n == "user32.dll" ||
        n == "gdi32.dll" || n == "gdi32full.dll")
        return kModOs;
    if (n.rfind("ucrtbase", 0) == 0 || n.rfind("vcruntime", 0) == 0 || n.rfind("msvcp", 0) == 0) return kModCrt;
    if (n.rfind("nv", 0) == 0 || n.rfind("vulkan", 0) == 0 || n.rfind("amd", 0) == 0 || n.rfind("ig", 0) == 0 ||
        n.rfind("atio", 0) == 0)
        return kModDriver;
    return kModOther;
}

void RefreshModules()
{
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    bool added = false;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 512; ++i)
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(mods[i]);
        bool known = false;
        for (const Module& m : g_mods) known |= m.base == base;
        if (known) continue;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        Module m;
        m.base = base;
        m.end = base + nt->OptionalHeader.SizeOfImage;
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(mods[i], path, MAX_PATH);
        std::wstring w(path, n);
        size_t slash = w.find_last_of(L"\\/");
        if (slash != std::wstring::npos) w = w.substr(slash + 1);
        for (wchar_t c : w) m.name.push_back(char(c < 128 ? tolower(int(c)) : '?'));
        m.kind = mods[i] == exe ? kModExe : KindOf(m.name);
        const IMAGE_DATA_DIRECTORY& ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (ex.VirtualAddress && ex.Size)
        {
            m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(base + ex.VirtualAddress);
            m.npdata = ex.Size / sizeof(RUNTIME_FUNCTION);
        }
        g_mods.push_back(std::move(m));
        added = true;
    }
    if (!added) return;
    g_modOrder.resize(g_mods.size());
    for (size_t i = 0; i < g_mods.size(); ++i) g_modOrder[i] = int(i);
    std::sort(g_modOrder.begin(), g_modOrder.end(), [](int a, int b) { return g_mods[a].base < g_mods[b].base; });
}

int ModuleOf(uintptr_t pc)
{
    size_t lo = 0, hi = g_modOrder.size();
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        const Module& m = g_mods[g_modOrder[mid]];
        if (pc < m.base) hi = mid;
        else if (pc >= m.end) lo = mid + 1;
        else return g_modOrder[mid];
    }
    return -1;
}

const RUNTIME_FUNCTION* FunctionEntry(const Module& m, uintptr_t pc)
{
    uint32_t rva = uint32_t(pc - m.base);
    size_t lo = 0, hi = m.npdata;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION& f = m.pdata[mid];
        if (rva < f.BeginAddress) hi = mid;
        else if (rva >= f.EndAddress) lo = mid + 1;
        else
        {
            const RUNTIME_FUNCTION* e = &f;
            if (e->UnwindData & 1) e = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + (e->UnwindData & ~1u));
            return e;
        }
    }
    return nullptr;
}

const char* ExportName(Module& m, uint32_t rva, uint32_t* exportRva)
{
    if (!m.exportsLoaded)
    {
        m.exportsLoaded = true;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (d.VirtualAddress && d.Size)
        {
            auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(m.base + d.VirtualAddress);
            auto* funcs = reinterpret_cast<const DWORD*>(m.base + ed->AddressOfFunctions);
            auto* names = reinterpret_cast<const DWORD*>(m.base + ed->AddressOfNames);
            auto* ords = reinterpret_cast<const WORD*>(m.base + ed->AddressOfNameOrdinals);
            for (DWORD i = 0; i < ed->NumberOfNames; ++i)
            {
                uint32_t f = funcs[ords[i]];
                if (f >= d.VirtualAddress && f < d.VirtualAddress + d.Size) continue;   // forwarder
                m.exports.push_back({ f, reinterpret_cast<const char*>(m.base + names[i]) });
            }
            std::sort(m.exports.begin(), m.exports.end());
        }
    }
    auto it = std::upper_bound(m.exports.begin(), m.exports.end(), std::make_pair(rva, (const char*)nullptr),
                               [](const auto& a, const auto& b) { return a.first < b.first; });
    if (it == m.exports.begin()) { *exportRva = 0; return nullptr; }
    --it;
    *exportRva = it->first;
    return it->second;
}

// ---- guest code map ----------------------------------------------------------------
enum NodeKind : uint8_t { kNone = 0, kGuest = 1, kHook = 2, kImport = 3 };
struct Start { uintptr_t host; uint32_t guest; NodeKind kind; };
std::vector<Start> g_starts;           // every known host function start, sorted by host
std::vector<Start> g_recompStarts;     // recompiled bodies only (guest kind), sorted by host
uintptr_t g_recompLo = 0, g_recompHi = 0;
int g_exeMod = -1;
bool g_rtSyms = false;   // DbgHelp found hc09.pdb: runtime functions are named nodes

const Start* ExactStart(uintptr_t host)
{
    auto it = std::lower_bound(g_starts.begin(), g_starts.end(), host, [](const Start& s, uintptr_t h) { return s.host < h; });
    return it != g_starts.end() && it->host == host ? &*it : nullptr;
}
const Start* RecompBelow(uintptr_t pc)
{
    auto it = std::upper_bound(g_recompStarts.begin(), g_recompStarts.end(), pc, [](uintptr_t h, const Start& s) { return h < s.host; });
    return it == g_recompStarts.begin() ? nullptr : &*(it - 1);
}

// Targets of rel32 calls/jumps and RIP-relative LEAs in [p, end).
void CodeTargets(const uint8_t* p, const uint8_t* end, std::vector<uintptr_t>& out)
{
    for (const uint8_t* q = p; q + 5 <= end; ++q)
    {
        if (*q == 0xE8 || *q == 0xE9)
        {
            int32_t d; memcpy(&d, q + 1, 4);
            out.push_back(uintptr_t(q + 5 + d));
        }
        else if ((q[0] == 0x48 || q[0] == 0x4C) && q + 7 <= end && q[1] == 0x8D && (q[2] & 0xC7) == 0x05)
        {
            int32_t d; memcpy(&d, q + 3, 4);
            out.push_back(uintptr_t(q + 7 + d));
        }
    }
}

void BuildGuestMap()
{
    RefreshModules();
    g_exeMod = ModuleOf(reinterpret_cast<uintptr_t>(&ProfilerStart));
    if (g_exeMod < 0) return;
    const Module& exe = g_mods[g_exeMod];
    std::unordered_set<uint32_t> importSlots;
    for (uint32_t i = 0; i < g_xexImportSlotCount; ++i) importSlots.insert(g_xexImportSlots[i].va);
    // The recompiled translation units are linked before the runtime's (CMakeLists:
    // PPC_SOURCES first), so recompiled code lies below the runtime's code; runtime.cpp
    // (InstallFunctionTable) is the first runtime unit.
    uintptr_t runtimeLo = reinterpret_cast<uintptr_t>(&InstallFunctionTable);
    std::vector<Start> hooks;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
    {
        uintptr_t h = reinterpret_cast<uintptr_t>(m->host);
        uint32_t g = uint32_t(m->guest);
        if (importSlots.count(g)) g_starts.push_back({ h, g, kImport });
        else if (h < runtimeLo) g_recompStarts.push_back({ h, g, kGuest });
        else hooks.push_back({ h, g, kHook });
    }
    std::sort(g_recompStarts.begin(), g_recompStarts.end(), [](const Start& a, const Start& b) { return a.host < b.host; });
    if (g_recompStarts.empty()) return;
    g_recompLo = g_recompStarts.front().host;
    const RUNTIME_FUNCTION* last = FunctionEntry(exe, g_recompStarts.back().host);
    g_recompHi = last ? exe.base + last->EndAddress : g_recompStarts.back().host + 1;
    std::unordered_set<uintptr_t> recompHosts;
    for (const Start& s : g_recompStarts) recompHosts.insert(s.host);
    // A hooked function's original body (__imp__sub_X) has no table entry: find it as
    // the call/LEA target in the wrapper (or one call deeper) that starts a function in
    // the recompiled code, is no other function's start, and lies between X's
    // neighbours there (each unit holds its functions in guest order).
    auto fits = [&](uintptr_t t, uint32_t g) {
        if (t < g_recompLo || t >= g_recompHi || recompHosts.count(t)) return false;
        const RUNTIME_FUNCTION* f = FunctionEntry(exe, t);
        if (f && exe.base + f->BeginAddress != t) return false;
        const Start* below = RecompBelow(t);
        auto above = std::upper_bound(g_recompStarts.begin(), g_recompStarts.end(), t, [](uintptr_t h, const Start& s) { return h < s.host; });
        return below && below->guest < g && (above == g_recompStarts.end() || above->guest > g);
    };
    auto bodyOf = [&](uintptr_t h, uint32_t g, int depth, auto&& self) -> uintptr_t {
        const RUNTIME_FUNCTION* f = FunctionEntry(exe, h);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(h);
        const uint8_t* end = f ? reinterpret_cast<const uint8_t*>(exe.base + f->EndAddress) : p + 256;
        if (end - p > 0x4000) end = p + 0x4000;
        std::vector<uintptr_t> targets;
        CodeTargets(p, end, targets);
        for (uintptr_t t : targets)
            if (fits(t, g)) return t;
        if (depth > 0)
            for (uintptr_t t : targets)
                if (t >= runtimeLo && t < exe.end && t != h)
                    if (uintptr_t b = self(t, g, depth - 1, self)) return b;
        return 0;
    };
    size_t found = 0;
    std::vector<Start> bodies;
    std::map<uint32_t, int> missing;   // by 1 MiB of guest address
    for (const Start& s : hooks)
    {
        g_starts.push_back(s);
        if (uintptr_t b = bodyOf(s.host, s.guest, 1, bodyOf)) { bodies.push_back({ b, s.guest, kGuest }); ++found; }
        else ++missing[s.guest >> 20];
    }
    if (!missing.empty())
    {
        std::string m;
        for (auto& [r, n] : missing) m += " " + std::to_string(n) + "@" + std::to_string(r);
        fprintf(stderr, "[profile] hooked functions whose original was not found, by MiB of guest address:%s\n", m.c_str());
    }
    for (const Start& b : bodies) g_recompStarts.push_back(b);
    std::sort(g_recompStarts.begin(), g_recompStarts.end(), [](const Start& a, const Start& b) { return a.host < b.host; });
    for (const Start& s : g_recompStarts) g_starts.push_back(s);
    std::sort(g_starts.begin(), g_starts.end(), [](const Start& a, const Start& b) { return a.host < b.host; });
    // Runtime symbols (HC09_RUNTIME_PDB builds): names for the runtime's own functions.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (SymInitialize(GetCurrentProcess(), nullptr, FALSE))
    {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        IMAGEHLP_MODULEW64 info{};
        info.SizeOfStruct = sizeof(info);
        if (n && SymLoadModuleExW(GetCurrentProcess(), nullptr, path, nullptr, exe.base, DWORD(exe.end - exe.base), nullptr, 0) &&
            SymGetModuleInfoW64(GetCurrentProcess(), exe.base, &info) && info.SymType == SymPdb)
            g_rtSyms = true;
    }
    fprintf(stderr, "[profile] runtime symbols: %s\n", g_rtSyms ? "hc09.pdb (runtime functions named)" : "none (runtime code by guest caller)");
    fprintf(stderr, "[profile] guest code: %zu recompiled functions in %.1f MiB of host code, %zu hooked (original found for %zu), "
            "%zu imports\n", g_recompStarts.size() - found, double(g_recompHi - g_recompLo) / 1048576.0, hooks.size(), found,
            g_starts.size() - g_recompStarts.size() - hooks.size());
}

// ---- classification ----------------------------------------------------------------
// A sample's frames become keys: kind in the top byte.
enum KeyKind : uint8_t {
    kkGuest = 1,      // guest function (recompiled body)              value: guest address
    kkHook = 2,       // a runtime wrapper of a guest function          value: guest address
    kkImport = 3,     // an import's runtime implementation             value: guest thunk
    kkRuntime = 4,    // other runtime code under a guest-level caller  value: that caller's key (low 40 bits)
    kkOs = 5, kkOsWait = 6, kkCrt = 7, kkDriver = 8, kkOther = 9,   // value: module << 32 | export rva
    kkUnknown = 10,   // not in a module
    kkRtFn = 11,      // a runtime function (with symbols)                  value: its rva in hc09.exe
};
uint64_t Key(KeyKind k, uint64_t v) { return (uint64_t(k) << 56) | v; }
KeyKind KindOfKey(uint64_t k) { return KeyKind(k >> 56); }
constexpr uint32_t kD3DLo = GameAddr(0x82320000), kD3DHi = GameAddr(0x8233DBB8);

// The guest-level node of one host pc (kNone: not guest-level).
Start GuestLevel(uintptr_t pc)
{
    if (g_exeMod < 0) return { 0, 0, kNone };
    const Module& exe = g_mods[g_exeMod];
    if (pc < exe.base || pc >= exe.end) return { 0, 0, kNone };
    const RUNTIME_FUNCTION* f = FunctionEntry(exe, pc);
    if (f)
    {
        if (const Start* s = ExactStart(exe.base + f->BeginAddress)) return *s;
    }
    if (pc >= g_recompLo && pc < g_recompHi)
        if (const Start* s = RecompBelow(pc)) return { s->host, s->guest, kGuest };   // leaf without unwind data
    return { 0, 0, kNone };
}

// The runtime function (not guest code, not a table entry) containing pc, as an rva of
// hc09.exe; 0 without symbols or unwind data.
uint32_t RuntimeFn(uintptr_t pc)
{
    if (!g_rtSyms || g_exeMod < 0) return 0;
    const Module& exe = g_mods[g_exeMod];
    if (pc < exe.base || pc >= exe.end || (pc >= g_recompLo && pc < g_recompHi)) return 0;
    const RUNTIME_FUNCTION* f = FunctionEntry(exe, pc);
    if (!f || ExactStart(exe.base + f->BeginAddress)) return 0;
    return f->BeginAddress;
}

struct Stats
{
    uint64_t samples = 0;
    double suspendedUs = 0;
    std::unordered_map<uint64_t, uint64_t> self, incl, cat;
    std::unordered_map<uint64_t, uint64_t> inclGuest;   // inclusive, samples whose leaf is guest code (not D3D)
    // callee -> caller -> n: guest-level edges, and non-guest leaves -> their guest-level caller
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> callers;
    void Clear() { samples = 0; suspendedUs = 0; self.clear(); incl.clear(); inclGuest.clear(); cat.clear(); callers.clear(); }
};

enum Cat : uint8_t {
    cGuest, cGuestD3D, cHook, cFrontEnd, cRuntimeHook, cImport, cRuntimeGuest, cRuntime, cOsWait, cOs, cCrt, cDriver,
    cOther, cCount
};
const char* const kCatNames[cCount] = {
    "guest code", "guest D3D (0x82320000-0x8233DBB8)", "hook wrappers", "native front end (runtime under D3D hooks)",
    "runtime under other hooks", "imports (runtime)", "runtime helpers under guest code", "runtime (no guest caller)",
    "OS: in a system call (waits, yields)", "OS: other", "CRT", "GPU driver", "other DLLs",
};

uint64_t NodeKey(const Start& s)
{
    return Key(s.kind == kImport ? kkImport : s.kind == kHook ? kkHook : kkGuest, s.guest);
}

void Classify(const uint64_t* pcs, int n, Stats& st)
{
    ++st.samples;
    // Guest-level chain (hook wrappers count as their guest function), leaf first.
    uint64_t chain[128];
    int nc = 0;
    int firstKnown = -1;
    for (int i = 0; i < n && nc < 128; ++i)
    {
        Start s = GuestLevel(uintptr_t(pcs[i]));
        uint64_t k;
        if (s.kind == kNone)
        {
            uint32_t rva = RuntimeFn(uintptr_t(pcs[i]));
            if (!rva) continue;
            k = Key(kkRtFn, rva);
        }
        else
        {
            if (firstKnown < 0) firstKnown = i;
            k = s.kind == kImport ? Key(kkImport, s.guest) : Key(kkGuest, s.guest);
        }
        if (nc && chain[nc - 1] == k) continue;
        chain[nc++] = k;
    }
    // Self: the leaf.
    uint64_t leaf;
    Cat cat;
    uintptr_t pc = uintptr_t(pcs[0]);
    int mi = ModuleOf(pc);
    Start ls = GuestLevel(pc);
    Start anc = firstKnown >= 0 ? GuestLevel(uintptr_t(pcs[firstKnown])) : Start{ 0, 0, kNone };
    if (ls.kind != kNone)
    {
        leaf = NodeKey(ls);
        cat = ls.kind == kImport ? cImport : ls.kind == kHook ? cHook
            : (ls.guest >= kD3DLo && ls.guest < kD3DHi) ? cGuestD3D : cGuest;
    }
    else if (mi >= 0 && mi == g_exeMod)
    {
        uint32_t rva = RuntimeFn(pc);
        leaf = rva ? Key(kkRtFn, rva) : Key(kkRuntime, anc.kind == kNone ? 0 : (NodeKey(anc) & 0xFFFFFFFFFFull) | (uint64_t(anc.kind) << 40));
        cat = anc.kind == kNone ? cRuntime : anc.kind == kImport ? cImport
            : anc.kind == kHook ? ((anc.guest >= kD3DLo && anc.guest < kD3DHi) ? cFrontEnd : cRuntimeHook) : cRuntimeGuest;
    }
    else if (mi >= 0)
    {
        Module& m = g_mods[mi];
        uint32_t erva = 0;
        ExportName(m, uint32_t(pc - m.base), &erva);
        KeyKind kk = m.kind == kModOs ? kkOs : m.kind == kModCrt ? kkCrt : m.kind == kModDriver ? kkDriver : kkOther;
        cat = m.kind == kModOs ? cOs : m.kind == kModCrt ? cCrt : m.kind == kModDriver ? cDriver : cOther;
        if (m.kind == kModOs && pc - 2 >= m.base)
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(pc - 2);
            if (b[0] == 0x0F && b[1] == 0x05) { kk = kkOsWait; cat = cOsWait; }
        }
        leaf = Key(kk, (uint64_t(mi) << 32) | erva);
    }
    else
    {
        leaf = Key(kkUnknown, 0);
        cat = cOther;
    }
    ++st.self[leaf];
    ++st.cat[cat];
    if (ls.kind == kNone)   // a non-guest leaf: its nearest caller node (a runtime function or guest-level)
        for (int i = 0; i < nc; ++i)
            if (chain[i] != leaf) { st.callers[leaf][chain[i]]++; break; }
    // Inclusive: each guest-level function once per sample; caller edges.
    for (int i = 0; i < nc; ++i)
    {
        bool seen = false;
        for (int j = 0; j < i && !seen; ++j) seen = chain[j] == chain[i];
        if (!seen) { ++st.incl[chain[i]]; if (cat == cGuest) ++st.inclGuest[chain[i]]; }
        if (i + 1 < nc)
        {
            bool dup = false;
            for (int j = 0; j < i && !dup; ++j) dup = chain[j] == chain[i] && chain[j + 1] == chain[i + 1];
            if (!dup) st.callers[chain[i]][chain[i + 1]]++;
        }
    }
}

std::string KeyName(uint64_t k)
{
    char buf[256];
    uint64_t v = k & ((1ull << 56) - 1);
    switch (KindOfKey(k))
    {
    case kkGuest: snprintf(buf, sizeof(buf), "sub_%08X", uint32_t(v)); break;
    case kkHook: snprintf(buf, sizeof(buf), "hook:sub_%08X", uint32_t(v)); break;
    case kkImport: snprintf(buf, sizeof(buf), "imp:%08X", uint32_t(v)); break;
    case kkRuntime:
    {
        uint32_t g = uint32_t(v);
        NodeKind nk = NodeKind((v >> 40) & 0xFF);
        if (!v) snprintf(buf, sizeof(buf), "runtime");
        else snprintf(buf, sizeof(buf), "runtime<%s%08X", nk == kImport ? "imp:" : nk == kHook ? "hook:sub_" : "sub_", g);
        break;
    }
    case kkUnknown: snprintf(buf, sizeof(buf), "?"); break;
    case kkRtFn:
    {
        static std::unordered_map<uint32_t, std::string> names;
        auto it = names.find(uint32_t(v));
        if (it != names.end()) return it->second;
        alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(sbuf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::string s = "rt:";
        if (SymFromAddr(GetCurrentProcess(), g_mods[g_exeMod].base + v, &disp, si)) s += std::string(si->Name, si->NameLen);
        else { snprintf(buf, sizeof(buf), "hc09+%X", uint32_t(v)); s += buf; }
        for (char& c : s) if (c == ' ' || c == '\t') c = '_';
        if (s.size() > 120) s.resize(120);
        return names.emplace(uint32_t(v), s).first->second;
    }
    default:
    {
        uint32_t mi = uint32_t(v >> 32), rva = uint32_t(v);
        Module& m = g_mods[mi];
        uint32_t erva = 0;
        const char* e = ExportName(m, rva, &erva);
        snprintf(buf, sizeof(buf), "%s%s!%s", KindOfKey(k) == kkOsWait ? "wait:" : "", m.name.c_str(), e ? e : "?");
        for (char* c = buf; *c; ++c) if (*c == ' ') *c = '_';
    }
    }
    return buf;
}

// ---- sampler ----------------------------------------------------------------------
struct Target
{
    Thread th;
    std::string name;
    Stats st;
};

bool Matches(const std::vector<std::string>& sel, uint64_t tid, const std::string& name)
{
    std::string ln;
    for (char c : name) ln.push_back(char(tolower((unsigned char)c)));
    for (const std::string& s : sel)
    {
        if (s == "all") return true;
        if (!s.empty() && std::all_of(s.begin(), s.end(), ::isdigit)) { if (tid && std::to_string(tid) == s) return true; continue; }
        if (!ln.empty() && ln.find(s) != std::string::npos) return true;
    }
    return false;
}

// Walk the suspended thread's stack. No allocation, no locks.
int Unwind(CONTEXT& c, const Thread& th, uint64_t* pcs, int max)
{
    int n = 0;
    while (n < max)
    {
        uintptr_t pc = uintptr_t(c.Rip);
        if (!pc) break;
        pcs[n++] = pc;
        if (c.Rsp < th.stackLo || c.Rsp + 8 > th.stackHi) break;
        int mi = ModuleOf(pc);
        if (mi < 0) break;
        const Module& m = g_mods[mi];
        const RUNTIME_FUNCTION* f = m.pdata ? FunctionEntry(m, pc) : nullptr;
        if (!f)
        {
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);   // leaf: return address on top
            c.Rsp += 8;
        }
        else
        {
            void* hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, m.base, pc, const_cast<PRUNTIME_FUNCTION>(f), &c, &hd, &est, nullptr);
        }
        if (c.Rsp < th.stackLo || c.Rsp > th.stackHi) break;
    }
    return n;
}

void Report(std::vector<Target>& targets, double seconds, uint64_t swap0, uint64_t swap1, FILE* out, int window)
{
    uint64_t frames = swap1 - swap0;
    double msPerFrame = frames ? seconds * 1000.0 / double(frames) : 0;
    fprintf(stderr, "[profile] window %d: %.1f s, swaps %llu-%llu (%.1f frames/s)\n", window, seconds,
            (unsigned long long)swap0, (unsigned long long)swap1, double(frames) / seconds);
    if (out)
        fprintf(out, "window %d seconds %.3f swaps %llu %llu\n", window, seconds, (unsigned long long)swap0,
                (unsigned long long)swap1);
    for (Target& t : targets)
    {
        Stats& st = t.st;
        if (!st.samples) continue;
        // ms per frame of a share of this thread's samples (wall clock).
        auto ms = [&](uint64_t n) { return double(n) / double(st.samples) * msPerFrame; };
        fprintf(stderr, "[profile]   %s (tid %llu): %llu samples, suspended %.2f ms/s (%.2f%%), per frame %.2f ms:",
                t.name.c_str(), (unsigned long long)t.th.tid, (unsigned long long)st.samples, st.suspendedUs / 1000.0 / seconds,
                st.suspendedUs / 10000.0 / seconds, msPerFrame);
        for (int c = 0; c < cCount; ++c)
            if (st.cat.count(c)) fprintf(stderr, " | %s %.2f", kCatNames[c], ms(st.cat[c]));
        fputc('\n', stderr);
        auto top = [](const std::unordered_map<uint64_t, uint64_t>& m, size_t n) {
            std::vector<std::pair<uint64_t, uint64_t>> v;
            for (auto& [k, c] : m) v.push_back({ c, k });
            std::sort(v.rbegin(), v.rend());
            if (v.size() > n) v.resize(n);
            return v;
        };
        auto list = [&](const char* what, const std::unordered_map<uint64_t, uint64_t>& m) {
            std::string line;
            char b[32];
            for (auto& [c, k] : top(m, 8)) { snprintf(b, sizeof(b), " %.2f", ms(c)); line += " " + KeyName(k) + b; }
            fprintf(stderr, "[profile]     %s (ms/frame):%s\n", what, line.c_str());
        };
        list("self", st.self);
        list("inclusive", st.incl);
        if (out)
        {
            fprintf(out, "thread %llu %s samples %llu suspended_us %.0f\n", (unsigned long long)t.th.tid, t.name.c_str(),
                    (unsigned long long)st.samples, st.suspendedUs);
            for (auto& [c, n] : st.cat) fprintf(out, "cat %s|%llu\n", kCatNames[c], (unsigned long long)n);
            for (auto& [k, n] : st.self) fprintf(out, "self %s %llu\n", KeyName(k).c_str(), (unsigned long long)n);
            for (auto& [k, n] : st.incl) fprintf(out, "incl %s %llu\n", KeyName(k).c_str(), (unsigned long long)n);
            for (auto& [k, n] : st.inclGuest) fprintf(out, "inclg %s %llu\n", KeyName(k).c_str(), (unsigned long long)n);
            for (auto& [callee, m] : st.callers)
                for (auto& [caller, n] : m)
                    fprintf(out, "edge %s %s %llu\n", KeyName(callee).c_str(), KeyName(caller).c_str(), (unsigned long long)n);
        }
        st.Clear();
    }
    if (out) fflush(out);
}

void SamplerMain(std::vector<std::string> sel, int hz, double every, std::string outPath)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    FILE* out = outPath.empty() ? nullptr : fopen(outPath.c_str(), "a");
    if (out) fprintf(out, "# hc09 profile: HC09_PROFILE_HZ=%d\n", hz);
    std::vector<Target> targets;
    std::vector<uint64_t> pcs(256);
    using clock = std::chrono::steady_clock;
    auto windowStart = clock::now(), lastSelect = clock::time_point{}, next = clock::now();
    const auto period = std::chrono::nanoseconds(1000000000 / hz);
    uint64_t swap0 = GpuSwapCount();
    int window = 0;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    for (;;)
    {
        auto now = clock::now();
        if (now - lastSelect > std::chrono::seconds(1))
        {
            // (Re)select: the game names its threads after they start, and renames some.
            lastSelect = now;
            RefreshModules();
            std::vector<Thread> all;
            { std::lock_guard<std::mutex> lk(g_threadMu); all = g_threads; }
            for (const Thread& th : all)
            {
                std::string name = th.tid ? KernelThreadName(th.tid) : th.hostName;
                bool want = Matches(sel, th.tid, name);
                auto it = std::find_if(targets.begin(), targets.end(), [&](const Target& t) { return t.th.handle == th.handle; });
                if (want && it == targets.end())
                {
                    targets.push_back({ th, name, {} });
                    fprintf(stderr, "[profile] sampling \"%s\" (tid %llu)\n", name.c_str(), (unsigned long long)th.tid);
                }
                else if (it != targets.end()) it->name = name;
            }
        }
        for (Target& t : targets)
        {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            if (SuspendThread(t.th.handle) == DWORD(-1)) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            int n = 0;
            if (GetThreadContext(t.th.handle, &c)) n = Unwind(c, t.th, pcs.data(), int(pcs.size()));
            ResumeThread(t.th.handle);
            QueryPerformanceCounter(&b);
            t.st.suspendedUs += double(b.QuadPart - a.QuadPart) * 1e6 / double(qf.QuadPart);
            if (n) Classify(pcs.data(), n, t.st);
        }
        double sec = std::chrono::duration<double>(clock::now() - windowStart).count();
        if (sec >= every)
        {
            uint64_t swap1 = GpuSwapCount();
            Report(targets, sec, swap0, swap1, out, window++);
            swap0 = swap1;
            windowStart = clock::now();
        }
        // Next tick on a fixed schedule (late ticks are dropped, not caught up).
        next += period;
        auto after = clock::now();
        if (next < after) next = after;
        if (timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next - after).count() / 100);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        else Sleep(1);
    }
}

}  // namespace

void ProfilerRegisterThread(uint64_t tid, const char* name)
{
    if (!Enabled()) return;
    Thread th;
    th.tid = tid;
    if (name) th.hostName = name;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &th.handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    th.stackHi = reinterpret_cast<uintptr_t>(tib->StackBase);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<const uint8_t*>(tib->StackBase) - 1, &mbi, sizeof(mbi));
    th.stackLo = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    std::lock_guard<std::mutex> lk(g_threadMu);
    g_threads.push_back(th);
}

void ProfilerStart()
{
    if (!Enabled()) return;
    std::string spec = Hc09Env("HC09_PROFILE");
    for (char& c : spec) c = char(tolower((unsigned char)c));
    // The game names its loop threads twice (LoopRender, then FGGameRender, ...).
    if (spec == "1" || spec == "on") spec = "fggamerender,looprender,fggamesim,loopsimulation,gpu command processor";
    std::vector<std::string> sel;
    for (size_t p = 0; p <= spec.size();)
    {
        size_t q = spec.find(',', p);
        if (q == std::string::npos) q = spec.size();
        if (q > p) sel.push_back(spec.substr(p, q - p));
        p = q + 1;
    }
    int hz = 1000;
    if (const char* e = Hc09Env("HC09_PROFILE_HZ")) hz = std::max(1, atoi(e));
    double every = 10;
    if (const char* e = Hc09Env("HC09_PROFILE_EVERY")) every = std::max(1.0, atof(e));
    std::string out = "captures/profile.txt";
    if (const char* e = Hc09Env("HC09_PROFILE_OUT")) out = e;
    BuildGuestMap();
    fprintf(stderr, "[profile] HC09_PROFILE=%s: %d samples/s per thread, report every %.0f s, counts -> %s\n", spec.c_str(), hz,
            every, out.empty() ? "(none)" : out.c_str());
    std::thread(SamplerMain, sel, hz, every, out).detach();
}

#else
void ProfilerRegisterThread(uint64_t, const char*) {}
void ProfilerStart() {}
#endif
