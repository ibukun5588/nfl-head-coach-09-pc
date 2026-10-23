// hc09-recomp runtime core: guest memory, image loader, indirect-call table,
// thread/stack bootstrap, and the host entry point.
#include "dev_env.h"
#include "runtime.h"
#include "crash_report.h"
#include "kernel.h"
#include "xex_imports.h"
#include "build_id.h"
#include "game_addr.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// ---- Guest memory --------------------------------------------------------

uint8_t* g_base = nullptr;

// Memory map. Everything the game can hand to the GPU must have a physical
// address, and on the console *all* RAM is physical: the virtual heap at
// 0x40000000 and the image at 0x82000000 are just page mappings of the same
// 512 MiB. We reproduce that: one 512 MiB file backs physical memory, and
//   va 0x40000000..0x5FFFFFFF  (virtual heap, 64K-page region)  -> pa va & 0x1FFFFFFF
//   va 0x82000000..image end   (the executable)                  -> pa 0x02000000..
//   va 0xA/0xC/0xE0000000      (physical views)                  -> pa va & 0x1FFFFFFF
// so MmGetPhysicalAddress is a mask and the GPU's physical reads see the
// bytes the CPU wrote. The virtual heap grows up from pa 0x04000000 (above
// the image's physical home), physical allocations grow down from 512 MiB,
// and the two share the budget like the real machine.
static constexpr uint32_t kPhysicalSize = 0x20000000;      // 512 MiB
static constexpr uint32_t kImagePhys    = 0x02000000;      // where the image lives physically
static constexpr uint32_t kStackSize    = 0x00100000;      // 1 MiB initial-thread stack (heap-allocated)
static constexpr uint32_t kHeapBase     = 0x43240000;      // bump heap grows upward, right after the image (pa 0x03240000)
static constexpr uint32_t kHeapEnd      = 0x60000000;      // end of the physically-backed virtual region

static std::atomic<uint32_t> g_heapCur{ kHeapBase };
static std::atomic<uint32_t> g_physCur{ kPhysicalSize };   // physical allocations grow downward

uint32_t GuestAlloc(uint32_t size, uint32_t align)
{
    if (align < 16) align = 16;
    uint32_t cur, aligned, next;
    do
    {
        cur = g_heapCur.load(std::memory_order_relaxed);
        aligned = (cur + (align - 1)) & ~(align - 1);
        next = aligned + ((size + 15) & ~15u);
        if (next > kHeapEnd || (next & 0x1FFFFFFF) > g_physCur.load(std::memory_order_relaxed))
        {
            fprintf(stderr, "[runtime] GuestAlloc OOM requesting %u bytes (heap at 0x%08X, physical allocations down to pa 0x%08X)\n",
                    size, cur, g_physCur.load());
            return 0;
        }
    } while (!g_heapCur.compare_exchange_weak(cur, next, std::memory_order_relaxed));
    return aligned;
}

