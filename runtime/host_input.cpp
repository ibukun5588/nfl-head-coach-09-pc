// Host input: a controller + game-window keyboard -> one Xbox 360 pad.
//
// Controller, two backends (HC09_PAD_API / hc09.ini [input] controller):
// - SDL3 (SDL3.dll next to hc09.exe, loaded at run time): any gamepad SDL
//   knows -- PlayStation (DualShock 4, DualSense; DualShock 3 with the DsHidMini
//   driver in SDL mode), Switch Pro, Xbox, ... -- mapped by position onto the
//   360 layout: Cross/Circle/Square/Triangle = A/B/X/Y, L1/R1 = LB/RB, L2/R2 =
//   LT/RT, L3/R3 = stick clicks, Share/Create = BACK, Options = START, the
//   touchpad click = BACK too. Rumble goes to the pad.
// - XInput (xinput1_4.dll): the first connected XInput pad; the 360 layout is
//   XInput's, so buttons, triggers and thumbsticks map 1:1. Polling an empty
//   XInput slot takes long (~1 ms), so the slot in use is read at most every
//   2 ms and the others are scanned every 2 s.
// auto (default): SDL's first gamepad when SDL3.dll loads and a pad is
// connected, otherwise XInput. sdl / xinput: only that backend.
//
// Keyboard (only while the game window has focus):
//   arrows        D-pad              W A S D       left stick
//   numpad 8 4 2 6 right stick       Space / K     A
//   Esc / L       B                  J             X
//   I             Y                  Enter         START
//   Backspace/Tab BACK               Q / E         LB / RB
//   Z / C         LT / RT            Shift         left stick click
//   numpad 5      right stick click
//
// HC09_HOST_INPUT=0 turns host input off, =1 forces it on. Default: on when
// the game presents in a visible window (HC09_GPU_LIVE=1 and HC09_GPU_WINDOW
// not 0/hidden), so unattended runs stay driven by HC09_INPUT alone.
#include "dev_env.h"
#include "host_input.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(HC09_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#if defined(HC09_HAVE_VULKAN)
#include "vk_platform.h"
#endif

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

#if defined(HC09_HAVE_SDL3)
// The SDL3 functions used, resolved from SDL3.dll at run time (hc09.exe starts
// without it). The header gives the types; SDL3's ABI is stable across 3.x.
#define HC09_SDL_FNS(X)                                                                       \
    X(SDL_SetHint) X(SDL_Init) X(SDL_GetError) X(SDL_GetGamepads) X(SDL_free)                  \
    X(SDL_OpenGamepad) X(SDL_CloseGamepad) X(SDL_GamepadConnected) X(SDL_UpdateGamepads)        \
    X(SDL_GetGamepadButton) X(SDL_GetGamepadAxis) X(SDL_GetGamepadType) X(SDL_GetGamepadName)   \
    X(SDL_RumbleGamepad) X(SDL_GetVersion) X(SDL_GetGamepadNameForID)

// HC09_PAD_VIRTUAL=ps3|ps4|ps5|xbox: an SDL virtual gamepad of that kind (to test
// without the pad), which Sdl::Ensure then finds like a real one. HC09_PAD_SCRIPT=
// "<sec>:<input>[,<input>...] ..." presses it, each step held for 200 ms, seconds
// after the first controller poll. Inputs: cross circle square triangle (or a b
// x y), l1 r1 l2 r2 l3 r3, share options ps touchpad, up down left right (D-pad),
// lx- lx+ ly- ly+ rx- rx+ ry- ry+ (a stick all the way; y- = up). Rumble the game
// asks for is logged.
struct VirtualPad
{
    struct In { bool axis; int index; int16_t value; };
    struct Step { double at; std::vector<In> ins; bool down = false, up = false; };
    decltype(&::SDL_AttachVirtualJoystick) attach = nullptr;
    decltype(&::SDL_OpenJoystick) open = nullptr;
    decltype(&::SDL_SetJoystickVirtualButton) button = nullptr;
    decltype(&::SDL_SetJoystickVirtualAxis) axis = nullptr;
    SDL_Joystick* joy = nullptr;
    std::vector<Step> steps;

    static bool SDLCALL Rumble(void*, Uint16 lo, Uint16 hi)
    {
        static Uint16 lastLo = 0, lastHi = 0;
        if (lo != lastLo || hi != lastHi) fprintf(stderr, "[input] virtual pad rumble %u %u\n", lo, hi);
        lastLo = lo; lastHi = hi;
        return true;
    }

