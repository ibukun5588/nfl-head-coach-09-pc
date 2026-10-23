// Diagnostic hooks on guest functions. Recompiled functions are emitted as
// weak aliases of their __imp__ bodies, so a strong definition here wraps the
// guest function for every direct and indirect call; each wrapper forwards to
// the original unchanged. Everything is gated by an environment variable.
#include "dev_env.h"
#include "kernel.h"
#include "game_addr.h"
#include "gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <utility>

extern "C" PPC_FUNC(__imp__sub_82327F80);

bool PacingTraceOn();                        // hooks_pacing.cpp
void PacingD3DWait(int which, double seconds);
bool NativeLiveFencesOn();                   // gpu_live.cpp (native_live.h)
void NativeLiveFenceWait(uint32_t cur, uint32_t fence, uint32_t caller, uint32_t type);
void NativeLiveFenceNeeded(uint32_t cur, uint32_t fence);
extern "C" PPC_FUNC(__imp__sub_823288C8);
extern "C" PPC_FUNC(__imp__sub_8233A7C8);
extern "C" PPC_FUNC(__imp__sub_8233A330);
extern "C" PPC_FUNC(__imp__sub_8233A188);
extern "C" PPC_FUNC(__imp__sub_82339E20);

// ---- HC09_TRACE_TILING=1: D3D's GPU-callback tiling worker ------------------
// Armed by the first job post (sub_8233A7C8). Logs, per call, the worker step
// (sub_8233A330), the end-of-program handler (sub_8233A188), the command-list
// interpreter (sub_82339E20) and every ring kick, with the shared job state.
// Budget: HC09_TRACE_TILING_LINES (default 1500).
static std::atomic<bool> g_tilingArmed{ false };
static bool TilingOn()
{
    static const bool on = Hc09Env("HC09_TRACE_TILING") != nullptr;
    return on && g_tilingArmed.load(std::memory_order_relaxed);
}
static void TileLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void TileLog(const char* fmt, ...)
{
    static const long budget = Hc09Env("HC09_TRACE_TILING_LINES") ? atol(Hc09Env("HC09_TRACE_TILING_LINES")) : 1500;
    static std::atomic<long> used{ 0 };
    long n = used.fetch_add(1);
    if (n > budget) return;
    if (n == budget) { fprintf(stderr, "[tile] (budget exhausted)\n"); return; }
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    fprintf(stderr, "[tile] t%llu sw%llu %s\n", (unsigned long long)KernelCurrentTid(),
            (unsigned long long)GpuSwapCount(), buf);
}
static std::string Fields(uint32_t p, std::initializer_list<uint32_t> offs)
{
    std::string s;
    char b[32];
    for (uint32_t o : offs)
    {
        snprintf(b, sizeof(b), " +%u=%08X", o, p >= 0x40000000u ? GLoad32(p + o) : 0u);
        s += b;
    }
    return s;
}
static std::string SharedState(uint32_t s)
{
    return Fields(s, { 0, 16, 20, 24, 28, 32, 36, 56, 60, 72, 76, 96, 100, 104 });
}
static std::string ListState(uint32_t s)
{
    return Fields(s, { 48, 52, 72, 108, 112, 356, 360, 364, 368, 372 });
}

// sub_8233A330(worker r3): one step of a D3D worker. worker+0 -> shared job
// state; worker+4 = worker index; +56/+60 = job queue write/read counters.
PPC_FUNC(sub_8233A330)
{
    if (!TilingOn()) { __imp__sub_8233A330(ctx, base); return; }
    uint32_t w = ctx.r3.u32, s = GLoad32(w);
    TileLog("step enter worker %08X idx %u q %u/%u shared %08X:%s", w, GLoad32(w + 4), GLoad32(w + 56), GLoad32(w + 60),
            s, SharedState(s).c_str());
    __imp__sub_8233A330(ctx, base);
    TileLog("step leave worker %08X q %u/%u shared:%s", w, GLoad32(w + 56), GLoad32(w + 60), SharedState(s).c_str());
}

