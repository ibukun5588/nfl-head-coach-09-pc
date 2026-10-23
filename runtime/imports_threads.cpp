// Kernel objects: threads, dispatcher objects (event/semaphore/mutant/timer),
// handles, critical sections, spinlocks, TLS.
//
// Model
// -----
// Every kernel object has a guest address (its "object pointer", what the
// Ke*/Ob* APIs take) and may have one or more handles (what the Nt* APIs take).
// Objects the game creates through Nt* get a small guest block allocated so
// ObReferenceObjectByHandle has a real address to hand back. Objects the game
// initializes itself in its own memory (KeInitializeSemaphore(pSem,...), or an
// inline-initialized KEVENT) are registered at that address — lazily, on first
// use, by reading the DISPATCHER_HEADER.Type byte if we have never seen it.
//
// Waits use one process-wide condition variable: every state change notifies
// it, every waiter re-checks its predicate. Simple, correct, and plenty fast
// for a boot-stage runtime.
//
// Guest calling convention reminder: args r3..r10, return in r3.
#include "dev_env.h"
#include "kernel.h"
#include "profiler.h"

#include "gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include <cstring>
#include <map>
#include <tuple>

// runtime.cpp: the calling host thread, and a host thread's CPU time (seconds).
void* HostCurrentThreadHandle();
double HostThreadCpuSeconds(void* thread);

constexpr uint32_t WAIT_OBJECT_0 = 0;

struct ThreadExit { uint32_t code; };               // thrown to unwind a guest thread

std::mutex g_dispMu;                   // guards all dispatcher state + tables
std::condition_variable g_dispCv;      // process-wide "something changed"
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_byGuest;   // ptr  -> obj
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_byHandle;  // hndl -> obj
thread_local std::shared_ptr<KObject> t_self;      // current guest thread object

namespace {
uint32_t g_nextHandle = 0x00000100;
std::atomic<uint64_t> g_nextTid{ 1 };
std::vector<std::shared_ptr<KObject>> g_allThreads;
thread_local uint32_t t_tls[256] = { 0 };           // KeTls* slots, per thread
std::atomic<uint32_t> g_tlsNext{ 1 };
} // namespace

// ---- object plumbing (callers hold g_dispMu unless noted) ---------------

void MirrorHeader(const std::shared_ptr<KObject>& o)
{
    // Keep DISPATCHER_HEADER {Type, Absolute, Size, Inserted, SignalState} in
    // guest memory roughly truthful for code that peeks at it.
    if (!o->guest) return;
    GStore8(o->guest + 0, o->type);
    int32_t ss = 0;
    switch (o->type)
    {
    case T_Semaphore: ss = o->count; break;
    case T_Mutant:    ss = o->ownerTid ? 0 : 1; break;
    default:          ss = o->signaled ? 1 : 0; break;
    }
    GStore32(o->guest + 4, uint32_t(ss));
}

std::shared_ptr<KObject> NewObject(ObjType t, uint32_t guest)
{
    auto o = std::make_shared<KObject>();
    o->type = t;
    o->guest = guest ? guest : GuestAlloc(0x40, 16);
    g_byGuest[o->guest] = o;
    MirrorHeader(o);
    return o;
}

uint32_t NewHandle(const std::shared_ptr<KObject>& o)
{
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_byHandle[h] = o;
    return h;
}

std::shared_ptr<KObject> ByHandle(uint32_t h)
{
    if (h == 0xFFFFFFFE || h == 0xFFFFFFFF) return t_self; // current thread/process pseudo-handles
    auto it = g_byHandle.find(h);
    return it == g_byHandle.end() ? nullptr : it->second;
}

// Resolve an object pointer; lazily register game-initialized dispatcher
// structs by reading their header type.
std::shared_ptr<KObject> ByGuest(uint32_t p)
{
    if (!p) return nullptr;
    auto it = g_byGuest.find(p);
    if (it != g_byGuest.end()) return it->second;
    uint8_t ty = GLoad8(p);
    int32_t ss = int32_t(GLoad32(p + 4));
    std::shared_ptr<KObject> o;
    switch (ty)
    {
    case T_NotificationEvent:
    case T_SynchronizationEvent:
        o = NewObject(ObjType(ty), p); o->manualReset = (ty == T_NotificationEvent); o->signaled = ss != 0; break;
    case T_Semaphore:
        o = NewObject(T_Semaphore, p); o->count = ss; o->limit = int32_t(GLoad32(p + 16)); if (o->limit <= 0) o->limit = 0x7FFFFFFF; break;
    case T_Mutant:
        o = NewObject(T_Mutant, p); break;
    case T_NotificationTimer:
    case T_SynchronizationTimer:
        o = NewObject(ObjType(ty), p); o->manualReset = (ty == T_NotificationTimer); break;
    default:
        fprintf(stderr, "[kernel] unknown dispatcher object @0x%08X (type byte %u); treating as manual event\n", p, ty);
        o = NewObject(T_NotificationEvent, p); o->manualReset = true; break;
    }
    return o;
}

