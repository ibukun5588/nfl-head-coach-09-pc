// hc09-recomp: on-field gameplay diagnostics (all off by default).
//
//   HC09_BLOCK_LOG=<file>        append per-play blocking data of on-field (3D) plays to <file>
//   HC09_BLOCK_AUTORESUME=<s>    press A when the blocking manager has not run for s seconds (halftime pause)
//   HC09_SLIDERS=u<k>:<v>,...    override on-field slider bytes (u = User set, c = CPU set)
//   HC09_SKILL=<0-3>             force the game skill level of both teams at every snap (0 Rookie .. 3 All-Madden)
//   HC09_TWEAK=1|<port>          live tweak / debug server on http://127.0.0.1:<port>/ (1 = 8099)
//   HC09_TWEAK_DIR=<dir>         files it serves: index.html (the page), tweaks.tsv (named values)
//
// Layouts (from reverse engineering the game):
// - BlockingManager::Update sub_82529DD0(mgr) runs every simulation tick; mgr+36 = block clock (s, restarts at
//   the snap). Gameplay state = *(*(0x830A8A2C)+32) (3/4 = live). Players: *(0x8313B600) -> 22 pointers.
// - Player: +144 pos (x, y; y is downfield), +160 facing (rad), +300 top speed, +304 velocity (x, y), +496
//   controller (+4 -> assignment list, +12 -> current assignment: +16 type id, vtable slot 9 = name), +502 team,
//   +644 ratings component (getter for rating i at vtable +16+12i returns {s16 value +4, min +6, max +8}),
//   +648 height, +652 weight (lb), +756 position id, +988 PlayerBlockInfo (+24 state, +28 partner,
//   +48 contest: +0 drive, +16 power, +20 quickness).
// - Ball holder = sub_8272D3A8(*(0x8313AAF4)); offense team = GameRuleMgr(*(0x830A8610))+28 vfunc 116;
//   pass play = sub_8256A5E8(*(0x830A8FC0)); PlayInfo+184 = ball x.
// - sub_82526068(mgr, blk, def, ...) resolves the strength contest; sub_82401CE0(r, b, rbs, bbs, dir r7, user,
//   style r9) is the rush-move roll (r3 = success); sub_824FADA0(mgr, blk, def) starts a shed (r3 = started);
//   sub_824DA310(mgr, player r4, ...) flags a penalty (the caller identifies the check).
// Log lines (t = block clock s, players are indexes 0..21):
//   P <play> off=<team> pass=<0|1> spot=<PlayInfo+184>,<+188> gs=<state>  a new play (block clock restarts at the snap)
//   R <play> <i> <team> <pos> <wt> STR AGI SPD AWR PBK RBK IMP RBS RBF PBS PBF PWM FNM BKS TAK   (0-255)
//   L <play> <i> ACC CTH JMP PUR PRC MCOV ZCOV PRS RLS CIT SPC RTE                       (0-255)
//   T <play> <t> <i> <from> <to> <partner>                               block state change
//   A <play> <t> <i> <from type> <to type> <name>                         assignment change
//   C <play> <t> <blk> <def> <Pb> <Qb> <Pd> <Qd> <driveB> <driveD>        strength contest
//   M <play> <t> <rusher> <blocker> <dir> <style> <ok>                    rush-move roll
//   S <play> <t> <blk> <def> <ok>                                         shed start
//   Q <play> <t> <holder> <x> <y> <nearestDefDist>                        every 3 ticks
//   X <play> <t> <x,y of players 0..21>                                    every 3 ticks
//   F <play> <t> <player> <r5> <r6> lr=<caller>                            penalty flag
//   G <play> <t> <from> <to>                                              gameplay state change
//   V <play> <t> <receiver> <status>                                      receiver status change (RecOpen: 0 OPEN, 1 PARTIAL, 2 COVERED)
//
// Tweak server API (HTTP, localhost only):
//   GET  /api/state?since=<seq>   {"snap": live state of all players, "events": log lines after seq, "seq": n}
//   GET  /api/roster              the 45 rating getters, weight, height, team, position of each player (at the snap)
//   GET  /api/tweaks              tweaks.tsv rows with their current values
//   POST /api/cmd                 one command per line, applied at once (rating holds are re-written every tick):
//        poke <addr hex> <f32|u8|s16|u16|u32> <value>   write guest memory
//        rating <player> <idx> <value|-1>               hold a player's effective rating (-1 releases it)
//        slider <u|c> <k> <value>                       on-field slider byte k of the User / CPU set
//        speed <k>                                      game speed (0.05 .. 8; 1 = normal)
//   GET  /<file>                  static file from HC09_TWEAK_DIR
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "dev_env.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "runtime.h"
#include "overlay.h"
#include "overlay_font.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

bool InputInjectPress(const char* button);   // imports_input.cpp
void SetTimeWarp(double k);                   // hooks_pacing.cpp
double TimeWarp();

extern "C" PPC_FUNC(__imp__sub_82529DD0);
extern "C" PPC_FUNC(__imp__sub_82526068);
extern "C" PPC_FUNC(__imp__sub_82401CE0);
extern "C" PPC_FUNC(__imp__sub_824FADA0);
extern "C" PPC_FUNC(__imp__sub_824DA310);
extern "C" PPC_FUNC(__imp__sub_824B10D0);

namespace {
float F(uint32_t ea) { uint32_t u = GLoad32(ea); float f; memcpy(&f, &u, 4); return f; }
void SetF(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); GStore32(ea, u); }

constexpr uint32_t kPlayerMgr = 0x8313B600, kRuleMgr = 0x830A8610, kPlayInfo = 0x830A8FC0,
                   kStateMgr = 0x830A8A2C, kBallSys = 0x8313AAF4, kBlockMgr = 0x830A84E4, kCustomAI = 0x830A85E0,
                   kRecOpen = 0x830A85B8;
const int kLogRatings[] = {0, 1, 2, 4, 12, 13, 22, 23, 24, 25, 26, 27, 28, 29, 11};
// coverage / receiver ratings (L lines): ACC CTH JMP PUR PRC MCOV ZCOV PRS RLS CIT SPC RTE (getter order, 38-42)
const int kCovRatings[] = {3, 5, 14, 30, 31, 32, 33, 38, 39, 40, 41, 42};
constexpr int kNumRatings = 45;   // getters 0..44 (44 = the carrier's break-tackle value); 45+ are not ratings

// ---- output: the log file and/or the live server's event ring ------------------------------------------
FILE* LogFile()
{
    static FILE* f = [] {
        const char* p = Hc09Env("HC09_BLOCK_LOG");
        FILE* h = p && *p ? fopen(p, "a") : nullptr;
        if (h) fprintf(stderr, "[blocklog] on-field blocking -> %s\n", p);
        return h;
    }();
    return f;
}
int TweakPort()
{
    static const int port = [] {
        const char* p = Hc09Env("HC09_TWEAK");
        if (!p || !*p || strcmp(p, "0") == 0) return 0;
        const int v = atoi(p);
        return v > 1 ? v : 8099;
    }();
    return port;
}
bool On() { return LogFile() || TweakPort(); }

struct Live {
    std::mutex mu;
    std::deque<std::pair<uint64_t, std::string>> events;   // (seq, line), the last kMaxEvents
    uint64_t seq = 0;
    std::string snap = "{}", roster = "[]";
    std::map<std::pair<int, int>, int> ratingHold;          // (player, rating idx) -> value
} g_live;
constexpr size_t kMaxEvents = 20000;

// One log line: to the file (if any) and to the live ring (if the server runs). Lines with
// live=false (R, Q, X: bulky, the server has them in its snapshot/roster) go to the file only.
void Emit(bool live, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (FILE* f = LogFile()) fputs(buf, f);
    if (live && TweakPort()) {
        std::lock_guard<std::mutex> lk(g_live.mu);
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
        g_live.events.emplace_back(++g_live.seq, buf);
        if (g_live.events.size() > kMaxEvents) g_live.events.pop_front();
    }
}

// ---- knobs: behaviour changes done by hooks (set with "knob <name> <value>") --------------------------------
struct Knob { const char* name; float def; std::atomic<float> v; };
Knob g_knobs[] = {
    {"throw_dist", 1.0f, {1.0f}},    // x the throw distance: aim point = thrower + k * (aim - thrower), at launch
    {"throw_speed", 1.0f, {1.0f}},   // x the ball speed at launch
    {"throw_range", 1.0f, {1.0f}},   // x MaxThrowDistance (sub_823EFD28)
    {"throw_acc", -1.0f, {-1.0f}},   // PassAccuracy (sub_824104A8) forced to this 0..1; -1 = the game's own
    {"field_clamp", 1.0f, {1.0f}},   // 0 = no sideline / end-line clamp of the aim point while ThrowBall runs
    // the in-game overlay (OverlayBuild below)
    {"ov_on", 1.0f, {1.0f}},         // master switch
    {"ov_rings", 1.0f, {1.0f}},      // team rings under the players
    {"ov_labels", 1.0f, {1.0f}},     // position labels
    {"ov_links", 1.0f, {1.0f}},      // who blocks whom (green = winning the push, red = losing)
    {"ov_cones", 0.0f, {0.0f}},      // cones: 0 off, 1 every player, 2 the selected player + the QB
    {"ov_zones", 1.0f, {1.0f}},      // coverage: zone rectangles, deep/hook landmarks, man-coverage links
    {"read_cov_pts", 4.0f, {4.0f}},  // QB read: points that make a receiver COVERED (the game: 4); applied in the RecOpen hook
    {"ov_events", 1.0f, {1.0f}},     // flashes: rush moves, sheds, throws, flags
    {"ov_info", 1.0f, {1.0f}},       // info box of the selected player
    {"ov_sel", -1.0f, {-1.0f}},      // selected player 0..21 (-1 none, -2 = the ball carrier)
    {"ov_map", 2.0f, {2.0f}},        // field -> world axes: 2 = (x, height, -y), calibrated 2026-10-17; bits 1/2 flip x/y, 4 swap, 8 z-up, 16 transpose
    {"ov_scale", 1.0f, {1.0f}},      // text size
    {"ov_dx", 0.0f, {0.0f}},         // calibration: world offset (yards) added to x, height, z
    {"ov_dh", 0.0f, {0.0f}},
    {"ov_dz", 0.0f, {0.0f}},
};
Knob* FindKnob(const std::string& n)
{
    for (Knob& k : g_knobs) if (n == k.name) return &k;
    return nullptr;
}
float KnobV(const char* n) { Knob* k = FindKnob(n); return k ? k->v.load() : 0.f; }