// sub_8233A188(shared r3, worker r4): reached the 0xC0000000 end of a program.
PPC_FUNC(sub_8233A188)
{
    if (!TilingOn()) { __imp__sub_8233A188(ctx, base); return; }
    uint32_t s = ctx.r3.u32, w = ctx.r4.u32;
    TileLog("end-of-program enter shared %08X worker %08X:%s", s, w, SharedState(s).c_str());
    __imp__sub_8233A188(ctx, base);
    TileLog("end-of-program leave -> next program 0x%08X:%s", ctx.r3.u32, SharedState(s).c_str());
}

// sub_82339E20(list r3, state r4): the command-list interpreter (tile loop,
// predicates, IB submissions). Returns where to continue.
PPC_FUNC(sub_82339E20)
{
    if (!TilingOn()) { __imp__sub_82339E20(ctx, base); return; }
    uint32_t list = ctx.r3.u32, st = ctx.r4.u32;
    std::string words;
    char b[16];
    for (int i = 0; i < 40 && list >= 0x40000000u; ++i)
    {
        snprintf(b, sizeof(b), " %08X", GLoad32(list + i * 4));
        words += b;
    }
    TileLog("list enter @%08X state %08X:%s", list, st, ListState(st).c_str());
    TileLog("list words:%s", words.c_str());
    __imp__sub_82339E20(ctx, base);
    TileLog("list leave -> 0x%08X state:%s", ctx.r3.u32, ListState(st).c_str());
}

// sub_8233A7C8(job r3): D3D GPU-callback that queues `job` for the D3D worker
// thread of the current hardware thread and wakes it (runs on the CP thread,
// from the PM4_INTERRUPT handler). HC09_TRACE_JOBS=1 dumps the first 24 jobs.
PPC_FUNC(sub_8233A7C8)
{
    g_tilingArmed.store(true, std::memory_order_relaxed);
    if (TilingOn())
        TileLog("job post 0x%08X on cpu %u (from INTERRUPT handler)", ctx.r3.u32, GLoad8(ctx.r13.u32 + KPCR_CUR_CPU));
    static const bool on = Hc09Env("HC09_TRACE_JOBS") != nullptr;
    static std::atomic<int> n{ 0 };
    int k = on ? n.fetch_add(1) : -1;
    if (on && k < 24)
    {
        uint32_t job = ctx.r3.u32;
        fprintf(stderr, "[job] post 0x%08X on cpu %u (gpu swaps %llu):", job, GLoad8(ctx.r13.u32 + KPCR_CUR_CPU),
                (unsigned long long)GpuSwapCount());
        if (job >= 0x40000000u)
            for (int i = 0; i < 24; ++i) fprintf(stderr, " %08X", GLoad32(job + i * 4));
        fprintf(stderr, "\n");
        uint32_t prog = job >= 0x40000000u ? GLoad32(job) : 0;
        if (k == 0 && prog >= 0x40000000u)
        {
            fprintf(stderr, "[job]   program @0x%08X:", prog);
            for (int i = 0; i < 64; ++i) fprintf(stderr, "%s%08X", (i % 8) ? " " : "\n[job]     ", GLoad32(prog + i * 4));
            fprintf(stderr, "\n");
        }
    }
    // Posts per GPU swap for the first 40 swaps that have any (shows whether
    // the loop is bounded per frame).
    if (on)
    {
        static std::mutex mu;
        static uint64_t lastSwap = ~0ull, perSwap = 0;
        static int swapsReported = 0;
        std::lock_guard<std::mutex> lk(mu);
        uint64_t sw = GpuSwapCount();
        if (sw != lastSwap)
        {
            if (lastSwap != ~0ull && swapsReported++ < 40)
                fprintf(stderr, "[job] %llu post(s) during gpu swap %llu\n", (unsigned long long)perSwap, (unsigned long long)lastSwap);
            lastSwap = sw; perSwap = 0;
        }
        ++perSwap;
    }
    __imp__sub_8233A7C8(ctx, base);
}