namespace {

// Is the object currently satisfiable for `self`? If so (and consume), take it.
bool TrySatisfy(const std::shared_ptr<KObject>& o, bool consume)
{
    switch (o->type)
    {
    case T_NotificationEvent:
    case T_NotificationTimer:
        return o->signaled;
    case T_SynchronizationEvent:
    case T_SynchronizationTimer:
        if (!o->signaled) return false;
        if (consume) o->signaled = false;
        return true;
    case T_Semaphore:
        if (o->count <= 0) return false;
        if (consume) o->count--;
        return true;
    case T_Mutant:
    {
        uint64_t me = t_self ? t_self->thread->tid : 0;
        if (o->ownerTid == 0 || o->ownerTid == me)
        {
            if (consume) { o->ownerTid = me; o->recursion++; }
            return true;
        }
        return false;
    }
    case T_Thread:
        return o->thread && o->thread->finished.load();
    default:
        return true;
    }
}

// Convert a guest LARGE_INTEGER timeout pointer to an optional deadline.
// Negative = relative 100ns units; positive = absolute (treated as relative
// to now for simplicity); null = infinite.
struct Deadline { bool infinite; std::chrono::steady_clock::time_point tp; };
Deadline ReadTimeout(uint32_t pTimeout)
{
    Deadline d{ true, {} };
    if (!pTimeout) return d;
    int64_t v = int64_t(GLoad64(pTimeout));
    d.infinite = false;
    int64_t rel100ns = v < 0 ? -v : 0;             // absolute → "no wait" for now
    d.tp = std::chrono::steady_clock::now() + std::chrono::nanoseconds(rel100ns * 100);
    return d;
}

// Core wait: any/all over a set of objects. Returns STATUS_* / WAIT index.
uint32_t WaitObjects(std::vector<std::shared_ptr<KObject>>& objs, bool waitAll, uint32_t pTimeout)
{
    Deadline dl = ReadTimeout(pTimeout);
    std::unique_lock<std::mutex> lk(g_dispMu);
    for (;;)
    {
        if (waitAll)
        {
            bool all = true;
            for (auto& o : objs) if (!TrySatisfy(o, false)) { all = false; break; }
            if (all)
            {
                for (auto& o : objs) { TrySatisfy(o, true); MirrorHeader(o); }
                return WAIT_OBJECT_0;
            }
        }
        else
        {
            for (size_t i = 0; i < objs.size(); ++i)
                if (TrySatisfy(objs[i], true)) { MirrorHeader(objs[i]); return WAIT_OBJECT_0 + uint32_t(i); }
        }
        if (dl.infinite) g_dispCv.wait(lk);
        else if (g_dispCv.wait_until(lk, dl.tp) == std::cv_status::timeout)
        {
            // one last check after timeout
            if (!waitAll)
                for (size_t i = 0; i < objs.size(); ++i)
                    if (TrySatisfy(objs[i], true)) { MirrorHeader(objs[i]); return WAIT_OBJECT_0 + uint32_t(i); }
            return STATUS_TIMEOUT;
        }
    }
}

// ---- thread trampoline ----------------------------------------------------

void ThreadMain(std::shared_ptr<KObject> self)
{
    auto& th = *self->thread;
    t_self = self;
    ProfilerRegisterThread(th.tid);

    // Honor CREATE_SUSPENDED: wait until resumed.
    {
        std::unique_lock<std::mutex> lk(g_dispMu);
        g_dispCv.wait(lk, [&] { return th.suspendCount.load() <= 0; });
    }

    PPCContext& ctx = *th.ctx;
    uint32_t sp = (th.stackBase + th.stackSize - 0x100) & ~0xFu;
    InitThreadContext(ctx, sp, th.pcr);
    SetupPcr(th.pcr, self->guest, th.cpu, th.stackBase, th.stackSize);

    fprintf(stderr, "[kernel] thread %llu start: entry=0x%08X param=0x%08X xapi=0x%08X stack=0x%08X cpu=%u\n",
            (unsigned long long)th.tid, th.startAddr, th.param, th.xapiStartup, sp, th.cpu);
    try
    {
        if (th.xapiStartup)
        {
            // Kernel calls XapiThreadStartup(startAddr, param) which runs the
            // CRT per-thread setup and then calls startAddr(param).
            ctx.r3.u64 = th.startAddr;
            ctx.r4.u64 = th.param;
            CallGuest(th.xapiStartup, ctx);
        }
        else
        {
            ctx.r3.u64 = th.param;
            CallGuest(th.startAddr, ctx);
        }
        th.exitCode = ctx.r3.u32;
    }
    catch (const ThreadExit& e)
    {
        th.exitCode = e.code;
    }
    fprintf(stderr, "[kernel] thread %llu exit (code 0x%X)\n", (unsigned long long)th.tid, th.exitCode);
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        th.finished = true;
        self->signaled = true;
        MirrorHeader(self);
    }
    g_dispCv.notify_all();
}

std::shared_ptr<KObject> MakeThreadObject(uint32_t startAddr, uint32_t param, uint32_t xapi,
                                          uint32_t stackSize, bool suspended)
{
    auto o = NewObject(T_Thread);
    auto th = std::make_shared<KThread>();
    th->startAddr = startAddr; th->param = param; th->xapiStartup = xapi;
    th->stackSize = stackSize ? ((stackSize + 0xFFF) & ~0xFFFu) : 0x40000; // default 256 KiB
    th->stackBase = GuestAlloc(th->stackSize, 0x1000);
    th->pcr       = GuestAlloc(0x1000, 0x1000);
    th->tid       = g_nextTid.fetch_add(1);
    th->suspendCount = suspended ? 1 : 0;
    th->ctx = new PPCContext(); // aligned new honors alignas(0x40)
    o->thread = th;
    g_allThreads.push_back(o);
    return o;
}

} // namespace

// ---- thread identity for diagnostics ---------------------------------------
namespace { std::mutex g_nameMu; std::unordered_map<uint64_t, std::string> g_threadNames; }

uint64_t KernelCurrentTid() { return (t_self && t_self->thread) ? t_self->thread->tid : 0; }
std::string KernelCurrentThreadName()
{
    uint64_t tid = KernelCurrentTid();
    std::lock_guard<std::mutex> lk(g_nameMu);
    auto it = g_threadNames.find(tid);
    return it == g_threadNames.end() ? (tid == 1 ? "main" : "") : it->second;
}
std::string KernelThreadName(uint64_t tid)
{
    std::lock_guard<std::mutex> lk(g_nameMu);
    auto it = g_threadNames.find(tid);
    return it == g_threadNames.end() ? (tid == 1 ? "main" : "") : it->second;
}
void KernelSetThreadName(uint64_t tid, const std::string& name)
{
    std::lock_guard<std::mutex> lk(g_nameMu);
    g_threadNames[tid] = name;
}