#ifdef _WIN32
// Windows: the same layout built from placeholders. Reserve the 4 GiB range
// as one placeholder, split it at every alias boundary, map the pagefile-backed
// 512 MiB section into the alias slots and turn the gaps into private memory.
// VirtualAlloc2/MapViewOfFile3 (Windows 10 1803+) come from kernelbase.dll;
// resolved at run time so no onecore.lib is needed.
static void MapGuestMemory()
{
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto pVirtualAlloc2 = kb ? reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2")) : nullptr;
    auto pMapViewOfFile3 = kb ? reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3")) : nullptr;
    if (!pVirtualAlloc2 || !pMapViewOfFile3)
    { fprintf(stderr, "[runtime] VirtualAlloc2/MapViewOfFile3 unavailable (Windows 10 1803+ required)\n"); exit(1); }

    HANDLE proc = GetCurrentProcess();
    uint8_t* base = static_cast<uint8_t*>(pVirtualAlloc2(proc, nullptr, PPC_MEMORY_SIZE,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!base) { fprintf(stderr, "[runtime] reserve 4GiB guest space failed (%lu)\n", GetLastError()); exit(1); }
    g_base = base;

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        0, kPhysicalSize, nullptr);
    if (!section) { fprintf(stderr, "[runtime] physical memory section failed (%lu)\n", GetLastError()); exit(1); }

    // Address order, covering [0, 4 GiB) exactly. shared = view of the section at `offset`.
    uint64_t imageMapSize = (PPC_IMAGE_SIZE + 0xFFFF) & ~0xFFFFull;
    struct Region { uint64_t va, size, offset; bool shared; };
    const Region regions[] = {
        { 0x00000000ull, kPhysicalSize, 0, true },
        { 0x20000000ull, 0x20000000ull, 0, false },
        { 0x40000000ull, kPhysicalSize, 0, true },
        { 0x60000000ull, PPC_IMAGE_BASE - 0x60000000ull, 0, false },
        { PPC_IMAGE_BASE, imageMapSize, kImagePhys, true },
        { PPC_IMAGE_BASE + imageMapSize, 0xA0000000ull - (PPC_IMAGE_BASE + imageMapSize), 0, false },  // function table
        { 0xA0000000ull, kPhysicalSize, 0, true },
        { 0xC0000000ull, kPhysicalSize, 0, true },
        { 0xE0000000ull, kPhysicalSize, 0, true },
    };
    const size_t n = sizeof(regions) / sizeof(regions[0]);
    for (size_t i = 0; i < n; ++i)
    {
        const Region& r = regions[i];
        // Split this region off the front of the remaining placeholder (the last one is what remains).
        if (i + 1 < n && !VirtualFree(base + r.va, r.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        { fprintf(stderr, "[runtime] split placeholder at 0x%08llX failed (%lu)\n", (unsigned long long)r.va, GetLastError()); exit(1); }
        void* p = r.shared
            ? pMapViewOfFile3(section, proc, base + r.va, r.offset, r.size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)
            : pVirtualAlloc2(proc, base + r.va, r.size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (p != base + r.va)
        { fprintf(stderr, "[runtime] map guest 0x%08llX (+0x%llX) failed (%lu)\n", (unsigned long long)r.va, (unsigned long long)r.size, GetLastError()); exit(1); }
    }
    fprintf(stderr, "[runtime] guest memory @ host %p (4 GiB; 512 MiB physical RAM backs 0x40000000 heap, image @ pa 0x%08X, views A/C/E)\n",
            g_base, kImagePhys);
}
#else
static void MapGuestMemory()
{
    // One flat 4 GiB mapping. MAP_NORESERVE keeps it lazy so we only pay for
    // pages actually touched (image, stack, heap, function table).
    void* p = mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
    {
        perror("[runtime] mmap 4GiB guest space");
        exit(1);
    }
    g_base = static_cast<uint8_t*>(p);

    // Physical memory. The console has 512 MiB that the kernel maps into
    // the virtual space several times over (0xA0000000 64K pages, 0xC0000000
    // 16M pages, 0xE0000000 4K pages), and the GPU addresses it by physical
    // address (= virtual & 0x1FFFFFFF). Back all of those with one shared
    // mapping so a buffer allocated at 0xA0xxxxxx is the same bytes the GPU
    // reads at 0x00xxxxxx.
    int fd = memfd_create("hc09-physical", 0);
    if (fd < 0 || ftruncate(fd, kPhysicalSize) != 0) { perror("[runtime] physical memory"); exit(1); }
    for (uint32_t va : { 0x00000000u, 0x40000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u })
        if (mmap(g_base + va, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) == MAP_FAILED)
        { perror("[runtime] mmap physical alias"); exit(1); }
    // The executable: va 0x82000000.. is pa 0x02000000.. (page-rounded).
    uint32_t imageMapSize = (PPC_IMAGE_SIZE + 0xFFFF) & ~0xFFFFu;
    if (mmap(g_base + PPC_IMAGE_BASE, imageMapSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, kImagePhys) == MAP_FAILED)
    { perror("[runtime] mmap image alias"); exit(1); }
    fprintf(stderr, "[runtime] guest memory @ host %p (4 GiB; 512 MiB physical RAM backs 0x40000000 heap, image @ pa 0x%08X, views A/C/E)\n",
            g_base, kImagePhys);
}
#endif

// Physical allocations come from the top-down of physical memory, like the
// console (titles get the high pages; the low 1 MiB stays zero for null
// dereference safety), returned as 0xA0000000-based virtual addresses.
static std::mutex g_physMu;
static std::vector<std::pair<uint32_t, uint32_t>> g_physBlocks;   // (pa, size) in allocation order

void PhysicalFree(uint32_t va)
{
    std::lock_guard<std::mutex> lk(g_physMu);
    uint32_t pa = va & 0x1FFFFFFF;
    if (!g_physBlocks.empty() && g_physBlocks.back().first == pa)
    {
        // Most recent block: give the space back (the common free-after-use pattern).
        g_physCur.store(pa + g_physBlocks.back().second);
        g_physBlocks.pop_back();
        // Also release any older blocks that were already freed-but-pinned? (none tracked) 
        return;
    }
    fprintf(stderr, "[kernel] MmFreePhysicalMemory(0x%08X): not the most recent block, leaking\n", va);
}

uint32_t PhysicalAlloc(uint32_t size, uint32_t align)
{
    std::lock_guard<std::mutex> lk(g_physMu);
    if (align < 0x1000) align = 0x1000;
    size = (size + 0xFFF) & ~0xFFFu;
    uint32_t cur, next;
    do
    {
        cur = g_physCur.load(std::memory_order_relaxed);
        next = (cur - size) & ~(align - 1);
        if (next < 0x100000 || next < (g_heapCur.load(std::memory_order_relaxed) & 0x1FFFFFFF))
        { fprintf(stderr, "[runtime] PhysicalAlloc OOM (%u bytes; heap up to 0x%08X)\n", size, g_heapCur.load()); return 0; }
    } while (!g_physCur.compare_exchange_weak(cur, next, std::memory_order_relaxed));
    g_physBlocks.emplace_back(next, cur - next);
    return 0xA0000000u + next;
}

// ---- Image loader --------------------------------------------------------

static void LoadImage(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[runtime] cannot open image %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || uint64_t(sz) > PPC_IMAGE_SIZE + 0x1000)
    {
        fprintf(stderr, "[runtime] image size %ld unexpected\n", sz);
        exit(1);
    }
    size_t rd = fread(g_base + PPC_IMAGE_BASE, 1, size_t(sz), f);
    fclose(f);
    fprintf(stderr, "[runtime] loaded %zu bytes at guest 0x%llX\n",
            rd, (unsigned long long)PPC_IMAGE_BASE);
    // The image must be the game version this hc09 was built for (1.0 or the 1.2 title update: their code is
    // recompiled separately). Its PE header's TimeDateStamp tells them apart. HC09_IMAGE_CHECK=0 skips the check.
    const uint8_t* img = g_base + PPC_IMAGE_BASE;
    uint32_t pe = rd >= 0x40 ? uint32_t(img[0x3C] | img[0x3D] << 8 | img[0x3E] << 16 | img[0x3F] << 24) : 0;
    uint32_t stamp = pe && pe + 12 <= rd && img[pe] == 'P' && img[pe + 1] == 'E'
        ? uint32_t(img[pe + 8] | img[pe + 9] << 8 | img[pe + 10] << 16 | img[pe + 11] << 24) : 0;
    const char* check = Hc09Env("HC09_IMAGE_CHECK");
    if (stamp != kImageTimestamp && !(check && !strcmp(check, "0")))
    {
        const char* is = stamp == 0x485C057Fu ? "game version 1.0" : stamp == 0x48BF0CB0u ? "game version 1.2"
                       : "not a Head Coach 09 image this port knows";
        char msg[1024];
        snprintf(msg, sizeof msg,
                 "This hc09 is built for game version %s, but %s is %s (image time stamp 0x%08X).\n\n"
                 "Each version has its own hc09.exe and headcoach.basefile: start the one that matches, or pick "
                 "the version in the launcher.", kGameVersion, path, is, stamp);
        fprintf(stderr, "[runtime] *** wrong game version ***\n%s\n", msg);
#ifdef _WIN32
        const char* win = Hc09Env("HC09_GPU_WINDOW");   // unattended runs (hidden/offscreen window): no message box
        if (!win || (strcmp(win, "hidden") && strcmp(win, "0")))
            MessageBoxA(nullptr, msg, "hc09: wrong game version", MB_OK | MB_ICONERROR);
#endif
        exit(4);
    }
    fprintf(stderr, "[runtime] game version %s (image time stamp 0x%08X)\n", kGameVersion, stamp);
}

// ---- Indirect-call lookup table -----------------------------------------

void RegisterGuestFunction(uint32_t guest, PPCFunc* host)
{
    *reinterpret_cast<PPCFunc**>(
        g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE +
        (uint64_t(guest - PPC_CODE_BASE) * 2)) = host;
}

// Host code address -> guest function, for crash reports. Sorted by host
// address so a faulting PC can be mapped with a binary search from a signal
// handler without allocating.
static std::vector<std::pair<uintptr_t, uint32_t>> g_hostToGuest;

static uint32_t GuestFunctionForHostPc(uintptr_t pc)
{
    if (g_hostToGuest.empty()) return 0;
    size_t lo = 0, hi = g_hostToGuest.size();
    while (hi - lo > 1)
    {
        size_t mid = (lo + hi) / 2;
        if (g_hostToGuest[mid].first <= pc) lo = mid; else hi = mid;
    }
    return g_hostToGuest[lo].first <= pc ? g_hostToGuest[lo].second : 0;
}

// The recompiled function table: linked in, or hc09.exe's in the split build (runtime/split_dll.cpp).
#if HC09_SPLIT
const PPCFuncMapping* GameFuncMappings();
#else
static const PPCFuncMapping* GameFuncMappings() { return PPCFuncMappings; }
#endif

void InstallFunctionTable()
{
    size_t n = 0;
    for (const PPCFuncMapping* m = GameFuncMappings(); m->host != nullptr; ++m)
    {
        RegisterGuestFunction(uint32_t(m->guest), m->host);
        g_hostToGuest.emplace_back(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest));
        ++n;
    }
    std::sort(g_hostToGuest.begin(), g_hostToGuest.end());
    fprintf(stderr, "[runtime] installed %zu guest functions\n", n);
}

// ---- Import logging ------------------------------------------------------
//
// Three views of the import stream, so a long run stays legible:
//  * the first 200 calls verbatim (the boot sequence),
//  * every FIRST occurrence of a new import name, always (subsystem progress),
//  * a per-import histogram dumped on exit/termination (exposes spin loops).

#include <map>
#include <string>
#include <mutex>
#include <unordered_map>
#include <csignal>
#include <chrono>
#include <thread>
#include "xenos/xenos.h"
#ifndef _WIN32
#include <ucontext.h>
#endif
#ifndef SIGTRAP
#define SIGTRAP 5   // POSIX number, for the exit code (Windows' <signal.h> lacks it)
#endif

static std::atomic<uint64_t> g_importCalls{ 0 };

// Import call counts without a lock. The game makes ~1-2 M import calls per
// second from six threads; a mutex plus a std::string map lookup per call was
// measurable game-side cost. Every caller passes a string literal, so the
// table is keyed by the name pointer (open addressing, CAS to insert); a name
// reached through two literals gets two slots, merged by content when read.
struct ImportSlot
{
    std::atomic<const char*> name{ nullptr };
    std::atomic<uint64_t> count{ 0 };
};
static constexpr size_t kImportSlots = 4096;
static ImportSlot g_importSlots[kImportSlots];

// The slot of a name pointer, inserted (and its first call counted) if new; *first: the first
// time this name (by content) is seen. Lookups only read: callers count (or batch) the rest.
static ImportSlot* FindSlot(const char* name, bool* inserted, bool* first)
{
    *inserted = *first = false;
    size_t h = size_t((uint64_t(reinterpret_cast<uintptr_t>(name)) * 0x9E3779B97F4A7C15ull) >> 52);
    for (size_t i = 0; i < kImportSlots; ++i)
    {
        ImportSlot& s = g_importSlots[(h + i) & (kImportSlots - 1)];
        const char* cur = s.name.load(std::memory_order_acquire);
        if (!cur)
        {
            if (s.name.compare_exchange_strong(cur, name, std::memory_order_acq_rel))
            {
                s.count.fetch_add(1, std::memory_order_relaxed);
                *inserted = true;
                // New pointer: new name unless another literal of it is counted already.
                *first = true;
                for (const ImportSlot& o : g_importSlots)
                {
                    const char* on = o.name.load(std::memory_order_acquire);
                    if (on && on != name && !strcmp(on, name) && o.count.load(std::memory_order_relaxed)) *first = false;
                }
                return &s;
            }
            // Lost the race: `cur` now holds the winner; fall through to compare.
        }
        if (cur == name) return &s;
    }
    return nullptr;   // table full (thousands of distinct imports: not expected)
}

// Counts by name (snapshot, merged by content).
static std::map<std::string, uint64_t> ImportCounts()
{
    std::map<std::string, uint64_t> m;
    for (const ImportSlot& s : g_importSlots)
        if (const char* n = s.name.load(std::memory_order_acquire)) m[n] += s.count.load(std::memory_order_relaxed);
    return m;
}

static void DumpImportHistogram()
{
    // Lock-free: also safe from a signal handler that interrupted ImportLog.
    std::map<std::string, uint64_t> hist = ImportCounts();
    fprintf(stderr, "\n[runtime] ---- import histogram (%llu calls, %zu distinct) ----\n",
            (unsigned long long)g_importCalls.load(), hist.size());
    // Sort by count, descending.
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& [k, c] : hist) v.emplace_back(c, k);
    std::sort(v.rbegin(), v.rend());
    for (auto& [c, k] : v)
        fprintf(stderr, "  %10llu  %s\n", (unsigned long long)c, k.c_str());
    fflush(stderr);
}

