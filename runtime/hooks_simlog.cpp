// hc09-recomp: SuperSim play log (diagnostic, off by default).
//
//   HC09_SIM_LOG=<file>   append one line per simulated play to <file>
//
// For measuring how simmed games play (yards per carry, QB scrambles, plays per game, ...) without
// the game's own UI. Layouts (from reverse engineering the game's simulation):
// - sub_82972F60(mgr) runs one play: Setup, PreSnap, Resolve. The play state (SimPlay) is *(mgr+12);
//   after the call its events are at +1648 + 48*i (count at +1644), type at +0 of each event.
// - Player pointers in the play point at 56-byte sim records (+4 = PGID). They live in the sim data
//   loaded by sub_826C0108 (hooks_autowin.cpp records its address): side s's records start at
//   sim + 28 + s*4508, its team id (TGID) is at sim + 4 + s*4508 + 4.
// Line format:
//   G <game> sim=<addr> t0=<TGID side 0> t1=<TGID side 1>
//   P <game> type kind down los fd yds qb carrier off | ev ev ... (each event: code:word1,..,word11
//     with player pointers replaced by P<pgid>s<side>)
#include "dev_env.h"
#include "runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern std::atomic<uint32_t> g_simLoadedData;
extern std::atomic<uint32_t> g_simGameSerial;
extern "C" PPC_FUNC(__imp__sub_82972F60);

namespace {
constexpr uint32_t kSideStride = 4508, kRecords = 28, kRecordSize = 56, kMaxRecords = 80;

FILE* Log()
{
    static FILE* f = [] {
        const char* p = Hc09Env("HC09_SIM_LOG");
        FILE* h = p && *p ? fopen(p, "a") : nullptr;
        if (h) fprintf(stderr, "[simlog] SuperSim plays -> %s\n", p);
        return h;
    }();
    return f;
}

float F(uint32_t ea) { uint32_t u = GLoad32(ea); float f; memcpy(&f, &u, 4); return f; }

// "P<pgid>s<side>" if v points at a player record of the current sim, else hex.
void Word(char* out, size_t n, uint32_t v, uint32_t sim)
{
    for (uint32_t s = 0; s < 2 && sim; ++s) {
        const uint32_t first = sim + kRecords + s * kSideStride;
        if (v >= first && v < first + kMaxRecords * kRecordSize && (v - first) % kRecordSize == 0) {
            snprintf(out, n, "P%us%u", GLoad32(v + 4), s);
            return;
        }
    }
    snprintf(out, n, "%X", v);
}

uint32_t Pgid(uint32_t v) { return v ? GLoad32(v + 4) : 0; }
int Side(uint32_t v, uint32_t sim)
{
    for (uint32_t s = 0; s < 2 && sim; ++s) {
        const uint32_t first = sim + kRecords + s * kSideStride;
        if (v >= first && v < first + kMaxRecords * kRecordSize) return int(s);
    }
    return -1;
}

std::mutex g_mu;
uint32_t g_lastGame = 0, g_lastHash = 0;
}  // namespace

PPC_FUNC(sub_82972F60)
{
    const uint32_t mgr = ctx.r3.u32;
    __imp__sub_82972F60(ctx, base);
    FILE* f = Log();
    if (!f) return;
    const uint32_t p = GLoad32(mgr + 12);
    if (!p) return;
    const uint32_t n = GLoad32(p + 1644);
    if (n == 0 || n > 64) return;
    const uint32_t sim = g_simLoadedData.load(), game = g_simGameSerial.load();

    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t h = 2166136261u;   // skip a call that left the same events (PreSnap-only steps)
    for (uint32_t i = 0; i < n * 12; ++i) h = (h ^ GLoad32(p + 1648 + i * 4)) * 16777619u;
    h ^= n;
    if (game == g_lastGame && h == g_lastHash) return;
    if (game != g_lastGame)
        fprintf(f, "G %u sim=%08X t0=%u t1=%u\n", game, sim,
                sim ? GLoad32(sim + 4 + 4) : 0, sim ? GLoad32(sim + 4 + kSideStride + 4) : 0);
    g_lastGame = game; g_lastHash = h;

    const uint32_t qb = GLoad32(p + 1296), carrier = GLoad32(p + 1300);
    fprintf(f, "P %u %d %d %d %.2f %.2f %.2f %u %u %d |", game, int(GLoad32(p + 4)), int(GLoad32(p + 8)),
            int(GLoad32(p + 1332)), F(p + 1340), F(p + 1344), F(p + 1384), Pgid(qb), Pgid(carrier), Side(qb, sim));
    char w[32];
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = p + 1648 + i * 48;
        fprintf(f, " %d:", int(GLoad32(e)));
        for (uint32_t k = 1; k < 12; ++k) {
            Word(w, sizeof w, GLoad32(e + k * 4), sim);
            fprintf(f, k == 1 ? "%s" : ",%s", w);
        }
    }
    fputc('\n', f);
    fflush(f);
}
