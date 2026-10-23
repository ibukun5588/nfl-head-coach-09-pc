// Xam input: one virtual gamepad on user 0, plus scripted button presses.
//
// A console always has a pad connected, and the front end waits for it, so
// by default user 0 reports a connected gamepad with nothing held. Users 1-3
// are not connected. HC09_PAD=0 disconnects the virtual pad as well.
//
// HC09_INPUT="<sec>:<button>[,<button>...] ..." presses buttons at the given
// number of seconds after boot, e.g. HC09_INPUT="140:start 146:a 150:dup".
// A time prefixed with "f" is a GPU swap number instead ("f4500:start"):
// frame-locked presses stay in step with the game even when a frame capture
// (HC09_CAPTURE_FRAME) stalls the GPU thread for seconds. A time prefixed with
// "+" ("+2.5:a", "f+500:a") is relative to the moment the previous press fired,
// so a chain of menu presses follows the first one wherever the boot put it.
// Presses fire in script order, each at least HC09_INPUT_GAP_MS (default 500 ms)
// and 10 swaps after the previous one was released: a press is only noticed
// when the game polls the pad, and overdue presses must not arrive in the same
// poll (the title once received START, A, START at once on a slow boot).
// Each press is held for HC09_INPUT_HOLD_MS (default 150 ms) in
// XamInputGetState and produces a KEYDOWN/KEYUP pair for
// XamInputGetKeystroke(Ex). Buttons: a b x y start back lb rb ls rs
// dup ddown dleft dright.
//
// Host input (host_input.cpp: an XInput controller and the window's keyboard)
// is merged into the same pad: buttons are ORed with the script, triggers and
// thumbsticks come from the host. Edges of the host state (buttons, triggers,
// 8-way thumbstick directions) produce keystrokes, repeated while held like
// the console's (first repeat after 400 ms, then every 100 ms).
// XamInputSetState drives the controller's rumble. HC09_INPUT_DEBUG=1 logs
// host keystrokes too.
//
// HC09_INPUT_FILE=<path>: presses typed into a file while the game runs (driving
// it step by step without a restart). Every 100 ms the lines appended since the
// last look are read; each whitespace-separated word is a button (queued like a
// script press, in order), "dump" (the next presented frame as a PNG in
// HC09_GPU_LIVE_DIR, gpu_live.cpp) or "wait:<ms>" (pause before the next word).
#include "dev_env.h"
#include "kernel.h"
#include "gpu.h"
#include "host_input.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t ERROR_SUCCESS              = 0;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;
constexpr uint32_t ERROR_EMPTY                = 0x10D2;

struct ButtonDef { const char* name; uint16_t mask; uint16_t vk; };
// XINPUT_GAMEPAD_* masks and VK_PAD_* virtual keys.
constexpr ButtonDef kButtons[] = {
    { "dup",    0x0001, 0x5810 }, { "ddown", 0x0002, 0x5811 }, { "dleft", 0x0004, 0x5812 },
    { "dright", 0x0008, 0x5813 }, { "start", 0x0010, 0x5814 }, { "back",  0x0020, 0x5815 },
    { "ls",     0x0040, 0x5816 }, { "rs",    0x0080, 0x5817 }, { "lb",    0x0100, 0x5804 },
    { "rb",     0x0200, 0x5805 }, { "a",     0x1000, 0x5800 }, { "b",     0x2000, 0x5801 },
    { "x",      0x4000, 0x5802 }, { "y",     0x8000, 0x5803 },
};

struct Press
{
    double at; uint16_t mask; bool atSwap = false, relative = false;
    double downAt = 0, downSwap = 0, upAt = 0, upSwap = 0;
    bool downSent = false, upSent = false;
};
struct Keystroke { uint16_t vk, flags; bool scripted; };

constexpr uint16_t KEYDOWN = 0x1, KEYUP = 0x2, REPEAT = 0x4;
constexpr size_t   kMaxQueuedKeys = 64;   // the game may not read keystrokes at all (in play)

// The full XINPUT_GAMEPAD as the game sees it.
struct PadState
{
    uint16_t buttons = 0;
    uint8_t  lt = 0, rt = 0;
    int16_t  lx = 0, ly = 0, rx = 0, ry = 0;
    bool operator==(const PadState&) const = default;
};