#ifndef _WIN32
static void OnTerminate(int sig)
{
    fprintf(stderr, "\n[runtime] caught signal %d\n", sig);
    DumpImportHistogram();
    _exit(128 + sig);
}
#endif

// Hardware faults (SIGSEGV/SIGFPE/SIGILL/SIGBUS): say which guest function
// and thread, and for memory faults which guest address, then exit.
extern thread_local PPCContext* t_curCtx;

static bool GuestReadable(uint32_t a) { return a >= 0x1000 && a < PPC_MEMORY_SIZE - 8; }

// Dump the guest registers (at -O0 every write to ctx lands in memory, so
// they are current at the fault) and walk the PPC stack back-chain: each
// frame's saved LR sits 8 bytes below the caller's stack pointer.
void DumpGuestBacktrace(const PPCContext& c, const char* indent)
{
    uint32_t sp = c.r1.u32;
    for (int depth = 0; depth < 24 && GuestReadable(sp); ++depth)
    {
        uint32_t callerSp = GLoad32(sp);
        if (!GuestReadable(callerSp) || callerSp <= sp) break;
        uint32_t ret = GLoad32(callerSp - 8);
        if (ret < PPC_CODE_BASE || ret >= PPC_CODE_BASE + PPC_CODE_SIZE) break;
        fprintf(stderr, "%s#%-2d return 0x%08X  (call site 0x%08X, sp 0x%08X)\n",
                indent, depth + 1, ret, ret - 4, callerSp);
        sp = callerSp;
    }
}

// Dump the guest registers (at -O0 every write to ctx lands in memory, so
// they are current at the fault) and walk the PPC stack back-chain: each
// frame's saved LR sits 8 bytes below the caller's stack pointer.
static void DumpGuestState()
{
    PPCContext* c = t_curCtx;
    if (!c) { fprintf(stderr, "          (no guest context on this thread)\n"); return; }
    // Registers the recompiler keeps in C++ locals (tools/build.py --variant with *_as_local options)
    // are not in the context: only what is there is printed.
#ifndef PPC_CONFIG_CTR_AS_LOCAL
    fprintf(stderr, "          lr=0x%08X ctr=0x%08X r1=0x%08X r13=0x%08X\n",
            uint32_t(c->lr), uint32_t(c->ctr.u32), uint32_t(c->r1.u32), uint32_t(c->r13.u32));
#else
    fprintf(stderr, "          lr=0x%08X r1=0x%08X r13=0x%08X\n", uint32_t(c->lr), uint32_t(c->r1.u32), uint32_t(c->r13.u32));
#endif
#define R(n) uint32_t(c->r##n.u32)
    fprintf(stderr, "          r3 =0x%08X r4 =0x%08X r5 =0x%08X r6 =0x%08X r7 =0x%08X\n", R(3), R(4), R(5), R(6), R(7));
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    fprintf(stderr, "          r8 =0x%08X r9 =0x%08X r10=0x%08X r11=0x%08X r12=0x%08X\n", R(8), R(9), R(10), R(11), R(12));
#else
    fprintf(stderr, "          r8 =0x%08X r9 =0x%08X r10=0x%08X\n", R(8), R(9), R(10));
#endif
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    fprintf(stderr, "          r28=0x%08X r29=0x%08X r30=0x%08X r31=0x%08X\n", R(28), R(29), R(30), R(31));
#endif
#undef R
    fprintf(stderr, "          guest backtrace (saved LRs via back-chain):\n");
    DumpGuestBacktrace(*c, "            ");
}

