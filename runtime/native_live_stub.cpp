// Builds without the Vulkan backend (no live renderer): the native renderer's entry
// points (native_live.h) report it off, so the D3D hooks only forward to the game's D3D.
#include "native_live.h"

bool NativeRendererSelected() { return false; }
bool NativeRendererLive() { return false; }
d3dn::Backend* NativeLiveBackend() { return nullptr; }
bool NativeLiveProfiling() { return false; }
void NativeLiveAddFrontendNs(int, uint64_t) {}
void NativeLiveKicked(uint32_t, uint32_t) {}
bool NativeLiveFencesOn() { return false; }
void NativeLiveFenceWait(uint32_t, uint32_t, uint32_t, uint32_t) {}
void NativeLiveFenceNeeded(uint32_t, uint32_t) {}