// sub_823288C8(device r3, target r4, r5): D3D waits until the GPU read pointer
// (*(device+10896), the RPtr write-back) has passed ring position `target`,
// measured back from D3D's write position (device+10908).
// HC09_TRACE_RINGWAIT=1 logs entry/exit (first 40 calls, then any call that
// takes longer than 100 ms).
PPC_FUNC(sub_823288C8)
{
    // Native fences: wait host-side for a fence already kicked (the original then
    // returns at once); one not kicked yet (== dev+10908) the original kicks first.
    if (NativeLiveFencesOn())
    {
        uint32_t cur = GLoad32(ctx.r3.u32 + 10908), fence = ctx.r4.u32;
        if (fence != cur) NativeLiveFenceWait(cur, fence, uint32_t(ctx.lr), ctx.r5.u32);
        else NativeLiveFenceNeeded(cur + 2, fence);
    }
    static const bool on = Hc09Env("HC09_TRACE_RINGWAIT") != nullptr;
    if (!on && PacingTraceOn())   // HC09_TRACE_PACING: time it (hooks_pacing.cpp)
    {
        auto t0 = std::chrono::steady_clock::now();
        __imp__sub_823288C8(ctx, base);
        PacingD3DWait(0, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return;
    }
    if (!on) { __imp__sub_823288C8(ctx, base); return; }
    static std::atomic<int> calls{ 0 };
    int n = calls.fetch_add(1);
    uint32_t dev = ctx.r3.u32, target = ctx.r4.u32, caller = uint32_t(ctx.lr);
    auto rptr = [&] { return GLoad32(GLoad32(dev + 10896)); };
    if (n == 0)
    {
        fprintf(stderr, "[ringwait] device 0x%08X, fence at 0x%08X\n", dev, GLoad32(dev + 10896));
        GpuWatchPhysical(GLoad32(dev + 10896));
    }
    uint32_t w0 = GLoad32(dev + 10908), r0 = rptr();
    auto t0 = std::chrono::steady_clock::now();
    static std::atomic<int> late{ 0 };
    if (n < 40 || (GpuSwapCount() >= 4000 && late.fetch_add(1) < 60))
        fprintf(stderr, "[ringwait] #%d tid %llu from 0x%08X: target %u, write %u, rptr %u (gpu swaps %llu)\n",
                n, (unsigned long long)KernelCurrentTid(), caller, target, w0, r0,
                (unsigned long long)GpuSwapCount());
    __imp__sub_823288C8(ctx, base);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (n < 40 || ms > 100)
        fprintf(stderr, "[ringwait] #%d done after %lld ms: write %u, rptr %u (entry: target %u write %u rptr %u)\n",
                n, (long long)ms, GLoad32(dev + 10908), rptr(), target, w0, r0);
}

// sub_82335610(device r3, width r4, height r5, params r6, out r7): D3D's
// per-present scaler setup; params is a 56-byte block (source rect first; an
// all-zero rect means the whole back buffer). HC09_TRACE_SCALER=1 logs the
// block every time it changes (first 30 changes), with the caller chain.
extern "C" PPC_FUNC(__imp__sub_82335610);
PPC_FUNC(sub_82335610)
{
    static const bool on = Hc09Env("HC09_TRACE_SCALER") != nullptr;
    if (on)
    {
        static std::mutex mu;
        static uint32_t last[14] = { 0xFFFFFFFF };
        static int changes = 0;
        uint32_t p = ctx.r6.u32, cur[14] = {};
        for (int i = 0; i < 14 && p; ++i) cur[i] = GLoad32(p + i * 4);
        std::lock_guard<std::mutex> lk(mu);
        if (memcmp(cur, last, sizeof(cur)) != 0 && changes++ < 30)
        {
            memcpy(last, cur, sizeof(cur));
            fprintf(stderr, "[scaler] params @0x%08X (backbuffer %ux%u, swaps %llu):", p, ctx.r4.u32, ctx.r5.u32,
                    (unsigned long long)GpuSwapCount());
            for (int i = 0; i < 14; ++i) fprintf(stderr, " %08X", cur[i]);
            fprintf(stderr, "\n");
            DumpGuestBacktrace(ctx, "          ");
        }
    }
    uint32_t dev = ctx.r3.u32, bw = ctx.r4.u32, bh = ctx.r5.u32, params = ctx.r6.u32, out = ctx.r7.u32;
    __imp__sub_82335610(ctx, base);
    if (on && out)
    {
        // out: rect x0,y0,x1,y1 at +0..+12, then +16/+20 (output size).
        int32_t x0 = int32_t(GLoad32(out)), y0 = int32_t(GLoad32(out + 4)), x1 = int32_t(GLoad32(out + 8)), y1 = int32_t(GLoad32(out + 12));
        static std::atomic<int> bad{ 0 };
        if ((x1 - x0 <= 0 || y1 - y0 <= 0) && bad.fetch_add(1) < 10)
        {
            fprintf(stderr, "[scaler] DEGENERATE rect (%d,%d)-(%d,%d) out +16=%u +20=%u from params @0x%08X, backbuffer %ux%u;"
                            " dev+21540=%u +21544=%u +21548=%u +13588=0x%X (swaps %llu)\n",
                    x0, y0, x1, y1, GLoad32(out + 16), GLoad32(out + 20), params, bw, bh,
                    GLoad32(dev + 21540), GLoad32(dev + 21544), GLoad32(dev + 21548), GLoad32(dev + 13588),
                    (unsigned long long)GpuSwapCount());
            fprintf(stderr, "[scaler]   params:");
            for (int i = 0; i < 14; ++i) fprintf(stderr, " %08X", GLoad32(params + i * 4));
            fprintf(stderr, "\n");
            DumpGuestBacktrace(ctx, "          ");
        }
    }
}

// sub_82327F80: D3D's ring kick (the only store to CP_RB_WPTR).
// HC09_TRACE_KICKS=1 reports each new (thread, caller) pair that kicks, with
// the number of kicks so far, to show who keeps the GPU fed.
PPC_FUNC(sub_82327F80)
{
    if (TilingOn())
    {
        // (device r3, packets r4, count r5): 2-dword entries copied to the ring.
        std::string pk;
        char b[32];
        for (uint32_t i = 0; i < ctx.r5.u32 && i < 6; ++i)
        {
            snprintf(b, sizeof(b), " [%08X %08X]", GLoad32(ctx.r4.u32 + i * 8), GLoad32(ctx.r4.u32 + i * 8 + 4));
            pk += b;
        }
        TileLog("kick from 0x%08X count %u:%s", uint32_t(ctx.lr), ctx.r5.u32, pk.c_str());
    }
    static const bool on = Hc09Env("HC09_TRACE_KICKS") != nullptr;
    if (on)
    {
        static std::mutex mu;
        static std::map<std::pair<uint64_t, uint32_t>, uint64_t> seen;
        static uint64_t total = 0;
        uint64_t tid = KernelCurrentTid();
        uint32_t caller = uint32_t(ctx.lr);
        bool fresh;
        uint64_t n;
        {
            std::lock_guard<std::mutex> lk(mu);
            n = ++total;
            fresh = seen[{ tid, caller }]++ == 0;
        }
        if (fresh)
        {
            fprintf(stderr, "[kick] new kicker: tid %llu \"%s\" from guest 0x%08X (kick #%llu, gpu swaps %llu)\n",
                    (unsigned long long)tid, KernelCurrentThreadName().c_str(), caller,
                    (unsigned long long)n, (unsigned long long)GpuSwapCount());
            DumpGuestBacktrace(ctx, "          ");
        }
    }
    __imp__sub_82327F80(ctx, base);
}

// ---- HC09_GAME_LOG=1: the game's own debug messages ----------------------------
// sub_8279F208 is an empty function (blr) in the retail build: the stripped
// printf-style logger (format in r3, arguments in r4..r10, then the stack),
// called from ~2,950 places (DATAERROR:/DATAWARNING:/ERROR: ... messages). The
// same stub also fills vtable slots as a no-op, so r3 is only taken as a format
// when it points at printable text in the image. Each distinct format is
// printed its first 20 times (HC09_GAME_LOG_ALL=1: every time).
extern "C" PPC_FUNC(__imp__sub_8279F208);
namespace {
bool GuestText(uint32_t p, std::string* out, size_t max = 512)
{
    if (p < 0x82000000u || p >= 0x83400000u) return false;
    std::string s;
    for (size_t i = 0; i < max; ++i)
    {
        uint8_t c = GLoad8(p + uint32_t(i));
        if (!c) break;
        if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) return false;
        s += char(c);
    }
    if (s.size() < 3) return false;
    *out = s;
    return true;
}
std::string GuestFormat(const std::string& fmt, PPCContext& ctx)
{
    const uint64_t gpr[7] = { ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64 };
    const double fpr[4] = { ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64 };
    int gi = 0, fi = 0;
    auto nextInt = [&]() -> uint64_t {
        if (gi < 7) return gpr[gi++];
        return GLoad32(ctx.r1.u32 + 0x54 + 8 * uint32_t(gi++ - 7));
    };
    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i)
    {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0123456789.lhzjtL", fmt[j])) ++j;
        if (j >= fmt.size()) break;
        char conv = fmt[j];
        std::string spec = fmt.substr(i, j - i + 1);
        char buf[600];
        if (conv == '%') out += '%';
        else if (conv == 's')
        {
            std::string s;
            uint32_t p = uint32_t(nextInt());
            if (p >= 0x40000000u && p < 0xC0000000u)
                for (int k = 0; k < 256; ++k) { uint8_t c = GLoad8(p + k); if (!c) break; s += char(c); }
            else s = "(null)";
            out += s;
        }
        else if (strchr("fFeEgG", conv)) { snprintf(buf, sizeof(buf), "%g", fi < 4 ? fpr[fi++] : 0.0); out += buf; }
        else if (strchr("xXoupdic", conv))
        {
            uint64_t v = nextInt();
            std::string s2 = spec;
            s2.erase(std::remove_if(s2.begin(), s2.end(), [](char c) { return c == 'l' || c == 'h' || c == 'z' || c == 'j' || c == 't'; }), s2.end());
            if (conv == 'p') snprintf(buf, sizeof(buf), "0x%08X", uint32_t(v));
            else if (conv == 'd' || conv == 'i') { s2.back() = 'd'; snprintf(buf, sizeof(buf), s2.c_str(), int(uint32_t(v))); }
            else snprintf(buf, sizeof(buf), s2.c_str(), uint32_t(v));
            out += buf;
        }
        i = j;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
}  // namespace

PPC_FUNC(sub_8279F208)
{
    static const bool on = Hc09Env("HC09_GAME_LOG") != nullptr;
    static const bool all = Hc09Env("HC09_GAME_LOG_ALL") != nullptr;
    if (on)
    {
        std::string fmt;
        if (GuestText(ctx.r3.u32, &fmt))
        {
            static std::mutex mu;
            static std::map<uint32_t, int> count;
            bool print;
            { std::lock_guard<std::mutex> lk(mu); print = all || count[ctx.r3.u32]++ < 20; }
            if (print)
                fprintf(stderr, "[game] sw%llu t%llu (from 0x%08X) %s\n", (unsigned long long)GpuSwapCount(),
                        (unsigned long long)KernelCurrentTid(), uint32_t(ctx.lr), GuestFormat(fmt, ctx).c_str());
        }
    }
    __imp__sub_8279F208(ctx, base);
}

// HC09_GAME_LOG: sub_82C57E18 is the game's stricmp. PrimaryCharMgr::
// InitUniformMaterials compares every material name of the head scene with
// "arm_mat" (0x8215863C) and "hand_mat" (0x82158644); log the names it sees
// (the arm/hand skin reference lookup fails: players' arms render black).
extern "C" PPC_FUNC(__imp__sub_82C57E18);
PPC_FUNC(sub_82C57E18)
{
    static const bool on = Hc09Env("HC09_GAME_LOG") != nullptr;
    if (on && (ctx.r3.u32 == GameAddr(0x8215863C) || ctx.r3.u32 == GameAddr(0x82158644)))
    {
        static std::atomic<int> n{ 0 };
        if (n++ < 400)
        {
            std::string name;
            if (ctx.r4.u32 >= 0x40000000u && ctx.r4.u32 < 0xC0000000u)
                for (int k = 0; k < 64; ++k) { uint8_t c = GLoad8(ctx.r4.u32 + k); if (!c) break; name += char(c >= 32 && c < 127 ? c : '?'); }
            fprintf(stderr, "[game] skin lookup: %s vs material \"%s\" (0x%08X)\n", ctx.r3.u32 == GameAddr(0x8215863C) ? "arm_mat" : "hand_mat",
                    name.c_str(), ctx.r4.u32);
        }
    }
    __imp__sub_82C57E18(ctx, base);
}

// HC09_GAME_LOG: PrimaryCharMgr::InitUniformMaterials (sub_8290B8D0, r4 = character
// setup; +40 -> head object). The arm/hand skin lookup scans the head's material
// list at +168 when the word at +192 is positive, else the list at +136 (list:
// +16 item array, +24 count; item +12 -> +4 = name). Dump both lists.
extern "C" PPC_FUNC(__imp__sub_8290B8D0);
namespace {
std::string HeadMaterialList(uint32_t list)
{
    std::string s;
    uint32_t items = GLoad32(list + 16), count = GLoad32(list + 24);
    s += std::to_string(int32_t(count)) + ":";
    for (uint32_t i = 0; i < count && i < 24 && items >= 0x40000000u; ++i)
    {
        uint32_t it = GLoad32(items + 4 * i), m = it >= 0x40000000u ? GLoad32(it + 12) : 0, nm = m >= 0x40000000u ? GLoad32(m + 4) : 0;
        s += ' ';
        if (nm < 0x40000000u) { s += "?"; continue; }
        for (int k = 0; k < 40; ++k) { uint8_t c = GLoad8(nm + k); if (!c) break; s += char(c >= 32 && c < 127 ? c : '?'); }
    }
    return s;
}
}  // namespace
PPC_FUNC(sub_8290B8D0)
{
    static const bool on = Hc09Env("HC09_GAME_LOG") != nullptr;
    if (on)
    {
        static std::atomic<int> n{ 0 };
        uint32_t setup = ctx.r4.u32, head = setup >= 0x40000000u ? GLoad32(setup + 40) : 0;
        if (n++ < 60 && head >= 0x40000000u)
        {
            std::string scene;
            uint32_t sn = GLoad32(head + 212);
            if (sn >= 0x40000000u) for (int k = 0; k < 40; ++k) { uint8_t c = GLoad8(sn + k); if (!c) break; scene += char(c); }
            fprintf(stderr, "[game] InitUniformMaterials: head 0x%08X scene \"%s\" +192=%d list+168 [%s] list+136 [%s]\n", head,
                    scene.c_str(), int32_t(GLoad32(head + 192)), HeadMaterialList(head + 168).c_str(), HeadMaterialList(head + 136).c_str());
        }
    }
    __imp__sub_8290B8D0(ctx, base);
}

// HC09_GAME_LOG: sub_828C9FE0 writes one player's name into the locked jersey
// name texture (TextureUtils::CreateIndirectNameTexture; r3 = row pointer from
// LockRect + row * pitch, r5 = name). Log where it writes and what lands there.
extern "C" PPC_FUNC(__imp__sub_828C9FE0);
PPC_FUNC(sub_828C9FE0)
{
    static const bool on = Hc09Env("HC09_GAME_LOG") != nullptr;
    static std::atomic<int> n{ 0 };
    uint32_t dst = ctx.r3.u32, name = ctx.r5.u32;
    bool log = on && n++ < 12;
    __imp__sub_828C9FE0(ctx, base);
    if (log)
    {
        std::string nm, bytes;
        if (name >= 0x40000000u) for (int k = 0; k < 32; ++k) { uint8_t c = GLoad8(name + k); if (!c) break; nm += char(c); }
        char b[16];
        for (int k = 0; k < 32 && dst >= 0x40000000u; ++k) { snprintf(b, sizeof(b), "%02X", GLoad8(dst + k)); bytes += b; }
        // The whole 4 KiB row: distinct texels and where the first one unlike texel 0 is.
        std::map<uint32_t, int> distinct;
        int firstDiff = -1;
        uint32_t t0 = dst >= 0x40000000u ? GLoad32(dst) : 0;
        for (uint32_t k = 0; k < 4096 && dst >= 0x40000000u; k += 4)
        {
            uint32_t t = GLoad32(dst + k);
            ++distinct[t];
            if (firstDiff < 0 && t != t0) firstDiff = int(k / 4);
        }
        std::string sample;
        if (firstDiff >= 0)
            for (int k = 0; k < 12; ++k) { snprintf(b, sizeof(b), "%08X ", GLoad32(dst + 4 * uint32_t(firstDiff + k))); sample += b; }
        fprintf(stderr, "[game] name texture row: \"%s\" -> 0x%08X (pa 0x%08X) ok=%u bytes %s; %zu distinct texels, first different at %d: %s\n",
                nm.c_str(), dst, dst & 0x1FFFFFFF, ctx.r3.u32 & 0xFF, bytes.c_str(), distinct.size(), firstDiff, sample.c_str());
    }
}

// HC09_GAME_LOG: sub_829D7320 creates texture `r5` of an XPR2 packed resource
// (r3 -> "XPR2", +4 header size, +8 data size, +12 resource count; 16-byte entries
// at +12 + 16 * i: +4 type, +8 -> 52-byte D3D texture header, +16 -> name, both
// relative to +12; the header's fetch constant starts at +28). The texels
// are copied from the data region (r3 + 12 + header size) at the header's base
// offset. Log every XPR2 holding a 32x1 texture (the players' skin ramps) with
// each resource's offsets and how much of its first page is non-zero (their
// texels sit at row 16 of the tile: packed mips, xenos_texture.cpp PackedBaseOffset).
extern "C" PPC_FUNC(__imp__sub_829D7320);
PPC_FUNC(sub_829D7320)
{
    static const bool on = Hc09Env("HC09_GAME_LOG") != nullptr;
    uint32_t xpr = ctx.r3.u32;
    if (on && xpr >= 0x40000000u && GLoad32(xpr) == 0x58505232u)
    {
        uint32_t hdrSize = GLoad32(xpr + 4), dataSize = GLoad32(xpr + 8), count = GLoad32(xpr + 12);
        uint32_t entries = xpr + 12, data = xpr + 12 + hdrSize;
        bool hasRamp = false;
        for (uint32_t i = 0; i < count && i < 64; ++i)
            if (GLoad32(entries + GLoad32(entries + 16 * i + 8) + 36) == 0x1Fu) hasRamp = true;
        static std::atomic<int> n{ 0 };
        if (hasRamp && n++ < 40)
        {
            // Where the data region stops being non-zero (a short read leaves a zero tail).
            uint32_t lastNonZero = 0;
            for (uint32_t k = 0; k < dataSize; k += 4) if (GLoad32(data + k)) lastNonZero = k + 4;
            fprintf(stderr, "[game] XPR2 0x%08X header %u data %u (non-zero up to %u) resources %u, index %d\n", xpr, hdrSize, dataSize,
                    lastNonZero, count, ctx.r5.s32);
            for (uint32_t i = 0; i < count && i < 64; ++i)
            {
                uint32_t e = entries + 16 * i, type = GLoad32(e + 4), h = entries + GLoad32(e + 8);
                std::string name;
                uint32_t nm = entries + GLoad32(e + 16);
                for (int k = 0; k < 48 && GLoad32(e + 16) && GLoad32(e + 16) < hdrSize; ++k) { uint8_t c = GLoad8(nm + k); if (!c) break; name += char(c >= 32 && c < 127 ? c : '?'); }
                uint32_t d1 = GLoad32(h + 32), d2 = GLoad32(h + 36), d5 = GLoad32(h + 48), off = d1 & ~0xFFFu, nz = 0;
                for (uint32_t k = 0; k < 0x1000 && off + k < dataSize; k += 4) if (GLoad32(data + off + k)) ++nz;
                fprintf(stderr, "[game]   #%u %c%c%c%c entry %08X %08X %08X %08X \"%s\" fetch1 %08X fetch2 %08X fetch5 %08X: base offset 0x%X (%u non-zero words in its first page)\n",
                        i, char(type >> 24), char(type >> 16), char(type >> 8), char(type), GLoad32(e + 4), GLoad32(e + 8), GLoad32(e + 12),
                        GLoad32(e + 16), name.c_str(), d1, d2, d5, off, nz);
            }
        }
    }
    __imp__sub_829D7320(ctx, base);
}
