// The launcher's controller list and test panel: see pad_test.h.
#include "pad_test.h"

#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <map>

#if defined(HC09_HAVE_SDL3)
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#endif

namespace {
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

std::wstring Widen(const char* s)
{
    if (!s) return L"?";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
}  // namespace

struct PadTester::Impl
{
    GetStateFn getState = nullptr;
#if defined(HC09_HAVE_SDL3)
#define PT_FNS(X) X(SDL_SetHint) X(SDL_Init) X(SDL_GetGamepads) X(SDL_free) X(SDL_OpenGamepad) X(SDL_GamepadConnected) \
    X(SDL_UpdateGamepads) X(SDL_GetGamepadButton) X(SDL_GetGamepadAxis) X(SDL_GetGamepadType) X(SDL_GetGamepadNameForID) \
    X(SDL_GetGamepadFromID)
#define PT_DECL(f) decltype(&::f) f = nullptr;
    PT_FNS(PT_DECL)
#undef PT_DECL
    bool sdl = false;
    std::map<SDL_JoystickID, SDL_Gamepad*> open;
#endif
};

void PadTester::Init(const std::wstring& sdlDll)
{
    if (impl_) return;
    impl_ = new Impl;
    for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
        if (HMODULE m = LoadLibraryW(dll))
            if ((impl_->getState = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")))) break;
#if defined(HC09_HAVE_SDL3)
    if (HMODULE m = LoadLibraryW(sdlDll.c_str()))
    {
        bool all = true;
#define PT_LOAD(f) impl_->f = reinterpret_cast<decltype(impl_->f)>(GetProcAddress(m, #f)); all = all && impl_->f;
        PT_FNS(PT_LOAD)
#undef PT_LOAD
        if (all)
        {
            // As runtime/host_input.cpp: SDL's own joystick thread, pads read without focus, PS3 pads over HIDAPI.
            impl_->SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
            impl_->SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            impl_->SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
            impl_->sdl = impl_->SDL_Init(SDL_INIT_GAMEPAD);
        }
    }
#else
    (void)sdlDll;
#endif
}

std::vector<PadEntry> PadTester::List()
{
    std::vector<PadEntry> out;
    if (!impl_) return out;
#if defined(HC09_HAVE_SDL3)
    if (impl_->sdl)
    {
        impl_->SDL_UpdateGamepads();
        int n = 0;
        SDL_JoystickID* ids = impl_->SDL_GetGamepads(&n);
        for (int i = 0; ids && i < n; ++i)
            if (const char* nm = impl_->SDL_GetGamepadNameForID(ids[i]))
                if (std::none_of(out.begin(), out.end(), [&](const PadEntry& e) { return e.value == nm; }))
                    out.push_back({ Widen(nm), nm });
        impl_->SDL_free(ids);
    }
#endif
    if (impl_->getState)
        for (int i = 0; i < 4; ++i)
        {
            XINPUT_STATE s{};
            if (impl_->getState(DWORD(i), &s) == ERROR_SUCCESS)
                out.push_back({ L"Xbox controller " + std::to_wstring(i + 1) + L" (XInput)", "xinput:" + std::to_string(i) });
        }
    return out;
}

PadState PadTester::Read(const std::string& choice)
{
    PadState st;
    if (!impl_) return st;
    auto fromXInput = [&](int slot) {
        XINPUT_STATE s{};
        if (!impl_->getState || impl_->getState(DWORD(slot), &s) != ERROR_SUCCESS) return false;
        const XINPUT_GAMEPAD& g = s.Gamepad;
        static const WORD bits[15] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_BACK, 0,
                                       XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
                                       XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_DPAD_UP,
                                       XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT };
        for (int i = 0; i < 15; ++i) st.buttons[i] = bits[i] && (g.wButtons & bits[i]);
        st.lx = g.sThumbLX / 32768.f; st.ly = -g.sThumbLY / 32768.f; st.rx = g.sThumbRX / 32768.f; st.ry = -g.sThumbRY / 32768.f;
        st.lt = g.bLeftTrigger / 255.f; st.rt = g.bRightTrigger / 255.f;
        st.connected = true;
        st.name = L"Xbox controller " + std::to_wstring(slot + 1) + L" (XInput)";
        return true;
    };
    if (choice.rfind("xinput:", 0) == 0 && fromXInput(std::clamp(atoi(choice.c_str() + 7), 0, 3))) return st;
#if defined(HC09_HAVE_SDL3)
    if (impl_->sdl)
    {
        impl_->SDL_UpdateGamepads();
        int n = 0;
        SDL_JoystickID* ids = impl_->SDL_GetGamepads(&n);
        SDL_JoystickID pick = 0;
        for (int pass = 0; pass < 2 && !pick; ++pass)   // the chosen pad, else the first (as the game does)
            for (int i = 0; ids && i < n && !pick; ++i)
            {
                const char* nm = impl_->SDL_GetGamepadNameForID(ids[i]);
                if (pass == 1 || (nm && !_stricmp(nm, choice.c_str()))) pick = ids[i];
            }
        impl_->SDL_free(ids);
        if (pick)
        {
            SDL_Gamepad*& p = impl_->open[pick];
            if (!p) p = impl_->SDL_OpenGamepad(pick);
            if (p && impl_->SDL_GamepadConnected(p))
            {
                for (int i = 0; i < 15; ++i) st.buttons[i] = impl_->SDL_GetGamepadButton(p, SDL_GamepadButton(i));
                auto ax = [&](SDL_GamepadAxis a) { return impl_->SDL_GetGamepadAxis(p, a) / 32768.f; };
                st.lx = ax(SDL_GAMEPAD_AXIS_LEFTX); st.ly = ax(SDL_GAMEPAD_AXIS_LEFTY);
                st.rx = ax(SDL_GAMEPAD_AXIS_RIGHTX); st.ry = ax(SDL_GAMEPAD_AXIS_RIGHTY);
                st.lt = std::max(0.f, ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); st.rt = std::max(0.f, ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
                SDL_GamepadType t = impl_->SDL_GetGamepadType(p);
                st.playstation = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
                st.name = Widen(impl_->SDL_GetGamepadNameForID(pick));
                st.connected = true;
                return st;
            }
        }
    }
#endif
    for (int i = 0; i < 4; ++i) if (fromXInput(i)) return st;
    return st;
}

// ---- drawing --------------------------------------------------------------------------------------------------------
void PadDraw(HDC dc, const RECT& r, const PadState& s, int dpi)
{
    auto S = [&](double v) { return int(std::lround(v * dpi / 96.0)); };
    HBRUSH bg = CreateSolidBrush(RGB(246, 246, 246));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    if (!s.connected)
    {
        RECT t{ r.left + S(20), r.top + (r.bottom - r.top) / 2 - S(18), r.right - S(20), r.bottom };
        SetTextColor(dc, RGB(90, 90, 90));
        DrawTextW(dc, L"No controller found. Connect one and it shows here (a DualShock 3 needs DsHidMini).", -1, &t,
                  DT_CENTER | DT_WORDBREAK);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        return;
    }
    const COLORREF on = RGB(46, 160, 67), off = RGB(214, 214, 214), ink = RGB(40, 40, 40);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    int x0 = r.left, y0 = r.top, w = r.right - r.left;
    auto box = [&](double x, double y, double bw, double bh, bool pressed, const wchar_t* label) {
        HBRUSH b = CreateSolidBrush(pressed ? on : off);
        HGDIOBJ ob = SelectObject(dc, b);
        RoundRect(dc, x0 + S(x), y0 + S(y), x0 + S(x + bw), y0 + S(y + bh), S(6), S(6));
        SelectObject(dc, ob);
        DeleteObject(b);
        RECT t{ x0 + S(x), y0 + S(y), x0 + S(x + bw), y0 + S(y + bh) };
        SetTextColor(dc, pressed ? RGB(255, 255, 255) : ink);
        DrawTextW(dc, label, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    auto stick = [&](double cx, double cy, float sx, float sy, bool pressed, const wchar_t* label) {
        double rad = 30;
        HBRUSH b = CreateSolidBrush(pressed ? RGB(200, 235, 205) : RGB(232, 232, 232));
        HGDIOBJ ob = SelectObject(dc, b);
        Ellipse(dc, x0 + S(cx - rad), y0 + S(cy - rad), x0 + S(cx + rad), y0 + S(cy + rad));
        SelectObject(dc, ob);
        DeleteObject(b);
        HBRUSH dot = CreateSolidBrush(on);
        ob = SelectObject(dc, dot);
        double dx = cx + sx * (rad - 7), dy = cy + sy * (rad - 7);
        Ellipse(dc, x0 + S(dx - 6), y0 + S(dy - 6), x0 + S(dx + 6), y0 + S(dy + 6));
        SelectObject(dc, ob);
        DeleteObject(dot);
        RECT t{ x0 + S(cx - 30), y0 + S(cy + rad + 2), x0 + S(cx + 30), y0 + S(cy + rad + 18) };
        SetTextColor(dc, ink);
        DrawTextW(dc, label, -1, &t, DT_CENTER | DT_SINGLELINE);
    };
    auto trigger = [&](double x, double y, float v, const wchar_t* label) {
        HBRUSH b = CreateSolidBrush(off);
        RECT full{ x0 + S(x), y0 + S(y), x0 + S(x + 70), y0 + S(y + 14) };
        FillRect(dc, &full, b);
        DeleteObject(b);
        b = CreateSolidBrush(on);
        RECT part{ full.left, full.top, full.left + LONG((full.right - full.left) * std::clamp(v, 0.f, 1.f)), full.bottom };
        FillRect(dc, &part, b);
        DeleteObject(b);
        RECT t{ full.left, full.top, full.right, full.bottom };
        SetTextColor(dc, ink);
        DrawTextW(dc, label, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    const bool ps = s.playstation;
    double W = w * 96.0 / dpi, cx = W / 2;
    // the name
    RECT title{ x0 + S(8), y0 + S(4), r.right - S(8), y0 + S(20) };
    SetTextColor(dc, ink);
    DrawTextW(dc, s.name.c_str(), -1, &title, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // shoulders and triggers
    trigger(12, 24, s.lt, ps ? L"L2" : L"LT");
    trigger(W - 82, 24, s.rt, ps ? L"R2" : L"RT");
    box(12, 42, 70, 18, s.buttons[9], ps ? L"L1" : L"LB");
    box(W - 82, 42, 70, 18, s.buttons[10], ps ? L"R1" : L"RB");
    // D-pad, sticks, face buttons
    double dx = 52, dy = 98;
    box(dx - 10, dy - 30, 20, 20, s.buttons[11], L"\x25B2");
    box(dx - 10, dy + 10, 20, 20, s.buttons[12], L"\x25BC");
    box(dx - 30, dy - 10, 20, 20, s.buttons[13], L"\x25C0");
    box(dx + 10, dy - 10, 20, 20, s.buttons[14], L"\x25B6");
    stick(cx - 62, 100, s.lx, s.ly, s.buttons[7], ps ? L"L3" : L"LS");
    stick(cx + 62, 100, s.rx, s.ry, s.buttons[8], ps ? L"R3" : L"RS");
    double fx = W - 52, fy = 98;
    box(fx - 10, fy + 10, 22, 20, s.buttons[0], ps ? L"\x2715" : L"A");
    box(fx + 12, fy - 10, 22, 20, s.buttons[1], ps ? L"\x25CB" : L"B");
    box(fx - 32, fy - 10, 22, 20, s.buttons[2], ps ? L"\x25A1" : L"X");
    box(fx - 10, fy - 30, 22, 20, s.buttons[3], ps ? L"\x25B3" : L"Y");
    // Back / Guide / Start
    box(cx - 70, 30, 44, 18, s.buttons[4], ps ? L"Select" : L"Back");
    box(cx - 20, 30, 40, 18, s.buttons[5], ps ? L"PS" : L"Guide");
    box(cx + 26, 30, 44, 18, s.buttons[6], L"Start");
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}