// Virtual key of a thumbstick's 8-way direction (VK_PAD_LTHUMB_UP 0x5820 ..
// DOWNLEFT 0x5827; the right stick is 0x5830 ..), 0 = centred.
uint16_t ThumbKey(int16_t x, int16_t y, uint16_t base)
{
    constexpr int T = 0x4000;
    bool u = y > T, d = y < -T, r = x > T, l = x < -T;
    if (u && l) return base + 4;
    if (u && r) return base + 5;
    if (d && r) return base + 6;
    if (d && l) return base + 7;
    if (u) return base + 0;
    if (d) return base + 1;
    if (r) return base + 2;
    if (l) return base + 3;
    return 0;
}

// Keystrokes from the host controller/keyboard: a virtual key per button,
// trigger and stick, KEYDOWN/REPEAT/KEYUP on its edges.
struct HostKeys
{
    static constexpr int kSlots = 18;     // 14 buttons + 2 triggers + 2 sticks
    uint16_t down[kSlots] = {};           // virtual key held in each slot, 0 = none
    double repeatAt[kSlots] = {};
    bool debug = false;

    void Update(const HostPadState& h, double now, std::deque<Keystroke>& keys)
    {
        uint16_t want[kSlots] = {};
        int n = 0;
        for (auto& b : kButtons) want[n++] = (h.buttons & b.mask) ? b.vk : 0;
        want[n++] = h.lt > 30 ? 0x5806 : 0;   // VK_PAD_LTRIGGER (XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        want[n++] = h.rt > 30 ? 0x5807 : 0;   // VK_PAD_RTRIGGER
        want[n++] = ThumbKey(h.lx, h.ly, 0x5820);
        want[n++] = ThumbKey(h.rx, h.ry, 0x5830);
        for (int i = 0; i < kSlots; ++i)
        {
            if (down[i] && down[i] != want[i]) Push(keys, down[i], KEYUP);
            if (want[i] && down[i] != want[i])
            {
                Push(keys, want[i], KEYDOWN);
                repeatAt[i] = now + 0.4;
            }
            else if (want[i] && now >= repeatAt[i])
            {
                Push(keys, want[i], KEYDOWN | REPEAT);
                repeatAt[i] = now + 0.1;
            }
            down[i] = want[i];
        }
    }

    void Push(std::deque<Keystroke>& keys, uint16_t vk, uint16_t flags)
    {
        if (keys.size() >= kMaxQueuedKeys) keys.pop_front();
        keys.push_back({ vk, flags, false });
        if (debug) fprintf(stderr, "[input] host keystroke vk 0x%04X flags %u\n", vk, flags);
    }
};

struct Pad
{
    bool connected = true;
    double holdSec = 0.15, gapSec = 0.5;
    std::vector<Press> script;
    std::deque<Keystroke> keys;
    uint32_t packet = 0;
    PadState last;
    HostKeys hostKeys;
    std::mutex mu;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    Pad()
    {
        if (const char* p = Hc09Env("HC09_PAD")) connected = strcmp(p, "0") != 0;
        if (const char* h = Hc09Env("HC09_INPUT_HOLD_MS")) holdSec = atof(h) / 1000.0;
        if (const char* g = Hc09Env("HC09_INPUT_GAP_MS")) gapSec = atof(g) / 1000.0;
        if (const char* d = Hc09Env("HC09_INPUT_DEBUG")) hostKeys.debug = strcmp(d, "0") != 0;
        if (const char* s = Hc09Env("HC09_INPUT"))
        {
            std::istringstream in(s);
            std::string tok;
            while (in >> tok)
            {
                size_t c = tok.find(':');
                if (c == std::string::npos) continue;
                bool atSwap = tok[0] == 'f';
                size_t t = atSwap ? 1 : 0;
                bool relative = tok[t] == '+';
                if (relative) ++t;
                Press p{ atof(tok.substr(t, c - t).c_str()), 0, atSwap, relative };
                std::string names = tok.substr(c + 1);
                size_t pos = 0;
                while (pos <= names.size())
                {
                    size_t e = names.find(',', pos);
                    std::string n = names.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                    for (auto& b : kButtons) if (n == b.name) p.mask |= b.mask;
                    if (e == std::string::npos) break;
                    pos = e + 1;
                }
                if (p.mask) script.push_back(p);
                else fprintf(stderr, "[input] HC09_INPUT: no known button in \"%s\"\n", tok.c_str());
            }
        }
#if HC09_DEV_TOOLS
        fprintf(stderr, "[input] virtual pad on user 0: %s, %zu scripted press(es)\n",
                connected ? "connected" : "disconnected", script.size());
#else
        fprintf(stderr, "[input] pad on user 0: %s\n", connected ? "connected" : "disconnected");
#endif
    }

