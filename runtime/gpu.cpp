// GPU command processor front end. See gpu.h.
//
// Packet formats follow the public R600/Xenos PM4 documentation and the
// behaviour documented by emulators: a 32-bit header whose top two bits give
// the packet type; type 0 writes a run of registers, type 1 writes two
// registers, type 2 is a no-op, type 3 is an opcode with a dword payload.
#include "dev_env.h"
#include "gpu.h"
#include "kernel.h"
#include "xenos/xenos.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Type-3 opcodes we act on. Everything else is skipped by its length.
enum : uint32_t
{
    PM4_NOP                 = 0x10,
    PM4_ME_INIT             = 0x48,
    PM4_REG_RMW             = 0x21,
    PM4_INDIRECT_BUFFER_PFD = 0x37,
    PM4_WAIT_REG_MEM        = 0x3C,
    PM4_MEM_WRITE           = 0x3D,
    PM4_REG_TO_MEM          = 0x3E,
    PM4_INDIRECT_BUFFER     = 0x3F,
    PM4_COND_WRITE          = 0x45,
    PM4_EVENT_WRITE         = 0x46,
    PM4_INTERRUPT           = 0x54,
    PM4_EVENT_WRITE_SHD     = 0x58,
    PM4_EVENT_WRITE_EXT     = 0x5A,
    PM4_EVENT_WRITE_ZPD     = 0x5B,
    PM4_XE_SWAP             = 0x64,
    PM4_SET_BIN_MASK        = 0x50,
    PM4_SET_BIN_SELECT      = 0x51,
    PM4_SET_BIN_MASK_LO     = 0x60,
    PM4_SET_BIN_MASK_HI     = 0x61,
    PM4_SET_BIN_SELECT_LO   = 0x62,
    PM4_SET_BIN_SELECT_HI   = 0x63,
    PM4_DRAW_INDX           = 0x22,
    PM4_DRAW_INDX_2         = 0x36,
    PM4_IM_LOAD             = 0x27,
    PM4_IM_LOAD_IMMEDIATE   = 0x2B,
    PM4_SET_CONSTANT        = 0x2D,
    PM4_LOAD_ALU_CONSTANT   = 0x2F,
    PM4_INVALIDATE_STATE    = 0x3B,
    PM4_SET_CONSTANT2       = 0x55,
    PM4_SET_SHADER_CONSTANTS= 0x56,
};

// SET_CONSTANT / LOAD_ALU_CONSTANT target: type in bits 16-23 selects the bank.
// Returns false for an unknown bank.
bool ConstantIndex(uint32_t offsetType, uint32_t* index)
{
    uint32_t i = offsetType & 0x7FF;
    switch ((offsetType >> 16) & 0xFF)
    {
    case 0: i += 0x4000; break;   // ALU (float4 constants)
    case 1: i += 0x4800; break;   // fetch constants
    case 2: i += 0x4900; break;   // bool constants
    case 3: i += 0x4908; break;   // loop constants
    case 4: i += 0x2000; break;   // registers
    default: return false;
    }
    *index = i;
    return true;
}

// Top byte of the VdSwap ordinal stamped into XE_SWAP payload dword 6.
constexpr uint32_t kSwapOrdinalTag = 0x5A000000;

// Rolling log of the most recent packets, printed when a wait stalls.
struct RecentPacket { uint32_t hdr, pos, a0, a1, a2; bool ib; };
static RecentPacket g_recent[24]; static uint32_t g_recentN = 0;
static void DumpRecentPackets()
{
    fprintf(stderr, "[gpu]   last packets before the stall:\n");
    uint32_t n = g_recentN < 24 ? g_recentN : 24;
    for (uint32_t i = 0; i < n; ++i)
    {
        const auto& r = g_recent[(g_recentN - n + i) % 24];
        uint32_t type = r.hdr >> 30;
        if (type == 3) fprintf(stderr, "[gpu]     @%s+%-5u type3 op 0x%02X count %-3u  %08X %08X %08X\n", r.ib ? "ib" : "ring", r.pos, (r.hdr >> 8) & 0x7F, ((r.hdr >> 16) & 0x3FFF) + 1, r.a0, r.a1, r.a2);
        else if (type == 0) fprintf(stderr, "[gpu]     @%s+%-5u type0 regs 0x%04X x%-3u  %08X %08X %08X\n", r.ib ? "ib" : "ring", r.pos, r.hdr & 0x7FFF, ((r.hdr >> 16) & 0x3FFF) + 1, r.a0, r.a1, r.a2);
        else fprintf(stderr, "[gpu]     @%s+%-5u type%u hdr 0x%08X\n", r.ib ? "ib" : "ring", r.pos, type, r.hdr);
    }
}

inline uint32_t Reg(uint32_t idx)              { return GLoad32(kGpuRegBase + idx * 4); }
inline void     SetReg(uint32_t idx, uint32_t v){ GStore32(kGpuRegBase + idx * 4, v); }
void WriteRegister(uint32_t idx, uint32_t v);
void WatchWrite(uint32_t addr, uint32_t newBe);

struct Gpu
{
    uint32_t ringBase = 0, ringSizeDw = 0;
    uint32_t rptr = 0;                          // dword index into the ring
    uint32_t wbPtr = 0;                         // read-pointer write-back address
    uint32_t intCallback = 0, intData = 0;
    std::atomic<uint64_t> swaps{ 0 };
    std::atomic<uint64_t> packets{ 0 };
    std::atomic<uint64_t> vblanks{ 0 };
    std::atomic<uint32_t> vdSwapOrdinal{ 0 };   // last VdSwap that formatted a packet
    std::atomic<uint64_t> staleSwaps{ 0 };      // XE_SWAPs replayed (ordinal already executed, or untagged)
    int64_t lastSwapOrdinal = -1;               // CP thread only
    // Predicated tiling: a type-3 packet with header bit 0 set only executes
    // when (binSelect & binMask) != 0. CP thread only.
    uint64_t binMask = ~0ull, binSelect = ~0ull;
    std::atomic<uint64_t> predicatedSkips{ 0 };
    std::atomic<uint64_t> interrupts{ 0 };      // PM4_INTERRUPT packets executed
    // CP thread idle time (GpuCpIdle): ring empty (the game has queued nothing)
    // and WAIT_REG_MEM that had to wait (on memory or registers the CPU writes).
    std::atomic<uint64_t> ringEmptyNs{ 0 }, memWaitNs{ 0 };
    std::atomic<bool> started{ false };
    std::mutex intMu;                           // interrupts are serialized
    bool debug = false;
};
Gpu g;

