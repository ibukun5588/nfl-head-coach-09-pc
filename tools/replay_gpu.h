#pragma once
// hc09-replay --gpu: present a decoded frame in a Vulkan window.
#include <string>

#include "xenos_texture.h"

// Shows `img` until the window is closed (Esc or the close button), or for
// `seconds` if >= 0. `dumpPath`: if non-empty, writes the first presented
// swapchain image there as PNG (what the window actually showed). Returns 0 on
// success, 3 if the validation layer reported anything, 1 on failure.
int ReplayPresent(const xenos::Image& img, const char* title, double seconds, const std::string& dumpPath);

// hc09-replay --gpu-render: draw the replayed frame with the Vulkan renderer
// (runtime/gpu_vk/vk_edram: draws, resolves and clears on the GPU) instead of
// the software rasterizer. Returns the guest physical memory the replay must
// use (`bytes`, zeroed, GPU-visible), or nullptr on failure.
uint8_t* GpuRenderBegin(size_t bytes);
// Finish rendering, print statistics, release everything
// including the guest memory returned by GpuRenderBegin.
void GpuRenderEnd();
// HC09_REPLAY_REPEAT: from now on the renderer gets every draw as the live
// observer (no capture); GpuRenderRepeatEnd prints its cost per repeated frame.
void GpuRenderRepeatBegin();
void GpuRenderRepeatEnd(int frames);