// ---- HC09_TRACE_WAITS=1: where guest threads block ---------------------------
// Every 5 s, per guest thread: CPU time (host thread times), and the time spent
// blocked in kernel waits / delays / yields by call site (the guest's lr).
// Diagnostic for the frame-pacing study (which thread limits a frame).
namespace {
bool WaitTraceOn() { static const bool on = Hc09Env("HC09_TRACE_WAITS") != nullptr; return on; }
struct WaitStat { uint64_t ns = 0, calls = 0, timeouts = 0; int64_t timeout100ns = 0; };
std::mutex g_wpMu;
std::map<std::tuple<uint64_t, int, uint32_t>, WaitStat> g_waitStats;   // (tid, kind, lr)
std::map<uint64_t, void*> g_hostThreads;                                // tid -> host thread
const char* const kWaitKind[] = { "wait", "delay", "yield" };

void WaitReporter()
{
    using namespace std::chrono;
    std::map<uint64_t, double> lastCpu;
    auto t0 = steady_clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(seconds(5));
        auto t1 = steady_clock::now();
        double dt = duration<double>(t1 - t0).count();
        t0 = t1;
        std::map<std::tuple<uint64_t, int, uint32_t>, WaitStat> s;
        std::map<uint64_t, double> cpu;
        {
            std::lock_guard<std::mutex> l(g_wpMu);
            s.swap(g_waitStats);
            for (auto& [tid, h] : g_hostThreads) cpu[tid] = HostThreadCpuSeconds(h);
        }
        std::map<uint64_t, double> blocked;
        for (auto& [k, v] : s) blocked[std::get<0>(k)] += 1e-9 * double(v.ns);
        fprintf(stderr, "[waits] sw%llu %.1f s (ms per second of wall time):\n", (unsigned long long)GpuSwapCount(), dt);
        for (auto& [tid, c] : cpu)
        {
            std::string nm;
            { std::lock_guard<std::mutex> lk(g_nameMu); auto it = g_threadNames.find(tid); nm = it != g_threadNames.end() ? it->second : tid == 1 ? "main" : ""; }
            fprintf(stderr, "[waits]   t%-3llu %-24s cpu %6.1f  blocked %6.1f\n", (unsigned long long)tid, nm.c_str(),
                    1000.0 * (c - lastCpu[tid]) / dt, 1000.0 * blocked[tid] / dt);
            lastCpu[tid] = c;
        }
        std::vector<std::pair<uint64_t, std::tuple<uint64_t, int, uint32_t>>> top;
        for (auto& [k, v] : s) top.push_back({ v.ns, k });
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 24; ++i)
        {
            auto& k = top[i].second;
            const WaitStat& v = s[k];
            fprintf(stderr, "[waits]   t%-3llu %-5s lr=%08X %6.1f ms/s %7.1f calls/s %6.1f timeouts/s (last timeout %.2f ms)\n",
                    (unsigned long long)std::get<0>(k), kWaitKind[std::get<1>(k)], std::get<2>(k), 1e-6 * double(v.ns) / dt,
                    v.calls / dt, v.timeouts / dt, v.timeout100ns / 1e4);
        }
    }
}

// Times one blocking call; no-op unless HC09_TRACE_WAITS is set.
struct WaitProbe
{
    bool on;
    int kind;
    uint32_t lr;
    int64_t timeout100ns;
    std::chrono::steady_clock::time_point t0;
    WaitProbe(int k, PPCContext& ctx, uint32_t pTimeout) : on(WaitTraceOn()), kind(k), lr(uint32_t(ctx.lr)), timeout100ns(-1)
    {
        if (!on) return;
        if (pTimeout) { int64_t v = int64_t(GLoad64(pTimeout)); timeout100ns = v < 0 ? -v : v; }
        t0 = std::chrono::steady_clock::now();
    }
    void Done(bool timedOut)
    {
        if (!on) return;
        uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        uint64_t tid = KernelCurrentTid();
        static std::once_flag once;
        std::call_once(once, [] { std::thread(WaitReporter).detach(); });
        std::lock_guard<std::mutex> l(g_wpMu);
        if (!g_hostThreads.count(tid)) g_hostThreads[tid] = HostCurrentThreadHandle();
        WaitStat& w = g_waitStats[{ tid, kind, lr }];
        w.ns += ns; ++w.calls; w.timeouts += timedOut; w.timeout100ns = timeout100ns;
    }
};
} // namespace

// ---- string helpers -------------------------------------------------------

std::string ReadAnsiString(uint32_t pStr)
{
    if (!pStr) return {};
    uint16_t len = GLoad16(pStr);
    uint32_t buf = GLoad32(pStr + 4);
    if (!buf || len > 1024) return {};
    std::string out; out.reserve(len);
    for (uint16_t i = 0; i < len; ++i) out.push_back(char(GLoad8(buf + i)));
    return out;
}

std::string ReadObjectName(uint32_t pObjAttr, uint32_t* rootDirOut)
{
    if (!pObjAttr) return {};
    uint32_t root = GLoad32(pObjAttr + 0);
    uint32_t pName = GLoad32(pObjAttr + 4);
    if (rootDirOut) *rootDirOut = root;
    return ReadAnsiString(pName);
}

// ---- public helpers (runtime.h) ------------------------------------------

void KernelInitMainThread(PPCContext* mainCtx)
{
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = NewObject(T_Thread);
    auto th = std::make_shared<KThread>();
    th->tid = g_nextTid.fetch_add(1);
    th->pcr = GuestAlloc(0x1000, 0x1000);
    th->cpu = 0;
    o->thread = th;
    t_self = o;
    g_allThreads.push_back(o);
    ProfilerRegisterThread(th->tid);
    SetupPcr(th->pcr, o->guest, th->cpu, 0, 0);
    th->ctx = mainCtx;
    fprintf(stderr, "[kernel] main thread registered as tid %llu (obj 0x%08X, pcr 0x%08X)\n",
            (unsigned long long)th->tid, o->guest, th->pcr);
}