// HC09_GPU_TRACE_FROM=<n>: once VdSwap #n has been formatted, log every ring
// packet, every write-pointer change and the IB/wait/swap packets inside IBs
// (budget: HC09_GPU_TRACE_LINES, default 4000 lines).
bool Tracing()
{
    static const long from = Hc09Env("HC09_GPU_TRACE_FROM") ? atol(Hc09Env("HC09_GPU_TRACE_FROM")) : -1;
    return from >= 0 && long(g.vdSwapOrdinal.load()) >= from;
}
void TraceLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void TraceLine(const char* fmt, ...)
{
    static const long budget = Hc09Env("HC09_GPU_TRACE_LINES") ? atol(Hc09Env("HC09_GPU_TRACE_LINES")) : 4000;
    static std::atomic<long> used{ 0 };
    long n = used.fetch_add(1);
    if (n > budget) return;
    if (n == budget) { fprintf(stderr, "[gpu-trace] (budget exhausted)\n"); return; }
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

// A register write coming from the command stream, with the hardware's side
// effects:
//  - SCRATCH_REG0..7: if enabled in SCRATCH_UMSK the value is also written to
//    memory at SCRATCH_ADDR + reg*4. This is how the GPU posts data (fence
//    values, the callback for PM4_INTERRUPT) where the CPU can read it.
//  - COHER_STATUS_HOST: a write starts a cache-coherency action; bit 31 is
//    "busy" until the action completes (which for us is at the next wait).
// Register write log for the live renderer's thread (GpuTakeRegisterWrites):
// every register a packet writes, once per interval between two takes. CP
// thread only (packets are the only writers of draw state).
constexpr uint32_t kLoggedRegs = 0x8000;
bool g_regLogOn = false;
std::vector<uint32_t> g_regLog;
uint8_t g_regLogged[kLoggedRegs];

void WriteRegister(uint32_t idx, uint32_t v)
{
    if (idx == REG_COHER_STATUS_HOST) v |= 0x80000000u;
    SetReg(idx, v);
    if (g_regLogOn && idx < kLoggedRegs && !g_regLogged[idx])
    {
        g_regLogged[idx] = 1;
        g_regLog.push_back(idx);
    }
    if (idx == REG_SCRATCH_ADDR || idx == REG_SCRATCH_UMSK)
        fprintf(stderr, "[gpu] scratch write-back: mask 0x%08X -> pa 0x%08X\n", Reg(REG_SCRATCH_UMSK), Reg(REG_SCRATCH_ADDR));
    if (idx >= REG_SCRATCH_REG0 && idx < REG_SCRATCH_REG0 + 8)
    {
        uint32_t n = idx - REG_SCRATCH_REG0;
        if (g.debug) fprintf(stderr, "[gpu]   scratch%u write 0x%08X (UMSK 0x%08X ADDR 0x%08X)\n", n, v, Reg(REG_SCRATCH_UMSK), Reg(REG_SCRATCH_ADDR));
        if (Reg(REG_SCRATCH_UMSK) & (1u << n))
        {
            uint32_t addr = (Reg(REG_SCRATCH_ADDR) & ~3u) + n * 4;
            WatchWrite(addr, __builtin_bswap32(v));
            GStore32(addr, v);
            static int logged = 0;
            if (logged++ < 16 || g.debug) fprintf(stderr, "[gpu]   scratch%u = 0x%08X -> mem 0x%08X\n", n, v, addr);
        }
    }
}


// GPU-side endian swizzle encoded in the low 2 bits of addresses.
uint32_t GpuSwap(uint32_t v, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1:  return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);  // 8-in-16
    case 2:  return __builtin_bswap32(v);                                // 8-in-32
    case 3:  return (v << 16) | (v >> 16);                               // 16-in-32
    default: return v;
    }
}
// Diagnostic watch on one guest dword (physical address): GPU writes that
// lower its (big-endian) value are reported with the packet that did it.
std::atomic<uint32_t> g_watchPa{ 0 };
thread_local const char* t_pktWhere = "";
thread_local uint32_t t_pktAddr = 0;
void WatchWrite(uint32_t addr, uint32_t newBe)
{
    uint32_t w = g_watchPa.load(std::memory_order_relaxed);
    if (!w || (addr & 0x1FFFFFFF) != w) return;
    uint32_t oldV = GLoad32(addr), newV = __builtin_bswap32(newBe);
    static std::atomic<int> drops{ 0 }, writes{ 0 };
    int nw = writes.fetch_add(1);
    if (nw < 8 || (newV < oldV && drops.fetch_add(1) < 20))
        fprintf(stderr, "[watch] pa 0x%08X: %u -> %u%s by packet @%s 0x%08X (gpu swaps %llu)\n", w, oldV, newV,
                newV < oldV ? " (DECREASE)" : "", t_pktWhere, t_pktAddr, (unsigned long long)g.swaps.load());
}
// GPU writes land in memory exactly as swizzled (no further byte swap).
inline void GpuStoreRaw(uint32_t addr, uint32_t v) { WatchWrite(addr, v); *(volatile uint32_t*)(g_base + addr) = v; }
inline uint32_t GpuLoadRaw(uint32_t addr)          { return *(volatile uint32_t*)(g_base + addr); }

// Lean CP (the native renderer draws, HC09_CP_LEAN, default on): the CP still runs the
// ring for what the game observes (fences, interrupts, swaps, waits, scratch and ring
// write-backs) but skips the draw state only a renderer of the PM4 stream reads: shader
// constants (registers 0x4000+), shader microcode loads and the per-draw bookkeeping.
// Off with the emulated renderer and while software captures or their traces are set up.
bool g_cpLean = false;

// Native fences (gpu_live.cpp, HC09_NATIVE_FENCES): D3D's completed-fence word is no
// longer written by the CP alone; its EVENT_WRITE_SHD there goes to `sink`, which
// publishes the older of the CP's and the native GPU's fence.
std::atomic<uint32_t> g_fenceRedirectPa{ 0 };
void (*g_fenceSink)(uint32_t value) = nullptr;

// A dword stream: the primary ring (wrapping) or an indirect buffer (linear).
struct Stream
{
    uint32_t base, sizeDw, pos;     // sizeDw == 0 -> linear
    uint32_t Read()
    {
        uint32_t v = GLoad32(base + pos * 4);
        pos = sizeDw ? (pos + 1) % sizeDw : pos + 1;
        return v;
    }
    void Skip(uint32_t n) { pos = sizeDw ? (pos + n) % sizeDw : pos + n; }
};

// The interrupt callback runs guest code; give it a thread context of its own.
struct IntThread
{
    PPCContext* ctx = nullptr;
    uint32_t pcr = 0;
    void Init(const char* name, uint8_t cpu)
    {
        ctx = new PPCContext();
        uint32_t stackSize = 0x20000;
        uint32_t stackBase = GuestAlloc(stackSize, 0x1000);
        pcr = KernelRegisterHostThread(name, cpu, stackBase, stackSize, ctx);
        InitThreadContext(*ctx, (stackBase + stackSize - 0x100) & ~0xFu, pcr);
    }
};