// Is [ea, ea+n) committed guest memory? (tweak addresses come from the user: never touch an unmapped page)
bool GuestOk(uint32_t ea, uint32_t n)
{
    if (ea < 0x10000u || ea > 0xFFFFFFFFu - n) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(g_base + ea, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return (uint8_t*)mbi.BaseAddress + mbi.RegionSize >= g_base + ea + n;
#else
    return ea >= 0x82000000u && ea < 0xA0000000u;
#endif
}

// Per-player notes for the overlay: the last few things that fired for each player (any thread).
struct Note { int64_t ms; std::string text; uint32_t rgba; };
std::mutex g_noteMu;
std::deque<Note> g_notes[22];
int64_t MonoMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr uint32_t RGBA(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | g << 8 | b << 16 | a << 24; }
void AddNote(int i, std::string text, uint32_t rgba)
{
    if (i < 0 || i >= 22) return;
    std::lock_guard<std::mutex> lk(g_noteMu);
    const int64_t now = MonoMs();
    if (!g_notes[i].empty() && g_notes[i].back().text == text && now - g_notes[i].back().ms < 1000) {
        g_notes[i].back().ms = now;   // the same thing firing every frame (e.g. a held tackle roll): one note
        return;
    }
    g_notes[i].push_back({now, std::move(text), rgba});
    if (g_notes[i].size() > 6) g_notes[i].pop_front();
}
int16_t g_rat[22][48];            // ratings at the snap (BuildRoster), read by the overlay
std::mutex g_nameMu;              // g_assignNames is written by the sim thread, read by the overlay
std::atomic<int> g_ovHolder{-1};  // the ball holder index (sim thread), for ov_sel = -2
std::mutex g_camMu;               // the main 3-D pass camera (SetView / SetProjection hooks below)
float g_camV[16], g_camP[16];
bool g_camOk = false;

// Address expressions in tweaks.tsv and poke: HEX, HEX+HEX, [expr]+HEX (brackets = read the pointer there).
uint32_t ResolveAddr(const char*& s)
{
    uint32_t a = 0;
    while (*s == ' ') ++s;
    if (*s == '[') {
        ++s;
        const uint32_t inner = ResolveAddr(s);
        if (*s == ']') ++s;
        a = GuestOk(inner, 4) ? GLoad32(inner) : 0;
    } else {
        char* end = nullptr;
        a = uint32_t(strtoul(s, &end, 16));
        s = end;
    }
    while (*s == '+') {
        ++s;
        char* end = nullptr;
        a += uint32_t(strtoul(s, &end, 16));
        s = end;
    }
    return a;
}
uint32_t ResolveAddr(const std::string& e) { const char* s = e.c_str(); return ResolveAddr(s); }

// ---- guest helpers ----------------------------------------------------------------------------------------
uint32_t Call(const PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t r3)
{
    PPCContext c = ctx;
    c.r3.u64 = r3;
    PPCFunc* f = fn ? PPC_LOOKUP_FUNC(base, fn) : nullptr;
    if (!f) return 0;
    f(c, base);
    return c.r3.u32;
}
uint32_t PlayerAt(int i)
{
    const uint32_t m = GLoad32(kPlayerMgr);
    return m ? GLoad32(m + 4 * i) : 0;
}
int PlayerIndex(uint32_t p)
{
    if (!p) return -1;
    for (int i = 0; i < 22; ++i)
        if (PlayerAt(i) == p) return i;
    return -2;
}
uint32_t RatingRecord(const PPCContext& ctx, uint8_t* base, uint32_t player, int idx)
{
    const uint32_t comp = GLoad32(player + 644);
    if (!comp) return 0;
    return Call(ctx, base, GLoad32(GLoad32(comp) + 16 + 12 * idx), comp);
}
int Rating(const PPCContext& ctx, uint8_t* base, uint32_t player, int idx)
{
    const uint32_t rec = RatingRecord(ctx, base, player, idx);
    if (!rec) return -1;
    const int v = int16_t(GLoad16(rec + 4)), lo = int16_t(GLoad16(rec + 6)), hi = int16_t(GLoad16(rec + 8));
    return v < lo ? lo : v > hi ? hi : v;
}
// controller+4 = assignment list, 12-byte entries {assignment*, params*, ?}: [0] = current (what the game's own
// GetAssignmentType sub_82731ED8 reads), [1] = the next one queued. (Before 2026-10-18 this read [1] by mistake.)
uint32_t CurAssignment(uint32_t p)
{
    const uint32_t c = GLoad32(p + 496), l = c ? GLoad32(c + 4) : 0;
    return l ? GLoad32(l) : 0;
}
std::string GuestString(uint32_t ea)
{
    std::string s;
    for (int i = 0; ea && i < 64; ++i) {
        const char ch = char(GLoad8(ea + i));
        if (!ch) break;
        s += (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') ? ch : '?';
    }
    return s;
}
std::map<uint32_t, std::string> g_assignNames;   // type id -> name (sim thread only)
const std::string& AssignName(const PPCContext& ctx, uint8_t* base, uint32_t a, uint32_t type)
{
    auto it = g_assignNames.find(type);
    if (it != g_assignNames.end()) return it->second;
    const uint32_t vt = GLoad32(a);
    std::string n = vt ? GuestString(Call(ctx, base, GLoad32(vt + 36), a)) : std::string();
    std::lock_guard<std::mutex> lk(g_nameMu);
    return g_assignNames[type] = n.empty() ? "type " + std::to_string(type) : n;
}

// ---- auto-resume -------------------------------------------------------------------------------------------
std::atomic<int64_t> g_lastUpdateMs{0};
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void StartAutoResume()
{
    const char* s = Hc09Env("HC09_BLOCK_AUTORESUME");
    const double sec = s ? atof(s) : 0;
    if (sec <= 0) return;
    std::thread([sec] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const int64_t last = g_lastUpdateMs.load();
            if (last && NowMs() - last > int64_t(sec * 1000)) {
                fprintf(stderr, "[blocklog] no blocking update for %.0f s: pressing A\n", sec);
                InputInjectPress("a");
                g_lastUpdateMs.store(NowMs());
            }
        }
    }).detach();
}

// ---- per-tick state (sim thread) -----------------------------------------------------------------------------
struct BState {
    std::mutex mu;   // serialises the hooks' writers
    uint32_t play = 0, ticks = 0, gstate = 0;
    float lastClock = 1e9f, los = 0;
    bool live = false;
    int state[22] = {}, partner[22] = {};
    uint32_t assign[22] = {};
    int recv[22] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
} g_b;
std::atomic<int> g_recv[22];   // receiver status + 1 for the overlay (0 = not a QB read)
// player slot byte (+501, the slot within his team, what RecOpen stores) of team `team` -> index 0..21
int PlayerById(uint32_t id, uint32_t team)
{
    if (id == 255) return -1;
    for (int i = 0; i < 22; ++i)
        if (const uint32_t p = PlayerAt(i); p && GLoad8(p + 501) == id && GLoad8(p + 502) == team) return i;
    return -1;
}
float Clock() { const uint32_t m = GLoad32(kBlockMgr); return m ? F(m + 36) : 0.f; }

void StartServer();
void ForceSkill(uint32_t gsk)                  // HC09_SKILL=<0-3>: both teams' game skill (GameSkill +32 / +36)
{
    static const int v = [] { const char* s = Hc09Env("HC09_SKILL"); return s && *s ? std::clamp(atoi(s), 0, 3) : -1; }();
    if (v < 0 || !gsk) return;
    GStore32(gsk + 32, uint32_t(v)); GStore32(gsk + 36, uint32_t(v));
}
void CovTick(float t, uint32_t off);           // coverage section
std::string CovSnapJson(int i);

// Commands that only write guest memory or the clock rate: safe from the server thread, at once.
void ApplyNow(const std::string& c)
{
    std::istringstream in(c);
    std::string op;
    in >> op;
    if (op == "knob") {
        std::string n;
        double v = 0;
        in >> n >> v;
        if (Knob* k = FindKnob(n)) {
            k->v.store(float(v));
            Emit(true, "K %u %.3f knob %s %g\n", g_b.play, Clock(), n.c_str(), v);
        }
    } else if (op == "poke") {
        std::string a, type;
        double v = 0;
        in >> a >> type >> v;
        const uint32_t ea = ResolveAddr(a);
        if (!GuestOk(ea, 4)) return;
        if (type == "f32") SetF(ea, float(v));
        else if (type == "u8") GStore8(ea, uint8_t(v));
        else if (type == "s16" || type == "u16") GStore16(ea, uint16_t(int(v)));
        else if (type == "u32") GStore32(ea, uint32_t(v));
        Emit(true, "K %u %.3f poke %08X %s %g\n", g_b.play, Clock(), ea, type.c_str(), v);
    } else if (op == "slider") {
        std::string set;
        int k = -1, v = -1;
        in >> set >> k >> v;
        const uint32_t ai = GLoad32(kCustomAI);
        if (ai && k >= 0 && k < 15 && v >= 0 && v <= 255) GStore8(ai + (set == "u" ? 28 : 43) + k, uint8_t(v));
        Emit(true, "K %u %.3f slider %s %d %d\n", g_b.play, Clock(), set.c_str(), k, v);
    } else if (op == "speed") {
        double k = 1;
        in >> k;
        SetTimeWarp(k < 0.05 ? 0.05 : k > 8 ? 8 : k);
        Emit(true, "K %u %.3f speed %.2f\n", g_b.play, Clock(), k);
    } else if (op == "rating") {
        int p = -1, idx = -1, v = -1;
        in >> p >> idx >> v;
        if (p < 0 || p >= 22 || idx < 0 || idx >= kNumRatings) return;
        {
            std::lock_guard<std::mutex> lk(g_live.mu);
            if (v < 0) g_live.ratingHold.erase({p, idx});
            else g_live.ratingHold[{p, idx}] = v > 255 ? 255 : v;
        }
        Emit(true, "K %u %.3f rating %d %d %d\n", g_b.play, Clock(), p, idx, v);
    }
}

// Held ratings: write the effective value into the rating record every tick (guest calls: sim thread only).
void RunCommands(const PPCContext& ctx, uint8_t* base)
{
    std::map<std::pair<int, int>, int> hold;
    {
        std::lock_guard<std::mutex> lk(g_live.mu);
        hold = g_live.ratingHold;
    }
    for (auto& [key, v] : hold) {
        const uint32_t p = PlayerAt(key.first);
        if (!p) continue;
        if (const uint32_t rec = RatingRecord(ctx, base, p, key.second)) {
            const int hi = int16_t(GLoad16(rec + 8));
            if (v > hi) GStore16(rec + 8, uint16_t(v));
            GStore16(rec + 4, uint16_t(v));
        }
    }
}

void BuildRoster(const PPCContext& ctx, uint8_t* base)
{
    std::string j = "[";
    for (int i = 0; i < 22; ++i) {
        const uint32_t p = PlayerAt(i);
        if (!p) continue;
        char b[160];
        snprintf(b, sizeof b, "%s{\"i\":%d,\"team\":%u,\"pos\":%u,\"wt\":%.0f,\"ht\":%.0f,\"r\":[", j.size() > 1 ? "," : "",
                 i, GLoad8(p + 502), GLoad32(p + 756), F(p + 652), F(p + 648));
        j += b;
        for (int r = 0; r < kNumRatings; ++r) {
            const int v = Rating(ctx, base, p, r);
            g_rat[i][r] = int16_t(v);
            j += (r ? "," : "") + std::to_string(v);
        }
        j += "]}";
    }
    j += "]";
    std::lock_guard<std::mutex> lk(g_live.mu);
    g_live.roster = j;
}

void BuildSnap(const PPCContext& ctx, uint8_t* base, uint32_t gs, float t, int holder, uint32_t off)
{
    std::string j;
    char b[320];
    snprintf(b, sizeof b, "{\"play\":%u,\"t\":%.3f,\"gs\":%u,\"live\":%d,\"off\":%u,\"los\":%.2f,\"holder\":%d,\"speed\":%.2f,\"p\":[",
             g_b.play, t, gs, g_b.live ? 1 : 0, off, g_b.los, holder, TimeWarp());
    j = b;
    for (int i = 0; i < 22; ++i) {
        const uint32_t p = PlayerAt(i);
        if (!p) { j += i ? ",null" : "null"; continue; }
        const uint32_t bi = GLoad32(p + 988), a = CurAssignment(p);
        const uint32_t type = a ? GLoad32(a + 16) : 0xFFFFFFFF;
        snprintf(b, sizeof b,
                 "%s{\"x\":%.2f,\"y\":%.2f,\"f\":%.3f,\"vx\":%.2f,\"vy\":%.2f,\"bs\":%d,\"bp\":%d,\"pw\":%.0f,\"qk\":%.0f,\"dr\":%.2f,\"as\":%d,\"bk\":%d",
                 i ? "," : "", F(p + 144), F(p + 148), F(p + 160), F(p + 304), F(p + 308),
                 bi ? int(GLoad32(bi + 24)) : -1, bi ? PlayerIndex(GLoad32(bi + 28)) : -1,
                 bi ? F(bi + 64) : 0.f, bi ? F(bi + 68) : 0.f, bi ? F(bi + 48) : 0.f, int(type), bi ? int(GLoad32(bi + 44)) : 0);
        j += b;
        j += CovSnapJson(i) + "}";
    }
    j += "],\"sl\":[";
    const uint32_t ai = GLoad32(kCustomAI);
    for (int k = 0; k < 30; ++k) j += (k ? "," : "") + std::to_string(ai ? GLoad8(ai + 28 + k) : 0);
    j += "]}";
    std::lock_guard<std::mutex> lk(g_live.mu);
    g_live.snap = j;
}
}  // namespace