    static bool Parse(const std::string& name, In& in)
    {
        struct Name { const char* n; bool axis; int index; int16_t value; };
        static constexpr Name kNames[] = {
            { "cross", false, SDL_GAMEPAD_BUTTON_SOUTH, 1 },   { "a", false, SDL_GAMEPAD_BUTTON_SOUTH, 1 },
            { "circle", false, SDL_GAMEPAD_BUTTON_EAST, 1 },   { "b", false, SDL_GAMEPAD_BUTTON_EAST, 1 },
            { "square", false, SDL_GAMEPAD_BUTTON_WEST, 1 },   { "x", false, SDL_GAMEPAD_BUTTON_WEST, 1 },
            { "triangle", false, SDL_GAMEPAD_BUTTON_NORTH, 1 }, { "y", false, SDL_GAMEPAD_BUTTON_NORTH, 1 },
            { "l1", false, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 1 }, { "r1", false, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 1 },
            { "l3", false, SDL_GAMEPAD_BUTTON_LEFT_STICK, 1 },  { "r3", false, SDL_GAMEPAD_BUTTON_RIGHT_STICK, 1 },
            { "share", false, SDL_GAMEPAD_BUTTON_BACK, 1 },     { "options", false, SDL_GAMEPAD_BUTTON_START, 1 },
            { "ps", false, SDL_GAMEPAD_BUTTON_GUIDE, 1 },       { "touchpad", false, SDL_GAMEPAD_BUTTON_TOUCHPAD, 1 },
            { "up", false, SDL_GAMEPAD_BUTTON_DPAD_UP, 1 },     { "down", false, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 1 },
            { "left", false, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1 }, { "right", false, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1 },
            { "l2", true, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767 }, { "r2", true, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767 },
            { "lx-", true, SDL_GAMEPAD_AXIS_LEFTX, -32768 },   { "lx+", true, SDL_GAMEPAD_AXIS_LEFTX, 32767 },
            { "ly-", true, SDL_GAMEPAD_AXIS_LEFTY, -32768 },   { "ly+", true, SDL_GAMEPAD_AXIS_LEFTY, 32767 },
            { "rx-", true, SDL_GAMEPAD_AXIS_RIGHTX, -32768 },  { "rx+", true, SDL_GAMEPAD_AXIS_RIGHTX, 32767 },
            { "ry-", true, SDL_GAMEPAD_AXIS_RIGHTY, -32768 },  { "ry+", true, SDL_GAMEPAD_AXIS_RIGHTY, 32767 },
        };
        for (const Name& k : kNames)
            if (name == k.n) { in = { k.axis, k.index, k.value }; return true; }
        return false;
    }

    // Attaches the pad (after SDL_Init); false if it could not be.
    bool Attach(HMODULE m, const char* kind, const char* script)
    {
        attach = reinterpret_cast<decltype(attach)>(GetProcAddress(m, "SDL_AttachVirtualJoystick"));
        open = reinterpret_cast<decltype(open)>(GetProcAddress(m, "SDL_OpenJoystick"));
        button = reinterpret_cast<decltype(button)>(GetProcAddress(m, "SDL_SetJoystickVirtualButton"));
        axis = reinterpret_cast<decltype(axis)>(GetProcAddress(m, "SDL_SetJoystickVirtualAxis"));
        if (!attach || !open || !button || !axis) return false;
        std::string k = kind;
        Uint16 vid = 0x054C, pid;
        if (k == "ps3") pid = 0x0268;
        else if (k == "ps4") pid = 0x09CC;
        else if (k == "ps5") pid = 0x0CE6;
        else if (k == "xbox") { vid = 0x045E; pid = 0x028E; }
        else { fprintf(stderr, "[input] HC09_PAD_VIRTUAL=%s: expected ps3, ps4, ps5 or xbox\n", kind); return false; }
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.vendor_id = vid;
        d.product_id = pid;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_TOUCHPAD + 1;
        d.button_mask = (1u << d.nbuttons) - 1;   // virtual button i = gamepad button i
        d.axis_mask = (1u << d.naxes) - 1;
        d.name = k == "xbox" ? "HC09 virtual Xbox 360 pad" : "HC09 virtual PlayStation pad";
        d.Rumble = Rumble;
        SDL_JoystickID id = attach(&d);
        if (!id || !(joy = open(id))) { fprintf(stderr, "[input] virtual pad not attached\n"); return false; }
        std::istringstream ss(script ? script : "");
        std::string tok;
        while (ss >> tok)
        {
            size_t colon = tok.find(':');
            if (colon == std::string::npos) { fprintf(stderr, "[input] HC09_PAD_SCRIPT: '%s' has no ':'\n", tok.c_str()); continue; }
            Step s{ atof(tok.substr(0, colon).c_str()), {} };
            std::stringstream names(tok.substr(colon + 1));
            std::string n;
            while (std::getline(names, n, ','))
            {
                In in;
                if (Parse(n, in)) s.ins.push_back(in);
                else fprintf(stderr, "[input] HC09_PAD_SCRIPT: unknown input '%s'\n", n.c_str());
            }
            steps.push_back(s);
        }
        fprintf(stderr, "[input] virtual %s pad attached (%04X:%04X), %zu scripted step(s)\n", kind, vid, pid, steps.size());
        return true;
    }

