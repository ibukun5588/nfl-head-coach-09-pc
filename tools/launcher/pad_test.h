#pragma once
// The launcher's controller list and test panel (docs/HANDOFF_M11.md step 7): the pads the game can use, read the way
// runtime/host_input.cpp reads them (SDL3 first, XInput otherwise), and a live drawing of the chosen one.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>

struct PadEntry
{
    std::wstring label;   // for the list
    std::string value;    // hc09.ini [input] pad: an SDL pad's name, or xinput:N
};

struct PadState
{
    bool connected = false;
    bool playstation = false;   // label the buttons Cross/Circle/...
    std::wstring name;
    bool buttons[15] = {};      // SDL_GamepadButton order: South East West North Back Guide Start LS RS LB RB Up Down Left Right
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;   // sticks -1..1 (y down), triggers 0..1
};

class PadTester
{
public:
    // SDL3.dll from beside hc09.exe (sdlDll); XInput from Windows.
    void Init(const std::wstring& sdlDll);
    std::vector<PadEntry> List();
    // The pad the game would use for hc09.ini's `choice` ("auto", a name, xinput:N).
    PadState Read(const std::string& choice);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

void PadDraw(HDC dc, const RECT& r, const PadState& s, int dpi);