void SetupPcr(uint32_t pcr, uint32_t threadObj, uint8_t cpu, uint32_t stackBase, uint32_t stackSize)
{
    if (!pcr) return;
    GStore32(pcr + KPCR_STACK_BASE,  stackBase ? stackBase + stackSize : 0);
    GStore32(pcr + KPCR_STACK_LIMIT, stackBase);
    GStore32(pcr + KPCR_CUR_THREAD,  threadObj);
    GStore8 (pcr + KPCR_CUR_CPU,     cpu);
}

uint8_t CpuFromAffinityMask(uint32_t mask)
{
    mask &= 0x3F;
    if (!mask) return 0;
    uint8_t c = 0;
    while (mask >>= 1) ++c;
    return c;
}

uint32_t KernelRegisterHostThread(const char* name, uint8_t cpu, uint32_t stackBase, uint32_t stackSize, PPCContext* ctx)
{
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = NewObject(T_Thread);
    auto th = std::make_shared<KThread>();
    th->tid = g_nextTid.fetch_add(1);
    th->pcr = GuestAlloc(0x1000, 0x1000);
    th->cpu = cpu;
    th->stackBase = stackBase; th->stackSize = stackSize;
    th->ctx = ctx;
    o->thread = th;
    t_self = o;
    g_allThreads.push_back(o);
    SetupPcr(th->pcr, o->guest, cpu, stackBase, stackSize);
    KernelSetThreadName(th->tid, name);
    ProfilerRegisterThread(th->tid);
    fprintf(stderr, "[kernel] host thread \"%s\" registered as tid %llu (obj 0x%08X, cpu %u)\n",
            name, (unsigned long long)th->tid, o->guest, cpu);
    return th->pcr;
}

void KernelDumpAllThreads()
{
    std::vector<std::shared_ptr<KObject>> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        snapshot = g_allThreads;
    }
    fprintf(stderr, "[kernel] ---- %zu guest threads ----\n", snapshot.size());
    for (auto& o : snapshot)
    {
        auto& th = *o->thread;
        std::string nm = (th.tid == 1) ? "main" : "";
        { std::lock_guard<std::mutex> lk(g_nameMu); auto it = g_threadNames.find(th.tid); if (it != g_threadNames.end()) nm = it->second; }
        fprintf(stderr, "  tid %-3llu cpu %u %-28s %s", (unsigned long long)th.tid, th.cpu, nm.c_str(),
                th.finished ? "finished" : th.suspendCount.load() > 0 ? "suspended" : "running");
        if (th.ctx && !th.finished)
        {
            fprintf(stderr, "  lr=0x%08X r1=0x%08X\n", uint32_t(th.ctx->lr), uint32_t(th.ctx->r1.u32));
            DumpGuestBacktrace(*th.ctx, "        ");
        }
        else fputc('\n', stderr);
    }
}

void KernelWaitAllThreads()
{
    std::vector<std::shared_ptr<KObject>> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        snapshot = g_allThreads;
    }
    for (auto& o : snapshot)
        if (o->thread && o->thread->host.joinable() && o != t_self)
            o->thread->host.join();
}

// ---- threads ---------------------------------------------------------------

// NTSTATUS ExCreateThread(PHANDLE r3, DWORD stackSize r4, LPDWORD pThreadId r5,
//                         PVOID xapiThreadStartup r6, PVOID startAddr r7,
//                         PVOID param r8, DWORD flags r9)
PPC_FUNC(__imp__ExCreateThread)
{
    ImportLog("ExCreateThread");
    uint32_t pHandle = ctx.r3.u32, stackSize = ctx.r4.u32, pTid = ctx.r5.u32;
    uint32_t xapi = ctx.r6.u32, start = ctx.r7.u32, param = ctx.r8.u32, flags = ctx.r9.u32;
    bool suspended = (flags & 0x1) != 0;

    std::shared_ptr<KObject> o;
    uint32_t h;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        o = MakeThreadObject(start, param, xapi, stackSize, suspended);
        o->thread->cpu = CpuFromAffinityMask(flags >> 24);   // XThread: processor mask in bits 24..31
        h = NewHandle(o);
        if (pHandle) GStore32(pHandle, h);
        if (pTid)    GStore32(pTid, uint32_t(o->thread->tid));
        fprintf(stderr, "[kernel] ExCreateThread: entry=0x%08X param=0x%08X xapi=0x%08X stack=%u flags=0x%X -> handle 0x%X tid %llu%s\n",
                start, param, xapi, stackSize, flags, h, (unsigned long long)o->thread->tid,
                suspended ? " (suspended)" : "");
    }
    o->thread->host = std::thread(ThreadMain, o);
    ctx.r3.u64 = STATUS_SUCCESS;
}

PPC_FUNC(__imp__ExTerminateThread)
{
    ImportLog("ExTerminateThread");
    uint32_t code = ctx.r3.u32;
    if (t_self && t_self->thread && t_self->thread->host.joinable())
        throw ThreadExit{ code };
    // Main thread asked to terminate: keep the process alive for the workers.
    fprintf(stderr, "[kernel] main thread called ExTerminateThread(0x%X); parking\n", code);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(60));
}

static void ResumeByObj(const std::shared_ptr<KObject>& o, PPCContext& ctx)
{
    uint32_t prev = 0;
    if (o && o->thread)
    {
        prev = uint32_t(o->thread->suspendCount.load());
        if (prev > 0) o->thread->suspendCount--;
        g_dispCv.notify_all();
    }
    ctx.r3.u64 = prev;
}