    void Update(double now)
    {
        if (!joy) return;
        for (Step& s : steps)
        {
            if (!s.down && now >= s.at)
            {
                for (const In& in : s.ins)
                    in.axis ? axis(joy, in.index, in.value) : button(joy, in.index, true);
                s.down = true;
                fprintf(stderr, "[input] virtual pad t=%.1fs: step %.1fs pressed\n", now, s.at);
            }
            if (s.down && !s.up && now >= s.at + 0.2)
            {
                // A virtual trigger rests at the axis minimum (SDL maps the full range to 0..max).
                for (const In& in : s.ins)
                    in.axis ? axis(joy, in.index, in.index >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? -32768 : 0)
                            : button(joy, in.index, false);
                s.up = true;
            }
        }
    }
};

struct Sdl
{
#define HC09_SDL_DECL(f) decltype(&::f) f = nullptr;
    HC09_SDL_FNS(HC09_SDL_DECL)
#undef HC09_SDL_DECL
    bool ok = false;
    SDL_Gamepad* pad = nullptr;
    bool preferred = false;   // the open pad is the one hc09.ini names
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
#if HC09_DEV_TOOLS
    VirtualPad virt;   // HC09_PAD_VIRTUAL: a scripted test pad (not in the release build)
#endif

    bool Load()
    {
        HMODULE m = LoadLibraryW(L"SDL3.dll");
        if (!m) return false;
        bool all = true;
#define HC09_SDL_LOAD(f) f = reinterpret_cast<decltype(f)>(GetProcAddress(m, #f)); all = all && f;
        HC09_SDL_FNS(HC09_SDL_LOAD)
#undef HC09_SDL_LOAD
        if (!all) { fprintf(stderr, "[input] SDL3.dll lacks a function; not used\n"); return false; }
        // No SDL event loop here: joystick events on SDL's own thread, and pads
        // read whether or not the game window has focus (as XInput is).
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
        if (!SDL_Init(SDL_INIT_GAMEPAD))
        {
            fprintf(stderr, "[input] SDL_Init(GAMEPAD) failed: %s\n", SDL_GetError());
            return false;
        }
        int v = SDL_GetVersion();
        fprintf(stderr, "[input] SDL %d.%d.%d loaded for gamepads\n", v / 1000000, v / 1000 % 1000, v % 1000);
#if HC09_DEV_TOOLS
        if (const char* k = Hc09Env("HC09_PAD_VIRTUAL"); k && *k) virt.Attach(m, k, Hc09Env("HC09_PAD_SCRIPT"));
#endif
        return ok = true;
    }

    static const char* TypeName(SDL_GamepadType t)
    {
        switch (t)
        {
        case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
        case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One";
        case SDL_GAMEPAD_TYPE_PS3:     return "PlayStation 3";
        case SDL_GAMEPAD_TYPE_PS4:     return "PlayStation 4";
        case SDL_GAMEPAD_TYPE_PS5:     return "PlayStation 5";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
        default: return "other";
        }
    }