// ---- CPU write watch (xenos::HostWatchWrites, HC09_WATCH_VFETCH) -------------
// The watched pages are made read-only in the four CPU aliases of physical
// memory (0x40000000 heap, 0xA/0xC/0xE0000000 views; the GPU side reads through
// the 0x00000000 alias and is unaffected). A write faults; the handler only
// records the writer (guest function, address, thread, saved-LR backtrace)
// into a ring and unprotects the page, so the store re-executes and succeeds.
// A reporter thread prints the hits and re-protects the pages every 100 ms,
// and reports when the watched bytes change.
namespace {
constexpr uint32_t kWatchViews[4] = { 0x40000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u };
struct WatchRange { uint32_t pa, bytes; };
WatchRange g_watchRanges[16];
std::atomic<uint32_t> g_watchCount{ 0 };
struct WatchHit { uint32_t fn, va, lr, bt[6]; uint64_t tid; uintptr_t pc; };
WatchHit g_watchHits[512];
std::atomic<uint32_t> g_watchHead{ 0 };

// Page protection on guest memory (also called from the fault handler: a
// single system call, no locks).
void ProtectGuest(uint32_t va, uint32_t bytes, bool writable)
{
#ifdef _WIN32
    DWORD old;
    VirtualProtect(g_base + va, bytes, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
#else
    mprotect(g_base + va, bytes, writable ? PROT_READ | PROT_WRITE : PROT_READ);
#endif
}

void WatchProtect(uint32_t pa, uint32_t bytes, bool writable)
{
    uint32_t first = pa & ~0xFFFu, last = (pa + bytes + 0xFFF) & ~0xFFFu;
    for (uint32_t view : kWatchViews) ProtectGuest(view + first, last - first, writable);
}

// Signal handler side: async-signal-safe (no locks, no stdio).
bool WatchHandleFault(uintptr_t faultAddr, uintptr_t pc)
{
    uintptr_t gb = uintptr_t(g_base);
    if (faultAddr < gb || faultAddr >= gb + PPC_MEMORY_SIZE) return false;
    uint32_t va = uint32_t(faultAddr - gb), view = va & 0xE0000000u, pa = va & 0x1FFFFFFFu;
    if (view != 0x40000000u && view != 0xA0000000u && view != 0xC0000000u && view != 0xE0000000u) return false;
    uint32_t n = g_watchCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
    {
        const WatchRange& w = g_watchRanges[i];
        if (pa < (w.pa & ~0xFFFu) || pa >= ((w.pa + w.bytes + 0xFFF) & ~0xFFFu)) continue;
        WatchHit& h = g_watchHits[g_watchHead.fetch_add(1) % 512];
        h.va = va; h.pc = pc; h.fn = GuestFunctionForHostPc(pc); h.tid = KernelCurrentTid();
        h.lr = 0;
        for (uint32_t& b : h.bt) b = 0;
        if (PPCContext* c = t_curCtx)
        {
            h.lr = uint32_t(c->lr);
            uint32_t sp = c->r1.u32;
            for (int d = 0; d < 6 && GuestReadable(sp); ++d)
            {
                uint32_t callerSp = GLoad32(sp);
                if (!GuestReadable(callerSp) || callerSp <= sp) break;
                h.bt[d] = GLoad32(callerSp - 8);
                sp = callerSp;
            }
        }
        for (uint32_t v : kWatchViews) ProtectGuest(v + (pa & ~0xFFFu), 0x1000, true);
        return true;
    }
    return false;
}

void WatchReporter()
{
    uint32_t printed = 0;
    std::map<uint32_t, uint64_t> perFn;
    std::vector<uint8_t> last[16];
    for (uint64_t tick = 0;; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint32_t head = g_watchHead.load();
        if (head - printed > 512) printed = head - 512;
        for (; printed != head; ++printed)
        {
            const WatchHit& h = g_watchHits[printed % 512];
            uint64_t c = ++perFn[h.fn];
            if (c > 3 && (c & (c - 1))) continue;   // first 3, then powers of two
            uint32_t pa = h.va & 0x1FFFFFFF;
            bool inRange = false;
            for (uint32_t i = 0; i < g_watchCount.load(); ++i)
                inRange |= pa >= g_watchRanges[i].pa && pa < g_watchRanges[i].pa + g_watchRanges[i].bytes;
            fprintf(stderr, "[watch] write #%llu by sub_%08X (host pc %p) tid %llu to va 0x%08X%s lr 0x%08X bt",
                    (unsigned long long)c, h.fn, (void*)h.pc, (unsigned long long)h.tid, h.va,
                    inRange ? "" : " (same page, outside range)", h.lr);
            for (uint32_t b : h.bt) if (b) fprintf(stderr, " 0x%08X", b);
            fprintf(stderr, "\n");
        }
        uint32_t n = g_watchCount.load();
        for (uint32_t i = 0; i < n; ++i)
        {
            const WatchRange& w = g_watchRanges[i];
            if (tick % 10 == 0)
            {
                const uint8_t* p = g_base + w.pa;   // GPU alias, never protected
                std::vector<uint8_t> now(p, p + w.bytes);
                if (!last[i].empty() && now != last[i])
                {
                    size_t at = 0;
                    while (at < now.size() && now[at] == last[i][at]) ++at;
                    fprintf(stderr, "[watch] pa 0x%08X changed (first difference at +0x%zX: %02X%02X%02X%02X%02X%02X%02X%02X)\n",
                            w.pa, at, now[at & ~7], now[(at & ~7) + 1], now[(at & ~7) + 2], now[(at & ~7) + 3],
                            now[(at & ~7) + 4], now[(at & ~7) + 5], now[(at & ~7) + 6], now[(at & ~7) + 7]);
                }
                last[i].swap(now);
            }
            WatchProtect(w.pa, w.bytes, false);
        }
    }
}
}  // namespace

// ---- Page write tracking for decoded-data caches (xenos::HostTrackWrites) ----
// The live renderer's texture cache: a decoded texture's pages are made
// read-only in the four CPU views (the GPU/renderer alias at 0 stays writable).
// The first CPU write to such a page faults; the handler stamps the page with
// a new write epoch and makes it writable again (the store re-executes). A
// cache entry is stale once one of its pages carries an epoch newer than the
// one returned when it was decoded. Host code that writes guest memory with a
// system call (file reads) unprotects first: GuestHostWrite.
namespace {
constexpr uint32_t kTrackPages = 0x20000000u >> 12;
constexpr uint8_t kPageProtected = 1, kPageTracked = 2;   // tracked: ever protected by HostTrackWrites
std::atomic<uint64_t> g_trackEpoch{ 1 };
std::atomic<uint64_t>* g_pageEpoch = nullptr;   // per 4 KiB physical page: epoch of its last tracked write
std::atomic<uint8_t>* g_pageState = nullptr;
std::mutex g_trackMu;

void TrackUnprotect(uint32_t page)
{
    g_pageEpoch[page].store(g_trackEpoch.fetch_add(1) + 1, std::memory_order_release);
    for (uint32_t v : kWatchViews) ProtectGuest(v + (page << 12), 0x1000, true);
}

// Fault handler side (no locks): a write to a tracked page.
bool TrackHandleFault(uintptr_t faultAddr)
{
    uintptr_t gb = uintptr_t(g_base);
    if (!g_pageState || faultAddr < gb || faultAddr >= gb + PPC_MEMORY_SIZE) return false;
    uint32_t va = uint32_t(faultAddr - gb), view = va & 0xE0000000u;
    if (view != 0x40000000u && view != 0xA0000000u && view != 0xC0000000u && view != 0xE0000000u) return false;
    uint32_t page = (va & 0x1FFFFFFFu) >> 12;
    uint8_t s = g_pageState[page].load(std::memory_order_acquire);
    if (!(s & kPageTracked)) return false;
    // Only the thread that clears the flag unprotects; others retry until it has.
    if (g_pageState[page].fetch_and(uint8_t(~kPageProtected)) & kPageProtected) TrackUnprotect(page);
    return true;
}
}  // namespace

void GuestHostWrite(uint32_t va, uint32_t bytes)
{
    if (!g_pageState || !bytes) return;
    uint32_t view = va & 0xE0000000u;
    if (view != 0x40000000u && view != 0xA0000000u && view != 0xC0000000u && view != 0xE0000000u && view != 0) return;
    uint32_t pa = va & 0x1FFFFFFFu;
    for (uint32_t page = pa >> 12; page <= (pa + bytes - 1) >> 12 && page < kTrackPages; ++page)
        if (g_pageState[page].fetch_and(uint8_t(~kPageProtected)) & kPageProtected) TrackUnprotect(page);
}

namespace xenos {
uint64_t HostTrackWrites(uint32_t pa, uint32_t bytes)
{
    std::lock_guard<std::mutex> lk(g_trackMu);
    if (!g_pageState)
    {
        g_pageEpoch = new std::atomic<uint64_t>[kTrackPages]();
        g_pageState = new std::atomic<uint8_t>[kTrackPages]();
    }
    uint64_t epoch = g_trackEpoch.load();
    pa &= 0x1FFFFFFFu;
    if (!bytes) return epoch;
    uint32_t first = pa >> 12, last = std::min(kTrackPages - 1, (pa + bytes - 1) >> 12);
    // Flags first, then the protection: a write in between does not fault and
    // lands before the caller decodes the range.
    for (uint32_t page = first; page <= last;)
    {
        if (g_pageState[page].load() & kPageProtected) { ++page; continue; }
        uint32_t run = page;
        while (run <= last && !(g_pageState[run].load() & kPageProtected))
        {
            g_pageState[run].fetch_or(kPageProtected | kPageTracked);
            ++run;
        }
        for (uint32_t v : kWatchViews) ProtectGuest(v + (page << 12), (run - page) << 12, false);
        page = run;
    }
    return epoch;
}

bool HostWrittenSince(uint32_t pa, uint32_t bytes, uint64_t& epoch)
{
    if (!g_pageEpoch || !bytes) return false;
    // Global epoch first: a write after this load gets a newer epoch than `now`.
    uint64_t now = g_trackEpoch.load(std::memory_order_acquire);
    if (now <= epoch) return false;   // no tracked write anywhere since
    pa &= 0x1FFFFFFFu;
    uint32_t first = pa >> 12, last = std::min(kTrackPages - 1, (pa + bytes - 1) >> 12);
    for (uint32_t page = first; page <= last; ++page)
        if (g_pageEpoch[page].load(std::memory_order_acquire) > epoch) return true;
    epoch = now;   // nothing of this range was written up to `now`
    return false;
}

void HostWatchWrites(uint32_t pa, uint32_t bytes)
{
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    uint32_t n = g_watchCount.load();
    if (n >= 16 || !bytes) return;
    g_watchRanges[n] = { pa & 0x1FFFFFFF, bytes };
    g_watchCount.store(n + 1, std::memory_order_release);
    static bool started = false;
    if (!started) { started = true; std::thread(WatchReporter).detach(); }
    fprintf(stderr, "[watch] protecting pa 0x%08X..0x%08X in the CPU views\n", pa, pa + bytes);
}
}  // namespace xenos

// ---- CPU accesses to resolved memory the native renderer keeps on the GPU ----
// vk_native keeps resolved 8888 data in device memory (HC09_NATIVE_RESOLVE_GPU);
// guest memory has it only once written back. gpu_live.cpp calls GuestWatchAccess
// for each range the GPU takes: its pages get no access in the four CPU views. The
// game's first read or write of such a page faults; the handler calls the hook
// (GuestSetAccessHook: it blocks until the renderer thread has written the range
// back), restores the page (read-only again where write tracking wants it), and the
// access proceeds on the newest data, as on the console. HC09_NATIVE_WATCH_RESOLVES=1
// prints the accesses (guest function, caller, read or write).
namespace {
std::atomic<uint8_t>* g_accessWatch = nullptr;   // per 4 KiB physical page: 1 armed, 2 was armed
std::mutex g_accessMu;                           // arming vs restoring a page
void (*g_accessHook)(uint32_t pa) = nullptr;
bool g_accessLog = false;
struct AccessHit { uint32_t fn, va, lr, kind; uint64_t tid; };   // kind: 0 read, 1 write, 2 unknown
AccessHit g_accessHits[256];
std::atomic<uint32_t> g_accessHead{ 0 };

void AccessProtect(uint32_t page, bool none)
{
    bool readOnly = g_pageState && (g_pageState[page].load() & kPageProtected);
#ifdef _WIN32
    DWORD old, mode = none ? PAGE_NOACCESS : readOnly ? PAGE_READONLY : PAGE_READWRITE;
    for (uint32_t v : kWatchViews) VirtualProtect(g_base + v + (page << 12), 0x1000, mode, &old);
#else
    int mode = none ? PROT_NONE : readOnly ? PROT_READ : PROT_READ | PROT_WRITE;
    for (uint32_t v : kWatchViews) mprotect(g_base + v + (page << 12), 0x1000, mode);
#endif
}

bool AccessWatchHandleFault(uintptr_t faultAddr, uintptr_t pc, uint32_t kind)
{
    if (!g_accessWatch) return false;
    uintptr_t gb = uintptr_t(g_base);
    if (faultAddr < gb || faultAddr >= gb + PPC_MEMORY_SIZE) return false;
    uint32_t va = uint32_t(faultAddr - gb), view = va & 0xE0000000u;
    if (view != 0x40000000u && view != 0xA0000000u && view != 0xC0000000u && view != 0xE0000000u) return false;
    uint32_t page = (va & 0x1FFFFFFFu) >> 12;
    uint8_t state = 1;
    if (!g_accessWatch[page].compare_exchange_strong(state, 2))
    {
        // Not armed. Another thread is handling this page: retry until it has restored
        // it. A write to a page write tracking protects is that handler's.
        if (state != 2 || kind == 1) return false;
        if (kind == 2 && g_pageState && (g_pageState[page].load() & kPageProtected)) return false;
        return true;
    }
    if (g_accessLog)
    {
        AccessHit& h = g_accessHits[g_accessHead.fetch_add(1) % 256];
        h.va = va; h.fn = GuestFunctionForHostPc(pc); h.tid = KernelCurrentTid(); h.kind = kind;
        h.lr = t_curCtx ? uint32_t(t_curCtx->lr) : 0;
    }
    if (g_accessHook) g_accessHook(page << 12);   // the newest data into guest memory
    std::lock_guard<std::mutex> lk(g_accessMu);
    if (g_accessWatch[page].load() == 2) AccessProtect(page, false);   // re-armed meanwhile: it faults again
    return true;
}

void AccessWatchReporter()
{
    uint32_t printed = 0;
    std::map<uint32_t, uint64_t> perFn;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint32_t head = g_accessHead.load();
        if (head - printed > 256) printed = head - 256;
        for (; printed != head; ++printed)
        {
            const AccessHit& h = g_accessHits[printed % 256];
            uint64_t c = ++perFn[h.fn];
            if (c > 3 && (c & (c - 1))) continue;   // first 3, then powers of two
            fprintf(stderr, "[watch-resolve] CPU %s #%llu by sub_%08X (lr 0x%08X) tid %llu to va 0x%08X: memory the GPU kept, written back\n",
                    h.kind == 0 ? "read" : h.kind == 1 ? "write" : "access", (unsigned long long)c, h.fn, h.lr, (unsigned long long)h.tid, h.va);
        }
    }
}
}  // namespace