PPC_FUNC(__imp__NtResumeThread)
{
    ImportLog("NtResumeThread");
    uint32_t pPrev = ctx.r4.u32;
    std::shared_ptr<KObject> o;
    { std::lock_guard<std::mutex> lk(g_dispMu); o = ByHandle(ctx.r3.u32); }
    ResumeByObj(o, ctx);
    if (pPrev) GStore32(pPrev, ctx.r3.u32);
    ctx.r3.u64 = o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

PPC_FUNC(__imp__KeResumeThread)
{
    ImportLog("KeResumeThread");
    std::shared_ptr<KObject> o;
    { std::lock_guard<std::mutex> lk(g_dispMu); o = ByGuest(ctx.r3.u32); }
    ResumeByObj(o, ctx); // returns previous suspend count
}

PPC_FUNC(__imp__NtSuspendThread)
{
    ImportLog("NtSuspendThread");
    std::shared_ptr<KObject> o;
    { std::lock_guard<std::mutex> lk(g_dispMu); o = ByHandle(ctx.r3.u32); }
    uint32_t prev = 0;
    if (o && o->thread) { prev = uint32_t(o->thread->suspendCount.load()); o->thread->suspendCount++; }
    if (ctx.r4.u32) GStore32(ctx.r4.u32, prev);
    ctx.r3.u64 = o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
    // NOTE: suspension of an already-running thread is not enforced yet.
}

// DWORD KeSetAffinityThread(PKTHREAD r3, DWORD affinity r4, PDWORD oldAffinity r5)
// Returns the previous affinity. We model one hardware thread per guest thread
// (the highest bit of the mask), stored in that thread's KPCR.current_cpu.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    ImportLog("KeSetAffinityThread");
    uint32_t mask = ctx.r4.u32;
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = ByGuest(ctx.r3.u32);
    uint32_t old = 1;
    if (o && o->thread)
    {
        auto& th = *o->thread;
        old = 1u << th.cpu;
        th.cpu = CpuFromAffinityMask(mask);
        if (th.pcr) GStore8(th.pcr + KPCR_CUR_CPU, th.cpu);
        fprintf(stderr, "[kernel] KeSetAffinityThread: tid %llu mask 0x%02X -> cpu %u\n",
                (unsigned long long)th.tid, mask, th.cpu);
    }
    else
        fprintf(stderr, "[kernel] KeSetAffinityThread: unknown thread object 0x%08X (mask 0x%02X)\n", ctx.r3.u32, mask);
    if (ctx.r5.u32) GStore32(ctx.r5.u32, old);
    ctx.r3.u64 = old;
}
PPC_FUNC(__imp__KeSetBasePriorityThread) { ImportLog("KeSetBasePriorityThread"); ctx.r3.u64 = 0; }

// VOID KeDelayExecutionThread(mode r3, alertable r4, PLARGE_INTEGER interval r5)
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    ImportLog("KeDelayExecutionThread");
    WaitProbe probe(1, ctx, ctx.r5.u32);
    int64_t v = ctx.r5.u32 ? int64_t(GLoad64(ctx.r5.u32)) : 0;
    int64_t rel100ns = v < 0 ? -v : 0;
    if (rel100ns <= 0) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::nanoseconds(rel100ns * 100));
    probe.Done(false);
    ctx.r3.u64 = STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtYieldExecution)
{
    ImportLog("NtYieldExecution");
    WaitProbe probe(2, ctx, 0);
    std::this_thread::yield();
    probe.Done(false);
    ctx.r3.u64 = 0;
}

// ---- TLS (per host thread) --------------------------------------------------

PPC_FUNC(__imp__KeTlsAlloc)
{
    ImportLog("KeTlsAlloc");
    uint32_t idx = g_tlsNext.fetch_add(1);
    ctx.r3.u64 = (idx < 256) ? idx : 0xFFFFFFFFu;
}
PPC_FUNC(__imp__KeTlsFree)     { ImportLog("KeTlsFree"); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__KeTlsSetValue) { ImportLog("KeTlsSetValue"); if (ctx.r3.u32 < 256) t_tls[ctx.r3.u32] = ctx.r4.u32; ctx.r3.u64 = 1; }
PPC_FUNC(__imp__KeTlsGetValue) { ImportLog("KeTlsGetValue"); ctx.r3.u64 = (ctx.r3.u32 < 256) ? t_tls[ctx.r3.u32] : 0; }

// ---- critical sections & spinlocks (real locks keyed by guest address) ----
//
// The guest structure is not used (zeroed at init, as before): each address gets a
// host lock on first use. The game's GameKernel loops poll their critical sections
// ~1.5 M times/s each (docs/REPORT_GAME_CPU.md), so the path is lock-free: a
// two-level table (64 Ki pages of 64 KiB of guest address space -> 16 Ki slots, one
// per 4-byte-aligned address) finds the lock without a global mutex (that is taken
// only to create a page or a lock), and the lock is a recursive owner word: one
// compare-exchange uncontended, a short spin, then std::atomic::wait.

namespace {
struct GuestLock
{
    std::atomic<uint32_t> owner{ 0 };   // ThreadKey() of the holder, 0 = free
    uint32_t recursion = 0;             // touched by the holder only
    std::atomic<uint32_t> waiters{ 0 };