void DispatchInterrupt(IntThread& t, uint32_t source, uint8_t cpu)
{
    uint32_t cb, data;
    { std::lock_guard<std::mutex> lk(g.intMu); cb = g.intCallback; data = g.intData; }
    if (!cb) return;
    std::lock_guard<std::mutex> lk(g.intMu);
    GStore8(t.pcr + KPCR_CUR_CPU, cpu);
    t.ctx->r3.u64 = source;
    t.ctx->r4.u64 = data;
    CallGuest(cb, *t.ctx);
}

IntThread g_cpThread;      // command processor (also delivers PM4_INTERRUPT)

// WAIT_REG_MEM / COND_WRITE compare function (bits 0..2 of the info word).
bool WaitCompare(uint32_t waitInfo, uint32_t value, uint32_t ref, uint32_t mask)
{
    value &= mask;
    switch (waitInfo & 7)
    {
    case 0: return false;           // never
    case 1: return value <  ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value >  ref;
    default: return true;           // always
    }
}
static const char* kCmpName[8] = { "never", "<", "<=", "==", "!=", ">=", ">", "always" };

void ExecuteStream(Stream& s, uint32_t endDw, int depth);

// HC09_TRACE_SIGNALS=N: every N-th frame, list the packets through which the
// CP makes something visible to the CPU (fence/memory writes, interrupts) or
// waits on it, each with the number of draws executed since the last XE_SWAP:
// how far a frame gets before the CPU can observe GPU progress (overlapping
// the GPU's execution of a frame with the next one depends on it).
void NoteSignal(uint32_t opcode)
{
    static const long every = Hc09Env("HC09_TRACE_SIGNALS") ? atol(Hc09Env("HC09_TRACE_SIGNALS")) : 0;
    if (every <= 0) return;
    static uint32_t draws = 0;
    static std::string line;
    const char* name = nullptr;
    switch (opcode)
    {
    case PM4_DRAW_INDX: case PM4_DRAW_INDX_2: ++draws; return;
    case PM4_EVENT_WRITE_SHD: name = "SHD"; break;
    case PM4_EVENT_WRITE_EXT: name = "EXT"; break;
    case PM4_EVENT_WRITE_ZPD: name = "ZPD"; break;
    case PM4_MEM_WRITE: name = "MEM"; break;
    case PM4_REG_TO_MEM: name = "R2M"; break;
    case PM4_COND_WRITE: name = "CW"; break;
    case PM4_INTERRUPT: name = "INT"; break;
    case PM4_WAIT_REG_MEM: name = "WAIT"; break;
    case PM4_XE_SWAP:
        if ((g.swaps.load() + 1) % uint64_t(every) == 0)
            fprintf(stderr, "[signals] swap %llu: %u draws;%s\n", (unsigned long long)g.swaps.load() + 1, draws, line.c_str());
        draws = 0;
        line.clear();
        return;
    default: return;
    }
    if (line.size() < 4000) line += " " + std::string(name) + "@" + std::to_string(draws);
}

// HC09_GPU_CPUPROF=1: CP thread time per packet kind, exclusive of nested
// indirect buffers (bucket = type-3 opcode; 128 = type-0 register writes,
// 129 = type 1); every ~10 s the top kinds in ms/frame ([cp-profile]). The
// DRAW_INDX bucket includes the live renderer's OnDraw.
struct CpProfile
{
    bool on = false;
    double sec[130] = {};
    uint64_t count[130] = {};
    double child = 0;   // time of packets nested in the one being timed
    uint64_t swaps0 = 0;
    // WAIT_REG_MEM that had to wait: count and time per duration bucket
    // (<50 us, <1 ms, <5 ms, >=5 ms) and per polled address.
    uint64_t waitCount[4] = {}, waitNs[4] = {};
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> waitByAddr;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    CpProfile() { const char* e = Hc09Env("HC09_GPU_CPUPROF"); on = e && *e && strcmp(e, "0"); }
    void Report()
    {
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        uint64_t frames = g.swaps.load() - swaps0;
        if (wall < 10.0 || !frames) return;
        int order[130];
        for (int i = 0; i < 130; ++i) order[i] = i;
        std::sort(order, order + 130, [&](int a, int b) { return sec[a] > sec[b]; });
        std::string line;
        char buf[96];
        double total = 0;
        for (double v : sec) total += v;
        for (int k = 0; k < 8 && sec[order[k]] > 0; ++k)
        {
            int i = order[k];
            char name[16];
            if (i == 128) snprintf(name, sizeof(name), "type0");
            else if (i == 129) snprintf(name, sizeof(name), "type1");
            else snprintf(name, sizeof(name), "op%02X", i);
            snprintf(buf, sizeof(buf), "%s%s %.2f ms (%.0f/frame)", k ? ", " : "", name, 1e3 * sec[i] / double(frames),
                     double(count[i]) / double(frames));
            line += buf;
        }
        fprintf(stderr, "[cp-profile] %.1f ms/frame in packets: %s\n", 1e3 * total / double(frames), line.c_str());
        static const char* kBucket[4] = { "<50us", "<1ms", "<5ms", ">=5ms" };
        line.clear();
        for (int b = 0; b < 4; ++b)
        {
            snprintf(buf, sizeof(buf), "%s%s %.1f/frame %.2f ms", b ? ", " : "", kBucket[b], double(waitCount[b]) / double(frames),
                     1e-6 * double(waitNs[b]) / double(frames));
            line += buf;
        }
        std::vector<std::pair<uint64_t, uint32_t>> addrs;
        for (auto& [a, cn] : waitByAddr) addrs.push_back({ cn.second, a });
        std::sort(addrs.rbegin(), addrs.rend());
        for (size_t k = 0; k < addrs.size() && k < 3; ++k)
        {
            snprintf(buf, sizeof(buf), "%s0x%08X %.2f ms (%.1f/frame)", k ? ", " : "; top: ", addrs[k].second,
                     1e-6 * double(addrs[k].first) / double(frames), double(waitByAddr[addrs[k].second].first) / double(frames));
            line += buf;
        }
        fprintf(stderr, "[cp-profile]   waits that waited: %s\n", line.c_str());
        for (int b = 0; b < 4; ++b) { waitCount[b] = 0; waitNs[b] = 0; }
        waitByAddr.clear();
        for (int i = 0; i < 130; ++i) { sec[i] = 0; count[i] = 0; }
        swaps0 = g.swaps.load();
        t0 = std::chrono::steady_clock::now();
    }
};
CpProfile g_cpProf;

void NoteWait(uint32_t addr, uint64_t ns)
{
    if (!g_cpProf.on) return;
    int b = ns < 50000 ? 0 : ns < 1000000 ? 1 : ns < 5000000 ? 2 : 3;
    ++g_cpProf.waitCount[b];
    g_cpProf.waitNs[b] += ns;
    auto& e = g_cpProf.waitByAddr[addr];
    ++e.first;
    e.second += ns;
}

