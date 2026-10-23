#pragma once
// Host audio output for the game's own mix (XAudioSubmitRenderDriverFrame):
// 48 kHz, 5.1, played through WASAPI shared mode on Windows (downmixed to the
// device's layout). Portable header; on other platforms nothing is played.
#include <cstddef>
#include <cstdint>

constexpr int kAudioChannels = 6;   // FL FR C LFE RL RR, the 360's render-driver layout

// Whether submitted frames are played (decided once: HC09_AUDIO_OUT, default
// on only when the game presents in a visible window).
bool HostAudioEnabled();
// Queue `frames` sample frames of kAudioChannels interleaved host-order floats.
void HostAudioSubmit(const float* samples, size_t frames);
// Sample frames queued but not yet handed to the device; -1 while no device is
// open (the render pump then keeps its own 5.33 ms clock).
long HostAudioQueued();
// Device periods that found the ring short (after the game's first frame).
uint64_t HostAudioUnderruns();