// ---- hooks ---------------------------------------------------------------------------------------------------
PPC_FUNC(sub_82529DD0)
{
    const PPCContext saved = ctx;
    __imp__sub_82529DD0(ctx, base);
    if (!On() || !GLoad32(kPlayerMgr)) return;
    static bool started = false;
    if (!started) { started = true; if (TweakPort()) StartServer(); }
    std::lock_guard<std::mutex> lk(g_b.mu);
    if (TweakPort()) RunCommands(saved, base);
    const uint32_t sm = GLoad32(kStateMgr);
    const uint32_t gs = sm ? GLoad32(sm + 32) : 0;
    const float t = Clock();
    if (gs != g_b.gstate) {
        Emit(true, "G %u %.3f %u %u\n", g_b.play, t, g_b.gstate, gs);
        g_b.gstate = gs;
    }
    const bool live = gs == 3 || gs == 4;
    if (g_b.play) g_lastUpdateMs.store(NowMs());
    const uint32_t rules = GLoad32(kRuleMgr), sub = rules ? GLoad32(rules + 28) : 0;
    const uint32_t off = sub ? Call(saved, base, GLoad32(GLoad32(sub) + 116), sub) : 99;
    const uint32_t bsys = GLoad32(kBallSys);
    const int holder = PlayerIndex(bsys ? Call(saved, base, 0x8272D3A8, bsys) : 0);
    g_ovHolder.store(holder);
    if (live && (!g_b.live || t + 0.05f < g_b.lastClock)) {
        if (++g_b.play == 1) StartAutoResume();
        g_b.ticks = 0;
        const uint32_t pi = GLoad32(kPlayInfo);
        const uint32_t pass = pi ? (Call(saved, base, 0x8256A5E8, pi) & 0xFF) : 9;
        g_b.los = holder >= 0 ? F(PlayerAt(holder) + 148) : 0.f;
        // game skill per team (0 rookie .. 3 all-madden): *(0x830A8518)+32+4*team, read by sub_824BE600.
        // HC09_SKILL=<0-3> forces both teams' value (an experiment option): written here and after every
        // GameSkill::Refresh sub_824A8FD8, which re-derives +32/+36 from the settings when its dirty flag (+48) is
        // set and runs before every read (824BE600, 824BD7C0, ...).
        const uint32_t gsk = GLoad32(0x830A8518);
        ForceSkill(gsk);
        Emit(true, "P %u off=%u pass=%u spot=%.2f,%.2f gs=%u skill=%d,%d\n", g_b.play, off, pass, pi ? F(pi + 184) : 0.f,
             pi ? F(pi + 188) : 0.f, gs, gsk ? int(GLoad32(gsk + 32)) : -1, gsk ? int(GLoad32(gsk + 36)) : -1);
        for (int i = 0; i < 22; ++i) {
            const uint32_t p = PlayerAt(i);
            if (!p) continue;
            if (LogFile()) {
                std::string line = "R " + std::to_string(g_b.play) + " " + std::to_string(i);
                char b[64];
                snprintf(b, sizeof b, " %u %u %.0f", GLoad8(p + 502), GLoad32(p + 756), F(p + 652));
                line += b;
                for (int r : kLogRatings) line += " " + std::to_string(Rating(saved, base, p, r));
                Emit(false, "%s\n", line.c_str());
                line = "L " + std::to_string(g_b.play) + " " + std::to_string(i);
                for (int r : kCovRatings) line += " " + std::to_string(Rating(saved, base, p, r));
                Emit(false, "%s\n", line.c_str());
            }
            const uint32_t bi = GLoad32(p + 988);
            g_b.state[i] = bi ? int(GLoad32(bi + 24)) : -1;
            g_b.partner[i] = bi ? PlayerIndex(GLoad32(bi + 28)) : -1;
        }
        if (TweakPort()) BuildRoster(saved, base);
    }
    g_b.live = live;
    g_b.lastClock = t;
    for (int i = 0; i < 22; ++i) {   // assignment changes
        const uint32_t p = PlayerAt(i), a = p ? CurAssignment(p) : 0;
        const uint32_t type = a ? GLoad32(a + 16) : 0xFFFFFFFF;
        if (a && type != g_b.assign[i]) {
            const std::string& an = AssignName(saved, base, a, type);
            Emit(true, "A %u %.3f %d %d %d %s\n", g_b.play, t, i, int(g_b.assign[i]), int(type), an.c_str());
            AddNote(i, "-> " + an, RGBA(200, 210, 220));
            g_b.assign[i] = type;
        }
    }
    // receiver status the QB AI reads (RecOpen *(0x830A85B8): +20 status[5] 0 OPEN 1 PARTIAL 2 COVERED, +40 player id[5])
    if (const uint32_t ro = GLoad32(kRecOpen); live && GuestOk(ro, 48)) {
        int st[22];
        for (int i = 0; i < 22; ++i) st[i] = -1;
        for (int k = 0; k < 5; ++k) {
            const int i = PlayerById(GLoad8(ro + 40 + k), off);
            if (i >= 0) st[i] = int(GLoad32(ro + 20 + 4 * k));
        }
        for (int i = 0; i < 22; ++i)
            if (st[i] != g_b.recv[i]) {
                if (st[i] >= 0) {
                    static const char* kSt[3] = {"OPEN", "partial", "covered"};
                    Emit(true, "V %u %.3f %d %d\n", g_b.play, t, i, st[i]);
                    if (st[i] < 3 && g_b.recv[i] >= 0) AddNote(i, kSt[st[i]], st[i] == 0 ? RGBA(90, 230, 130) : st[i] == 1 ? RGBA(240, 200, 80) : RGBA(255, 120, 120));
                }
                g_b.recv[i] = st[i];
                g_recv[i].store(st[i] + 1);
            }
    } else if (!live) {
        for (int i = 0; i < 22; ++i) { g_b.recv[i] = -1; g_recv[i].store(0); }
    }
    if (live) CovTick(t, off);
    if (TweakPort()) BuildSnap(saved, base, gs, t, holder, off);
    if (!live) {
        if (FILE* f = LogFile()) fflush(f);
        return;
    }
    for (int i = 0; i < 22; ++i) {
        const uint32_t p = PlayerAt(i), bi = p ? GLoad32(p + 988) : 0;
        if (!bi) continue;
        const int s = int(GLoad32(bi + 24)), pa = PlayerIndex(GLoad32(bi + 28));
        if (s != g_b.state[i] || pa != g_b.partner[i]) {
            Emit(true, "T %u %.3f %d %d %d %d\n", g_b.play, t, i, g_b.state[i], s, pa);
            if (s == 4) AddNote(i, "engaged #" + std::to_string(pa), RGBA(120, 200, 255));
            else if (s == 6 && g_b.state[i] == 4) AddNote(i, "shed / wrap anim", RGBA(255, 170, 60));
            g_b.state[i] = s; g_b.partner[i] = pa;
        }
    }
    if (g_b.ticks++ % 3 == 0 && LogFile()) {
        if (holder >= 0) {
            const uint32_t hp = PlayerAt(holder);
            const float x = F(hp + 144), y = F(hp + 148);
            float best = 1e9f;
            for (int i = 0; i < 22; ++i) {
                const uint32_t p = PlayerAt(i);
                if (!p || GLoad8(p + 502) == GLoad8(hp + 502)) continue;
                const float dx = F(p + 144) - x, dy = F(p + 148) - y, d = dx * dx + dy * dy;
                if (d < best) best = d;
            }
            Emit(false, "Q %u %.3f %d %.2f %.2f %.2f\n", g_b.play, t, holder, x, y, best < 1e8f ? sqrtf(best) : -1.f);
        } else {
            Emit(false, "Q %u %.3f %d\n", g_b.play, t, holder);
        }
        std::string line = "X " + std::to_string(g_b.play);
        char b[48];
        snprintf(b, sizeof b, " %.3f", t);
        line += b;
        for (int i = 0; i < 22; ++i) {
            const uint32_t p = PlayerAt(i);
            if (p) { snprintf(b, sizeof b, " %.2f,%.2f", F(p + 144), F(p + 148)); line += b; }
            else line += " -";
        }
        Emit(false, "%s\n", line.c_str());
    }
}