void ExecuteType3(Stream& s, uint32_t opcode, uint32_t count, int depth)
{
    NoteSignal(opcode);
    switch (opcode)
    {
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
    {
        uint32_t ptr = s.Read();
        uint32_t len = s.Read() & 0xFFFFF;
        if (depth < 8)
        {
            Stream ib{ ptr, 0, 0 };
            ExecuteStream(ib, len, depth + 1);
        }
        else fprintf(stderr, "[gpu] indirect buffer nesting too deep, skipping\n");
        break;
    }
    case PM4_XE_SWAP:
    {
        uint32_t at = s.base + ((s.sizeDw ? (s.pos + s.sizeDw - 1) % s.sizeDw : s.pos - 1)) * 4;   // header
        uint32_t payload[7];
        for (uint32_t i = 0; i < 7 && i < count; ++i) payload[i] = s.Read();
        s.Skip(count > 7 ? count - 7 : 0);
        uint32_t magic = payload[0], frontBuffer = payload[1], tag = payload[6];
        uint64_t n = g.swaps.fetch_add(1);
        // A packet VdSwap formatted carries its ordinal. Executing an ordinal at
        // or below the last one executed means the GPU replayed old memory.
        uint32_t newest = g.vdSwapOrdinal.load() & 0xFFFFFF;
        bool ours = (tag & 0xFF000000) == kSwapOrdinalTag;
        int64_t ord = ours ? int64_t(tag & 0xFFFFFF) : -1;
        bool stale = !ours || ord <= g.lastSwapOrdinal;
        if (ours && ord > g.lastSwapOrdinal) g.lastSwapOrdinal = ord;
        uint64_t staleN = stale ? g.staleSwaps.fetch_add(1) + 1 : g.staleSwaps.load();
        if (n < 5 || (n % 600) == 0 || (stale && staleN <= 5))
            fprintf(stderr, "[gpu] swap #%llu (frontbuffer 0x%08X%s; in %s @0x%08X; VdSwap ordinal %s%lld, newest %u; replayed so far %llu)\n",
                    (unsigned long long)n + 1, frontBuffer, magic == 0x53574150 ? "" : ", bad magic",
                    s.sizeDw ? "ring" : "indirect buffer", at, ours ? "" : "untagged ", (long long)ord, newest,
                    (unsigned long long)staleN);
        if (n >= 3600 && (n % 600) == 0 && Hc09Env("HC09_GPU_SWAPDUMP")) DumpRecentPackets();
        xenos::FrameEnd(frontBuffer, count > 4 ? payload[4] : 0, count > 5 ? payload[5] : 0);
        if (g_cpProf.on) g_cpProf.Report();
        break;
    }
    case PM4_EVENT_WRITE_SHD:
    {
        uint32_t initiator = s.Read();
        uint32_t address   = s.Read();
        uint32_t value     = s.Read();
        uint32_t data = (initiator >> 31) ? uint32_t(g.swaps.load()) : value;  // bit 31: write the counter
        uint32_t redirect = g_fenceRedirectPa.load(std::memory_order_acquire);
        if (redirect && (address & 0x1FFFFFFCu) == redirect) g_fenceSink(data);
        else GpuStoreRaw(address & ~3u, GpuSwap(data, address));
        SetReg(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        break;
    }
    case PM4_EVENT_WRITE_EXT:
    {
        s.Read();                               // initiator
        uint32_t address = s.Read();
        // Screen extents the GPU reports after a draw: cover the whole 720p target.
        uint16_t ext[6] = { 0, 1280, 0, 720, 0, 1 };
        for (int i = 0; i < 3; ++i)
        {
            uint32_t v = uint32_t(ext[i * 2]) | (uint32_t(ext[i * 2 + 1]) << 16);
            GpuStoreRaw((address & ~3u) + i * 4, GpuSwap(v, 1));
        }
        break;
    }
    case PM4_ME_INIT:
    {
        fprintf(stderr, "[gpu]   ME_INIT:");
        for (uint32_t i = 0; i < count; ++i) fprintf(stderr, " %08X", s.Read());
        fprintf(stderr, "\n");
        break;
    }
    case PM4_EVENT_WRITE_ZPD:
    {
        static std::atomic<int> n{ 0 };
        if (n.fetch_add(1) < 8)
        {
            Stream peek = s;
            fprintf(stderr, "[gpu] EVENT_WRITE_ZPD initiator %08X; regs 0x2310..0x2313 = %08X %08X %08X %08X (gpu swaps %llu)\n",
                    peek.Read(), Reg(0x2310), Reg(0x2311), Reg(0x2312), Reg(0x2313), (unsigned long long)g.swaps.load());
            for (uint32_t r = 0x2310; r <= 0x2313; ++r)
            {
                uint32_t a = Reg(r) & 0x1FFFFFFC;
                if (a >= 0x100000)
                    fprintf(stderr, "[gpu]   @reg 0x%04X -> pa 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", r, a,
                            GLoad32(a), GLoad32(a + 4), GLoad32(a + 8), GLoad32(a + 12),
                            GLoad32(a + 16), GLoad32(a + 20), GLoad32(a + 24), GLoad32(a + 28));
            }
        }
        s.Skip(count);
        break;
    }
    case PM4_EVENT_WRITE:
        s.Skip(count);
        break;
    // Bin mask/select for predicated tiling (see the predicate check in ExecuteStream).
    case PM4_SET_BIN_MASK:
    {
        uint64_t hi = s.Read(), lo = s.Read();
        g.binMask = (hi << 32) | lo;
        break;
    }
    case PM4_SET_BIN_SELECT:
    {
        uint64_t hi = s.Read(), lo = s.Read();
        g.binSelect = (hi << 32) | lo;
        break;
    }
    // ---- draw state: constants and shader microcode --------------------------
    case PM4_SET_CONSTANT:
    {
        uint32_t index;
        if (!ConstantIndex(s.Read(), &index) || (g_cpLean && index >= 0x4000)) { s.Skip(count - 1); break; }
        for (uint32_t n = 1; n < count; ++n) WriteRegister(index++, s.Read());
        break;
    }
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS:
    {
        uint32_t index = s.Read() & 0xFFFF;
        if (g_cpLean && index >= 0x4000) { s.Skip(count - 1); break; }
        for (uint32_t n = 1; n < count; ++n) WriteRegister(index++, s.Read());
        break;
    }
    case PM4_LOAD_ALU_CONSTANT:
    {
        uint32_t address = s.Read() & 0x1FFFFFFF, offsetType = s.Read(), dwords = s.Read() & 0xFFF, index;
        if (!ConstantIndex(offsetType, &index) || (g_cpLean && index >= 0x4000)) break;
        for (uint32_t n = 0; n < dwords; ++n) WriteRegister(index++, GLoad32(address + n * 4));
        break;
    }
    case PM4_IM_LOAD:
    {
        uint32_t addrType = s.Read(), startSize = s.Read();
        uint32_t address = (addrType & ~3u) & 0x1FFFFFFF;
        if (!g_cpLean) xenos::LoadShader(xenos::ShaderType(addrType & 3), g_base + address, startSize & 0xFFFF, address);
        break;
    }
    case PM4_IM_LOAD_IMMEDIATE:
    {
        if (g_cpLean) { s.Skip(count); break; }
        uint32_t type = s.Read(), startSize = s.Read(), dwords = startSize & 0xFFFF;
        if (dwords > count - 2) dwords = count - 2;
        std::vector<uint32_t> code(dwords);
        for (uint32_t n = 0; n < dwords; ++n) code[n] = __builtin_bswap32(s.Read());   // back to big-endian bytes
        xenos::LoadShader(xenos::ShaderType(type & 3), reinterpret_cast<const uint8_t*>(code.data()), dwords, 0);
        break;
    }
    case PM4_INVALIDATE_STATE:
        s.Skip(count);
        break;
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2:
    {
        // DRAW_INDX: viz-query token, initiator[, DMA base, DMA size].
        // DRAW_INDX_2: initiator[, ...] (indices in the packet are not modelled).
        if (opcode == PM4_DRAW_INDX) s.Read();
        uint32_t init = s.Read();
        WriteRegister(xenos::reg::VGT_DRAW_INITIATOR, init);
        xenos::DrawPacket d;
        d.primType = init & 0x3F;
        d.sourceSelect = (init >> 6) & 3;
        d.indexSize32 = (init >> 11) & 1;
        d.numIndices = init >> 16;
        if (d.sourceSelect == 0)
        {
            d.dmaBase = s.Read();
            uint32_t size = s.Read();
            WriteRegister(xenos::reg::VGT_DMA_BASE, d.dmaBase);
            WriteRegister(xenos::reg::VGT_DMA_SIZE, size);
            d.dmaWords = size & 0xFFFFFF;
            d.dmaEndian = size >> 30;
        }
        d.where = t_pktWhere;
        d.packetAddr = t_pktAddr;
        if (!g_cpLean) xenos::Draw(d);
        break;   // ExecuteStream realigns past any remaining payload
    }
    case PM4_SET_BIN_MASK_LO:   g.binMask   = (g.binMask   & 0xFFFFFFFF00000000ull) | s.Read(); break;
    case PM4_SET_BIN_MASK_HI:   g.binMask   = (g.binMask   & 0x00000000FFFFFFFFull) | (uint64_t(s.Read()) << 32); break;
    case PM4_SET_BIN_SELECT_LO: g.binSelect = (g.binSelect & 0xFFFFFFFF00000000ull) | s.Read(); break;
    case PM4_SET_BIN_SELECT_HI: g.binSelect = (g.binSelect & 0x00000000FFFFFFFFull) | (uint64_t(s.Read()) << 32); break;
    case PM4_MEM_WRITE:
    {
        uint32_t address = s.Read();
        for (uint32_t i = 1; i < count; ++i)
        {
            uint32_t v = s.Read();
            GpuStoreRaw((address & ~3u) + (i - 1) * 4, GpuSwap(v, address));
        }
        break;
    }
    case PM4_REG_TO_MEM:
    {
        uint32_t reg = s.Read() & 0x7FFF;
        uint32_t address = s.Read();
        GpuStoreRaw(address & ~3u, GpuSwap(Reg(reg), address));
        break;
    }
    case PM4_REG_RMW:
    {
        uint32_t info = s.Read(), andMask = s.Read(), orMask = s.Read();
        uint32_t idx = info & 0x1FFF;
        uint32_t v = Reg(idx);
        if ((info >> 30) & 1) andMask = Reg(andMask & 0x1FFF);
        if ((info >> 31) & 1) orMask  = Reg(orMask & 0x1FFF);
        WriteRegister(idx, (v & andMask) | orMask);
        break;
    }
    case PM4_WAIT_REG_MEM:
    {
        uint32_t info = s.Read(), poll = s.Read(), ref = s.Read(), mask = s.Read(), wait = s.Read();
        // HC09_TRACE_HANDSHAKE=<swap #>: from that GPU swap on, log memory waits
        // (D3D's GPU-callback handshake) with how long they took, and INTERRUPTs.
        static const long hsFrom = Hc09Env("HC09_TRACE_HANDSHAKE") ? atol(Hc09Env("HC09_TRACE_HANDSHAKE")) : -1;
        static std::atomic<int> hsLogged{ 0 };
        bool hsThis = hsFrom >= 0 && (info & 0x10) && long(g.swaps.load()) >= hsFrom && hsLogged.load() < 200;
        auto hsStart = std::chrono::steady_clock::now();
        static int logged = 0;
        if (logged++ < 16 || g.debug)
            fprintf(stderr, "[gpu]   WAIT_REG_MEM %s 0x%08X %s ref 0x%08X mask 0x%08X wait 0x%X (current 0x%08X)\n",
                    (info & 0x10) ? "mem" : "reg", poll, kCmpName[info & 7], ref, mask, wait,
                    (info & 0x10) ? GpuSwap(GpuLoadRaw(poll & ~3u), poll) : Reg(poll & 0x7FFF));
        struct WaitTimer
        {
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            int spins = 0;
            uint32_t addr = 0;
            ~WaitTimer()
            {
                if (!spins) return;
                uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                g.memWaitNs += ns;
                NoteWait(addr, ns);
            }
        } waitTimer;
        waitTimer.addr = poll & ~3u;
        bool stallReported = false;
        for (int& spins = waitTimer.spins;; ++spins)
        {
            if (!(info & 0x10) && (poll & 0x7FFF) == REG_COHER_STATUS_HOST)
            {
                // Cache-coherency request: D3D wrote the action bits (TC/VC
                // invalidate over COHER_BASE/SIZE) and waits for the busy bit
                // to clear. We have no GPU caches, so it completes at once.
                SetReg(REG_COHER_STATUS_HOST, Reg(REG_COHER_STATUS_HOST) & ~0x80000000u);
            }
            uint32_t v = (info & 0x10) ? GpuSwap(GpuLoadRaw(poll & ~3u), poll) : Reg(poll & 0x7FFF);
            if (WaitCompare(info, v, ref, mask)) break;
            // Spin (yield) for the first millisecond, then sleep: with the 1 ms
            // timer a sleep lasts ~1-2 ms however short it is asked to be, and
            // most waits (the CPU answering a callback) end within microseconds.
            auto waited = std::chrono::steady_clock::now() - waitTimer.t0;
            if (!stallReported && waited > std::chrono::seconds(2))
            {
                stallReported = true;
                fprintf(stderr, "[gpu] WAIT_REG_MEM stalled (%s 0x%08X %s ref 0x%08X mask 0x%08X)\n",
                        (info & 0x10) ? "mem" : "reg", poll, kCmpName[info & 7], ref, mask);
                DumpRecentPackets();
            }
            if (wait >= 0x100 && waited > std::chrono::milliseconds(1)) std::this_thread::sleep_for(std::chrono::microseconds(100));
            else std::this_thread::yield();
        }
        if (hsThis && hsLogged.fetch_add(1) < 200)
        {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - hsStart).count();
            fprintf(stderr, "[handshake] wait mem 0x%08X %s 0x%X satisfied after %lld us (value 0x%08X) @%s 0x%08X, swaps %llu\n",
                    poll & ~3u, kCmpName[info & 7], ref, (long long)us, GpuSwap(GpuLoadRaw(poll & ~3u), poll),
                    t_pktWhere, t_pktAddr, (unsigned long long)g.swaps.load());
        }
        break;
    }
    case PM4_COND_WRITE:
    {
        uint32_t info = s.Read(), poll = s.Read(), ref = s.Read(), mask = s.Read(), waddr = s.Read(), wdata = s.Read();
        uint32_t v = (info & 0x10) ? GpuSwap(GpuLoadRaw(poll & ~3u), poll) : Reg(poll & 0x7FFF);
        if (WaitCompare(info, v, ref, mask))
        {
            if (info & 0x100) GpuStoreRaw(waddr & ~3u, GpuSwap(wdata, waddr));
            else WriteRegister(waddr & 0x7FFF, wdata);
        }
        break;
    }
    case PM4_INTERRUPT:
    {
        uint32_t cpuMask = s.Read();
        uint64_t ni = g.interrupts.fetch_add(1);
        static const long hsFrom = Hc09Env("HC09_TRACE_HANDSHAKE") ? atol(Hc09Env("HC09_TRACE_HANDSHAKE")) : -1;
        static std::atomic<int> hsN{ 0 };
        if (hsFrom >= 0 && long(g.swaps.load()) >= hsFrom && hsN.fetch_add(1) < 120)
            fprintf(stderr, "[handshake] INTERRUPT #%llu cpu mask 0x%X @%s 0x%08X, swaps %llu\n", (unsigned long long)ni,
                    cpuMask, t_pktWhere, t_pktAddr, (unsigned long long)g.swaps.load());
        for (uint8_t c = 0; c < 6; ++c)
            if (cpuMask & (1u << c)) DispatchInterrupt(g_cpThread, 1, c);
        break;
    }
    default:
    {
        // Report each opcode we skip the first time it appears: anything with a
        // memory or control-flow effect here is a gap in the command processor.
        static std::atomic<uint64_t> seen[2]{};
        uint64_t bit = 1ull << (opcode & 63);
        if (!(seen[opcode >> 6].fetch_or(bit) & bit))
        {
            Stream peek = s;
            uint32_t a0 = count > 0 ? peek.Read() : 0, a1 = count > 1 ? peek.Read() : 0, a2 = count > 2 ? peek.Read() : 0;
            fprintf(stderr, "[gpu] first unhandled type3 op 0x%02X (count %u, in %s): %08X %08X %08X\n",
                    opcode, count, s.sizeDw ? "ring" : "ib", a0, a1, a2);
        }
        s.Skip(count);
        break;
    }
    }
}