    // The pad now (script + host); queues keystroke events for presses starting/ending.
    PadState Update()
    {
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double swaps = double(GpuSwapCount());
        uint16_t held = 0;
#if HC09_DEV_TOOLS   // scripted presses (HC09_INPUT, HC09_INPUT_FILE, the live tweaker): not in the release build
        for (size_t i = 0; i < script.size(); ++i)
        {
            Press& p = script[i];
            const Press* prev = i ? &script[i - 1] : nullptr;
            if (!p.downSent)
            {
                if (prev && (!prev->upSent || now < prev->upAt + gapSec || swaps < prev->upSwap + 10)) break;
                double base = p.relative && prev ? (p.atSwap ? prev->downSwap : prev->downAt) : 0;
                if ((p.atSwap ? swaps : now) < base + p.at) break;
                p.downSent = true;
                p.downAt = now;
                p.downSwap = swaps;
                for (auto& b : kButtons) if (p.mask & b.mask) keys.push_back({ b.vk, KEYDOWN, true });
                fprintf(stderr, "[input] t=%.1fs swap %.0f press 0x%04X\n", now, swaps, p.mask);
            }
            if (now >= p.downAt + holdSec && !p.upSent)
            {
                p.upSent = true;
                p.upAt = now;
                p.upSwap = swaps;
                for (auto& b : kButtons) if (p.mask & b.mask) keys.push_back({ b.vk, KEYUP, true });
            }
            if (!p.upSent) held |= p.mask;
        }
#endif
        PadState st;
        st.buttons = held;
        if (HostInputEnabled())
        {
            HostPadState h = HostInputPoll();
            hostKeys.Update(h, now, keys);
            st.buttons |= h.buttons;
            st.lt = h.lt; st.rt = h.rt;
            st.lx = h.lx; st.ly = h.ly; st.rx = h.rx; st.ry = h.ry;
        }
        if (!(st == last)) { ++packet; last = st; }
        return st;
    }
};

Pad& ThePad() { static Pad pad; return pad; }

bool PadPresent(uint32_t user) { return ThePad().connected && (user == 0 || user == 0xFF); }

} // namespace

#if HC09_DEV_TOOLS
// Queue a scripted press now (after any pending script presses), e.g. from a diagnostic watchdog.
// False for an unknown button name.
bool InputInjectPress(const char* button)
{
    Pad& pad = ThePad();
    std::lock_guard<std::mutex> lk(pad.mu);
    uint16_t mask = 0;
    for (auto& b : kButtons) if (strcmp(button, b.name) == 0) mask = b.mask;
    if (!mask) return false;
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - pad.t0).count();
    pad.script.push_back(Press{ now, mask });
    return true;
}

void GpuLiveRequestDump();   // gpu_live.cpp

namespace {
// HC09_INPUT_FILE: words appended to the file while the game runs (see the top of this file).
struct InputFile
{
    InputFile()
    {
        const char* path = Hc09Env("HC09_INPUT_FILE");
        if (!path || !*path) return;
        std::string p = path;
        fprintf(stderr, "[input] HC09_INPUT_FILE: reading presses from %s\n", p.c_str());
        std::thread([p] {
            long offset = 0;
            if (FILE* f = fopen(p.c_str(), "rb")) { fseek(f, 0, SEEK_END); offset = ftell(f); fclose(f); }   // only what comes later
            std::string pending;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                FILE* f = fopen(p.c_str(), "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long end = ftell(f);
                if (end < offset) offset = 0;   // the file was rewritten
                std::string chunk(size_t(end - offset), '\0');
                fseek(f, offset, SEEK_SET);
                chunk.resize(fread(chunk.data(), 1, chunk.size(), f));
                fclose(f);
                offset += long(chunk.size());
                pending += chunk;
                size_t nl = pending.rfind('\n');
                if (nl == std::string::npos) continue;   // only whole lines
                std::istringstream in(pending.substr(0, nl));
                pending.erase(0, nl + 1);
                std::string w;
                while (in >> w)
                {
                    if (w == "dump") GpuLiveRequestDump();
                    else if (w.rfind("wait:", 0) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(atoi(w.c_str() + 5)));
                    else if (!InputInjectPress(w.c_str())) { fprintf(stderr, "[input] HC09_INPUT_FILE: unknown word \"%s\"\n", w.c_str()); continue; }
                    fprintf(stderr, "[input] HC09_INPUT_FILE: %s (swap %llu)\n", w.c_str(), (unsigned long long)GpuSwapCount());
                }
            }
        }).detach();
    }
} g_inputFile;
}
#endif  // HC09_DEV_TOOLS