    // The open gamepad, or the first one connected; false when there is none.
    bool Ensure()
    {
        if (pad && SDL_GamepadConnected(pad)) return true;
        if (pad)
        {
            fprintf(stderr, "[input] SDL gamepad disconnected\n");
            SDL_CloseGamepad(pad);
            pad = nullptr;
        }
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        // The pad hc09.ini names first ([input] pad, the launcher's Controller list), then any.
        for (int pass = PreferredName().empty() ? 1 : 0; pass < 2 && !pad; ++pass)
            for (int i = 0; ids && i < n && !pad; ++i)
            {
                const char* nm = SDL_GetGamepadNameForID(ids[i]);
                bool isPref = nm && !_stricmp(nm, PreferredName().c_str());
                if (pass == 0 && !isPref) continue;
                if ((pad = SDL_OpenGamepad(ids[i])))
                {
                    preferred = isPref;
                    type = SDL_GetGamepadType(pad);
                    const char* name = SDL_GetGamepadName(pad);
                    fprintf(stderr, "[input] SDL gamepad connected: %s (%s)%s\n", name ? name : "?", TypeName(type),
                            isPref ? " (the chosen controller)" : "");
                }
            }
        SDL_free(ids);
        return pad != nullptr;
    }

    // [input] pad = <SDL pad name>: the one to use when it is connected ("" = the first one found).
    static const std::string& PreferredName()
    {
        static const std::string n = [] {
            const char* e = Hc09Env("HC09_PAD_NAME");
            std::string s = e ? e : "";
            return (s.empty() || !_stricmp(s.c_str(), "auto") || !_strnicmp(s.c_str(), "xinput:", 7)) ? std::string() : s;
        }();
        return n;
    }
    // The chosen pad connected after another was opened: switch to it.
    void PreferIfConnected()
    {
        if (!pad || preferred || PreferredName().empty()) return;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        bool there = false;
        for (int i = 0; ids && i < n; ++i)
            if (const char* nm = SDL_GetGamepadNameForID(ids[i]); nm && !_stricmp(nm, PreferredName().c_str())) there = true;
        SDL_free(ids);
        if (!there) return;
        SDL_CloseGamepad(pad);
        pad = nullptr;
        Ensure();
    }

    static int16_t Neg(int16_t v) { return v == -32768 ? 32767 : int16_t(-v); }
    static uint8_t Trigger(int16_t v) { return v <= 0 ? 0 : uint8_t((int(v) * 255 + 16383) / 32767); }

    XINPUT_GAMEPAD Read()
    {
        struct Map { SDL_GamepadButton b; WORD mask; };
        static constexpr Map kMap[] = {
            { SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A },   { SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B },
            { SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X },    { SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y },
            { SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK }, { SDL_GAMEPAD_BUTTON_TOUCHPAD, XINPUT_GAMEPAD_BACK },
            { SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
        };
        XINPUT_GAMEPAD g{};
        for (const Map& m : kMap)
            if (SDL_GetGamepadButton(pad, m.b)) g.wButtons |= m.mask;
        g.bLeftTrigger = Trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        g.bRightTrigger = Trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        g.sThumbLX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        g.sThumbLY = Neg(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));   // SDL: +y = down
        g.sThumbRX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        g.sThumbRY = Neg(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        return g;
    }

    HostPadKind Kind() const
    {
        if (!pad) return HostPadKind::None;
        switch (type)
        {
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return HostPadKind::PlayStation;
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return HostPadKind::Xbox;
        default: return HostPadKind::Other;
        }
    }
};
#endif

enum class PadApi { Auto, Sdl, XInput };

PadApi PadApiSetting()
{
    const char* e = Hc09Env("HC09_PAD_API");
    if (e && !_stricmp(e, "sdl")) return PadApi::Sdl;
    if (e && !_stricmp(e, "xinput")) return PadApi::XInput;
    return PadApi::Auto;
}

struct Host
{
    std::mutex mu;
    GetStateFn getState = nullptr;
    SetStateFn setState = nullptr;
    PadApi api = PadApiSetting();
    int slot = -1;                     // XInput pad in use, -1 = none
    bool sdlInUse = false;             // the state comes from SDL's pad
    double lastPoll = -1, lastScan = -10, lastSdlScan = -10;
    XINPUT_GAMEPAD pad{};
#if defined(HC09_HAVE_SDL3)
    Sdl sdl;
#endif
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    Host()
    {
        if (api != PadApi::Sdl)
            for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
                if (HMODULE m = LoadLibraryW(dll))
                {
                    getState = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
                    setState = reinterpret_cast<SetStateFn>(GetProcAddress(m, "XInputSetState"));
                    if (getState) break;
                }
        bool sdlOk = false;
#if defined(HC09_HAVE_SDL3)
        if (api != PadApi::XInput) sdlOk = sdl.Load();
#endif
        fprintf(stderr, "[input] host input: SDL3 %s, XInput %s, keyboard %s\n",
                sdlOk ? "loaded" : (api == PadApi::XInput ? "off" : "not found"),
                getState ? "loaded" : (api == PadApi::Sdl ? "off" : "not found"),
#if defined(HC09_HAVE_VULKAN)
                "from the game window");
#else
                "unavailable");
#endif
    }