// Total dwords of a packet, header included, from its header alone.
uint32_t PacketLength(uint32_t hdr)
{
    switch (hdr >> 30)
    {
    case 0:  return ((hdr >> 16) & 0x3FFF) + 2;
    case 1:  return 3;
    case 2:  return 1;
    default: return ((hdr >> 16) & 0x3FFF) + 2;
    }
}

void ExecuteStream(Stream& s, uint32_t endDw, int depth)
{
    // For the ring, endDw is the write pointer; for an IB, its length.
    // Packets are only executed whole, and the stream always advances by the
    // length in the header (not by what a handler happened to read): a wrong
    // step on the ring would carry the read pointer past the write pointer
    // and replay a full lap of stale packets.
    auto available = [&]() -> uint32_t {
        return s.sizeDw ? (endDw + s.sizeDw - s.pos) % s.sizeDw : (s.pos < endDw ? endDw - s.pos : 0);
    };
    while (available() > 0)
    {
        uint32_t pktPos = s.pos;
        uint32_t hdr = GLoad32(s.base + pktPos * 4);
        t_pktWhere = s.sizeDw ? "ring" : "ib";
        t_pktAddr = s.base + pktPos * 4;
        uint32_t len = PacketLength(hdr);
        if (len > available())
        {
            static int overruns = 0;
            if (overruns++ < 16)
                fprintf(stderr, "[gpu] packet 0x%08X @%s+%u needs %u dwords, only %u before the %s; %s\n",
                        hdr, s.sizeDw ? "ring" : "ib", pktPos, len, available(),
                        s.sizeDw ? "write pointer" : "end of the buffer",
                        s.sizeDw ? "waiting for the rest" : "skipping (parse error)");
            if (s.sizeDw) return;               // ring: the CP stalls until the packet is complete
            s.pos = endDw;
            return;
        }
        s.Read();
        uint64_t pn = g.packets.load(std::memory_order_relaxed);   // the CP thread is the only writer
        g.packets.store(pn + 1, std::memory_order_relaxed);
        {
            Stream peek = s;
            RecentPacket& r = g_recent[g_recentN++ % 24];
            r = { hdr, pktPos, peek.Read(), peek.Read(), peek.Read(), s.sizeDw == 0 };
        }
        if (pn < 64 || g.debug)
        {
            uint32_t type = hdr >> 30;
            if (type == 3)
                fprintf(stderr, "[gpu]   pkt %llu @%s+%u: type3 op 0x%02X count %u\n", (unsigned long long)pn,
                        s.sizeDw ? "ring" : "ib", pktPos, (hdr >> 8) & 0x7F, ((hdr >> 16) & 0x3FFF) + 1);
            else if (type == 0)
            {
                Stream peek = s;
                fprintf(stderr, "[gpu]   pkt %llu @%s+%u: type0 regs 0x%04X x%u%s  hdr %08X val %08X\n", (unsigned long long)pn,
                        s.sizeDw ? "ring" : "ib", pktPos, hdr & 0x7FFF, ((hdr >> 16) & 0x3FFF) + 1, (hdr >> 15) & 1 ? " (one reg)" : "",
                        hdr, peek.Read());
            }
            else
                fprintf(stderr, "[gpu]   pkt %llu @%s+%u: type%u hdr 0x%08X\n", (unsigned long long)pn,
                        s.sizeDw ? "ring" : "ib", pktPos, type, hdr);
        }
        if (Tracing())
        {
            uint32_t op = (hdr >> 8) & 0x7F;
            bool interesting = (hdr >> 30) == 3 && (op == PM4_INDIRECT_BUFFER || op == PM4_INDIRECT_BUFFER_PFD ||
                                                    op == PM4_WAIT_REG_MEM || op == PM4_XE_SWAP || op == PM4_INTERRUPT);
            if (depth == 0 || interesting)
            {
                Stream peek = s;
                uint32_t a0 = peek.Read(), a1 = peek.Read(), a2 = peek.Read();
                TraceLine("[gpu-trace] d%d @%s 0x%08X+%u  hdr %08X (type%u op 0x%02X len %u)  %08X %08X %08X\n",
                          depth, s.sizeDw ? "ring" : "ib", s.base, pktPos, hdr, hdr >> 30, op, len, a0, a1, a2);
            }
        }
        struct PacketTimer
        {
            int bucket = -1;
            double savedChild = 0;
            std::chrono::steady_clock::time_point t0;
            explicit PacketTimer(uint32_t h)
            {
                if (!g_cpProf.on || (h >> 30) == 2) return;
                bucket = (h >> 30) == 0 ? 128 : (h >> 30) == 1 ? 129 : int((h >> 8) & 0x7F);
                savedChild = g_cpProf.child;
                g_cpProf.child = 0;
                t0 = std::chrono::steady_clock::now();
            }
            ~PacketTimer()
            {
                if (bucket < 0) return;
                double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                g_cpProf.sec[bucket] += dt - g_cpProf.child;
                ++g_cpProf.count[bucket];
                g_cpProf.child = savedChild + dt;
            }
        } packetTimer(hdr);
        switch (hdr >> 30)
        {
        case 0:
        {
            uint32_t count = ((hdr >> 16) & 0x3FFF) + 1, base = hdr & 0x7FFF;
            bool oneReg = (hdr >> 15) & 1;
            if (g_cpLean && base >= 0x4000) { s.Skip(count); break; }
            for (uint32_t i = 0; i < count; ++i) WriteRegister(oneReg ? base : base + i, s.Read());
            break;
        }
        case 1:
        {
            WriteRegister(hdr & 0x7FF, s.Read());
            WriteRegister((hdr >> 11) & 0x7FF, s.Read());
            break;
        }
        case 2:
            break;
        case 3:
        {
            uint32_t count = ((hdr >> 16) & 0x3FFF) + 1, opcode = (hdr >> 8) & 0x7F;
            // Predicated packet (header bit 0): executes only if a selected bin
            // is enabled. Predicated swaps are never executed (same rule as
            // Xenia): D3D predicates the per-tile copies of end-of-frame work.
            if ((hdr & 1) && ((g.binSelect & g.binMask) == 0 || opcode == PM4_XE_SWAP))
            {
                uint64_t n = g.predicatedSkips.fetch_add(1);
                if (n < 8)
                    fprintf(stderr, "[gpu] predicated skip: op 0x%02X (select %016llX mask %016llX) @%s 0x%08X\n", opcode,
                            (unsigned long long)g.binSelect, (unsigned long long)g.binMask, t_pktWhere, t_pktAddr);
                s.Skip(count);
                break;
            }
            ExecuteType3(s, opcode, count, depth);
            break;
        }
        }
        uint32_t next = s.sizeDw ? (pktPos + len) % s.sizeDw : pktPos + len;
        if (s.pos != next)
        {
            static int mismatches = 0;
            if (mismatches++ < 16)
                fprintf(stderr, "[gpu] handler for packet 0x%08X (type%u op 0x%02X) consumed %u of %u dwords; realigned\n",
                        hdr, hdr >> 30, (hdr >> 8) & 0x7F,
                        s.sizeDw ? (s.pos + s.sizeDw - pktPos) % s.sizeDw : s.pos - pktPos, len);
            s.pos = next;
        }
    }
}