void GuestSetAccessHook(void (*hook)(uint32_t pa), bool log)
{
    g_accessHook = hook;
    g_accessLog = log;
    if (log) std::thread(AccessWatchReporter).detach();
}

void GuestWatchAccess(uint32_t pa, uint32_t bytes)
{
    std::lock_guard<std::mutex> lk(g_accessMu);
    if (!g_accessWatch) g_accessWatch = new std::atomic<uint8_t>[kTrackPages]();
    pa &= 0x1FFFFFFFu;
    for (uint32_t page = pa >> 12; page <= (pa + bytes - 1) >> 12 && page < kTrackPages; ++page)
        if (g_accessWatch[page].exchange(1) != 1) AccessProtect(page, true);
}

// Fatal fault report: guest function, thread, guest address for memory
// faults, registers and back-chain, then exit with 128 + the POSIX signal
// number on every platform (scripts read the exit code).
#ifdef _WIN32
static thread_local EXCEPTION_POINTERS* t_faultEp = nullptr;   // OnException's, for the crash report's minidump
#endif

[[noreturn]] static void ReportFault(int sig, const char* what, uintptr_t pc, bool memFault, uintptr_t fa)
{
    uint32_t fn = GuestFunctionForHostPc(pc);
    char details[512];
    int n = snprintf(details, sizeof details, "          in guest function sub_%08X (host pc %p)\n"
                     "          on thread tid %llu \"%s\"\n", fn, (void*)pc,
                     (unsigned long long)KernelCurrentTid(), KernelCurrentThreadName().c_str());
    if (memFault && n > 0 && n < int(sizeof details))
    {
        uintptr_t gb = uintptr_t(g_base);
        if (fa >= gb && fa < gb + PPC_MEMORY_SIZE)
            snprintf(details + n, sizeof details - size_t(n), "          faulting guest address 0x%08X\n", uint32_t(fa - gb));
        else
            snprintf(details + n, sizeof details - size_t(n), "          faulting host address %p (outside guest memory)\n", (void*)fa);
    }
    fprintf(stderr, "\n[runtime] *** %s ***\n%s", what, details);
    DumpGuestState();
    fflush(stderr);
#ifdef _WIN32
    CrashReportWrite(what, details, t_faultEp);
#else
    CrashReportWrite(what, details, nullptr);
#endif
    DumpImportHistogram();
    _exit(128 + sig);
}