    double Now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

    // SDL's pad when there is one (looked for every 2 s while none is open);
    // true when it gave the state.
    bool PollSdl(double now)
    {
#if defined(HC09_HAVE_SDL3)
        if (!sdl.ok) return false;
#if HC09_DEV_TOOLS
        sdl.virt.Update(now);
#endif
        sdl.SDL_UpdateGamepads();
        if (!sdl.pad)
        {
            if (now - lastSdlScan < 2.0) return false;
            lastSdlScan = now;
        }
        else if (now - lastSdlScan >= 2.0)
        {
            lastSdlScan = now;
            sdl.PreferIfConnected();
        }
        if (!sdl.Ensure()) { sdlInUse = false; return false; }
        if (slot >= 0) { fprintf(stderr, "[input] XInput controller %d set aside for SDL's pad\n", slot); slot = -1; }
        pad = sdl.Read();
        sdlInUse = true;
        return true;
#else
        (void)now;
        return false;
#endif
    }

    void PollPad()
    {
        double now = Now();
        if (now - lastPoll < 0.002) return;
        lastPoll = now;
        // [input] pad = xinput:N: that XInput controller, ahead of SDL's pads, while it is connected.
        static const int xinputPref = [] {
            const char* e = Hc09Env("HC09_PAD_NAME");
            return e && !_strnicmp(e, "xinput:", 7) ? (atoi(e + 7) < 0 ? 0 : atoi(e + 7) > 3 ? 3 : atoi(e + 7)) : -1;
        }();
        if (xinputPref >= 0 && getState)
        {
            XINPUT_STATE xs{};
            if (getState(DWORD(xinputPref), &xs) == ERROR_SUCCESS)
            {
                if (slot != xinputPref) fprintf(stderr, "[input] XInput controller %d (the chosen controller)\n", xinputPref);
                slot = xinputPref;
                pad = xs.Gamepad;
                sdlInUse = false;
                return;
            }
        }
        if (PollSdl(now)) return;
        if (!getState) { pad = {}; return; }
        XINPUT_STATE s{};
        if (slot >= 0)
        {
            if (getState(DWORD(slot), &s) == ERROR_SUCCESS) { pad = s.Gamepad; return; }
            fprintf(stderr, "[input] controller %d disconnected\n", slot);
            slot = -1;
            pad = {};
        }
        if (now - lastScan < 2.0) return;
        lastScan = now;
        for (int i = 0; i < 4; ++i)
            if (getState(DWORD(i), &s) == ERROR_SUCCESS)
            {
                slot = i;
                pad = s.Gamepad;
                fprintf(stderr, "[input] controller %d connected\n", i);
                return;
            }
    }

    HostPadKind Kind() const
    {
#if defined(HC09_HAVE_SDL3)
        if (sdlInUse) return sdl.Kind();
#endif
        return slot >= 0 ? HostPadKind::Xbox : HostPadKind::None;
    }

    void Rumble(uint16_t left, uint16_t right)
    {
#if defined(HC09_HAVE_SDL3)
        // Until the next call, as XInput's (SDL caps the duration at its maximum).
        if (sdlInUse && sdl.pad) { sdl.SDL_RumbleGamepad(sdl.pad, left, right, 0xFFFFFFFFu); return; }
#endif
        if (slot < 0 || !setState) return;
        XINPUT_VIBRATION v{ left, right };
        setState(DWORD(slot), &v);
    }
};

Host& TheHost() { static Host h; return h; }

// The kind of the last controller seen (kept while it is unplugged or asleep, so
// button prompts do not flip); read without the lock by the renderer.
std::atomic<int> g_padKind{ int(HostPadKind::None) };