void WorkerMain()
{
    g_cpThread.Init("GPU command processor", 2);
    uint64_t idle = 0;
    for (;;)
    {
        uint32_t wptrRaw = g.ringSizeDw ? Reg(REG_CP_RB_WPTR) : 0;
        uint32_t wptr = g.ringSizeDw ? wptrRaw % g.ringSizeDw : 0;
        static uint32_t lastRaw = 0; static int kicks = 0;
        if (wptrRaw != lastRaw)
        {
            if (Tracing())
                TraceLine("[gpu-trace] write pointer %u -> %u (read index %u, VdSwap ordinal %u)\n",
                          lastRaw, wptrRaw, g.rptr, g.vdSwapOrdinal.load());
            if (kicks++ < 64 || g.debug)
                fprintf(stderr, "[gpu] write pointer 0x%08X -> 0x%08X (ring index %u, read index %u)\n",
                        lastRaw, wptrRaw, wptr, g.rptr);
            lastRaw = wptrRaw;
        }
        if (!g.ringSizeDw || wptr == g.rptr)
        {
            // Nothing queued: back off a little (the ring kick is a plain MMIO
            // store we cannot trap, so this is a poll).
            auto t0 = std::chrono::steady_clock::now();
            if (++idle < 50) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(200));
            g.ringEmptyNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            continue;
        }
        idle = 0;
        Stream ring{ g.ringBase, g.ringSizeDw, g.rptr };
        ExecuteStream(ring, wptr, 0);
        if (ring.pos != wptr) std::this_thread::yield();   // partial packet: wait for the rest
        g.rptr = ring.pos;
        SetReg(REG_CP_RB_RPTR, g.rptr);
        if (g.wbPtr) GStore32(g.wbPtr, g.rptr);
        if (g.debug)
            fprintf(stderr, "[gpu] consumed ring to %u (packets %llu, swaps %llu)\n",
                    g.rptr, (unsigned long long)g.packets.load(), (unsigned long long)g.swaps.load());
    }
}

