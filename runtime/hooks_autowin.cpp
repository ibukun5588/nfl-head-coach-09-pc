// hc09-recomp: optional "auto-win simmed games" tilt (a request), off by default.
//
//   HC09_AUTOWIN_SIM=<0..100>   (environment or hc09.ini [env]); 0 / unset = the game as shipped.
//
// How the game sims a whole game (HC09_TRACE_SIM, tools/gen_sim_hooks.py; PROGRESS 2026-10-06):
// - sub_826C0108(r3, side r4, team id r5, sim r6) loads one side of the simmed game into the
//   sim data: side header at sim + 4 + side*4508 (+0 side, +4 TGID), then up to 80 player
//   records of 56 bytes from sim + 28 + side*4508 (+4 player id, +8 39 "compound ability"
//   bytes, +48 float), count at +4504 of the header.
// - sub_82968260(record r3, index r4, value r5, lo r6, hi r7) computes ability byte `index` of
//   a player record: clamp(value, lo, hi) * 255 / hi, at least 1, stored at record + 8 + index
//   and returned. It runs when the sides load and again during the sim (per-play updates).
// The tilt: the user's team is the game's own answer to
//   select 'TGID' into ? from 'STTM' where 'CFUC' = ?   (CFUC = coach/team is user controlled)
// asked once per simmed game; every ability byte of a player on the user's side is moved
// toward 255 by the strength: b + (255 - b) * strength / 100. The plays then really go the
// user's way, so the box score matches the result. Games without the user's team (the other
// games of a simmed week) and practice (the user's team on both sides) are untouched.
#include "dev_env.h"
#include "runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_826C0108);
extern "C" PPC_FUNC(__imp__sub_82968260);
extern "C" PPC_FUNC(__imp__sub_82C66928);   // the game's database query (r3 status, r6 query, r7.. arguments)

namespace {
constexpr uint32_t kQueryUserTeam = 0x820362B8;   // "select 'TGID' into ? from 'STTM' where 'CFUC' = ?"
constexpr uint32_t kSideStride = 4508, kRecordsOffset = 28, kRecordBytes = 4480;

int Strength()
{
    static const int s = [] {
        const char* e = Hc09Env("HC09_AUTOWIN_SIM");
        int v = e ? atoi(e) : 0;
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        if (v) fprintf(stderr, "[autowin] simmed games tilted toward the user's team, strength %d\n", v);
        return v;
    }();
    return s;
}

std::atomic<uint32_t> g_sim{0};          // sim data of the game being simmed
std::atomic<int> g_userSide{-1};         // the user's side in it, -1 = none, -2 = both (practice)
std::atomic<uint32_t> g_userTeam{0xFFFFFFFFu};

uint32_t QueryUserTeam(const PPCContext& ctx, uint8_t* base)
{
    PPCContext q = ctx;
    q.r1.u64 = ctx.r1.u64 - 0x400;              // a frame below the hooked function's own
    const uint32_t out = q.r1.u32 + 0x200;      // inside the reserved space, above the callee's frame
    GStore32(out, 0xFFFFFFFFu);
    q.r3.u64 = 0; q.r4.u64 = 0; q.r5.u64 = 0;
    q.r6.u64 = kQueryUserTeam;
    q.r7.u64 = out;
    q.r8.u64 = 1;
    __imp__sub_82C66928(q, base);
    return GLoad32(out);
}
}  // namespace

// Last loaded sim data and a count of side-0 loads (one per simmed game); read by hooks_simlog.cpp.
std::atomic<uint32_t> g_simLoadedData{0};
std::atomic<uint32_t> g_simGameSerial{0};

// Side loader: learn the user's team at side 0 of each simmed game, then which side it is.
PPC_FUNC(sub_826C0108)
{
    g_simLoadedData = ctx.r6.u32;
    if (ctx.r4.u32 == 0) g_simGameSerial.fetch_add(1);
    if (Strength()) {
        const int side = int(ctx.r4.u32);
        const uint32_t team = ctx.r5.u32, sim = ctx.r6.u32;
        if (side == 0 || g_sim.load() != sim) {
            g_sim = sim;
            g_userSide = -1;
            g_userTeam = QueryUserTeam(ctx, base);
        }
        if (team == g_userTeam.load() && (side == 0 || side == 1)) {
            if (g_userSide.load() == (side ^ 1) || g_userSide.load() == -2) {
                // Both sides are the user's team: practice (FEPracticeSimPlayCallState sims the
                // user's offense against the user's defense). Tilt neither.
                g_userSide = -2;
                fprintf(stderr, "[autowin] sim 0x%08X: user team %u on both sides (practice): no tilt\n", sim, team);
            } else {
                g_userSide = side;
                fprintf(stderr, "[autowin] sim 0x%08X: user team %u is side %d\n", sim, team, side);
            }
        }
    }
    __imp__sub_826C0108(ctx, base);
}

// Ability byte: tilt the user's side.
PPC_FUNC(sub_82968260)
{
    const uint32_t rec = ctx.r3.u32, index = ctx.r4.u32;
    __imp__sub_82968260(ctx, base);
    const int s = Strength(), side = g_userSide.load();
    if (!s || side < 0 || index >= 39) return;
    const uint32_t first = g_sim.load() + kRecordsOffset + uint32_t(side) * kSideStride;
    if (rec < first || rec >= first + kRecordBytes) return;
    const uint32_t b = ctx.r3.u32 & 0xFF;
    const uint32_t nb = b + (255 - b) * uint32_t(s) / 100;
    GStore8(rec + 8 + index, uint8_t(nb));
    ctx.r3.u64 = nb;
}