PPC_FUNC(sub_824DA310)
{
    const uint32_t pl = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32, lr = uint32_t(ctx.lr);
    __imp__sub_824DA310(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    Emit(true, "F %u %.3f %d %X %X lr=%08X\n", g_b.play, Clock(), PlayerIndex(pl), a5, a6, lr);
    AddNote(PlayerIndex(pl), "FLAG", RGBA(255, 220, 60));
}

PPC_FUNC(sub_82526068)
{
    const uint32_t blk = ctx.r4.u32, def = ctx.r5.u32;
    __imp__sub_82526068(ctx, base);
    if (!On()) return;
    const uint32_t a = blk ? GLoad32(blk + 988) : 0, b = def ? GLoad32(def + 988) : 0;
    if (!a || !b) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    Emit(true, "C %u %.3f %d %d %.1f %.1f %.1f %.1f %.3f %.3f\n", g_b.play, Clock(), PlayerIndex(blk), PlayerIndex(def),
         F(a + 48 + 16), F(a + 48 + 20), F(b + 48 + 16), F(b + 48 + 20), F(a + 48), F(b + 48));
}

PPC_FUNC(sub_82401CE0)
{
    const uint32_t r = ctx.r3.u32, b = ctx.r4.u32, dir = ctx.r7.u32, style = ctx.r9.u32;
    __imp__sub_82401CE0(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    Emit(true, "M %u %.3f %d %d %d %d %u\n", g_b.play, Clock(), PlayerIndex(r), PlayerIndex(b), int(dir), int(style),
         ctx.r3.u32 & 0xFF);
    const bool won = (ctx.r3.u32 & 0xFF) != 0;
    AddNote(PlayerIndex(r), won ? "MOVE won" : "move failed", won ? RGBA(90, 230, 130) : RGBA(150, 160, 170));
    if (won) AddNote(PlayerIndex(b), "beaten by a move", RGBA(255, 110, 110));
}

PPC_FUNC(sub_824FADA0)
{
    const uint32_t blk = ctx.r4.u32, def = ctx.r5.u32;
    __imp__sub_824FADA0(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    Emit(true, "S %u %.3f %d %d %u\n", g_b.play, Clock(), PlayerIndex(blk), PlayerIndex(def), ctx.r3.u32 & 0xFF);
    if (ctx.r3.u32 & 0xFF) { AddNote(PlayerIndex(def), "SHED", RGBA(255, 170, 60)); AddNote(PlayerIndex(blk), "shed by #" + std::to_string(PlayerIndex(def)), RGBA(255, 110, 110)); }
}

// ---- passing: throw knobs and the per-throw "W" event -------------------------------------------------------
// ThrowBall sub_8249CCB0(qb, f1 dt, r5 on-the-run) runs once per throw; inside it the ball is launched by
// LegacyBall::Launch sub_82596778(lb, &target vec4, f1 speed yd/s, f2 wobble, f3 spin, r6 mode).
// MaxThrowDistance sub_823EFD28(qb) -> f1 yd; PassAccuracy sub_824104A8(qb) -> f1 0..1.
// Rules = *(*(0x830A8610)+28): +40 sideline x, +36 end line y (the aim-point clamp in ThrowBall).
// W <play> <t> <qb> <dist yd> <speed yd/s> <accuracy> <range yd> <P28> <P32> <aimx> <aimy> <target> <sep yd>
//   <nearest defender> <defender to aim point yd>   (target = SetIntendedReceiver sub_82724868 inside ThrowBall;
//   sep = the target's distance to the nearest opponent at the release)
extern "C" PPC_FUNC(__imp__sub_8249CCB0);
extern "C" PPC_FUNC(__imp__sub_82596778);
extern "C" PPC_FUNC(__imp__sub_823EFD28);
extern "C" PPC_FUNC(__imp__sub_824104A8);
extern "C" PPC_FUNC(__imp__sub_82724868);
namespace {
void CovOnThrow(int tgt, float ax, float ay);   // coverage section below
thread_local uint32_t t_thrower = 0, t_target = 0;
thread_local float t_acc = -1, t_range = -1, t_dist = 0, t_speed = 0, t_ax = 0, t_ay = 0;
}  // namespace

PPC_FUNC(sub_8249CCB0)
{
    const uint32_t qb = ctx.r3.u32;
    t_thrower = qb; t_target = 0; t_acc = t_range = -1; t_dist = t_speed = 0;
    const uint32_t rules = GLoad32(kRuleMgr) ? GLoad32(GLoad32(kRuleMgr) + 28) : 0;
    const bool lift = rules && KnobV("field_clamp") < 0.5f;
    const float side = lift ? F(rules + 40) : 0, end = lift ? F(rules + 36) : 0;
    if (lift) { SetF(rules + 40, 1000.f); SetF(rules + 36, 1000.f); }
    __imp__sub_8249CCB0(ctx, base);
    if (lift) { SetF(rules + 40, side); SetF(rules + 36, end); }
    t_thrower = 0;
    if (!On() || !t_speed) return;
    const uint32_t pm = GLoad32(0x830A8540);
    std::lock_guard<std::mutex> lk(g_b.mu);
    // the target receiver and his separation now: nearest opponent to him, and nearest to the aim point
    const int tgt = PlayerIndex(t_target);
    float sepR = -1, sepA = -1;
    int nearD = -1;
    if (t_target)
        for (int i = 0; i < 22; ++i) {
            const uint32_t p = PlayerAt(i);
            if (!p || GLoad8(p + 502) == GLoad8(t_target + 502)) continue;
            const float d = hypotf(F(p + 144) - F(t_target + 144), F(p + 148) - F(t_target + 148));
            const float da = hypotf(F(p + 144) - t_ax, F(p + 148) - t_ay);
            if (sepR < 0 || d < sepR) { sepR = d; nearD = i; }
            if (sepA < 0 || da < sepA) sepA = da;
        }
    Emit(true, "W %u %.3f %d %.1f %.1f %.3f %.1f %.2f %.2f %.2f %.2f %d %.2f %d %.2f\n", g_b.play, Clock(), PlayerIndex(qb),
         t_dist, t_speed, t_acc, t_range, pm ? F(pm + 28) : 0.f, pm ? F(pm + 32) : 0.f, t_ax, t_ay, tgt, sepR, nearD, sepA);
    char tb[64];
    snprintf(tb, sizeof tb, "THROW %.0f yd @%.0f acc %.2f", t_dist, t_speed, t_acc);
    AddNote(PlayerIndex(qb), tb, RGBA(255, 220, 120));
    CovOnThrow(tgt, t_ax, t_ay);
}

PPC_FUNC(sub_82596778)
{
    if (t_thrower && ctx.r4.u32) {
        const uint32_t tp = ctx.r4.u32;
        const float qx = F(t_thrower + 144), qy = F(t_thrower + 148);
        float tx = F(tp), ty = F(tp + 4);
        const float k = KnobV("throw_dist");
        if (k != 1.0f) {
            tx = qx + k * (tx - qx); ty = qy + k * (ty - qy);
            SetF(tp, tx); SetF(tp + 4, ty);
        }
        const float ks = KnobV("throw_speed");
        if (ks != 1.0f) ctx.f1.f64 *= ks;
        t_dist = sqrtf((tx - qx) * (tx - qx) + (ty - qy) * (ty - qy));
        t_speed = float(ctx.f1.f64);
        t_ax = tx; t_ay = ty;
    }
    __imp__sub_82596778(ctx, base);
}

PPC_FUNC(sub_82724868)
{
    if (t_thrower) t_target = ctx.r4.u32;   // SetIntendedReceiver(ball, receiver) -> ball+288, inside ThrowBall
    __imp__sub_82724868(ctx, base);
}

PPC_FUNC(sub_823EFD28)
{
    __imp__sub_823EFD28(ctx, base);
    const float k = KnobV("throw_range");
    if (k != 1.0f) ctx.f1.f64 *= k;
    t_range = float(ctx.f1.f64);
}

PPC_FUNC(sub_824104A8)
{
    __imp__sub_824104A8(ctx, base);
    const float a = KnobV("throw_acc");
    if (a >= 0) ctx.f1.f64 = a;
    t_acc = float(ctx.f1.f64);
}

// ---- catching, tackling and ball-carrier moves: "what fired" events -----------------------------------------
//   H <play> <t> <player> <passer> <p> <difficulty>        catch / interception chance (rules slot 42 sub_824D72C0)
//   U <play> <t> <player> <taken>                          a hand/arm touch of the pass (rules slot 4 sub_824D5950)
//   O <play> <t> <player>                                  possession awarded (slot 15 sub_8251F340): catch or INT
//   D <play> <t> <player>                                  dropped pass (slot 21 sub_824B3100)
//   Y <play> <t> <defender> <carrier> <value> <roll> 1    tackle attempt that started (sub_8251E5C0: roll < value)
//   B <play> <t> <carrier> <defender> <score> <broken>     broken-tackle roll (sub_82506D98)
//   J <play> <t> <carrier> <move>                          special move started (sub_824583E8; state+104 = move index)
//   R <play> <t> <carrier> <ok>                            break-tackle pre-roll (sub_82417AB0)
extern "C" PPC_FUNC(__imp__sub_824D72C0);
extern "C" PPC_FUNC(__imp__sub_824D5950);
extern "C" PPC_FUNC(__imp__sub_8251F340);
extern "C" PPC_FUNC(__imp__sub_824B3100);
extern "C" PPC_FUNC(__imp__sub_8251E5C0);
extern "C" PPC_FUNC(__imp__sub_82506D98);
extern "C" PPC_FUNC(__imp__sub_824583E8);
extern "C" PPC_FUNC(__imp__sub_82417AB0);

PPC_FUNC(sub_824D72C0)
{
    const uint32_t pl = ctx.r4.u32, passer = ctx.r5.u32;
    const double diff = ctx.f1.f64;
    __imp__sub_824D72C0(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int i = PlayerIndex(pl);
    Emit(true, "H %u %.3f %d %d %.3f %.1f\n", g_b.play, Clock(), i, PlayerIndex(passer), ctx.f1.f64, diff);
    char b[64];
    snprintf(b, sizeof b, "catch chance %.2f", ctx.f1.f64);
    AddNote(i, b, RGBA(150, 220, 255));
}
PPC_FUNC(sub_824D5950)
{
    const uint32_t pl = ctx.r5.u32;
    __imp__sub_824D5950(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    Emit(true, "U %u %.3f %d %u\n", g_b.play, Clock(), PlayerIndex(pl), ctx.r3.u32 & 0xFF);
}
PPC_FUNC(sub_8251F340)
{
    const uint32_t pl = ctx.r4.u32;
    __imp__sub_8251F340(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int i = PlayerIndex(pl);
    Emit(true, "O %u %.3f %d\n", g_b.play, Clock(), i);
    AddNote(i, "HAS THE BALL", RGBA(255, 220, 120));
}
PPC_FUNC(sub_824B3100)
{
    const uint32_t pl = ctx.r4.u32;
    __imp__sub_824B3100(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int i = PlayerIndex(pl);
    Emit(true, "D %u %.3f %d\n", g_b.play, Clock(), i);
    AddNote(i, "DROPPED", RGBA(255, 110, 110));
}
PPC_FUNC(sub_8251E5C0)
{
    const uint32_t def = ctx.r4.u32, car = ctx.r5.u32;
    const double v = ctx.f1.f64, roll = ctx.f2.f64;
    __imp__sub_8251E5C0(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int d = PlayerIndex(def), c = PlayerIndex(car);
    const bool ok = (ctx.r3.u32 & 0xFF) != 0;
    if (ok) {   // every defender near the carrier rolls every frame: only the attempts that start are logged
        Emit(true, "Y %u %.3f %d %d %.1f %.1f 1\n", g_b.play, Clock(), d, c, v, roll);
        char b[64];
        snprintf(b, sizeof b, "TACKLE try (%.0f vs %.0f)", roll, v);
        AddNote(d, b, RGBA(255, 160, 90));
    }
}
PPC_FUNC(sub_82506D98)
{
    const uint32_t car = ctx.r4.u32, def = ctx.r5.u32;
    const double score = ctx.f1.f64;
    __imp__sub_82506D98(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int c = PlayerIndex(car), d = PlayerIndex(def);
    const bool broken = (ctx.r3.u32 & 0xFF) != 0;
    Emit(true, "B %u %.3f %d %d %.1f %u\n", g_b.play, Clock(), c, d, score, broken ? 1u : 0u);
    AddNote(c, broken ? "BROKE THE TACKLE" : "tackled", broken ? RGBA(90, 230, 130) : RGBA(255, 110, 110));
    if (broken) AddNote(d, "missed tackle", RGBA(255, 110, 110));
}
PPC_FUNC(sub_824583E8)
{
    const uint32_t bc = ctx.r3.u32, st = ctx.r4.u32;
    __imp__sub_824583E8(ctx, base);
    if (!On() || !(ctx.r3.u32 & 0xFF) || !GuestOk(st, 108)) return;
    static const char* kMove[] = {"JUKE", "SPIN", "HURDLE", "DIVE", "STIFF ARM", "OOMPH", "BALL TO OUTSIDE ARM", "TRUCK / JUMP CUT"};
    const uint32_t m = GLoad8(st + 104);
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int i = PlayerIndex(bc);
    Emit(true, "J %u %.3f %d %u\n", g_b.play, Clock(), i, m);
    if (m < 8) AddNote(i, kMove[m], RGBA(255, 230, 90));
}
PPC_FUNC(sub_82417AB0)
{
    const uint32_t bc = ctx.r3.u32;
    __imp__sub_82417AB0(ctx, base);
    if (!On() || !(ctx.r3.u32 & 0xFF)) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int i = PlayerIndex(bc);
    Emit(true, "R %u %.3f %d 1\n", g_b.play, Clock(), i);
    AddNote(i, "break-tackle pre-roll won", RGBA(90, 230, 130));
}

// ---- coverage and receivers ---------------------------------------------------------------------------------
// RecOpen::EvaluateReceiver sub_8251C6B0(ro, rcv) rates a receiver OPEN / PARTIAL / COVERED from points: every
// defender adds points by geometry (0.44-s projections, lane cone and windows from the QB, facing the ball). The
// points live in a register, so a C++ copy of its rules ("shadow") runs right after it on the very values it used
// (captured by return address from its callees) and is checked against its result every time.
//   Z <play> <t> <rcv> <status> <points> <catch x> <catch y> <def:points:rule+rule|...>   every receiver at a throw
//   E <play> <t> <player> <what> ...       coverage events: break <type> <s since throw> <yd to aim> (pushes of
//                                          Play Ball 58 / Chase Ball 2 after the throw), jam <rcv> <zone|man>,
//                                          cut <rcv> <faked-out %>, shadowbad <rcv> <real> <shadow> (scorer mismatch)
extern "C" PPC_FUNC(__imp__sub_8251C6B0);
extern "C" PPC_FUNC(__imp__sub_824ECF58);
extern "C" PPC_FUNC(__imp__sub_824DF1F0);
extern "C" PPC_FUNC(__imp__sub_824CF4C0);
extern "C" PPC_FUNC(__imp__sub_82508FE8);
extern "C" PPC_FUNC(__imp__sub_824BA320);
extern "C" PPC_FUNC(__imp__sub_82726300);
extern "C" PPC_FUNC(__imp__sub_823EB0B8);
extern "C" PPC_FUNC(__imp__sub_823EB3C8);
namespace {
constexpr float kPi = 3.14159265f, kDeg = kPi / 180;
float Hd(float x, float y) { return atan2f(y, x); }
float AD(float a, float b)   // |a - b| wrapped to [0, pi]
{
    float d = fmodf(fabsf(a - b), 2 * kPi);
    return d > kPi ? 2 * kPi - d : d;
}
// a guest call with up to 8 integer args and f1 (post-call in a hook: the stack below r1 is free)
uint32_t CallR(const PPCContext& ctx, uint8_t* base, uint32_t fn, std::initializer_list<uint32_t> regs, double f1 = 0,
               double* f1out = nullptr)
{
    PPCContext c = ctx;
    PPCRegister* r[] = {&c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10};
    int k = 0;
    for (uint32_t v : regs) if (k < 8) r[k++]->u64 = v;
    c.f1.f64 = f1;
    PPCFunc* f = PPC_LOOKUP_FUNC(base, fn);
    if (!f) return 0;
    f(c, base);
    if (f1out) *f1out = c.f1.f64;
    return c.r3.u32;
}
uint32_t AssignType(uint32_t p) { const uint32_t a = CurAssignment(p); return a ? GLoad32(a + 16) : 0; }
uint32_t AssignParams(uint32_t p)   // list[0].params
{
    const uint32_t c = GLoad32(p + 496), l = c ? GLoad32(c + 4) : 0;
    return l ? GLoad32(l + 4) : 0;
}
uint32_t Brain(uint32_t p) { const uint32_t c = GLoad32(p + 496); return c ? GLoad32(c + 12) : 0; }
int HandlePlayer(uint32_t h)   // sub_82726148: entity type 11 = player, slot (h>>16)&255, team (h>>8)&255
{
    return (h & 0xFF) == 11 ? PlayerById((h >> 16) & 0xFF, (h >> 8) & 0xFF) : -1;
}
bool IsCoverageType(uint32_t a) { return a == 19 || (a >= 20 && a <= 23) || a == 65 || a == 92 || a == 34; }

// values the real evaluation used, captured from its callees (sim thread)
struct CovCapture {
    bool on = false;
    uint32_t rcv = 0;
    bool hasCatch = false, hasRP = false;
    float cx = 0, cy = 0, rpx = 0, rpy = 0;
    float dpx[22] = {}, dpy[22] = {};
    bool hasDP[22] = {};
    int lane[22], inZone[22];
    struct Fac { uint32_t p; float half; bool r; };
    std::vector<Fac> facing;
};
thread_local CovCapture t_cv;

struct Contrib { int def; int pts; bool cov; std::string why; };
struct RecvEval { int status = -1, pts = 0; bool cov = false; float cx = 0, cy = 0, qx = 0, qy = 0; int64_t ms = 0; std::vector<Contrib> c; };
struct DefView {
    int type = -1, zone = -1, man = -1;   // assignment, zone index (0-8 deep, 9/10 flat, 11/12 CF, 14 hook), man target
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // zone rectangle (x across, y downfield)
    bool box = false;
    float lx = 0, ly = 0;                 // landmark (deep/CF target, hook spot)
    bool land = false;
};
std::mutex g_covMu;
RecvEval g_eval[22];
DefView g_defv[22];
struct CovPlay { uint32_t play = 0; float throwT = -1, ax = 0, ay = 0; int tgt = -1; } g_cp;
std::atomic<uint32_t> g_shadowN{0}, g_shadowBad{0};

bool FacingLookup(const PPCContext& ctx, uint8_t* base, uint32_t d, float half)
{
    for (auto& f : t_cv.facing)
        if (f.p == d && fabsf(f.half - half) < 1e-4f) return f.r;
    // not captured (the real code short-circuited before calling it): compute from the ball position
    const uint32_t bs = GLoad32(kBallSys);
    const uint32_t ball = bs ? CallR(ctx, base, 0x82724B30, {bs, GLoad32(bs + 28)}) : 0;
    if (!ball) return false;
    return AD(Hd(F(ball + 144) - F(d + 144), F(ball + 148) - F(d + 148)), F(d + 160)) < half;
}
// sub_82575440(float* out, p+256): the heading of the velocity (p+304) when moving (|v|^2 > 1.5e-5), else his facing.
// (Not a "float f(player)": it writes to *r3. Computed here instead of a guest call.)
float MoveHeading(const PPCContext&, uint8_t*, uint32_t p)
{
    const float vx = F(p + 304), vy = F(p + 308), vz = F(p + 312);
    if (vx * vx + vy * vy + vz * vz > 1.52587890625e-05f) return Hd(vx, vy);
    return F(p + 160);
}

// The shadow of sub_8251C6B0 (recopen.md section 2). Returns the status; fills the per-defender points.
int ShadowRecOpen(const PPCContext& ctx, uint8_t* base, RecvEval& out)
{
    const uint32_t rcv = t_cv.rcv, bs = GLoad32(kBallSys);
    const uint32_t qb = bs ? Call(ctx, base, 0x8272D3A8, bs) : 0;
    auto early = [&](const char* why) { out.c.push_back({-1, 0, true, why}); return 2; };
    if (!qb || GLoad8(qb + 502) != GLoad8(rcv + 502)) return early("no-QB");
    out.qx = F(qb + 144); out.qy = F(qb + 148);
    if (GLoad32(rcv + 212) & 0x00200000) return early("down");
    if (AssignType(qb) == 26) return early("QB-assign-26");
    const float rx = F(rcv + 144), ry = F(rcv + 148), qx = out.qx, qy = out.qy;
    if (hypotf(rx - qx, ry - qy) <= 5.0f) return early("within-5yd-of-QB");
    if (!t_cv.hasCatch || !t_cv.hasRP) return early("no-catch-point");
    const uint32_t rules = GLoad32(kRuleMgr) ? GLoad32(GLoad32(kRuleMgr) + 28) : 0;
    // the ball spot as the game reads it: rules vslot 6 (+24)(out vec4, rules); scratch below the guest stack
    float spotX = 0, spotY = g_b.los;
    if (rules) {
        PPCContext c = ctx;
        const uint32_t scratch = (ctx.r1.u32 - 0x800) & ~15u;
        c.r1.u64 = scratch - 0x100;
        CallR(c, base, GLoad32(GLoad32(rules) + 24), {scratch, rules});
        spotX = F(scratch); spotY = F(scratch + 4);
    }
    const float half = rules ? F(rules + 48) : 6.f;
    float cx = t_cv.cx, cy = t_cv.cy;
    out.cx = cx; out.cy = cy;
    if (cy - spotY < 2.5f && spotX + half > cx && cx > spotX - half) return early("catch-short-of-LOS2.5-in-box");
    if (!(GLoad32(qb + 212) & 0x100) && rules) {
        const float sx = F(rules + 40), ey = F(rules + 36);
        cx = std::clamp(cx, 1.0f - sx, sx - 1.0f); cy = std::clamp(cy, 1.0f - ey, ey - 1.0f);
    }
    const float hRcvToC = Hd(cx - rx, cy - ry), rcvToC = hypotf(cx - rx, cy - ry);
    const float hQbToC = Hd(cx - qx, cy - qy), throwDist = hypotf(cx - qx, cy - qy);
    const uint32_t defTeam = GLoad8(qb + 502) ^ 1, blk = GLoad32(kBlockMgr);
    const float rpx = t_cv.rpx, rpy = t_cv.rpy, rFace = F(rcv + 160);
    int pts = 0;
    bool covered = false;
    for (int slot = 0; slot < 11; ++slot) {
        const int di = PlayerById(slot, defTeam);
        if (di < 0) continue;
        const uint32_t d = PlayerAt(di);
        if (!t_cv.hasDP[di]) continue;
        Contrib cb{di, 0, false, ""};
        auto add = [&](int n, const char* why) { cb.pts += n; if (!cb.why.empty()) cb.why += "+"; cb.why += why; };
        auto cov = [&](const char* why) { cb.cov = true; if (!cb.why.empty()) cb.why += "+"; cb.why += why; };
        const uint32_t a = AssignType(d);
        const float dpx = t_cv.dpx[di], dpy = t_cv.dpy[di];
        const float sep = hypotf(rpx - dpx, rpy - dpy), hDtoR = Hd(rpx - dpx, rpy - dpy);
        if (!IsCoverageType(a)) {
            if (sep < 2.0f) cov("on-top<2yd");
            if (cb.cov) { covered = true; out.c.push_back(cb); }
            continue;
        }
        const float dx = F(d + 144), dy = F(d + 148), dFace = F(d + 160);
        const float dToC = hypotf(cx - dx, cy - dy), hQbToD = Hd(dx - qx, dy - qy), dToQb = hypotf(dx - qx, dy - qy);
        auto engaged = [&] { return blk && (CallR(ctx, base, 0x824B6EA0, {blk, d}) & 0xFF) != 0; };
        if (GLoad32(qb + 756) == 0 && t_cv.lane[di] > 0 && !engaged() && FacingLookup(ctx, base, d, kPi / 2) &&
            (sep < 8.0f || dToQb < 7.0f))
            add(1, "lane-cone30");
        auto window = [&](float radius, int bonus) {
            if (dToQb < throwDist && dToC < radius) {
                const float ang = AD(hQbToC, hQbToD);
                if (ang < 15 * kDeg && dToC < 7.0f) cov("lane15");
                else if (ang < 25 * kDeg) add(bonus, "lane25");
            }
        };
        switch (a) {
        case 65: case 92: {
            const uint32_t prm = AssignParams(d), br = Brain(d);
            if (a == 65 && prm && GLoad8(prm + 32) == GLoad8(rcv + 501)) {
                if (br && GLoad8(br + 48)) { cov("jammed"); }
            } else window(5.0f, 2);
            if (!cb.cov || a != 65) { if (sep < 4.0f) add(1, "sep<4"); }
            break;
        }
        case 19: {
            const uint32_t br = Brain(d);
            if (br && HandlePlayer(GLoad32(br + 4)) == PlayerIndex(rcv)) {
                const float aRcvBack = AD(rFace, 3 * kPi / 2);
                if (sep <= 3.0f && dToC - rcvToC <= 2.0f) add(1, "trail<=3");
                if (sep <= 2.5f) add(1, "sep<=2.5");
                if (dToC < rcvToC && aRcvBack < 15 * kDeg) add(1, "rcv-turned-back");
                if (AD(dFace, hDtoR) < 30 * kDeg && AD(hDtoR + kPi, rFace) < 30 * kDeg && sep < 3.0f) add(1, "face-to-face");
                if (AD(hDtoR + kPi, hRcvToC) < 30 * kDeg && sep < rcvToC) add(1, "ball-toward-D");
                if (AD(hDtoR + kPi, kPi / 2) < 30 * kDeg && sep < 3.0f) add(1, "over-the-top");
                if (sep <= 8.0f) {
                    const float mhR = MoveHeading(ctx, base, rcv);
                    if (AD(mhR, hDtoR) > kPi / 2 && AD(mhR, MoveHeading(ctx, base, d)) > 35 * kDeg) add(1, "crossing");
                }
                if (dToC < rcvToC && sep > 3.5f) add(2, "sits-on-spot");
                if (sep <= 2.0f) add(1, "sep<=2");
            } else {
                if (sep <= 4.0f) add(1, "near<=4");
                if (FacingLookup(ctx, base, d, kPi / 3)) {
                    if (dToC <= 8.0f || sep < 8.0f) {
                        window(10.0f, 1);
                        add(1, "faces-ball60");
                    }
                }
                if (sep <= 2.0f) add(1, "sep<=2");
            }
            break;
        }
        case 20: case 21: case 22: case 23: case 34: {
            if (a != 34) {
                if (sep < 7.0f && AD(MoveHeading(ctx, base, rcv), hDtoR) > kPi / 2) {
                    if (FacingLookup(ctx, base, d, kPi / 2)) add(1, "zone-faces-ball");
                    if (t_cv.inZone[di] > 0) add(1, "in-his-zone");
                } else if (sep < 3.75f) add(1, "zone-sep<3.75");
            }
            if (dToQb < throwDist && hypotf(dpx - qx, dpy - qy) < throwDist && dToC < 5.0f) {
                const float ang = AD(hQbToC, hQbToD);
                if (ang < 15 * kDeg && dToC < 7.0f) cov("lane15");
                else if (ang < 25 * kDeg) add(1, "lane25");
            }
            break;
        }
        }
        pts += cb.pts;
        covered |= cb.cov;
        if (cb.pts || cb.cov) out.c.push_back(cb);
    }
    out.pts = pts;
    out.cov = covered;
    return covered || pts >= 4 ? 2 : pts ? 1 : 0;
}

// per tick (sim thread, from the BlockingManager hook): what each defender is doing in coverage
void CovTick(float t, uint32_t off)
{
    const uint32_t zc = GLoad32(0x830A85D4);
    DefView v[22];
    for (int i = 0; i < 22; ++i) {
        const uint32_t p = PlayerAt(i);
        if (!p || GLoad8(p + 502) == off) continue;
        DefView& d = v[i];
        d.type = int(AssignType(p));
        const uint32_t prm = AssignParams(p), br = Brain(p);
        if (d.type == 19 && br) d.man = HandlePlayer(GLoad32(br + 4));
        if (d.type >= 20 && d.type <= 23 && prm) {
            d.zone = d.type == 21 ? 14 : int(GLoad32(prm + 8));
            if (d.zone >= 0 && d.zone <= 12 && zc) {
                const uint32_t b = zc + 20 + 16 * d.zone;
                d.y1 = F(b); d.y0 = F(b + 4); d.x0 = F(b + 8); d.x1 = F(b + 12); d.box = true;
            } else if (d.zone == 14 && br) {   // hook: hookX +-7 (neighbour trimming ignored), LOS+1.5 .. LOS+18
                const float hx = F(br + 40);
                d.x0 = hx - 7; d.x1 = hx + 7; d.y0 = g_b.los + 1.5f; d.y1 = g_b.los + 18; d.box = true;
                d.lx = hx; d.ly = F(br + 44); d.land = true;
            }
            if ((d.type == 22 || d.type == 23) && br) { d.lx = F(br + 32); d.ly = F(br + 36); d.land = true; }
        }
    }
    (void)t;
    std::lock_guard<std::mutex> lk(g_covMu);
    if (g_cp.play != g_b.play) { g_cp = {}; g_cp.play = g_b.play; for (auto& e : g_eval) e = {}; }
    for (int i = 0; i < 22; ++i) g_defv[i] = v[i];
}

std::string ContribText(const RecvEval& e)
{
    std::string s;
    for (auto& c : e.c) {
        if (!s.empty()) s += "|";
        s += std::to_string(c.def) + ":" + (c.cov ? "C" : std::to_string(c.pts)) + ":" + (c.why.empty() ? "-" : c.why);
    }
    return s.empty() ? "-" : s;
}

// ,"rs":status,"rp":points,"cp":[x,y],"rw":"why..."  (receivers)  ,"cz":[x0,x1,y0,y1],"zn":zone,"mt":man,"lm":[x,y]
std::string CovSnapJson(int i)
{
    std::lock_guard<std::mutex> lk(g_covMu);
    std::string s;
    char b[200];
    const RecvEval& e = g_eval[i];
    if (e.status >= 0 && MonoMs() - e.ms < 1500) {
        snprintf(b, sizeof b, ",\"rs\":%d,\"rp\":%d,\"cp\":[%.2f,%.2f]", e.status, e.pts, e.cx, e.cy);
        s += b;
        s += ",\"rw\":\"" + ContribText(e) + "\"";
    }
    const DefView& d = g_defv[i];
    if (d.type >= 0) {
        snprintf(b, sizeof b, ",\"zn\":%d,\"mt\":%d", d.zone, d.man);
        s += b;
        if (d.box) { snprintf(b, sizeof b, ",\"cz\":[%.2f,%.2f,%.2f,%.2f]", d.x0, d.x1, d.y0, d.y1); s += b; }
        if (d.land) { snprintf(b, sizeof b, ",\"lm\":[%.2f,%.2f]", d.lx, d.ly); s += b; }
    }
    return s;
}

void CovOnThrow(int tgt, float ax, float ay)   // g_b.mu held
{
    std::lock_guard<std::mutex> lk(g_covMu);
    g_cp.throwT = Clock(); g_cp.ax = ax; g_cp.ay = ay; g_cp.tgt = tgt;
    const int64_t now = MonoMs();
    for (int i = 0; i < 22; ++i) {
        const RecvEval& e = g_eval[i];
        if (e.status < 0 || now - e.ms > 1000) continue;
        Emit(true, "Z %u %.3f %d %d %d %.2f %.2f %s\n", g_b.play, Clock(), i, e.status, e.pts, e.cx, e.cy, ContribText(e).c_str());
    }
}
}  // namespace

PPC_FUNC(sub_8251C6B0)
{
    const bool on = On();
    const PPCContext saved = ctx;
    if (on) {
        t_cv.on = true; t_cv.rcv = ctx.r4.u32; t_cv.hasCatch = t_cv.hasRP = false;
        for (int i = 0; i < 22; ++i) { t_cv.hasDP[i] = false; t_cv.lane[i] = t_cv.inZone[i] = -1; }
        t_cv.facing.clear();
    }
    __imp__sub_8251C6B0(ctx, base);
    if (!on) return;
    t_cv.on = false;
    const int real = ctx.r3.s32;
    RecvEval e;
    e.ms = MonoMs();
    const int sh = ShadowRecOpen(saved, base, e);
    e.status = real;
    // knob read_cov_pts: the points that make COVERED (the game: 4, an immediate in the code). Only when the copy
    // agrees with the game, so a mismatch never invents a status.
    const float kp = KnobV("read_cov_pts");
    if (kp >= 1 && kp != 4 && sh == real && e.pts > 0) {
        const int st = (e.cov || e.pts >= int(kp)) ? 2 : 1;
        ctx.r3.u64 = uint64_t(st);
        e.status = st;
    }
    const int ri = PlayerIndex(t_cv.rcv);
    ++g_shadowN;
    if (sh != real) {
        if (++g_shadowBad <= 200) {
            std::lock_guard<std::mutex> lk(g_b.mu);
            Emit(true, "E %u %.3f %d shadowbad %d %d %d %s\n", g_b.play, Clock(), ri, real, sh, e.pts, ContribText(e).c_str());
        }
    }
    if (ri >= 0) {
        std::lock_guard<std::mutex> lk(g_covMu);
        g_eval[ri] = std::move(e);
    }
}
PPC_FUNC(sub_824ECF58)
{
    const uint32_t out = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_824ECF58(ctx, base);
    if (t_cv.on && lr == 0x8251C918) { t_cv.cx = F(out); t_cv.cy = F(out + 4); t_cv.hasCatch = true; }
}
PPC_FUNC(sub_824DF1F0)
{
    const uint32_t p = ctx.r4.u32, out = ctx.r5.u32, lr = uint32_t(ctx.lr);
    __imp__sub_824DF1F0(ctx, base);
    if (!t_cv.on) return;
    if (lr == 0x8251C934) { t_cv.rpx = F(out); t_cv.rpy = F(out + 4); t_cv.hasRP = true; }
    else if (lr == 0x8251CFF4) {
        const int i = PlayerIndex(p);
        if (i >= 0) { t_cv.dpx[i] = F(out); t_cv.dpy[i] = F(out + 4); t_cv.hasDP[i] = true; }
    }
}
PPC_FUNC(sub_824CF4C0)
{
    const uint32_t b = ctx.r5.u32, lr = uint32_t(ctx.lr);
    __imp__sub_824CF4C0(ctx, base);
    if (t_cv.on && lr == 0x8251D288) { const int i = PlayerIndex(b); if (i >= 0) t_cv.lane[i] = ctx.r3.u32 & 0xFF; }
}
PPC_FUNC(sub_82508FE8)
{
    const uint32_t d = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_82508FE8(ctx, base);
    if (t_cv.on && lr == 0x8251DA10) { const int i = PlayerIndex(d); if (i >= 0) t_cv.inZone[i] = ctx.r3.u32 & 0xFF; }
}
PPC_FUNC(sub_824BA320)
{
    const uint32_t p = ctx.r4.u32, hp = ctx.r5.u32;
    __imp__sub_824BA320(ctx, base);
    if (t_cv.on && GuestOk(hp, 4)) t_cv.facing.push_back({p, F(hp), (ctx.r3.u32 & 0xFF) != 0});
}

// PushAssignment(player, type, params): defenders breaking on the ball after the throw (Play Ball 58, Chase Ball 2)
PPC_FUNC(sub_82726300)
{
    const uint32_t p = ctx.r3.u32, type = ctx.r4.u32;
    __imp__sub_82726300(ctx, base);
    if (!On() || (type != 58 && type != 2)) return;
    float tt, ax, ay;
    {
        std::lock_guard<std::mutex> lk(g_covMu);
        if (g_cp.throwT < 0 || g_cp.play != g_b.play) return;
        tt = g_cp.throwT; ax = g_cp.ax; ay = g_cp.ay;
    }
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int i = PlayerIndex(p);
    if (i < 0) return;
    const float since = Clock() - tt, dist = hypotf(F(p + 144) - ax, F(p + 148) - ay);
    Emit(true, "E %u %.3f %d break %u %.2f %.1f\n", g_b.play, Clock(), i, type, since, dist);
    char b[64];
    snprintf(b, sizeof b, "%s +%.2fs (%.0f yd)", type == 58 ? "BREAK ON BALL" : "chase", since, dist);
    AddNote(i, b, type == 58 ? RGBA(255, 150, 90) : RGBA(200, 170, 140));
}
// the jam (Chuck) happened: (req, def, rcv); req+948 = 145 zone jam / 146 man jam
PPC_FUNC(sub_823EB0B8)
{
    const uint32_t req = ctx.r3.u32, def = ctx.r4.u32, rcv = ctx.r5.u32;
    const uint32_t kind = GuestOk(req, 952) ? GLoad32(req + 948) : 0;
    __imp__sub_823EB0B8(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int d = PlayerIndex(def), r = PlayerIndex(rcv);
    Emit(true, "E %u %.3f %d jam %d %s\n", g_b.play, Clock(), d, r, kind == 146 ? "man" : kind == 145 ? "zone" : "?");
    AddNote(d, "JAM #" + std::to_string(r), RGBA(255, 190, 90));
    AddNote(r, "jammed by #" + std::to_string(d), RGBA(255, 130, 110));
}
// Defensive Cut: the defender's chance (%) to be faked out by the receiver's cut (def, rcv)
PPC_FUNC(sub_823EB3C8)
{
    const uint32_t def = ctx.r3.u32, rcv = ctx.r4.u32;
    __imp__sub_823EB3C8(ctx, base);
    if (!On()) return;
    std::lock_guard<std::mutex> lk(g_b.mu);
    const int d = PlayerIndex(def), r = PlayerIndex(rcv);
    Emit(true, "E %u %.3f %d cut %d %u\n", g_b.play, Clock(), d, r, ctx.r3.u32);
    AddNote(d, "cut: faked-out " + std::to_string(ctx.r3.u32) + "%", RGBA(220, 200, 120));
}

// GameSkill::Refresh (r3 = *(0x830A8518)): re-derives the per-team skill from the settings when dirty
extern "C" PPC_FUNC(__imp__sub_824A8FD8);
PPC_FUNC(sub_824A8FD8)
{
    const uint32_t gsk = ctx.r3.u32;
    __imp__sub_824A8FD8(ctx, base);
    ForceSkill(gsk);
}

// HC09_SLIDERS=u<k>:<v>,c<k>:<v>,...  override on-field slider bytes after the CustomAI object (*(0x830A85E0))
// copies them from the GOPT options (sub_824B10D0): u = User set (this+28+k, options 23-37), c = CPU set
// (this+43+k, options 38-52). k: 0 QB Accuracy, 1 Pass Blocking, 2 WR Catching, 3 RB Ability, 4 Run Blocking,
// 5 Awareness, 6 Knock Downs, 7 Interceptions, 8 Break Block, 9 Tackling, 10-14 kicking.
PPC_FUNC(sub_824B10D0)
{
    const uint32_t self = ctx.r3.u32;
    __imp__sub_824B10D0(ctx, base);
    const char* s = Hc09Env("HC09_SLIDERS");
    if (!s || !*s || !self) return;
    std::string spec(s);
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string it = spec.substr(i, j - i);
        int k = -1, v = -1;
        if (it.size() > 2 && (it[0] == 'u' || it[0] == 'c') && sscanf(it.c_str() + 1, "%d:%d", &k, &v) == 2 &&
            k >= 0 && k < 15 && v >= 0 && v <= 255) {
            GStore8(self + (it[0] == 'u' ? 28 : 43) + k, uint8_t(v));
            fprintf(stderr, "[sliders] %s slider %d = %d\n", it[0] == 'u' ? "user" : "cpu", k, v);
        }
        i = j + 1;
    }
}

// ---- the HTTP server ---------------------------------------------------------------------------------------------
#ifdef _WIN32
namespace {
std::string TweakDir()
{
    const char* d = Hc09Env("HC09_TWEAK_DIR");
    return d && *d ? d : "tweak";
}
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
std::string JsonStr(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (uint8_t(c) >= 32) o += c;
    }
    return o + "\"";
}
// tweaks.tsv: id <tab> group <tab> name <tab> type <tab> addr <tab> default <tab> min <tab> max <tab> note
std::string TweaksJson()
{
    std::istringstream in(ReadFile(TweakDir() + "/tweaks.tsv"));
    std::string line, j = "[";
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c;
        size_t a = 0;
        for (;;) {
            size_t b = line.find('\t', a);
            c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (c.size() < 8) continue;
        double cur = 0;
        const uint32_t ea = c[3] == "knob" ? 0 : ResolveAddr(c[4]);
        if (c[3] == "knob") {
            cur = KnobV(c[4].c_str());
        } else if (GuestOk(ea, 4)) {
            if (c[3] == "f32") cur = F(ea);
            else if (c[3] == "u8") cur = GLoad8(ea);
            else if (c[3] == "s16") cur = int16_t(GLoad16(ea));
            else if (c[3] == "u16") cur = GLoad16(ea);
            else if (c[3] == "u32") cur = GLoad32(ea);
        }
        // the first value seen in gameplay is the "game value" (many are set at startup, so the image's is 0)
        static std::map<std::string, double> orig;
        static std::mutex origMu;
        double first = cur;
        {
            std::lock_guard<std::mutex> lk(origMu);
            auto it = orig.find(c[0]);
            if (it != orig.end()) first = it->second;
            else if (GLoad32(kPlayerMgr) && (c[3] == "knob" || ea)) orig[c[0]] = cur;
        }
        char v[64];
        snprintf(v, sizeof v, "%.6g,\"orig\":%.6g", cur, first);
        if (j.size() > 1) j += ",";
        j += "{\"id\":" + JsonStr(c[0]) + ",\"group\":" + JsonStr(c[1]) + ",\"name\":" + JsonStr(c[2]) +
             ",\"type\":" + JsonStr(c[3]) + ",\"addr\":" + JsonStr(c[4]) + ",\"def\":" + JsonStr(c[5]) +
             ",\"min\":" + JsonStr(c[6]) + ",\"max\":" + JsonStr(c[7]) +
             ",\"note\":" + JsonStr(c.size() > 8 ? c[8] : "") + ",\"cur\":" + v + "}";
    }
    return j + "]";
}
void Reply(SOCKET s, const char* status, const char* type, const std::string& body)
{
    char h[256];
    snprintf(h, sizeof h,
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
             status, type, body.size());
    std::string out = h + body;
    size_t sent = 0;
    while (sent < out.size()) {
        const int n = send(s, out.data() + sent, int(out.size() - sent), 0);
        if (n <= 0) break;
        sent += size_t(n);
    }
}
const char* MimeOf(const std::string& p)
{
    if (p.size() >= 5 && p.compare(p.size() - 5, 5, ".html") == 0) return "text/html; charset=utf-8";
    if (p.size() >= 3 && p.compare(p.size() - 3, 3, ".js") == 0) return "text/javascript";
    if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".css") == 0) return "text/css";
    if (p.size() >= 5 && p.compare(p.size() - 5, 5, ".json") == 0) return "application/json";
    return "text/plain; charset=utf-8";
}
void Handle(SOCKET s)
{
    std::string req;
    char buf[8192];
    size_t hdrEnd = std::string::npos, need = 0;
    for (;;) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, size_t(n));
        if (hdrEnd == std::string::npos && (hdrEnd = req.find("\r\n\r\n")) != std::string::npos) {
            const size_t cl = req.find("Content-Length:");
            need = hdrEnd + 4 + (cl != std::string::npos && cl < hdrEnd ? size_t(atoi(req.c_str() + cl + 15)) : 0);
        }
        if (hdrEnd != std::string::npos && req.size() >= need) break;
        if (req.size() > (1 << 20)) break;
    }
    if (hdrEnd == std::string::npos) return;
    std::istringstream first(req.substr(0, req.find("\r\n")));
    std::string method, target;
    first >> method >> target;
    const std::string body = req.substr(hdrEnd + 4);
    std::string path = target, query;
    if (const size_t q = target.find('?'); q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }

    if (path == "/api/state") {
        uint64_t since = 0;
        if (const size_t p = query.find("since="); p != std::string::npos) since = strtoull(query.c_str() + p + 6, nullptr, 10);
        std::string j;
        {
            std::lock_guard<std::mutex> lk(g_live.mu);
            j = "{\"seq\":" + std::to_string(g_live.seq) + ",\"snap\":" + g_live.snap + ",\"events\":[";
            bool firstEv = true;
            for (auto& [sq, line] : g_live.events) {
                if (sq <= since) continue;
                if (!firstEv) j += ",";
                firstEv = false;
                j += JsonStr(line);
            }
            j += "],\"holds\":[";
            bool fh = true;
            for (auto& [k, v] : g_live.ratingHold) {
                if (!fh) j += ",";
                fh = false;
                j += "[" + std::to_string(k.first) + "," + std::to_string(k.second) + "," + std::to_string(v) + "]";
            }
            j += "]}";
        }
        Reply(s, "200 OK", "application/json", j);
    } else if (path == "/api/cam") {
        std::string j = "{\"V\":[";
        {
            std::lock_guard<std::mutex> lk(g_camMu);
            char b[32];
            for (int k = 0; k < 16; ++k) { snprintf(b, sizeof b, "%s%.6g", k ? "," : "", g_camV[k]); j += b; }
            j += "],\"P\":[";
            for (int k = 0; k < 16; ++k) { snprintf(b, sizeof b, "%s%.6g", k ? "," : "", g_camP[k]); j += b; }
        }
        j += "],\"pos\":[";
        for (int i = 0; i < 22; ++i) {
            const uint32_t p = PlayerAt(i);
            char b[64];
            snprintf(b, sizeof b, "%s[%.3f,%.3f,%.3f]", i ? "," : "", p ? F(p + 144) : 0.f, p ? F(p + 148) : 0.f, p ? F(p + 152) : 0.f);
            j += b;
        }
        j += "]}";
        Reply(s, "200 OK", "application/json", j);
    } else if (path == "/api/roster") {
        std::string j;
        {
            std::lock_guard<std::mutex> lk(g_live.mu);
            j = g_live.roster;
        }
        Reply(s, "200 OK", "application/json", j);
    } else if (path == "/api/tweaks") {
        Reply(s, "200 OK", "application/json", TweaksJson());
    } else if (path == "/api/cmd" && method == "POST") {
        // Commands only write memory, the clock rate or the hold table, so they apply here at once (the
        // simulation may be between plays, where the blocking update does not run).
        std::istringstream in(body);
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            ++n;
            ApplyNow(line);
        }
        Reply(s, "200 OK", "application/json", "{\"done\":" + std::to_string(n) + "}");
    } else if (method == "GET") {
        std::string file = path == "/" ? "/index.html" : path;
        if (file.find("..") != std::string::npos) { Reply(s, "403 Forbidden", "text/plain", "no"); return; }
        const std::string data = ReadFile(TweakDir() + file);
        if (data.empty()) Reply(s, "404 Not Found", "text/plain", "not found: " + file);
        else Reply(s, "200 OK", MimeOf(file), data);
    } else {
        Reply(s, "400 Bad Request", "text/plain", "bad request");
    }
}
}  // namespace

namespace {
void StartServer()
{
    std::thread([] {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return;
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(TweakPort()));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (ls == INVALID_SOCKET || bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 16) != 0) {
            fprintf(stderr, "[tweak] cannot listen on 127.0.0.1:%d\n", TweakPort());
            return;
        }
        fprintf(stderr, "[tweak] live tweaker on http://127.0.0.1:%d/ (files from %s)\n", TweakPort(), TweakDir().c_str());
        for (;;) {
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::thread([c] { Handle(c); closesocket(c); }).detach();
        }
    }).detach();
}
}  // namespace
#else
namespace {
void StartServer() { fprintf(stderr, "[tweak] the live tweaker is Windows-only for now\n"); }
}  // namespace
#endif

// ---- the in-game overlay (drawn by the swapchain over the window image; runs on the renderer thread) -----------
// Camera: the main 3-D pass reads obj = *(0x831DAEE8): view at obj+288, projection at obj+352 (16 floats each,
// row-major, D3D row vectors: clip = p * View * Proj). The render world is in yards with y up (players' pose
// blocks); field x (lateral) and y (downfield) map to world x / z (ov_map calibrates the signs).
namespace {
const char* PosName(uint32_t pos)
{
    static const char* k[] = {"QB", "HB", "FB", "WR", "TE", "LT", "LG", "C", "RG", "RT", "LE", "RE", "DT", "LOLB",
                              "MLB", "ROLB", "CB", "FS", "SS", "K", "P", "KR", "PR", "KOS", "LS"};
    return pos < sizeof k / sizeof *k ? k[pos] : "?";
}
const char* kRatingName[45] = {"STR", "AGI", "SPD", "ACC", "AWR", "CTH", "CAR", "THP", "THA", "KPW", "KAC", "TAK",
    "PBK", "RBK", "JMP", "KR", "TRUC", "ELUS", "BCV", "STIF", "SPIN", "JUKE", "IMP", "RBS", "RBF", "PBS", "PBF", "PWM",
    "FNM", "BKS", "PUR", "PRC", "MCOV", "ZCOV", "IMPO", "INJ", "STA", "TGH", "PRS", "RLS", "CIT", "SPC", "RTE", "HPOW",
    "BTR"};
const char* kStateName[10] = {"none", "free", "approaching", "approach", "ENGAGED", "wrap anim", "shed/wrap", "double (2nd)",
                              "double-teamed", "targeted"};

struct Ov {
    std::vector<OvVtx>* out;
    float M[16];
    float x0, y0, w, h;
    int map;
    float ts;
    float off[3];

    void Tri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t c)
    {
        out->push_back({ax, ay, c}); out->push_back({bx, by, c}); out->push_back({cx, cy, c});
    }
    void Rect(float x, float y, float rw, float rh, uint32_t c) { Tri(x, y, x + rw, y, x, y + rh, c); Tri(x + rw, y, x + rw, y + rh, x, y + rh, c); }
    void Line(float ax, float ay, float bx, float by, float t, uint32_t c)
    {
        float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
        if (l < 0.01f) return;
        float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
        Tri(ax + nx, ay + ny, bx + nx, by + ny, ax - nx, ay - ny, c);
        Tri(bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny, c);
    }
    float TextW(const char* s) const
    {
        float x = 0;
        for (; *s; ++s) { const int g = uint8_t(*s) - 32; x += (g >= 0 && g < 95 ? kOvFontW[g] : 6) * ts; }
        return x;
    }
    void TextRaw(float x, float y, const char* s, uint32_t c)
    {
        for (; *s; ++s) {
            const int g = uint8_t(*s) - 32;
            if (g < 0 || g >= 95) { x += 6 * ts; continue; }
            for (int r = 0; r < kOvFontH; ++r) {
                uint32_t bits = kOvFontRows[g][r];
                int col = 0;
                while (bits) {
                    if (!(bits & 1)) { bits >>= 1; ++col; continue; }
                    int run = 0;
                    while (bits & 1) { bits >>= 1; ++run; }
                    Rect(x + col * ts, y + r * ts, run * ts, ts, c);
                    col += run;
                }
            }
            x += kOvFontW[g] * ts;
        }
    }
    void Text(float x, float y, const char* s, uint32_t c)
    {
        TextRaw(x + ts, y + ts, s, RGBA(0, 0, 0, (c >> 24) * 3 / 4));
        TextRaw(x, y, s, c);
    }
    // field (x lateral, y downfield, height) -> window pixels; false when behind the camera
    bool Proj(float fx, float fy, float ht, float& px, float& py) const
    {
        float a = fx, b = fy;
        if (map & 4) std::swap(a, b);
        if (map & 1) a = -a;
        if (map & 2) b = -b;
        float X = a, Y = ht, Z = b;
        if (map & 8) { Y = b; Z = ht; }
        X += off[0]; Y += off[1]; Z += off[2];
        float cx, cy, cw;
        if (map & 16) {
            cx = M[0] * X + M[1] * Y + M[2] * Z + M[3];
            cy = M[4] * X + M[5] * Y + M[6] * Z + M[7];
            cw = M[12] * X + M[13] * Y + M[14] * Z + M[15];
        } else {
            cx = X * M[0] + Y * M[4] + Z * M[8] + M[12];
            cy = X * M[1] + Y * M[5] + Z * M[9] + M[13];
            cw = X * M[3] + Y * M[7] + Z * M[11] + M[15];
        }
        if (cw < 0.05f) return false;
        px = x0 + (cx / cw * 0.5f + 0.5f) * w;
        py = y0 + (0.5f - cy / cw * 0.5f) * h;
        return px > x0 - w && px < x0 + 2 * w && py > y0 - h && py < y0 + 2 * h;
    }
    void GroundRing(float fx, float fy, float r, float t, uint32_t c)
    {
        float px0 = 0, py0 = 0, px, py;
        bool ok0 = false;
        for (int k = 0; k <= 28; ++k) {
            const float a = k * 6.2831853f / 28;
            const bool ok = Proj(fx + cosf(a) * r, fy + sinf(a) * r, 0, px, py);
            if (ok && ok0) Line(px0, py0, px, py, t, c);
            px0 = px; py0 = py; ok0 = ok;
        }
    }
    void GroundCone(float fx, float fy, float face, float half, float r, uint32_t c)
    {
        float cxp, cyp;
        if (!Proj(fx, fy, 0, cxp, cyp)) return;
        float px0 = 0, py0 = 0, px, py;
        bool ok0 = false;
        const int n = 20;
        for (int k = 0; k <= n; ++k) {
            const float a = face - half + 2 * half * k / n;
            const bool ok = Proj(fx + cosf(a) * r, fy + sinf(a) * r, 0, px, py);
            if (ok && ok0) Tri(cxp, cyp, px0, py0, px, py, c);
            px0 = px; py0 = py; ok0 = ok;
        }
    }
    void GroundLine(float ax, float ay, float bx, float by, float t, uint32_t c)
    {
        float pax, pay, pbx, pby;
        if (Proj(ax, ay, 0.05f, pax, pay) && Proj(bx, by, 0.05f, pbx, pby)) Line(pax, pay, pbx, pby, t, c);
    }
};
// The main 3-D pass (sub_8293C948) hands its view and projection to SetProjection sub_829C9970 / SetView
// sub_829C9B20 (r4 = 16 floats) from 0x8293CBA0 / 0x8293CBAC; other passes (shadows, jumbotron, UI) call them
// too, so only those two call sites are kept.
}  // namespace