void VblankMain()
{
    IntThread t; t.Init("GPU vblank", 2);
    using namespace std::chrono;
    // HC09_VBLANK_HZ=n: experiment (frame pacing study); the console's is 60.
    const int hz = Hc09Env("HC09_VBLANK_HZ") ? std::max(1, atoi(Hc09Env("HC09_VBLANK_HZ"))) : 60;
    const auto period = hz == 60 ? duration_cast<steady_clock::duration>(microseconds(16667))
                                 : duration_cast<steady_clock::duration>(duration<double>(1.0 / hz));
    if (hz != 60) fprintf(stderr, "[gpu] vblank at %d Hz (HC09_VBLANK_HZ)\n", hz);
    auto next = steady_clock::now();
    for (;;)
    {
        next += period;
        std::this_thread::sleep_until(next);
        uint64_t n = g.vblanks.fetch_add(1);
        // Scanout state as the handler will read it: vblank pending, scanline
        // at the bottom. The handler may acknowledge by writing the status
        // register; we re-arm it every vblank.
        SetReg(REG_D1MODE_V_COUNTER, 720);
        SetReg(REG_D1MODE_INT_STATUS, Reg(REG_D1MODE_INT_STATUS) | 1u);
        DispatchInterrupt(t, 0, 2);
        if (n == 0 || n == 60 || (n % 3600) == 0)
            fprintf(stderr, "[gpu] vblank #%llu delivered (swaps %llu, ring packets %llu)\n",
                    (unsigned long long)n + 1, (unsigned long long)g.swaps.load(),
                    (unsigned long long)g.packets.load());
    }
}

} // namespace

