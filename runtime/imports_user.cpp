// Xam users and notifications: one signed-in local profile on user 0, and
// notification listeners with real queues.
//
// A console boots with a profile signed in more often than not, and the game
// asks for one (XamShowSigninUI) as soon as START is pressed. Like Xenia we
// keep a local (offline) profile signed in on user 0; HC09_SIGNIN=0 starts
// with nobody signed in instead, and the sign-in UI then signs user 0 in.
//
// Listeners: XamNotifyCreateListener returns a waitable handle (an event,
// signaled while the queue is non-empty). The first listener that covers the
// system area gets the notifications games expect at startup (Xenia does the
// same): XN_SYS_UI on/off and XN_SYS_SIGNINCHANGED.
#include "dev_env.h"
#include "kernel.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace {

constexpr uint32_t ERROR_SUCCESS      = 0;
constexpr uint32_t ERROR_NO_SUCH_USER = 0x525;

constexpr uint32_t XN_SYS_UI            = 0x00000009;
constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;

constexpr uint64_t kXuid = 0xE000C0AC4E000009ull;   // offline (0xE...) local profile
constexpr const char* kName = "Coach";

struct Listener
{
    uint64_t areas = 0;
    std::deque<std::pair<uint32_t, uint32_t>> queue;   // (id, param)
    std::shared_ptr<KObject> event;
};

std::mutex g_userMu;
std::map<uint32_t, Listener> g_listeners;              // by handle
bool g_startupSent = false;
bool g_signedIn = true;

bool SignedIn(uint32_t user)
{
    static const bool init = [] {
        if (const char* s = Hc09Env("HC09_SIGNIN")) g_signedIn = strcmp(s, "0") != 0;
        fprintf(stderr, "[xam] local profile \"%s\" (xuid %016llX) %s on user 0\n", kName,
                (unsigned long long)kXuid, g_signedIn ? "signed in" : "not signed in");
        return true;
    }();
    (void)init;
    return user == 0 && g_signedIn;
}

// Notification area of an id: XNID(version, area, index) = version<<25 | area<<16 | index.
uint64_t AreaBit(uint32_t id) { return 1ull << ((id >> 16) & 0x3F); }

void EnqueueLocked(Listener& l, uint32_t id, uint32_t param)
{
    if (!(l.areas & AreaBit(id))) return;
    l.queue.emplace_back(id, param);
    SetEventObj(l.event);
}

void Broadcast(uint32_t id, uint32_t param)
{
    std::lock_guard<std::mutex> lk(g_userMu);
    fprintf(stderr, "[xam] notification 0x%08X param %u -> %zu listener(s)\n", id, param, g_listeners.size());
    for (auto& [h, l] : g_listeners) EnqueueLocked(l, id, param);
}

} // namespace

// DWORD XamUserGetSigninState(DWORD userIndex r3): 0 none, 1 local, 2 Live.
PPC_FUNC(__imp__XamUserGetSigninState)
{
    ImportLog("XamUserGetSigninState");
    ctx.r3.u64 = SignedIn(ctx.r3.u32) ? 1 : 0;
}

