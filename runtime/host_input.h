#pragma once
// Host input for the virtual pad on user 0 (runtime/imports_input.cpp): a real
// controller (SDL3 or XInput) and the game window's keyboard, merged into one Xbox 360
// gamepad state. Portable header; Win32 implementation in host_input.cpp (on
// other platforms every poll reports nothing).
#include <cstdint>

struct HostPadState
{
    uint16_t buttons = 0;              // XINPUT_GAMEPAD_* masks
    uint8_t  lt = 0, rt = 0;           // triggers 0..255
    int16_t  lx = 0, ly = 0, rx = 0, ry = 0;   // thumbsticks, +y = up
};

// Whether the game presents in a window a person can see (HC09_GPU_LIVE=1 and
// HC09_GPU_WINDOW not 0/hidden): the default for host input and audio output,
// so unattended runs neither read the desktop's devices nor make sound.
bool HostWindowVisible();
// Whether host input is used at all (decided once: HC09_HOST_INPUT, default
// HostWindowVisible()).
bool HostInputEnabled();
// Current merged state of the controller and the keyboard. Cheap to call often
// (the controller is polled at most every 2 ms, empty slots every 2 s).
HostPadState HostInputPoll();
// Rumble on the controller in use (0..65535 per motor).
void HostInputRumble(uint16_t left, uint16_t right);
// The kind of the last controller used (for button prompts; None until one is
// seen). Cheap, lock-free: any thread.
enum class HostPadKind { None, Xbox, PlayStation, Other };
HostPadKind HostInputPadKind();