    void lock()
    {
        uint32_t me = ThreadKey();
        if (owner.load(std::memory_order_relaxed) == me) { ++recursion; return; }
        uint32_t expected = 0;
        for (int spin = 0; !owner.compare_exchange_weak(expected, me, std::memory_order_acquire, std::memory_order_relaxed); ++spin)
        {
            if (spin < 64) { CpuRelax(); expected = 0; continue; }
            // seq_cst with unlock()'s store/load: neither side may miss the other (a lost wakeup).
            waiters.fetch_add(1, std::memory_order_seq_cst);
            if (expected) owner.wait(expected, std::memory_order_seq_cst);
            waiters.fetch_sub(1, std::memory_order_relaxed);
            expected = 0;
        }
        recursion = 1;
    }
    bool try_lock()
    {
        uint32_t me = ThreadKey();
        if (owner.load(std::memory_order_relaxed) == me) { ++recursion; return true; }
        uint32_t expected = 0;
        if (!owner.compare_exchange_strong(expected, me, std::memory_order_acquire, std::memory_order_relaxed)) return false;
        recursion = 1;
        return true;
    }
    // As std::recursive_mutex did (MSVC): no owner check; the count reaching 0 releases.
    void unlock()
    {
        if (--recursion) return;
        owner.store(0, std::memory_order_seq_cst);
        if (waiters.load(std::memory_order_seq_cst)) owner.notify_one();
    }
    static uint32_t ThreadKey()
    {
        static std::atomic<uint32_t> next{ 1 };
        thread_local uint32_t key = next.fetch_add(1, std::memory_order_relaxed);
        return key;
    }
    static void CpuRelax()
    {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#endif
    }
};

constexpr uint32_t kCsPageBits = 16;
std::atomic<std::atomic<GuestLock*>*> g_csPages[1u << (32 - kCsPageBits)];
std::mutex g_csMapMu;   // creating pages and locks only

GuestLock& CS(uint32_t p)
{
    std::atomic<GuestLock*>* page = g_csPages[p >> kCsPageBits].load(std::memory_order_acquire);
    if (page)
        if (GuestLock* l = page[(p & ((1u << kCsPageBits) - 1)) >> 2].load(std::memory_order_acquire)) return *l;
    std::lock_guard<std::mutex> lk(g_csMapMu);
    page = g_csPages[p >> kCsPageBits].load(std::memory_order_relaxed);
    if (!page)
    {
        page = new std::atomic<GuestLock*>[1u << (kCsPageBits - 2)]();
        g_csPages[p >> kCsPageBits].store(page, std::memory_order_release);
    }
    std::atomic<GuestLock*>& slot = page[(p & ((1u << kCsPageBits) - 1)) >> 2];
    GuestLock* l = slot.load(std::memory_order_relaxed);
    if (!l)
    {
        l = new GuestLock;
        slot.store(l, std::memory_order_release);
    }
    return *l;
}
}

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    ImportLog("RtlInitializeCriticalSection");
    uint32_t p = ctx.r3.u32;
    if (p) for (int i = 0; i < 28; i += 4) GStore32(p + i, 0);
    (void)CS(p);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    ImportLog("RtlInitializeCriticalSectionAndSpinCount");
    uint32_t p = ctx.r3.u32;
    if (p) for (int i = 0; i < 28; i += 4) GStore32(p + i, 0);
    (void)CS(p);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlEnterCriticalSection)    { ImportLog("RtlEnterCriticalSection");    CS(ctx.r3.u32).lock();   ctx.r3.u64 = 0; }
PPC_FUNC(__imp__RtlLeaveCriticalSection)    { ImportLog("RtlLeaveCriticalSection");    CS(ctx.r3.u32).unlock(); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__RtlTryEnterCriticalSection) { ImportLog("RtlTryEnterCriticalSection"); ctx.r3.u64 = CS(ctx.r3.u32).try_lock() ? 1 : 0; }

// Spinlocks: KSPIN_LOCK is a pointer-sized word; treat like a (non-recursive
// in spirit) critical section keyed by its address. IRQL is cosmetic here.
PPC_FUNC(__imp__KfAcquireSpinLock)                 { ImportLog("KfAcquireSpinLock");                 CS(ctx.r3.u32).lock();   ctx.r3.u64 = 0; /* old irql */ }
PPC_FUNC(__imp__KfReleaseSpinLock)                 { ImportLog("KfReleaseSpinLock");                 CS(ctx.r3.u32).unlock(); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)     { ImportLog("KeAcquireSpinLockAtRaisedIrql");     CS(ctx.r3.u32).lock();   ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)   { ImportLog("KeReleaseSpinLockFromRaisedIrql");   CS(ctx.r3.u32).unlock(); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)             { ImportLog("KeRaiseIrqlToDpcLevel");             ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KfLowerIrql)                       { ImportLog("KfLowerIrql");                       ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeEnterCriticalRegion)             { ImportLog("KeEnterCriticalRegion");             ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeLeaveCriticalRegion)             { ImportLog("KeLeaveCriticalRegion");             ctx.r3.u64 = 0; }

// ---- events -----------------------------------------------------------------

// NTSTATUS NtCreateEvent(PHANDLE r3, POBJECT_ATTRIBUTES r4, EVENT_TYPE r5 (0=notification,1=sync), BOOLEAN initial r6)
PPC_FUNC(__imp__NtCreateEvent)
{
    ImportLog("NtCreateEvent");
    std::lock_guard<std::mutex> lk(g_dispMu);
    bool manual = (ctx.r5.u32 == 0);
    auto o = NewObject(manual ? T_NotificationEvent : T_SynchronizationEvent);
    o->manualReset = manual; o->signaled = ctx.r6.u32 != 0; MirrorHeader(o);
    uint32_t h = NewHandle(o);
    if (ctx.r3.u32) GStore32(ctx.r3.u32, h);
    ctx.r3.u64 = STATUS_SUCCESS;
}

uint32_t SetEventObj(const std::shared_ptr<KObject>& o)
{
    if (!o) return 0;
    uint32_t prev;
    { std::lock_guard<std::mutex> lk(g_dispMu); prev = o->signaled; o->signaled = true; MirrorHeader(o); }
    g_dispCv.notify_all();
    return prev;
}
uint32_t ResetEventObj(const std::shared_ptr<KObject>& o)
{
    if (!o) return 0;
    std::lock_guard<std::mutex> lk(g_dispMu);
    uint32_t prev = o->signaled; o->signaled = false; MirrorHeader(o);
    return prev;
}

PPC_FUNC(__imp__NtSetEvent)
{
    ImportLog("NtSetEvent");
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByHandle(ctx.r3.u32); }
    uint32_t prev = SetEventObj(o);
    if (ctx.r4.u32) GStore32(ctx.r4.u32, prev);
    ctx.r3.u64 = o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}
PPC_FUNC(__imp__NtClearEvent)
{
    ImportLog("NtClearEvent");
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByHandle(ctx.r3.u32); }
    ResetEventObj(o);
    ctx.r3.u64 = o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}