#ifdef _WIN32
// Vectored exception handler: first look at every exception, before any SEH
// frame. Only hardware faults are ours; C++ exceptions, debugger notifications
// etc. pass through.
static LONG CALLBACK OnException(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    uintptr_t pc = uintptr_t(ep->ContextRecord->Rip);
    t_faultEp = ep;
    switch (er->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    {
        uintptr_t fa = er->NumberParameters >= 2 ? uintptr_t(er->ExceptionInformation[1]) : 0;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && AccessWatchHandleFault(fa, pc, er->ExceptionInformation[0] == 1 ? 1u : er->ExceptionInformation[0] == 0 ? 0u : 2u))
            return EXCEPTION_CONTINUE_EXECUTION;   // HC09_NATIVE_WATCH_RESOLVES: recorded and restored, retry the access
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->ExceptionInformation[0] == 1 &&
            (TrackHandleFault(fa) || WatchHandleFault(fa, pc)))
            return EXCEPTION_CONTINUE_EXECUTION;   // write tracking/watch: recorded and unprotected, retry the store
        ReportFault(SIGSEGV, er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "EXCEPTION_ACCESS_VIOLATION (bad memory access)"
                                                                             : "EXCEPTION_IN_PAGE_ERROR (bad memory access)", pc, true, fa);
    }
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
        ReportFault(SIGFPE, "EXCEPTION_INT_DIVIDE_BY_ZERO/OVERFLOW (divide by zero / FP trap)", pc, false, 0);
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
        ReportFault(SIGILL, "EXCEPTION_ILLEGAL_INSTRUCTION (illegal instruction)", pc, false, 0);
    case EXCEPTION_BREAKPOINT:
        // XenonRecomp's __builtin_debugtrap() (int3): an instruction (form) the
        // recompiler could not translate. The function named below contains it.
        ReportFault(SIGTRAP, "EXCEPTION_BREAKPOINT (untranslated instruction: recompiler emitted __builtin_debugtrap)", pc, false, 0);
    case EXCEPTION_STACK_OVERFLOW:
        ReportFault(SIGSEGV, "EXCEPTION_STACK_OVERFLOW (host stack exhausted)", pc, false, 0);
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

// Ctrl+C / Ctrl+Break / console close (SIGINT/SIGTERM on Linux). Runs on its own thread.
static BOOL WINAPI OnConsoleCtrl(DWORD type)
{
    int sig = type == CTRL_C_EVENT ? SIGINT : SIGTERM;
    fprintf(stderr, "\n[runtime] caught console control event %lu\n", type);
    DumpImportHistogram();
    _exit(128 + sig);
}
#else
static void OnFault(int sig, siginfo_t* info, void* uctx)
{
    uintptr_t pc = 0;
#if defined(__x86_64__)
    pc = uintptr_t(static_cast<ucontext_t*>(uctx)->uc_mcontext.gregs[REG_RIP]);
#endif
    if (sig == SIGSEGV && (AccessWatchHandleFault(uintptr_t(info->si_addr), pc, 2) || TrackHandleFault(uintptr_t(info->si_addr)) ||
                           WatchHandleFault(uintptr_t(info->si_addr), pc))) return;
    // SIGTRAP is XenonRecomp's __builtin_debugtrap(): an instruction (form) the
    // recompiler could not translate. The function named below contains it.
    const char* what = sig == SIGSEGV ? "SIGSEGV (bad memory access)" :
                       sig == SIGFPE  ? "SIGFPE (divide by zero / FP trap)" :
                       sig == SIGILL  ? "SIGILL (illegal instruction)" :
                       sig == SIGTRAP ? "SIGTRAP (untranslated instruction: recompiler emitted __builtin_debugtrap)" : "SIGBUS";
    ReportFault(sig, what, pc, sig == SIGSEGV || sig == SIGBUS, uintptr_t(info->si_addr));
}
#endif

void StubLog(const char* name, const PPCContext& ctx)
{
    ImportLog(name);
    static const bool on = Hc09Env("HC09_TRACE_STUBS") != nullptr;
    if (!on) return;
    static std::mutex mu; static std::unordered_map<const char*, int> seen;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (++seen[name] > 4) return;
    }
    fprintf(stderr, "[stub] %s r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X lr=%08X\n", name,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, uint32_t(ctx.lr));
}

// Heartbeat: every 10 M import calls (n = the multiple reached), from the thread whose call
// reached it.
static void ImportHeartbeat(uint64_t n, const char* name)
{
    {
        // Self-diagnosing heartbeat: the top callers since start. A spin loop
        // shows up here as one import with a count in the tens of millions,
        // without needing a well-timed Ctrl+C for the exit histogram.
        std::vector<std::pair<uint64_t, std::string>> top;
        // Progress markers: these rarely make the top 5 but show whether the
        // game is still presenting, loading and polling between beats.
        static const char* const kProgress[] = { "VdSwap", "NtReadFile", "NtCreateFile", "XamInputGetState", "KeWaitForSingleObject" };
        uint64_t progress[5] = {};
        {
            std::map<std::string, uint64_t> hist = ImportCounts();
            for (auto& [k, c] : hist) top.emplace_back(c, k);
            for (int i = 0; i < 5; ++i)
            {
                auto it = hist.find(kProgress[i]);
                progress[i] = it == hist.end() ? 0 : it->second;
            }
        }
        std::sort(top.rbegin(), top.rend());
        fprintf(stderr, "[import] heartbeat: %llu calls (on tid %llu \"%s\"); top:", (unsigned long long)n,
                (unsigned long long)KernelCurrentTid(), KernelCurrentThreadName().c_str());
        for (size_t i = 0; i < top.size() && i < 5; ++i)
            fprintf(stderr, " %s=%llu", top[i].second.c_str(), (unsigned long long)top[i].first);
        fprintf(stderr, "\n          progress:");
        for (int i = 0; i < 5; ++i)
            fprintf(stderr, " %s=%llu", kProgress[i], (unsigned long long)progress[i]);
        fprintf(stderr, "\n");
        // ...and where this thread is: the call site of the import we are in
        // plus its backtrace, so a spin loop names the guest function spinning.
        fprintf(stderr, "          caught in %s called from guest 0x%08X\n", name,
                t_curCtx ? uint32_t(t_curCtx->lr) : 0u);
        DumpGuestState();
        uint64_t beat = n / 10000000;
        // HC09_DUMP_EVERY=k: dump all threads every k-th beat (default 5, plus the first).
        static const long every = Hc09Env("HC09_DUMP_EVERY") ? atol(Hc09Env("HC09_DUMP_EVERY")) : 5;
        if (beat == 1 || (every > 0 && (beat % every) == 0)) KernelDumpAllThreads();
    }
}

// Calls after the first kExactImportCalls are counted per thread and added to the shared
// counters every kImportFlush calls: the GameKernel loops' idle polling makes millions of
// import calls per second from several threads, and one shared counter (plus the per-name
// ones) bounced between their cores (docs/REPORT_GAME_CPU.md). Counts in the heartbeat and
// the exit histogram lag by up to kImportFlush calls per thread; NEW detection is exact.
static constexpr uint64_t kExactImportCalls = 1000000;
static constexpr uint32_t kImportFlush = 4096;
struct PendingImports
{
    uint32_t calls = 0;
    struct Entry { ImportSlot* slot = nullptr; uint32_t n = 0; } e[64];
};
static thread_local PendingImports t_pendingImports;

