// hc09-replay --gpu (BACKEND_PLAN milestone 2): Vulkan device, window and
// swapchain presenting the frame the replay produced.
#include "replay_gpu.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef HC09_HAVE_VULKAN
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include "vk_device.h"
#include "vk_edram.h"
#include "vk_platform.h"
#include "vk_swapchain.h"

namespace {
struct RenderSession
{
    hc09vk::Device dev;
    hc09vk::Buffer guest;
    hc09vk::EdramRenderer renderer;
};
RenderSession* g_rs = nullptr;
}  // namespace

uint8_t* GpuRenderBegin(size_t bytes)
{
    using namespace hc09vk;
    g_rs = new RenderSession;
    std::string err;
    if (!g_rs->dev.CreateInstance({}, &err) || !g_rs->dev.CreateDevice(VK_NULL_HANDLE, &err))
    {
        fprintf(stderr, "[gpu] %s\n", err.c_str());
        return nullptr;
    }
    // HC09_GPU_IMPORT=1: guest memory is a pagefile-backed section view imported
    // into Vulkan, as hc09.exe's live renderer does with the game's memory.
    const char* imp = getenv("HC09_GPU_IMPORT");
    if (imp && *imp && strcmp(imp, "0"))
    {
#ifdef _WIN32
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, DWORD(uint64_t(bytes) >> 32),
                                            DWORD(bytes), nullptr);
        void* view = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes) : nullptr;
#else
        void* view = aligned_alloc(65536, bytes);
#endif
        if (!view || !g_rs->dev.ImportHostBuffer(view, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, g_rs->guest, &err))
        {
            fprintf(stderr, "[gpu] guest memory import: %s\n", view ? err.c_str() : "no section view");
            return nullptr;
        }
        fprintf(stderr, "[gpu] guest memory: imported host section view at %p\n", view);
    }
    // CPU-cached, coherent: the replay and the software resolves read and write
    // it, the shaders fetch vertices from it.
    VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (!g_rs->guest.buffer && !g_rs->dev.CreateBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cached, g_rs->guest) &&
        !g_rs->dev.CreateBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g_rs->guest))
    {
        fprintf(stderr, "[gpu] cannot allocate %zu MiB of guest memory\n", bytes >> 20);
        return nullptr;
    }
    memset(g_rs->guest.mapped, 0, bytes);
    if (!g_rs->renderer.Init(g_rs->dev, g_rs->guest, &err)) { fprintf(stderr, "[gpu] renderer: %s\n", err.c_str()); return nullptr; }
    xenos::SetDrawObserver(&g_rs->renderer, false);
    // HC09_GPU_NOWAIT=1: no GPU wait at copy resolves, as hc09.exe live (then the
    // resolve PNGs are read before the GPU wrote them; frame.png is complete).
    const char* nw = getenv("HC09_GPU_NOWAIT");
    if (nw && *nw && strcmp(nw, "0")) g_rs->renderer.SetResolveWaits(false);
    fprintf(stderr, "[gpu] rendering the replay with Vulkan on %s\n", g_rs->dev.props.deviceName);
    return static_cast<uint8_t*>(g_rs->guest.mapped);
}

hc09vk::RendererStats g_repeatStart;

void GpuRenderRepeatBegin()
{
    if (!g_rs) return;
    xenos::SetDrawObserver(nullptr, true);
    xenos::SetLiveObserver(&g_rs->renderer);
    g_repeatStart = g_rs->renderer.Stats();
}

void GpuRenderRepeatEnd(int frames)
{
    if (!g_rs || frames <= 0) return;
    xenos::SetLiveObserver(nullptr);
    const hc09vk::RendererStats& s = g_rs->renderer.Stats();
    double n = double(frames);
    fprintf(stderr, "[gpu] repeated %d frames: renderer %.2f ms/frame (textures %.2f, GPU waits %.2f, GPU busy %.2f), %u syncs, "
                    "%u textures decoded again\n",
            frames, 1e3 * (s.rendererSeconds - g_repeatStart.rendererSeconds) / n, 1e3 * (s.textureSeconds - g_repeatStart.textureSeconds) / n,
            1e3 * (s.waitSeconds - g_repeatStart.waitSeconds) / n, 1e3 * (s.gpuSeconds - g_repeatStart.gpuSeconds) / n,
            s.syncs - g_repeatStart.syncs, s.texturesRedecoded - g_repeatStart.texturesRedecoded);
    if (s.drawSections[0] > 0)   // HC09_GPU_CPUPROF=1
    {
        fprintf(stderr, "[gpu]   draw CPU ms/frame (%.0f draws/frame):", (s.draws - g_repeatStart.draws) / n);
        for (int i = 0; i < hc09vk::RendererStats::kDrawSections; ++i)
            fprintf(stderr, "%s %s %.2f", i ? "," : "", hc09vk::kDrawSectionNames[i],
                    1e3 * (s.drawSections[i] - g_repeatStart.drawSections[i]) / n);
        fprintf(stderr, "\n");
    }
}