#if defined(HC09_HAVE_VULKAN)
bool Key(uint32_t vk) { return hc09vk::WindowKeyDown(vk); }
#else
bool Key(uint32_t) { return false; }
#endif

// The axis value of larger magnitude: the pad and the keyboard both count.
int16_t Merge(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

int16_t KeyAxis(uint32_t neg, uint32_t pos)
{
    return int16_t((Key(pos) ? 32767 : 0) - (Key(neg) ? 32767 : 0));
}

}  // namespace

HostPadState HostInputPoll()
{
    HostPadState st;
    if (!HostInputEnabled()) return st;
    Host& h = TheHost();
    {
        std::lock_guard<std::mutex> lk(h.mu);
        h.PollPad();
        if (HostPadKind k = h.Kind(); k != HostPadKind::None) g_padKind.store(int(k), std::memory_order_relaxed);
        st.buttons = h.pad.wButtons;
        st.lt = h.pad.bLeftTrigger;
        st.rt = h.pad.bRightTrigger;
        st.lx = h.pad.sThumbLX; st.ly = h.pad.sThumbLY;
        st.rx = h.pad.sThumbRX; st.ry = h.pad.sThumbRY;
    }
    struct KeyButton { uint32_t vk; uint16_t mask; };
    static constexpr KeyButton kKeys[] = {
        { VK_UP, XINPUT_GAMEPAD_DPAD_UP },       { VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
        { VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },   { VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
        { VK_RETURN, XINPUT_GAMEPAD_START },     { VK_BACK, XINPUT_GAMEPAD_BACK },
        { VK_TAB, XINPUT_GAMEPAD_BACK },         { VK_SHIFT, XINPUT_GAMEPAD_LEFT_THUMB },
        { VK_NUMPAD5, XINPUT_GAMEPAD_RIGHT_THUMB }, { 'Q', XINPUT_GAMEPAD_LEFT_SHOULDER },
        { 'E', XINPUT_GAMEPAD_RIGHT_SHOULDER },  { VK_SPACE, XINPUT_GAMEPAD_A },
        { 'K', XINPUT_GAMEPAD_A },               { VK_ESCAPE, XINPUT_GAMEPAD_B },
        { 'L', XINPUT_GAMEPAD_B },               { 'J', XINPUT_GAMEPAD_X },
        { 'I', XINPUT_GAMEPAD_Y },
    };
    for (const KeyButton& k : kKeys)
        if (Key(k.vk)) st.buttons |= k.mask;
    if (Key('Z')) st.lt = 255;
    if (Key('C')) st.rt = 255;
    st.lx = Merge(st.lx, KeyAxis('A', 'D'));
    st.ly = Merge(st.ly, KeyAxis('S', 'W'));
    st.rx = Merge(st.rx, KeyAxis(VK_NUMPAD4, VK_NUMPAD6));
    st.ry = Merge(st.ry, KeyAxis(VK_NUMPAD2, VK_NUMPAD8));
    return st;
}

void HostInputRumble(uint16_t left, uint16_t right)
{
    if (!HostInputEnabled()) return;
    Host& h = TheHost();
    std::lock_guard<std::mutex> lk(h.mu);
    h.Rumble(left, right);
}

HostPadKind HostInputPadKind()
{
    return HostPadKind(g_padKind.load(std::memory_order_relaxed));
}

#else  // no host input on this platform yet

HostPadState HostInputPoll() { return {}; }
void HostInputRumble(uint16_t, uint16_t) {}
HostPadKind HostInputPadKind() { return HostPadKind::None; }

#endif

bool HostWindowVisible()
{
    const char* live = Hc09Env("HC09_GPU_LIVE");
    const char* win = Hc09Env("HC09_GPU_WINDOW");
    return live && *live && strcmp(live, "0") != 0 &&
           !(win && (!strcmp(win, "0") || !strcmp(win, "hidden")));
}

bool HostInputEnabled()
{
    static const bool enabled = [] {
        bool on;
        if (const char* e = Hc09Env("HC09_HOST_INPUT"); e && *e)
            on = strcmp(e, "0") != 0;
        else
            on = HostWindowVisible();
        fprintf(stderr, "[input] host controller/keyboard input %s\n", on ? "on" : "off");
        return on;
    }();
    return enabled;
}