// LONG KeSetEvent(PKEVENT r3, increment r4, wait r5) -> previous state
PPC_FUNC(__imp__KeSetEvent)
{
    ImportLog("KeSetEvent");
    // HC09_TRACE_EVENT=<hex guest address>: report who signals that event
    // (first 12 times, with a backtrace).
    static const uint32_t traced = Hc09Env("HC09_TRACE_EVENT") ? uint32_t(strtoul(Hc09Env("HC09_TRACE_EVENT"), nullptr, 16)) : 0;
    if (traced && ctx.r3.u32 == traced)
    {
        static std::atomic<int> n{ 0 };
        if (n.fetch_add(1) < 12)
        {
            fprintf(stderr, "[event] KeSetEvent(0x%08X) by tid %llu \"%s\" from 0x%08X\n", traced,
                    (unsigned long long)KernelCurrentTid(), KernelCurrentThreadName().c_str(), uint32_t(ctx.lr));
            DumpGuestBacktrace(ctx, "          ");
        }
    }
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByGuest(ctx.r3.u32); }
    ctx.r3.u64 = SetEventObj(o);
}
PPC_FUNC(__imp__KeResetEvent)
{
    ImportLog("KeResetEvent");
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByGuest(ctx.r3.u32); }
    ctx.r3.u64 = ResetEventObj(o);
}

// ---- semaphores ------------------------------------------------------------

// NTSTATUS NtCreateSemaphore(PHANDLE r3, POBJATTR r4, LONG initial r5, LONG max r6)
PPC_FUNC(__imp__NtCreateSemaphore)
{
    ImportLog("NtCreateSemaphore");
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = NewObject(T_Semaphore);
    o->count = int32_t(ctx.r5.u32); o->limit = int32_t(ctx.r6.u32); if (o->limit <= 0) o->limit = 0x7FFFFFFF;
    MirrorHeader(o);
    uint32_t h = NewHandle(o);
    if (ctx.r3.u32) GStore32(ctx.r3.u32, h);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// VOID KeInitializeSemaphore(PKSEMAPHORE r3, LONG count r4, LONG limit r5)
PPC_FUNC(__imp__KeInitializeSemaphore)
{
    ImportLog("KeInitializeSemaphore");
    std::lock_guard<std::mutex> lk(g_dispMu);
    uint32_t p = ctx.r3.u32;
    auto it = g_byGuest.find(p);
    auto o = (it != g_byGuest.end()) ? it->second : NewObject(T_Semaphore, p);
    o->type = T_Semaphore; o->count = int32_t(ctx.r4.u32); o->limit = int32_t(ctx.r5.u32);
    if (o->limit <= 0) o->limit = 0x7FFFFFFF;
    GStore32(p + 16, uint32_t(o->limit));
    MirrorHeader(o);
}
static int32_t ReleaseSemObj(const std::shared_ptr<KObject>& o, int32_t by)
{
    if (!o) return 0;
    int32_t prev;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        prev = o->count;
        int64_t n = int64_t(o->count) + by;
        o->count = int32_t(n > o->limit ? o->limit : n);
        MirrorHeader(o);
    }
    g_dispCv.notify_all();
    return prev;
}
// NTSTATUS NtReleaseSemaphore(HANDLE r3, LONG count r4, PLONG prev r5)
PPC_FUNC(__imp__NtReleaseSemaphore)
{
    ImportLog("NtReleaseSemaphore");
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByHandle(ctx.r3.u32); }
    int32_t prev = ReleaseSemObj(o, int32_t(ctx.r4.u32));
    if (ctx.r5.u32) GStore32(ctx.r5.u32, uint32_t(prev));
    ctx.r3.u64 = o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}
// LONG KeReleaseSemaphore(PKSEMAPHORE r3, increment r4, adjustment r5, wait r6)
PPC_FUNC(__imp__KeReleaseSemaphore)
{
    ImportLog("KeReleaseSemaphore");
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByGuest(ctx.r3.u32); }
    ctx.r3.u64 = uint32_t(ReleaseSemObj(o, int32_t(ctx.r5.u32)));
}

// ---- mutants ---------------------------------------------------------------