void GpuRenderEnd()
{
    if (!g_rs) return;
    g_rs->renderer.Flush();
    const hc09vk::RendererStats& s = g_rs->renderer.Stats();
    fprintf(stderr, "[gpu] %u draws (%u skipped), %u resolves, %u layout barriers, %u syncs, %u shaders, %u pipelines, %u textures%s\n", s.draws, s.skipped,
            s.resolves, s.barriers, s.syncs, s.shaders, s.pipelines, s.textures,
            g_rs->dev.validationMessages ? " — validation messages reported" : "");
    fprintf(stderr, "[gpu] textures: %u decoded on the CPU, %u on the GPU\n", s.textures, s.texturesOnGpu);
    fprintf(stderr, "[gpu] time: %.2f s compiling shaders, %.2f s creating pipelines, %.2f s decoding textures, %.2f s waiting for the GPU\n", s.compileSeconds,
            s.pipelineSeconds, s.textureSeconds, s.waitSeconds);
    fprintf(stderr, "[gpu] renderer %.1f ms (draws, resolves, frame end; waits included)\n", s.rendererSeconds * 1e3);
    fprintf(stderr, "[gpu] GPU busy %.2f s\n", s.gpuSeconds);
    for (const auto& [why, n] : s.skipReasons) fprintf(stderr, "[gpu]   skipped %u: %s\n", n, why.c_str());
    xenos::SetDrawObserver(nullptr, true);
    g_rs->renderer.Shutdown();
    g_rs->dev.DestroyBuffer(g_rs->guest);
    g_rs->dev.Destroy();
    delete g_rs;
    g_rs = nullptr;
}

int ReplayPresent(const xenos::Image& img, const char* title, double seconds, const std::string& dumpPath)
{
    using namespace hc09vk;
    std::string err;
    Window* win = WindowCreate(title, img.width, img.height);
    if (!win) { fprintf(stderr, "[vk] cannot create a window\n"); return 1; }
    Device dev;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    Swapchain sc;
    Image frame;
    int rc = 0;
    uint64_t presents = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool dumped = dumpPath.empty();
    uint32_t cw = 0, ch = 0;
    if (!dev.CreateInstance(WindowInstanceExtensions(), &err)) goto fail;
    if (WindowCreateSurface(win, dev.instance, &surface) != VK_SUCCESS) { err = "vkCreate*SurfaceKHR failed"; goto fail; }
    if (!dev.CreateDevice(surface, &err)) goto fail;
    WindowClientSize(win, &cw, &ch);
    if (!sc.Create(dev, surface, cw, ch, &err)) goto fail;
    if (!UploadRGBA8(dev, img.rgba.data(), img.width, img.height, frame)) { err = "frame upload failed"; goto fail; }
    fprintf(stderr, "[vk] presenting %ux%u in a %ux%u window (%s)%s\n", img.width, img.height, sc.extent.width,
            sc.extent.height, sc.format == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" : "R8G8B8A8_UNORM",
            seconds >= 0 ? "" : "; close the window or press Esc to quit");
    while (WindowPump(win))
    {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (seconds >= 0 && t >= seconds) break;
        WindowClientSize(win, &cw, &ch);
        if (!cw || !ch) { std::this_thread::sleep_for(std::chrono::milliseconds(16)); continue; }   // minimized
        if (cw != sc.extent.width || ch != sc.extent.height)
            if (!sc.Resize(cw, ch, &err)) goto fail;
        std::vector<uint32_t> shown;
        VkResult r = sc.Present(frame, dumped ? nullptr : &shown);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { if (!sc.Resize(cw, ch, &err)) goto fail; continue; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { err = std::string("present: ") + ResultName(r); goto fail; }
        ++presents;
        if (!dumped && !shown.empty())
        {
            xenos::Image out;
            out.width = sc.extent.width;
            out.height = sc.extent.height;
            out.rgba = std::move(shown);
            xenos::WritePng(dumpPath, out);
            fprintf(stderr, "[vk] presented image (%ux%u) -> %s\n", out.width, out.height, dumpPath.c_str());
            dumped = true;
        }
    }
    {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "[vk] %llu presents in %.1f s (%.1f per second)\n", (unsigned long long)presents, t,
                t > 0 ? presents / t : 0.0);
    }
    if (dev.validationMessages)
    {
        fprintf(stderr, "[vk] validation layer reported %u message(s)\n", dev.validationMessages);
        rc = 3;
    }
    goto done;
fail:
    fprintf(stderr, "[vk] %s\n", err.c_str());
    rc = 1;
done:
    if (dev.device)
    {
        vkDeviceWaitIdle(dev.device);
        dev.DestroyImage(frame);
        sc.Destroy();
    }
    if (surface) vkDestroySurfaceKHR(dev.instance, surface, nullptr);
    dev.Destroy();
    WindowDestroy(win);
    return rc;
}
#else
int ReplayPresent(const xenos::Image&, const char*, double, const std::string&)
{
    fprintf(stderr, "hc09-replay was built without Vulkan (no Vulkan SDK found at configure time)\n");
    return 1;
}
uint8_t* GpuRenderBegin(size_t)
{
    fprintf(stderr, "hc09-replay was built without Vulkan (no Vulkan SDK found at configure time)\n");
    return nullptr;
}
void GpuRenderEnd() {}
void GpuRenderRepeatBegin() {}
void GpuRenderRepeatEnd(int) {}
#endif