// DWORD XamInputGetState(DWORD userIndex r3, DWORD flags r4, XINPUT_STATE* r5)
// XINPUT_STATE { DWORD dwPacketNumber; XINPUT_GAMEPAD { WORD wButtons;
//   BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } }
PPC_FUNC(__imp__XamInputGetState)
{
    ImportLog("XamInputGetState");
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if (!PadPresent(user)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    Pad& pad = ThePad();
    std::lock_guard<std::mutex> lk(pad.mu);
    PadState st = pad.Update();
    if (out)
    {
        GStore32(out + 0, pad.packet);
        GStore16(out + 4, st.buttons);
        GStore8(out + 6, st.lt);
        GStore8(out + 7, st.rt);
        GStore16(out + 8, uint16_t(st.lx));
        GStore16(out + 10, uint16_t(st.ly));
        GStore16(out + 12, uint16_t(st.rx));
        GStore16(out + 14, uint16_t(st.ry));
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamInputSetState(DWORD userIndex r3, DWORD flags r4, XINPUT_VIBRATION* r5)
// XINPUT_VIBRATION { WORD wLeftMotorSpeed; WORD wRightMotorSpeed; }
PPC_FUNC(__imp__XamInputSetState)
{
    ImportLog("XamInputSetState");
    uint32_t user = ctx.r3.u32, vib = ctx.r5.u32;
    if (!PadPresent(user)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    if (vib) HostInputRumble(GLoad16(vib), GLoad16(vib + 2));
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamInputGetCapabilities(DWORD userIndex r3, DWORD flags r4, XINPUT_CAPABILITIES* r5)
// { BYTE Type; BYTE SubType; WORD Flags; XINPUT_GAMEPAD Gamepad; XINPUT_VIBRATION Vibration; }
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    ImportLog("XamInputGetCapabilities");
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if (!PadPresent(user)) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    if (out)
    {
        for (int i = 0; i < 20; i += 4) GStore32(out + i, 0);
        GStore8(out + 0, 1);            // XINPUT_DEVTYPE_GAMEPAD
        GStore8(out + 1, 1);            // XINPUT_DEVSUBTYPE_GAMEPAD
        GStore16(out + 4, 0xFFFF);      // every button
        GStore8(out + 6, 0xFF);         // triggers
        GStore8(out + 7, 0xFF);
        for (int i = 8; i < 16; i += 2) GStore16(out + i, 0xFFC0);   // thumbstick resolution
        GStore16(out + 16, 0xFFFF);     // vibration motors
        GStore16(out + 18, 0xFFFF);
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

// XINPUT_KEYSTROKE { WORD VirtualKey; WCHAR Unicode; WORD Flags; BYTE UserIndex; BYTE HidCode; }
static uint32_t NextKeystroke(uint32_t user, uint32_t out, uint32_t* userOut)
{
    if (!PadPresent(user)) return ERROR_DEVICE_NOT_CONNECTED;
    Pad& pad = ThePad();
    std::lock_guard<std::mutex> lk(pad.mu);
    pad.Update();
    if (pad.keys.empty()) return ERROR_EMPTY;
    Keystroke k = pad.keys.front();
    pad.keys.pop_front();
    if (out)
    {
        GStore16(out + 0, k.vk);
        GStore16(out + 2, 0);
        GStore16(out + 4, k.flags);
        GStore8(out + 6, 0);
        GStore8(out + 7, 0);
    }
    if (userOut) *userOut = 0;
    if (k.scripted)
        fprintf(stderr, "[input] keystroke vk 0x%04X %s delivered\n", k.vk, k.flags == KEYDOWN ? "down" : "up");
    return ERROR_SUCCESS;
}

// DWORD XamInputGetKeystrokeEx(DWORD* userIndex r3, DWORD flags r4, XINPUT_KEYSTROKE* r5)
// *userIndex is in/out: the requested user (0xFF = any), then who produced it.
PPC_FUNC(__imp__XamInputGetKeystrokeEx)
{
    ImportLog("XamInputGetKeystrokeEx");
    uint32_t pUser = ctx.r3.u32;
    uint32_t user = pUser ? GLoad32(pUser) : 0xFF, from = 0;
    uint32_t r = NextKeystroke(user, ctx.r5.u32, &from);
    if (r == ERROR_SUCCESS && pUser) GStore32(pUser, from);
    ctx.r3.u64 = r;
}

// DWORD XamInputGetKeystroke(DWORD userIndex r3, DWORD flags r4, XINPUT_KEYSTROKE* r5)
PPC_FUNC(__imp__XamInputGetKeystroke)
{
    ImportLog("XamInputGetKeystroke");
    ctx.r3.u64 = NextKeystroke(ctx.r3.u32, ctx.r5.u32, nullptr);
}