static void FlushImports(PendingImports& p, const char* name)
{
    for (PendingImports::Entry& e : p.e)
        if (e.n) { e.slot->count.fetch_add(e.n, std::memory_order_relaxed); e.n = 0; }
    uint64_t before = g_importCalls.fetch_add(p.calls, std::memory_order_relaxed), after = before + p.calls;
    p.calls = 0;
    if (before / 10000000 != after / 10000000) ImportHeartbeat(after / 10000000 * 10000000, name);
}

void ImportLog(const char* name)
{
    // Debug aid: HC09_FAULT_AT=<n> raises SIGFPE at the n-th import call so
    // the fault reporter (register dump + backtrace) can be exercised (exact counting throughout).
    static const long faultAt = Hc09Env("HC09_FAULT_AT") ? atol(Hc09Env("HC09_FAULT_AT")) : -1;
    bool inserted, first;
    if (faultAt >= 0 || g_importCalls.load(std::memory_order_relaxed) < kExactImportCalls)
    {
        uint64_t n = g_importCalls.fetch_add(1, std::memory_order_relaxed);
        if (faultAt >= 0 && (long)n == faultAt)
        {
#ifdef _WIN32
            RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
#else
            raise(SIGFPE);
#endif
        }
        ImportSlot* s = FindSlot(name, &inserted, &first);
        if (s && !inserted) s->count.fetch_add(1, std::memory_order_relaxed);
        if (n < 200)
            fprintf(stderr, "[import %4llu] %s\n", (unsigned long long)n, name);
        else if (n == 200)
            fprintf(stderr, "[import] (verbatim log off; new imports still reported)\n");
        if (first && n >= 200)
            fprintf(stderr, "[import %4llu] NEW: %s\n", (unsigned long long)n, name);
        if ((n % 10000000) == 0 && n > 0) ImportHeartbeat(n, name);
        return;
    }
    ImportSlot* s = FindSlot(name, &inserted, &first);
    if (first)
        fprintf(stderr, "[import ~%llu] NEW: %s\n", (unsigned long long)g_importCalls.load(std::memory_order_relaxed), name);
    PendingImports& p = t_pendingImports;
    if (s && !inserted)
    {
        PendingImports::Entry& e = p.e[(reinterpret_cast<uintptr_t>(s) / sizeof(ImportSlot)) & 63];
        if (e.slot != s)
        {
            if (e.n) e.slot->count.fetch_add(e.n, std::memory_order_relaxed);
            e.slot = s;
            e.n = 0;
        }
        ++e.n;
    }
    if (++p.calls >= kImportFlush) FlushImports(p, name);
}


// ---- Guest call helpers --------------------------------------------------

thread_local PPCContext* t_curCtx = nullptr;   // context of the guest code this host thread runs

void CallGuest(uint32_t guestAddr, PPCContext& ctx)
{
    t_curCtx = &ctx;
    if (guestAddr < PPC_CODE_BASE || guestAddr >= PPC_CODE_BASE + PPC_CODE_SIZE)
    {
        fprintf(stderr, "[runtime] CallGuest: address 0x%08X outside code range\n", guestAddr);
        return;
    }
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, guestAddr);
    if (!fn)
    {
        fprintf(stderr, "[runtime] CallGuest: no host function for 0x%08X\n", guestAddr);
        return;
    }
    fn(ctx, g_base);
}

void InitThreadContext(PPCContext& ctx, uint32_t stackTop, uint32_t pcr)
{
    memset(&ctx, 0, sizeof(ctx));
#ifndef PPC_CONFIG_SKIP_MSR
    ctx.msr = 0x200A000;
#endif
    ctx.fpscr.csr = 0x1F80;              // all FP exceptions masked (see BootGame)
    *reinterpret_cast<uint32_t*>(g_base + stackTop) = 0; // back chain
    ctx.r1.u64 = stackTop;
    ctx.r13.u64 = pcr;                   // per-thread PCR/TLS block
}

// ---- Boot ----------------------------------------------------------------

PPC_EXTERN_FUNC(_xstart); // generated entry at guest 0x828A9CB0 (C++ linkage)

int BootGame()
{
    static PPCContext ctx{};
    uint32_t stackBase = GuestAlloc(kStackSize, 0x1000);
    uint32_t sp = (stackBase + kStackSize - 0x100) & ~0xFu;
    uint32_t pcr = GuestAlloc(0x1000, 0x1000);
    InitThreadContext(ctx, sp, pcr);
    KernelInitMainThread(&ctx);

    fprintf(stderr, "[runtime] entering _xstart (guest 0x828A9CB0), sp=0x%X\n", sp);
    t_curCtx = &ctx;
    _xstart(ctx, g_base);
    fprintf(stderr, "[runtime] _xstart returned (r3=0x%llX); waiting for guest threads\n",
            (unsigned long long)ctx.r3.u64);
    KernelWaitAllThreads();
    return 0;
}

extern std::string g_vfsRoot;
extern std::string g_vfsCache;
#include "settings.h"
#include "profiler.h"