// NTSTATUS NtCreateMutant(PHANDLE r3, POBJATTR r4, BOOLEAN initialOwner r5)
PPC_FUNC(__imp__NtCreateMutant)
{
    ImportLog("NtCreateMutant");
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = NewObject(T_Mutant);
    if (ctx.r5.u32 && t_self) { o->ownerTid = t_self->thread->tid; o->recursion = 1; }
    MirrorHeader(o);
    uint32_t h = NewHandle(o);
    if (ctx.r3.u32) GStore32(ctx.r3.u32, h);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NTSTATUS NtReleaseMutant(HANDLE r3, PLONG prevCount r4)
PPC_FUNC(__imp__NtReleaseMutant)
{
    ImportLog("NtReleaseMutant");
    std::shared_ptr<KObject> o;
    int32_t prev = 0;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        o = ByHandle(ctx.r3.u32);
        if (o)
        {
            prev = o->recursion;
            if (o->recursion > 0 && --o->recursion == 0) o->ownerTid = 0;
            MirrorHeader(o);
        }
    }
    g_dispCv.notify_all();
    if (ctx.r4.u32) GStore32(ctx.r4.u32, uint32_t(prev));
    ctx.r3.u64 = o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

// ---- timers (minimal: never fire yet; logged) -------------------------------

PPC_FUNC(__imp__NtCreateTimer)
{
    ImportLog("NtCreateTimer");
    std::lock_guard<std::mutex> lk(g_dispMu);
    bool manual = (ctx.r5.u32 == 0);
    auto o = NewObject(manual ? T_NotificationTimer : T_SynchronizationTimer);
    o->manualReset = manual;
    uint32_t h = NewHandle(o);
    if (ctx.r3.u32) GStore32(ctx.r3.u32, h);
    ctx.r3.u64 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__NtSetTimerEx)  { ImportLog("NtSetTimerEx");  ctx.r3.u64 = STATUS_SUCCESS; }
PPC_FUNC(__imp__NtCancelTimer) { ImportLog("NtCancelTimer"); if (ctx.r4.u32) GStore32(ctx.r4.u32, 0); ctx.r3.u64 = STATUS_SUCCESS; }

// ---- waits -----------------------------------------------------------------

// NTSTATUS NtWaitForSingleObjectEx(HANDLE r3, mode r4, alertable r5, PLARGE_INTEGER timeout r6)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    ImportLog("NtWaitForSingleObjectEx");
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByHandle(ctx.r3.u32); }
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::vector<std::shared_ptr<KObject>> v{ o };
    WaitProbe probe(0, ctx, ctx.r6.u32);
    ctx.r3.u64 = WaitObjects(v, false, ctx.r6.u32);
    probe.Done(ctx.r3.u32 == STATUS_TIMEOUT);
}
// NTSTATUS KeWaitForSingleObject(PVOID obj r3, reason r4, mode r5, alertable r6, PLARGE_INTEGER timeout r7)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    ImportLog("KeWaitForSingleObject");
    std::shared_ptr<KObject> o; { std::lock_guard<std::mutex> lk(g_dispMu); o = ByGuest(ctx.r3.u32); }
    if (!o) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    std::vector<std::shared_ptr<KObject>> v{ o };
    WaitProbe probe(0, ctx, ctx.r7.u32);
    ctx.r3.u64 = WaitObjects(v, false, ctx.r7.u32);
    probe.Done(ctx.r3.u32 == STATUS_TIMEOUT);
}
// NTSTATUS KeWaitForMultipleObjects(count r3, PVOID* objs r4, waitType r5 (0=all,1=any),
//                                   reason r6, mode r7, alertable r8, timeout r9, waitBlocks r10)
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    ImportLog("KeWaitForMultipleObjects");
    uint32_t n = ctx.r3.u32, arr = ctx.r4.u32;
    std::vector<std::shared_ptr<KObject>> v;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        for (uint32_t i = 0; i < n && i < 64; ++i)
        {
            auto o = ByGuest(GLoad32(arr + i * 4));
            if (o) v.push_back(o);
        }
    }
    if (v.empty()) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    WaitProbe probe(0, ctx, ctx.r9.u32);
    ctx.r3.u64 = WaitObjects(v, ctx.r5.u32 == 0, ctx.r9.u32);
    probe.Done(ctx.r3.u32 == STATUS_TIMEOUT);
}

// ---- handles & object references --------------------------------------------

PPC_FUNC(__imp__NtClose)
{
    ImportLog("NtClose");
    std::shared_ptr<KFile> file;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        auto it = g_byHandle.find(ctx.r3.u32);
        if (it == g_byHandle.end()) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
        if (it->second && it->second->type == T_File) file = it->second->file;
        g_byHandle.erase(it);
    }
    FileOnClose(file);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NTSTATUS NtDuplicateObject(HANDLE src r3, PHANDLE out r4, ULONG options r5)
PPC_FUNC(__imp__NtDuplicateObject)
{
    ImportLog("NtDuplicateObject");
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = ByHandle(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t h = NewHandle(o);
    if (ctx.r4.u32) GStore32(ctx.r4.u32, h);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NTSTATUS ObReferenceObjectByHandle(HANDLE r3, POBJECT_TYPE r4, PVOID* out r5)
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    ImportLog("ObReferenceObjectByHandle");
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = ByHandle(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    o->refs++;
    if (ctx.r5.u32) GStore32(ctx.r5.u32, o->guest);
    ctx.r3.u64 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__ObReferenceObject)
{
    ImportLog("ObReferenceObject");
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = ByGuest(ctx.r3.u32);
    if (o) o->refs++;
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__ObDereferenceObject)
{
    ImportLog("ObDereferenceObject");
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto it = g_byGuest.find(ctx.r3.u32);
    if (it != g_byGuest.end()) it->second->refs--;
    ctx.r3.u64 = 0;
}

// ---- APCs (not delivered yet; acknowledged) ---------------------------------

PPC_FUNC(__imp__KeInitializeApc)    { ImportLog("KeInitializeApc");    ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeInsertQueueApc)   { ImportLog("KeInsertQueueApc");   ctx.r3.u64 = 1; }
PPC_FUNC(__imp__NtQueueApcThread)   { ImportLog("NtQueueApcThread");   ctx.r3.u64 = STATUS_SUCCESS; }
PPC_FUNC(__imp__KiApcNormalRoutineNop) { ImportLog("KiApcNormalRoutineNop"); ctx.r3.u64 = 0; }

// ---- exceptions ---------------------------------------------------------------
// XenonRecomp does not model SEH unwinding, so a raise simply returns to the
// caller. For the thread-naming exception (0x406D1388) that is exactly the
// handler's behavior anyway. Anything else is logged loudly.
PPC_FUNC(__imp__RtlRaiseException)
{
    ImportLog("RtlRaiseException");
    uint32_t rec = ctx.r3.u32;
    uint32_t code = rec ? GLoad32(rec) : 0;
    if (code == 0x406D1388)
    {
        // THREADNAME_INFO is in ExceptionInformation[1..]: {type, name ptr, tid, flags}
        uint32_t namePtr = rec ? GLoad32(rec + 0x14 + 4) : 0;
        std::string name;
        for (int i = 0; namePtr && i < 64; ++i) { char c = char(GLoad8(namePtr + i)); if (!c) break; name.push_back(c); }
        // THREADNAME_INFO.threadId (ExceptionInformation[2]); -1 = calling thread.
        uint32_t tidField = rec ? GLoad32(rec + 0x14 + 8) : 0xFFFFFFFFu;
        uint64_t tid = (tidField == 0xFFFFFFFFu) ? KernelCurrentTid() : tidField;
        if (!name.empty()) KernelSetThreadName(tid, name);
        fprintf(stderr, "[kernel] thread name: \"%s\" (tid %llu)\n", name.c_str(), (unsigned long long)tid);
    }
    else
        fprintf(stderr, "[kernel] *** RtlRaiseException code=0x%08X (unwinding unsupported; continuing) ***\n", code);
    ctx.r3.u64 = 0;
}