// DWORD XamUserGetSigninInfo(DWORD userIndex r3, DWORD flags r4, XUSER_SIGNIN_INFO* r5)
// { XUID xuid; DWORD dwInfoFlags; XUSER_SIGNIN_STATE state; DWORD dwGuestNumber;
//   DWORD dwSponsorUserIndex; CHAR szUserName[16]; }  (0x28 bytes)
PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    ImportLog("XamUserGetSigninInfo");
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if (out) for (int i = 0; i < 0x28; i += 4) GStore32(out + i, 0);
    if (!SignedIn(user)) { ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    if (out)
    {
        GStore64(out + 0x00, kXuid);
        GStore32(out + 0x0C, 1);                       // signed in locally
        for (size_t i = 0; kName[i] && i < 15; ++i) GStore8(out + 0x18 + uint32_t(i), uint8_t(kName[i]));
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamUserGetXUID(DWORD userIndex r3, DWORD typeMask r4, XUID* r5)
PPC_FUNC(__imp__XamUserGetXUID)
{
    ImportLog("XamUserGetXUID");
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if (!SignedIn(user)) { if (out) GStore64(out, 0); ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    if (out) GStore64(out, kXuid);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamUserGetName(DWORD userIndex r3, LPSTR buffer r4, DWORD bufferLen r5)
PPC_FUNC(__imp__XamUserGetName)
{
    ImportLog("XamUserGetName");
    uint32_t user = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32;
    if (!SignedIn(user)) { if (buf && len) GStore8(buf, 0); ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    if (buf && len)
    {
        uint32_t i = 0;
        for (; kName[i] && i + 1 < len; ++i) GStore8(buf + i, uint8_t(kName[i]));
        GStore8(buf + i, 0);
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamUserCheckPrivilege(DWORD userIndex r3, DWORD privilege r4, BOOL* result r5)
PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    ImportLog("XamUserCheckPrivilege");
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if (!SignedIn(user)) { if (out) GStore32(out, 0); ctx.r3.u64 = ERROR_NO_SUCH_USER; return; }
    if (out) GStore32(out, 1);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamShowSigninUI(DWORD usersNeeded r3, DWORD flags r4)
// The guide would show and the user would pick a profile. Sign user 0 in (if
// not already) and report the UI opening and closing.
PPC_FUNC(__imp__XamShowSigninUI)
{
    ImportLog("XamShowSigninUI");
    fprintf(stderr, "[xam] XamShowSigninUI(users %u, flags 0x%X) from 0x%08X: signing in user 0\n",
            ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
    bool changed = !SignedIn(0);
    g_signedIn = true;
    Broadcast(XN_SYS_UI, 1);
    if (changed) Broadcast(XN_SYS_SIGNINCHANGED, 1);
    Broadcast(XN_SYS_UI, 0);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// HANDLE XamNotifyCreateListener(QWORD areas r3, DWORD maxVersion r4)
PPC_FUNC(__imp__XamNotifyCreateListener)
{
    ImportLog("XamNotifyCreateListener");
    uint64_t areas = ctx.r3.u64;
    uint32_t h;
    std::shared_ptr<KObject> ev;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        ev = NewObject(T_NotificationEvent);
        ev->manualReset = true;
        h = NewHandle(ev);
    }
    SignedIn(0);
    {
        std::lock_guard<std::mutex> lk(g_userMu);
        Listener& l = g_listeners[h];
        l.areas = areas;
        l.event = ev;
        if (!g_startupSent && (areas & 1))
        {
            g_startupSent = true;
            EnqueueLocked(l, XN_SYS_UI, 1);
            EnqueueLocked(l, XN_SYS_UI, 0);
            if (g_signedIn) EnqueueLocked(l, XN_SYS_SIGNINCHANGED, 1);
        }
    }
    fprintf(stderr, "[xam] notify listener 0x%X for areas 0x%llX (max version %u) from 0x%08X\n", h,
            (unsigned long long)areas, ctx.r4.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = h;
}

// BOOL XNotifyGetNext(HANDLE listener r3, DWORD matchId r4, DWORD* id r5, ULONG_PTR* param r6)
PPC_FUNC(__imp__XNotifyGetNext)
{
    ImportLog("XNotifyGetNext");
    uint32_t h = ctx.r3.u32, match = ctx.r4.u32, pId = ctx.r5.u32, pParam = ctx.r6.u32;
    std::lock_guard<std::mutex> lk(g_userMu);
    auto it = g_listeners.find(h);
    if (it == g_listeners.end()) { ctx.r3.u64 = 0; return; }
    Listener& l = it->second;
    for (auto q = l.queue.begin(); q != l.queue.end(); ++q)
    {
        if (match && q->first != match) continue;
        if (pId) GStore32(pId, q->first);
        if (pParam) GStore32(pParam, q->second);
        fprintf(stderr, "[xam] XNotifyGetNext(0x%X) -> 0x%08X param %u\n", h, q->first, q->second);
        l.queue.erase(q);
        if (l.queue.empty()) ResetEventObj(l.event);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}