extern "C" PPC_FUNC(__imp__sub_829C9970);
extern "C" PPC_FUNC(__imp__sub_829C9B20);
PPC_FUNC(sub_829C9970)
{
    if (uint32_t(ctx.lr) == 0x8293CBA4 && TweakPort() && GuestOk(ctx.r4.u32, 64)) {
        std::lock_guard<std::mutex> lk(g_camMu);
        for (int k = 0; k < 16; ++k) g_camP[k] = F(ctx.r4.u32 + 4 * k);
        g_camOk = true;
    }
    __imp__sub_829C9970(ctx, base);
}
PPC_FUNC(sub_829C9B20)
{
    if (uint32_t(ctx.lr) == 0x8293CBB0 && TweakPort() && GuestOk(ctx.r4.u32, 64)) {
        std::lock_guard<std::mutex> lk(g_camMu);
        for (int k = 0; k < 16; ++k) g_camV[k] = F(ctx.r4.u32 + 4 * k);
    }
    __imp__sub_829C9B20(ctx, base);
}

void OverlayBuild(std::vector<OvVtx>& out, float x0, float y0, float w, float h)
{
    if (!TweakPort() || KnobV("ov_on") < 0.5f || !GLoad32(kPlayerMgr)) return;
    // live play only (not menus / cutscenes: the blocking update stops there); slow motion stretches the wait
    if (!g_b.live || g_b.gstate != 3 || MonoMs() - g_lastUpdateMs.load() > int64_t(400 / std::max(0.05, TimeWarp()))) return;
    Ov o{&out, {}, x0, y0, w, h, int(KnobV("ov_map")), std::max(0.5f, KnobV("ov_scale")) * h / 720.f,
          {KnobV("ov_dx"), KnobV("ov_dh"), KnobV("ov_dz")}};
    float V[16], P[16];
    {
        std::lock_guard<std::mutex> lk(g_camMu);
        if (!g_camOk) return;
        memcpy(V, g_camV, sizeof V);
        memcpy(P, g_camP, sizeof P);
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += V[r * 4 + k] * P[k * 4 + c];
            o.M[r * 4 + c] = s;
        }
    const uint32_t rules = GLoad32(kRuleMgr), sub = rules ? GLoad32(rules + 28) : 0;
    (void)sub;
    struct Pl { uint32_t p; float x, y, f; int team, pos, bs, bp; float pw; int bk, as; };
    Pl pl[22];
    int n = 0;
    for (int i = 0; i < 22; ++i) {
        const uint32_t p = PlayerAt(i);
        if (!GuestOk(p, 1000)) { pl[i].p = 0; continue; }
        const uint32_t bi = GLoad32(p + 988);
        const bool biOk = GuestOk(bi, 80);
        const uint32_t a = CurAssignment(p);
        pl[i] = {p, F(p + 144), F(p + 148), F(p + 160), GLoad8(p + 502), int(GLoad32(p + 756)),
                 biOk ? int(GLoad32(bi + 24)) : -1, -1, biOk ? F(bi + 64) : 0.f,
                 biOk ? int(GLoad32(bi + 44)) : 0, GuestOk(a, 20) ? int(GLoad32(a + 16)) : -1};
        if (biOk) pl[i].bp = PlayerIndex(GLoad32(bi + 28));
        ++n;
    }
    if (!n) return;
    // the offense: the team of the QB-position player nearest the ball holder is simplest; use block kinds instead:
    // a player whose PlayerBlockInfo kind (+44) is pass/run/lead blocking is on offense.
    int offTeam = -1;
    for (int i = 0; i < 22 && offTeam < 0; ++i)
        if (pl[i].p && pl[i].pos >= 5 && pl[i].pos <= 9) offTeam = pl[i].team;
    const int64_t now = MonoMs();
    int sel = int(KnobV("ov_sel"));
    if (sel == -2) {
        const uint32_t bs = GLoad32(kBallSys);
        (void)bs;   // the holder needs a guest call; the overlay uses the sim thread's last value instead
        sel = g_ovHolder.load();
    }
    const bool cones = KnobV("ov_cones") >= 0.5f, links = KnobV("ov_links") >= 0.5f, rings = KnobV("ov_rings") >= 0.5f,
               labels = KnobV("ov_labels") >= 0.5f, events = KnobV("ov_events") >= 0.5f, info = KnobV("ov_info") >= 0.5f;
    // cones: the area each player's own code looks at (ov_cones 1 = every player, 2 = the selected one + the QB).
    // QB: QB vision (AWR); pass blockers: target cone (AWR+PBK, 180-270 deg); run/lead blockers: run cone
    // (AWR+RBK, 150-210 deg, 15 yd); ball carrier: the 120 deg / 5 yd threat cone of his special moves; pass
    // rushers: the 35 deg / 4 yd cone a rush move needs. Coverage (Coverage\README.md): receivers: the 25 deg / 5 yd
    // cone + 2.5 yd ring of their only defender check (the look-back), coloured by the QB's read of them (green OPEN,
    // amber PARTIAL, red COVERED), plus their predicted catch point; defenders: the facing cone in which they see the
    // ball and break on it (man +-60, +-100 when his man is the target; flat/hook/CF +-90; deep +-100), their zone
    // rectangle (knob ov_zones) and a line to the man they cover. The selected receiver also shows the QB's 30 deg
    // throwing-lane cone and the 5 / 7 yd lane-window circles around the catch point, and who scored points on him.
    const float coneMode = KnobV("ov_cones");
    const int holderNow = g_ovHolder.load();
    RecvEval ev[22];
    DefView dv[22];
    int throwTgt = -1;
    {
        std::lock_guard<std::mutex> lk(g_covMu);
        for (int i = 0; i < 22; ++i) { ev[i] = g_eval[i]; dv[i] = g_defv[i]; }
        if (g_cp.play == g_b.play && g_cp.throwT >= 0) throwTgt = g_cp.tgt;
    }
    auto stCol = [](int st, uint32_t a) { return st == 0 ? RGBA(76, 209, 130, a) : st == 1 ? RGBA(240, 190, 60, a) : RGBA(255, 90, 90, a); };
    const bool zones = KnobV("ov_zones") >= 0.5f;
    if (cones)
        for (int i = 0; i < 22; ++i) {
            const Pl& q = pl[i];
            if (!q.p) continue;
            const bool isQB = q.pos == 0 && q.team == offTeam;
            if (coneMode >= 1.5f && i != sel && !isQB) continue;
            const float awr = g_rat[i][4];
            float half = 30, rad = 3;
            uint32_t col = RGBA(200, 200, 200, 28);   // facing only
            const bool fresh = ev[i].status >= 0 && now - ev[i].ms < 1500;
            if (i == holderNow && !isQB) { half = 60; rad = 5; col = RGBA(255, 230, 90, 55); }
            else if (isQB) { half = std::clamp(2.125f * (awr - 191), 20.f, 130.f); rad = 22; col = RGBA(255, 209, 102, 32); }
            else if (q.bk == 1) { half = std::clamp(270.f * (awr + g_rat[i][12]) / 510.f, 180.f, 270.f) / 2; rad = 7; col = RGBA(78, 161, 255, 30); }
            else if (q.bk == 2 || q.bk == 3) { half = std::clamp(105.f * (g_rat[i][13] + awr) / 255.f, 150.f, 210.f) / 2; rad = 15; col = RGBA(78, 161, 255, 14); }
            else if (q.team != offTeam && (q.as == 41 || q.as == 74)) { half = 35; rad = 4; col = RGBA(255, 107, 94, 70); }
            else if (q.team == offTeam && (fresh || q.as == 8 || q.as == 9 || q.as == 10 || q.as == 35 || q.as == 36 || q.as == 57)) {
                half = 25; rad = 5; col = fresh ? stCol(ev[i].status, 45) : RGBA(78, 161, 255, 30);
                o.GroundRing(q.x, q.y, 2.5f, 1.5f * o.ts, fresh ? stCol(ev[i].status, 120) : RGBA(78, 161, 255, 90));
                if (fresh && ev[i].cx != 0) {
                    o.GroundLine(q.x, q.y, ev[i].cx, ev[i].cy, 1.5f * o.ts, stCol(ev[i].status, 110));
                    o.GroundRing(ev[i].cx, ev[i].cy, 0.5f, 2.f * o.ts, stCol(ev[i].status, 200));
                }
            } else if (q.team != offTeam && dv[i].type >= 0) {
                const int a = dv[i].type;
                if (a == 19) { half = (dv[i].man >= 0 && dv[i].man == throwTgt) ? 100 : 60; rad = 8; col = RGBA(255, 120, 200, 40); }
                else if (a == 20 || a == 21 || a == 22) { half = 90; rad = 8; col = RGBA(255, 160, 60, 34); }
                else if (a == 23) { half = 100; rad = 10; col = RGBA(120, 200, 255, 30); }
                else if (a == 58) { half = 90; rad = 4; col = RGBA(255, 80, 80, 70); }   // playing the ball
            }
            o.GroundCone(q.x, q.y, q.f, half * 3.14159265f / 180, rad, col);
        }
    // coverage: zone rectangles, landmarks, man links
    if (zones)
        for (int i = 0; i < 22; ++i) {
            const Pl& q = pl[i];
            if (!q.p || q.team == offTeam || dv[i].type < 0) continue;
            if (coneMode >= 1.5f && i != sel) continue;
            const DefView& d = dv[i];
            const uint32_t zc = d.type == 23 ? RGBA(120, 200, 255, 120) : d.type == 21 ? RGBA(255, 200, 80, 120) : RGBA(255, 150, 60, 120);
            if (d.box) {
                const float y1 = std::min(d.y1, g_b.los + 40.f);   // deep boxes run to the end line: cut for drawing
                const float t = (i == sel ? 3.f : 1.5f) * o.ts;
                o.GroundLine(d.x0, d.y0, d.x1, d.y0, t, zc); o.GroundLine(d.x1, d.y0, d.x1, y1, t, zc);
                o.GroundLine(d.x1, y1, d.x0, y1, t, zc);    o.GroundLine(d.x0, y1, d.x0, d.y0, t, zc);
            }
            if (d.land) { o.GroundRing(d.lx, d.ly, 0.6f, 2.f * o.ts, zc); o.GroundLine(q.x, q.y, d.lx, d.ly, 1.f * o.ts, zc); }
            if (d.type == 19 && d.man >= 0 && pl[d.man].p) o.GroundLine(q.x, q.y, pl[d.man].x, pl[d.man].y, 2.f * o.ts, RGBA(255, 120, 200, 170));
        }
    // the selected receiver: the QB's throwing lane and who scores points against him
    if (cones && sel >= 0 && sel < 22 && pl[sel].p && pl[sel].team == offTeam && ev[sel].status >= 0 && now - ev[sel].ms < 1500 && ev[sel].cx != 0) {
        const RecvEval& e = ev[sel];
        const float hq = atan2f(e.cy - e.qy, e.cx - e.qx), L = hypotf(e.cx - e.qx, e.cy - e.qy);
        o.GroundCone(e.qx, e.qy, hq, 30 * 3.14159265f / 180, L, RGBA(255, 255, 255, 22));
        o.GroundCone(e.qx, e.qy, hq, 15 * 3.14159265f / 180, L, RGBA(255, 90, 90, 26));
        o.GroundRing(e.cx, e.cy, 5.f, 1.5f * o.ts, RGBA(255, 255, 255, 110));
        o.GroundRing(e.cx, e.cy, 7.f, 1.f * o.ts, RGBA(255, 90, 90, 110));
        for (auto& c : e.c)
            if (c.def >= 0 && pl[c.def].p)
                o.GroundLine(pl[c.def].x, pl[c.def].y, pl[sel].x, pl[sel].y, (c.cov ? 4.f : 1.5f + c.pts) * o.ts,
                             c.cov ? RGBA(255, 70, 70, 220) : RGBA(255, 190, 60, 200));
    }
    // block links
    if (links)
        for (int i = 0; i < 22; ++i) {
            const Pl& q = pl[i];
            if (!q.p || q.team != offTeam || q.bp < 0 || !pl[q.bp].p) continue;
            const Pl& d = pl[q.bp];
            uint32_t col = RGBA(240, 180, 41, 170);
            float t = 2;
            if (q.bs == 4) { col = q.pw > d.pw ? RGBA(76, 209, 130, 230) : RGBA(255, 90, 90, 230); t = 4; }
            else if (q.bs == 7 || q.bs == 8) { col = RGBA(183, 140, 255, 230); t = 4; }
            o.GroundLine(q.x, q.y, d.x, d.y, t * o.ts, col);
        }
    // rings, labels, flashes
    for (int i = 0; i < 22; ++i) {
        const Pl& q = pl[i];
        if (!q.p) continue;
        const bool off = q.team == offTeam;
        const uint32_t tc = off ? RGBA(78, 161, 255, 200) : RGBA(255, 107, 94, 200);
        if (rings) o.GroundRing(q.x, q.y, i == sel ? 1.1f : 0.8f, (i == sel ? 4.f : 2.5f) * o.ts, i == sel ? RGBA(183, 140, 255, 240) : tc);
        float hx, hy;
        if (!o.Proj(q.x, q.y, 2.3f, hx, hy)) continue;
        if (labels) {
            char b[16];
            snprintf(b, sizeof b, "%s", PosName(uint32_t(q.pos)));
            o.Text(hx - o.TextW(b) / 2, hy - 18 * o.ts, b, off ? RGBA(200, 225, 255) : RGBA(255, 210, 205));
        }
        if (events) {
            std::lock_guard<std::mutex> lk(g_noteMu);
            float yy = hy - 34 * o.ts;
            for (auto it = g_notes[i].rbegin(); it != g_notes[i].rend(); ++it) {
                if (now - it->ms > 1500) break;
                const char* tx = it->text.c_str();
                if (it->text.rfind("-> ", 0) == 0 || it->text.rfind("engaged", 0) == 0) continue;   // info box only
                o.Text(hx - o.TextW(tx) / 2, yy, tx, it->rgba);
                yy -= 16 * o.ts;
            }
        }
    }
    // the selected player's info box
    if (info && sel >= 0 && sel < 22 && pl[sel].p) {
        const Pl& q = pl[sel];
        std::vector<std::pair<std::string, uint32_t>> lines;
        char b[160];
        std::string an;
        {
            std::lock_guard<std::mutex> lk(g_noteMu);
            for (auto it = g_notes[sel].rbegin(); it != g_notes[sel].rend(); ++it)
                if (it->text.rfind("-> ", 0) == 0) { an = it->text.substr(3); break; }
        }
        snprintf(b, sizeof b, "%s #%d  %s  %s", PosName(uint32_t(q.pos)), sel, q.team == offTeam ? "offense" : "defense", an.c_str());
        lines.push_back({b, RGBA(255, 255, 255)});
        const float spd = sqrtf(F(q.p + 304) * F(q.p + 304) + F(q.p + 308) * F(q.p + 308));
        snprintf(b, sizeof b, "speed %.1f yd/s   block: %s%s", spd, q.bs >= 0 && q.bs < 10 ? kStateName[q.bs] : "-",
                 q.bp >= 0 ? (" #" + std::to_string(q.bp)).c_str() : "");
        lines.push_back({b, RGBA(200, 210, 220)});
        if (q.bs == 4 && q.bp >= 0 && pl[q.bp].p) {
            snprintf(b, sizeof b, "push power %.0f vs %.0f", q.pw, pl[q.bp].pw);
            lines.push_back({b, q.pw > pl[q.bp].pw ? RGBA(90, 230, 130) : RGBA(255, 120, 120)});
        }
        // ratings that drive this position's on-field code
        static const int kOL[] = {25, 26, 23, 24, 0, 4, 12, 13}, kDL[] = {27, 28, 29, 0, 11, 2, 4}, kQB[] = {7, 8, 4, 2},
                         kWR[] = {2, 5, 42, 39, 40, 41, 1}, kRB[] = {2, 17, 16, 18, 20, 21, 19, 6}, kDB[] = {2, 4, 11, 32, 33, 31, 38};
        const int* ks = kDB;
        size_t nk = sizeof kDB / sizeof *kDB;
        if (q.pos >= 5 && q.pos <= 9) { ks = kOL; nk = sizeof kOL / sizeof *kOL; }
        else if (q.pos >= 10 && q.pos <= 12) { ks = kDL; nk = sizeof kDL / sizeof *kDL; }
        else if (q.pos == 0) { ks = kQB; nk = sizeof kQB / sizeof *kQB; }
        else if (q.pos == 3 || q.pos == 4) { ks = kWR; nk = sizeof kWR / sizeof *kWR; }
        else if (q.pos == 1 || q.pos == 2) { ks = kRB; nk = sizeof kRB / sizeof *kRB; }
        std::string rl;
        for (size_t k = 0; k < nk; ++k) {
            snprintf(b, sizeof b, "%s %d  ", kRatingName[ks[k]], int(g_rat[sel][ks[k]] / 2.55f + 0.5f));
            rl += b;
        }
        lines.push_back({rl, RGBA(255, 214, 120)});
        // coverage: the QB's read of a receiver (who scored what), or what a defender covers
        static const char* kRead[3] = {"OPEN", "PARTIAL", "COVERED"};
        if (q.team == offTeam && ev[sel].status >= 0 && ev[sel].status < 3 && now - ev[sel].ms < 1500) {
            snprintf(b, sizeof b, "QB read: %s  (%d points)", kRead[ev[sel].status], ev[sel].pts);
            lines.push_back({b, stCol(ev[sel].status, 255)});
            int k = 0;
            for (auto& c : ev[sel].c) {
                if (k++ >= 3) break;
                if (c.def >= 0) snprintf(b, sizeof b, "  %s #%d: %s %s", PosName(uint32_t(pl[c.def].pos)), c.def,
                                         c.cov ? "COVERED" : ("+" + std::to_string(c.pts)).c_str(), c.why.c_str());
                else snprintf(b, sizeof b, "  %s", c.why.c_str());
                lines.push_back({b, c.cov ? RGBA(255, 120, 120) : RGBA(240, 200, 110)});
            }
        } else if (q.team != offTeam && dv[sel].type >= 0) {
            const DefView& d = dv[sel];
            static const char* kZone[15] = {"deep half L", "deep half R", "deep 3rd L", "deep 3rd M", "deep 3rd R",
                "deep 4th 1", "deep 4th 2", "deep 4th 3", "deep 4th 4", "flat L", "flat R", "curl-flat L", "curl-flat R", "none", "hook"};
            if (d.type == 19) snprintf(b, sizeof b, "man coverage on %s", d.man >= 0 ? ("#" + std::to_string(d.man)).c_str() : "?");
            else if (d.zone >= 0 && d.zone < 15) snprintf(b, sizeof b, "zone: %s (box %d)", kZone[d.zone], d.zone);
            else b[0] = 0;
            if (b[0]) lines.push_back({b, RGBA(150, 210, 255)});
        }
        {
            std::lock_guard<std::mutex> lk(g_noteMu);
            int k = 0;
            for (auto it = g_notes[sel].rbegin(); it != g_notes[sel].rend() && k < 4; ++it, ++k) {
                snprintf(b, sizeof b, "%4.1fs ago  %s", (now - it->ms) / 1000.0, it->text.c_str());
                lines.push_back({b, it->rgba});
            }
        }
        float hx, hy;
        if (o.Proj(q.x, q.y, 2.3f, hx, hy)) {
            float bw = 0;
            for (auto& l : lines) bw = std::max(bw, o.TextW(l.first.c_str()));
            const float lh = 17 * o.ts, bh = lh * lines.size() + 8 * o.ts;
            float bx = hx + 30 * o.ts, by = hy - bh / 2;
            if (bx + bw + 12 * o.ts > x0 + w) bx = hx - 30 * o.ts - bw - 12 * o.ts;
            by = std::clamp(by, y0 + 4, y0 + h - bh - 4);
            bx = std::max(bx, x0 + 4);
            o.Rect(bx, by, bw + 12 * o.ts, bh, RGBA(10, 14, 18, 190));
            o.Line(hx, hy, bx, by + bh / 2, 1.5f * o.ts, RGBA(183, 140, 255, 200));
            for (size_t k = 0; k < lines.size(); ++k) o.Text(bx + 6 * o.ts, by + 4 * o.ts + k * lh, lines[k].first.c_str(), lines[k].second);
        }
    }
}