#ifdef HC09_HAVE_VULKAN
void GpuLiveInit(uint8_t* physicalMemory);   // gpu_live.cpp (HC09_GPU_LIVE)
bool NativeRendererLive();
#endif

void GpuStart()
{
    if (g.started.exchange(true)) return;
#ifdef HC09_HAVE_VULKAN
    GpuLiveInit(g_base);
    {
        const char* e = Hc09Env("HC09_CP_LEAN");
        const bool traced = Hc09Env("HC09_CAPTURE_FRAME") || Hc09Env("HC09_RENDER_RANGE") || Hc09Env("HC09_RENDER_AFTER_LOAD") ||
                            Hc09Env("HC09_WATCH_VFETCH") || Hc09Env("HC09_TRACE_VS") || Hc09Env("HC09_TRACE_RESOLVES") ||
                            Hc09Env("HC09_PICK");
        g_cpLean = NativeRendererLive() && !traced && !(e && !strcmp(e, "0"));
        if (g_cpLean) fprintf(stderr, "[gpu] lean command processor: the native renderer draws (constants, shader loads, draws skipped)\n");
    }
#endif
    g.debug = Hc09Env("HC09_GPU_DEBUG") != nullptr;
    // Registers the driver reads: GPU idle, EDRAM timing, display mode and
    // the vblank line/status the interrupt handler checks.
    SetReg(REG_RBBM_STATUS, 0);
    SetReg(REG_RB_EDRAM_TIMING, 0x08100748);
    SetReg(REG_RB_BC_CONTROL, 0x0000200E);
    SetReg(REG_D1MODE_V_COUNTER, 720);
    SetReg(REG_D1MODE_VIEWPORT_SIZE, (1280u << 16) | 720u);
    std::thread(WorkerMain).detach();
    std::thread(VblankMain).detach();
    fprintf(stderr, "[gpu] command processor and vblank (60 Hz) started\n");
}

void GpuInitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2)
{
    g.ringBase = ptr;                               // physical addresses alias 1:1 (see MapGuestMemory)
    g.ringSizeDw = (1u << (sizeLog2 + 3)) / 4;
    g.rptr = 0;
    SetReg(REG_CP_RB_BASE, ptr);
    SetReg(REG_CP_RB_RPTR, 0);
    SetReg(REG_CP_RB_WPTR, 0);
    fprintf(stderr, "[gpu] ring buffer @ 0x%08X, %u dwords\n", ptr, g.ringSizeDw);
    GpuStart();
}

void GpuEnableReadPointerWriteBack(uint32_t ptr, uint32_t blockSizeLog2)
{
    g.wbPtr = ptr;
    SetReg(REG_CP_RB_RPTR_ADDR, ptr);
    if (ptr) GStore32(ptr, g.rptr);
    fprintf(stderr, "[gpu] read-pointer write-back @ 0x%08X (block 2^%u)\n", ptr, blockSizeLog2);
}

void GpuSetInterruptCallback(uint32_t callback, uint32_t userData)
{
    std::lock_guard<std::mutex> lk(g.intMu);
    g.intCallback = callback; g.intData = userData;
    fprintf(stderr, "[gpu] graphics interrupt callback 0x%08X (data 0x%08X)\n", callback, userData);
    GpuStart();
}

void GpuWriteSwapPacket(uint32_t buf, uint32_t frontBuffer, uint32_t texFormat,
                        uint32_t colorSpace, uint32_t width, uint32_t height, uint32_t ordinal)
{
    g.vdSwapOrdinal.store(ordinal);
    // The caller reserves 64 dwords of ring space for us. One type-3 packet
    // spanning all of it: header + 63 payload dwords.
    for (int i = 0; i < 64; ++i) GStore32(buf + i * 4, 0);
    GStore32(buf + 0x00, (3u << 30) | (62u << 16) | (PM4_XE_SWAP << 8));
    GStore32(buf + 0x04, 0x53574150);           // 'SWAP'
    GStore32(buf + 0x08, frontBuffer);
    GStore32(buf + 0x0C, texFormat);
    GStore32(buf + 0x10, colorSpace);
    GStore32(buf + 0x14, width);
    GStore32(buf + 0x18, height);
    GStore32(buf + 0x1C, kSwapOrdinalTag | (ordinal & 0xFFFFFF));
}

uint64_t GpuSwapCount() { return g.swaps.load(); }
void GpuRedirectFenceWord(uint32_t pa, void (*sink)(uint32_t value))
{
    g_fenceSink = sink;
    g_fenceRedirectPa.store(pa & 0x1FFFFFFCu);
}
void GpuStoreFenceWord(uint32_t pa, uint32_t value) { GpuStoreRaw(pa & 0x1FFFFFFCu, __builtin_bswap32(value)); }

void GpuCpIdle(double* ringEmptySeconds, double* memWaitSeconds)
{
    *ringEmptySeconds = double(g.ringEmptyNs.load()) * 1e-9;
    *memWaitSeconds = double(g.memWaitNs.load()) * 1e-9;
}

void GpuLogRegisterWrites(bool on)
{
    g_regLogOn = on;
}

void GpuTakeRegisterWrites(std::vector<std::pair<uint32_t, uint32_t>>& out)
{
    out.clear();
    for (uint32_t idx : g_regLog)
    {
        out.push_back({ idx, Reg(idx) });
        g_regLogged[idx] = 0;
    }
    g_regLog.clear();
}

void GpuCopyRegisters(uint32_t* out, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) out[i] = Reg(i);
}

// Host hooks for the portable Xenos module (runtime/xenos).
namespace xenos {
uint32_t HostReg(uint32_t index) { return index < kGpuRegCount ? GLoad32(kGpuRegBase + index * 4) : 0; }
const uint32_t* HostRegFile(bool* bigEndian) { *bigEndian = true; return reinterpret_cast<const uint32_t*>(g_base + kGpuRegBase); }
const uint8_t* HostPhys(uint32_t physicalAddress) { return g_base + (physicalAddress & 0x1FFFFFFF); }
uint64_t HostSwapCount() { return GpuSwapCount(); }
}  // namespace xenos

void GpuWatchPhysical(uint32_t pa)
{
    if (g_watchPa.exchange(pa & 0x1FFFFFFF) != (pa & 0x1FFFFFFF))
        fprintf(stderr, "[watch] watching GPU writes to pa 0x%08X\n", pa & 0x1FFFFFFF);
}

void GpuSetSystemIdentifierAddress(uint32_t identifierAddr)
{
    fprintf(stderr, "[gpu] system command buffer identifier at 0x%08X (pa 0x%08X)\n",
            identifierAddr, identifierAddr & 0x1FFFFFFF);
}