// ---- CPU check -----------------------------------------------------------
// The recompiled code is built with -march=x86-64-v<HC09_GUEST_LEVEL> (CMake HC09_GUEST_ARCH, default v3).
// This file is compiled without it, so it can tell the user what is missing instead of the game dying of
// an illegal instruction later. HC09_CPU_CHECK=test reports the check as failed (to see the message).
#ifndef HC09_GUEST_LEVEL
#define HC09_GUEST_LEVEL 0
#endif
#if defined(__x86_64__) || defined(_M_X64)
static void CpuId(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

// Missing features of x86-64-v<level> (the psABI micro-architecture levels), space-separated; empty = all there.
static std::string CpuMissing(int level)
{
    uint32_t l0[4], l1[4] = {}, l7[4] = {}, e0[4], e1[4] = {};
    CpuId(0, 0, l0);
    if (l0[0] >= 1) CpuId(1, 0, l1);
    if (l0[0] >= 7) CpuId(7, 0, l7);
    CpuId(0x80000000u, 0, e0);
    if (e0[0] >= 0x80000001u) CpuId(0x80000001u, 0, e1);
    uint64_t xcr0 = 0;
    if (l1[2] & (1u << 27))                                  // OSXSAVE: the OS saves extended state
    {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = (uint64_t)hi << 32 | lo;
    }
    struct F { int level; const char* name; uint32_t ok; };
    const F f[] = {
        {2, "SSE3", (l1[2] >> 0) & 1},   {2, "SSSE3", (l1[2] >> 9) & 1},  {2, "CMPXCHG16B", (l1[2] >> 13) & 1},
        {2, "SSE4.1", (l1[2] >> 19) & 1}, {2, "SSE4.2", (l1[2] >> 20) & 1}, {2, "POPCNT", (l1[2] >> 23) & 1},
        {2, "LAHF-SAHF", (e1[2] >> 0) & 1},
        {3, "AVX", (l1[2] >> 28) & 1},   {3, "AVX2", (l7[1] >> 5) & 1},   {3, "BMI1", (l7[1] >> 3) & 1},
        {3, "BMI2", (l7[1] >> 8) & 1},   {3, "FMA", (l1[2] >> 12) & 1},   {3, "MOVBE", (l1[2] >> 22) & 1},
        {3, "F16C", (l1[2] >> 29) & 1},  {3, "LZCNT", (e1[2] >> 5) & 1},
        {3, "OS support for AVX (XSAVE YMM state)", (xcr0 & 6) == 6},
        {4, "AVX512F", (l7[1] >> 16) & 1}, {4, "AVX512BW", (l7[1] >> 30) & 1}, {4, "AVX512CD", (l7[1] >> 28) & 1},
        {4, "AVX512DQ", (l7[1] >> 17) & 1}, {4, "AVX512VL", (l7[1] >> 31) & 1},
        {4, "OS support for AVX-512 (XSAVE ZMM state)", (xcr0 & 0xE6) == 0xE6},
    };
    std::string missing;
    for (const F& x : f)
        if (x.level <= level && !x.ok) missing += std::string(missing.empty() ? "" : ", ") + x.name;
    return missing;
}
#endif

static void CheckCpu()
{
#if HC09_GUEST_LEVEL >= 2 && (defined(__x86_64__) || defined(_M_X64))
    std::string missing = CpuMissing(HC09_GUEST_LEVEL);
    const char* test = Hc09Env("HC09_CPU_CHECK");
    const bool fake = test && !strcmp(test, "test");
    if (fake && missing.empty()) missing = "(none: HC09_CPU_CHECK=test)";
    if (missing.empty()) return;
    char msg[1024];
    snprintf(msg, sizeof msg,
             "This build of hc09 needs a CPU with x86-64-v%d%s.\n"
             "Missing on this computer: %s.\n\n"
#if HC09_DEV_TOOLS
             "Rebuild for an older CPU with  py -3.11 tools\\build.py <default.xex> --guest-arch x86-64-v2\n"
             "(or x86-64), i.e. CMake -DHC09_GUEST_ARCH=x86-64-v2.",
#else
             "This computer's processor is older than the game's build supports.",
#endif
             HC09_GUEST_LEVEL,
             HC09_GUEST_LEVEL == 3 ? " (AVX2, BMI1/2, FMA, MOVBE: Intel Haswell 2013 or later, AMD Excavator/Zen)"
             : HC09_GUEST_LEVEL == 2 ? " (SSE4.2, POPCNT: Intel Nehalem 2008 or later, AMD Bulldozer)" : " (AVX-512)",
             missing.c_str());
    fprintf(stderr, "[runtime] *** unsupported CPU ***\n%s\n", msg);
#ifdef _WIN32
    if (!fake) MessageBoxA(nullptr, msg, "hc09: unsupported CPU", MB_OK | MB_ICONERROR);
#endif
    exit(3);
#endif
}

#if HC09_SPLIT
int RuntimeMain(int argc, char** argv)   // hc09rt.dll: called by Hc09RtRun (runtime/split_dll.cpp)
#else
int main(int argc, char** argv)
#endif
{
    fprintf(stderr, "[runtime] hc09 build %s\n", HC09_BUILD_ID);
    CheckCpu();
    // Usage: hc09 [<image.basefile>] [--root <game-files-dir>]; hc09.ini fills in the rest.
    const char* image = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) g_vfsRoot = argv[++i];
        else if (!image) image = argv[i];
    }
    SettingsLoad(&g_vfsRoot);
    if (!image) image = "headcoach.basefile";
    while (!g_vfsRoot.empty() && g_vfsRoot.back() == '/') g_vfsRoot.pop_back();
    if (!g_vfsRoot.empty()) fprintf(stderr, "[runtime] game files root: %s\n", g_vfsRoot.c_str());
    else fprintf(stderr, "[runtime] no --root given: all file opens will fail (and be logged)\n");
    // Writable scratch volume (cache:, the HDD: saves) and the renderers' pipeline caches: HC09_CACHE_DIR /
    // hc09.ini [game] cache, else hc09-cache, and hc09-cache-v12 for the 1.2 build (one save folder per game
    // version, 2026-10-22: whether a 1.0 career loads cleanly in 1.2 is not known). Relative to the working
    // directory; the renderers read the folder from HC09_CACHE_DIR, set here.
    {
        const char* c = Hc09Env("HC09_CACHE_DIR");
        g_vfsCache = c && *c ? c : HC09_GAME_V12 ? "hc09-cache-v12" : "hc09-cache";
        while (g_vfsCache.size() > 1 && (g_vfsCache.back() == '/' || g_vfsCache.back() == '\\')) g_vfsCache.pop_back();
#if defined(_WIN32)
        _putenv_s("HC09_CACHE_DIR", g_vfsCache.c_str());
#else
        setenv("HC09_CACHE_DIR", g_vfsCache.c_str(), 1);
#endif
    }
    { std::error_code ec; std::filesystem::create_directories(g_vfsCache, ec); }
    fprintf(stderr, "[runtime] cache volume: %s\n", g_vfsCache.c_str());

#ifdef _WIN32
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    AddVectoredExceptionHandler(1, OnException);
    CrashReportInstall();
    // 1 ms timer resolution: the vblank/timer threads and CP polls sleep in
    // 0.1-1 ms steps (the default 15.6 ms tick would slow the game's clock).
    timeBeginPeriod(1);
#else
    signal(SIGTERM, OnTerminate);
    signal(SIGINT, OnTerminate);
    {
        struct sigaction sa{};
        sa.sa_sigaction = OnFault;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        for (int f : { SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGTRAP }) sigaction(f, &sa, nullptr);
    }
#endif
    atexit(DumpImportHistogram);
    MapGuestMemory();
    // HC09_WATCH_PA=<hex pa>:<bytes>[,...]: report CPU writes to these physical
    // ranges from boot (see the write watch above OnFault).
    if (const char* w = Hc09Env("HC09_WATCH_PA"))
        for (const char* p = w; *p;)
        {
            char* e;
            uint32_t pa = uint32_t(strtoul(p, &e, 16));
            if (*e != ':') break;
            uint32_t bytes = uint32_t(strtoul(e + 1, &e, 0));
            xenos::HostWatchWrites(pa, bytes);
            p = *e ? e + 1 : e;
        }
    LoadImage(image);
    InstallFunctionTable();
    PatchXexImports();
    ProfilerStart();
    return BootGame();
}

#ifdef _WIN32
// C23 roundeven: the generated code's SIMDe rounding (vrfin, simde_mm_round_ps)
// uses __builtin_roundeven[f], which clang lowers to a libm call at -O0 without
// SSE4.1. glibc has it; the Windows UCRT does not. Round half to even, exactly,
// whatever the current rounding mode.
extern "C" float roundevenf(float x)
{
    float r = std::round(x);   // halves away from zero
    if (std::fabs(x - std::trunc(x)) == 0.5f) r = 2.0f * std::round(x * 0.5f);
    return r;
}
extern "C" double roundeven(double x)
{
    double r = std::round(x);
    if (std::fabs(x - std::trunc(x)) == 0.5) r = 2.0 * std::round(x * 0.5);
    return r;
}
#endif

// ---- Guest time base (mftb, see guest_indirect.h) --------------------------
// 50 MHz like the Xenon's, from the host's steady clock (QPC on Windows).
uint64_t GuestTimeBase()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()) / 20;
}

// ---- Host thread CPU time (HC09_TRACE_WAITS, imports_threads.cpp) -----------
void* HostCurrentThreadHandle()
{
#ifdef _WIN32
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_INFORMATION, FALSE, 0);
    return h;
#else
    return nullptr;
#endif
}
double HostThreadCpuSeconds(void* thread)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!thread || !GetThreadTimes(HANDLE(thread), &c, &e, &k, &u)) return 0;
    return 1e-7 * double((uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime));
#else
    (void)thread;
    return 0;
#endif
}

// ---- Bad indirect call diagnostic (see guest_indirect.h) -------------------

[[noreturn]] void GuestBadIndirect(PPCContext& ctx, uint32_t target)
{
    fprintf(stderr,
            "\n[runtime] *** indirect call to invalid guest address 0x%08X ***\n"
            "          caller lr=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X\n"
            "          (a null/garbage function pointer — usually a virtual call on a\n"
            "           null object, i.e. an earlier create/open returned NULL)\n",
            target, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    fflush(stderr);
    DumpImportHistogram();
    char details[160];
    snprintf(details, sizeof details, "          target 0x%08X, caller lr 0x%08X\n", target, uint32_t(ctx.lr));
    CrashReportWrite("indirect call to an invalid guest address", details, nullptr);
    _exit(44);
}
