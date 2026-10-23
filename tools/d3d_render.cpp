// hc09-d3d-replay --render: a D3D-level capture rendered by the native renderer
// (runtime/gpu_vk/vk_native) through the same front end the live game uses
// (runtime/xenos/d3d_frontend: D3D calls -> bindings, clears, draws, resolves,
// present; predicated tiling disappears; work runs at D3D's submit points).
//
// --bench N: the frame's back-end calls (bindings, clears, draws with their
// registers, resolves, the present) are recorded while it renders and then
// replayed N more times through the renderer, as the live renderer thread would
// draw a frame that repeats (textures cached, resolve targets decoded again);
// prints ms/frame and, with HC09_NATIVE_PROF=1, the renderer's sections.
//
// Outputs in <out-dir>: frame.png (the front buffer Swap presents, decoded from
// guest memory as the console's scanout would see it) and resolve_NN_<dest>.png
// (every resolve's destination texture after the copy).
#include "d3d_frontend.h"
#include "d3d_replay.h"
#include "d3d_stream.h"
#include "xenos.h"
#include "xenos_shader.h"
#include "xenos_texture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t kPhysSize = 0x20000000;
uint8_t* g_mem = nullptr;                      // guest physical memory (GPU-visible when rendering)
std::vector<uint64_t> g_pageEpoch(kPhysSize >> 12, 0);
uint64_t g_epoch = 1;
}  // namespace

// Host hooks of runtime/xenos for the replay: memory changes as the capture's
// pages arrive, so textures decoded from it are tracked per page.
namespace xenos {
uint32_t HostReg(uint32_t) { return 0; }   // the renderer reads registers through SetThreadRegisterFile
const uint32_t* HostRegFile(bool* bigEndian) { *bigEndian = false; return nullptr; }
const uint8_t* HostPhys(uint32_t pa)
{
    static uint8_t zero[4096];
    return g_mem ? g_mem + (pa & (kPhysSize - 1)) : zero;
}
uint64_t HostSwapCount() { return 0; }
void HostWatchWrites(uint32_t, uint32_t) {}
uint64_t HostTrackWrites(uint32_t, uint32_t) { return g_epoch; }
bool HostWrittenSince(uint32_t pa, uint32_t bytes, uint64_t& epoch)
{
    pa &= kPhysSize - 1;
    uint64_t end = std::min<uint64_t>(uint64_t(pa) + bytes, kPhysSize);
    for (uint64_t p = pa >> 12; p < (end + 4095) >> 12; ++p)
        if (g_pageEpoch[size_t(p)] > epoch) return true;
    return false;
}
}  // namespace xenos

#ifdef HC09_HAVE_VULKAN
#include "vk_device.h"
#include "vk_native.h"

namespace {

struct NativeSink : ReplaySink, d3dn::Backend
{
    hc09vk::Device dev;
    hc09vk::Buffer guest;
    hc09vk::NativeRenderer renderer;
    d3dn::Frontend frontend{ *this };
    std::unordered_map<uint32_t, std::unique_ptr<d3dn::Frontend>> others;   // command-buffer devices
    uint32_t mainDevice = 0;   // physical address
    void MainDevice(uint32_t pa) override { mainDevice = pa; }
    d3dn::Frontend& For(uint32_t dev)
    {
        if (!dev || (dev & 0x1FFFFFFF) == mainDevice) return frontend;
        auto& f = others[dev];
        if (!f) f = std::make_unique<d3dn::Frontend>(*this, false);
        return *f;
    }
    d3dn::Frontend& For(const Call& c) { return For(c.r[0]); }
    std::string dir;
    std::unordered_set<uint32_t> owned;   // pages a native resolve wrote: the capture's later copies are stale
    std::vector<uint32_t> regs = std::vector<uint32_t>(xenos::kRegisterCount, 0);   // the back end's register file
    uint32_t resolves = 0, scratch = 0;
    static constexpr uint32_t kScratchBase = 0x01000000, kScratchEnd = 0x02000000;   // inline vertex data
    bool presented = false;
    // --bench: the back-end calls of the frame, replayed by Bench().
    struct BenchOp
    {
        enum Kind { kTargets, kClear, kDraw, kResolve, kPresent } kind = kDraw;
        d3dn::Surface rt[4], depth;
        uint32_t flags = 0, stencil = 0;
        int32_t rect[4] = {};
        float color[4] = {}, z = 0;
        d3dn::DrawOp op;
        d3dn::ResolveOp r;
        uint32_t front = 0, width = 0, height = 0;
    };
    int bench = 0;
    std::vector<BenchOp> benchOps;
    // The front end's time: calls into it (Event/Draw/Patched/Kick) minus the back end's.
    using clock = std::chrono::steady_clock;
    double feSeconds[3] = {}, beSeconds = 0;   // [calls, draws, kicks]
    double fePass[3] = {}, bePass = 0;          // the passes after the first (--repeat)
    int passes = 0;
    void Pass(int n) override
    {
        if (n == 1)
        {
            fprintf(stderr, "[native] front end, first pass: %.2f ms (calls %.2f, draws %.2f, kicks %.2f; back end %.2f)\n",
                    1000.0 * (feSeconds[0] + feSeconds[1] + feSeconds[2] - beSeconds), 1000.0 * feSeconds[0], 1000.0 * feSeconds[1],
                    1000.0 * feSeconds[2], 1000.0 * beSeconds);
            for (double& x : feSeconds) x = 0;
            beSeconds = 0;
        }
        else if (n > 1)
        {
            for (int k = 0; k < 3; ++k) { fePass[k] += feSeconds[k]; feSeconds[k] = 0; }
            bePass += beSeconds;
            beSeconds = 0;
        }
        passes = n;
    }
    struct Timed
    {
        double& acc;
        clock::time_point t0 = clock::now();
        ~Timed() { acc += std::chrono::duration<double>(clock::now() - t0).count(); }
    };

    bool Init(const char* outDir)
    {
        dir = outDir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string err;
        if (!dev.CreateInstance({}, &err) || !dev.CreateDevice(VK_NULL_HANDLE, &err)) { fprintf(stderr, "[native] %s\n", err.c_str()); return false; }
        VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        if (!dev.CreateBuffer(kPhysSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cached, guest) &&
            !dev.CreateBuffer(kPhysSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, guest))
        { fprintf(stderr, "[native] cannot allocate guest memory\n"); return false; }
        memset(guest.mapped, 0, kPhysSize);
        g_mem = static_cast<uint8_t*>(guest.mapped);
        const char* ex = getenv("HC09_GPU_EXACT");
        bool exact = !ex || strcmp(ex, "0") != 0;   // offline default: exact (comparable with the reference)
        if (!renderer.Init(dev, guest, &err, exact)) { fprintf(stderr, "[native] renderer: %s\n", err.c_str()); return false; }
        fprintf(stderr, "[native] rendering on %s\n", dev.props.deviceName);
        return true;
    }
    ~NativeSink() override
    {
        renderer.Shutdown();
        g_mem = nullptr;
        dev.DestroyBuffer(guest);
        dev.Destroy();
    }

    // ---- ReplaySink: memory and calls from the capture ---------------------------
    void Page(uint32_t page, const uint8_t* data) override
    {
        page &= 0x1FFFF000u;
        if (owned.count(page)) return;
        memcpy(g_mem + page, data, 4096);
        g_pageEpoch[page >> 12] = ++g_epoch;
    }
    void Write(uint32_t va, const uint8_t* data, uint32_t n) override
    {
        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t a = (va + i) & (kPhysSize - 1);
            g_mem[a] = data[i];
            g_pageEpoch[a >> 12] = ++g_epoch;
        }
    }
    static d3dn::CallArgs Args(const Call& c)
    {
        d3dn::CallArgs a;
        a.fn = c.fn;
        memcpy(a.r, c.r, sizeof(a.r));
        a.f1 = c.f1;
        a.ret = c.ret;
        for (auto& [tag, b] : c.blobs)
            if (tag < 8) a.blob[tag] = { b.data(), uint32_t(b.size()) };
        return a;
    }
    // The front end reads guest memory as the live game's does (flat, not the
    // capture's page map): its time here is close to the live game thread's.
    struct FlatMem : d3dn::GuestMemory
    {
        uint32_t Load32(uint32_t va) const override { return d3dn::BE32(g_mem + (va & (kPhysSize - 1))); }
        const uint8_t* Ptr(uint32_t va, uint32_t n) const override
        {
            uint32_t pa = va & (kPhysSize - 1);
            return uint64_t(pa) + n <= kPhysSize ? g_mem + pa : nullptr;
        }
    } flat;
    void Event(const Call& c, const uint8_t* device, const Memory&) override
    {
        d3dn::CallArgs a = Args(c);
        Timed t{ feSeconds[0] };
        For(c).Call(a, device, flat);
    }
    void Draw(const Call& c, const uint8_t* before, const uint8_t* after, const Memory&) override
    {
        d3dn::CallArgs a = Args(c);
        Timed t{ feSeconds[1] };
        For(c).Draw(a, before, after, flat);
    }
    void Patched(uint32_t device, uint32_t vs, uint32_t variant, uint32_t dst, const uint8_t* code, uint32_t bytes, const Memory&) override
    {
        Timed t{ feSeconds[0] };
        For(device).VertexShaderPatched(vs, variant, dst, code, bytes, flat);
    }
    void Kick() override
    {
        Timed t{ feSeconds[2] };
        frontend.Kick();
    }

    // ---- d3dn::Backend: the native renderer --------------------------------------
    void SetTargets(const d3dn::Surface rt[4], const d3dn::Surface& depth) override
    {
        Timed t{ beSeconds };
        if (bench)
        {
            BenchOp b;
            b.kind = BenchOp::kTargets;
            for (int i = 0; i < 4; ++i) b.rt[i] = rt[i];
            b.depth = depth;
            benchOps.push_back(std::move(b));
        }
        renderer.SetTargets(rt, depth);
    }
    void Clear(uint32_t flags, const int32_t rect[4], const float color[4], float z, uint32_t stencil) override
    {
        Timed t{ beSeconds };
        if (bench)
        {
            BenchOp b;
            b.kind = BenchOp::kClear;
            b.flags = flags;
            memcpy(b.rect, rect, sizeof(b.rect));
            memcpy(b.color, color, sizeof(b.color));
            b.z = z;
            b.stencil = stencil;
            benchOps.push_back(std::move(b));
        }
        renderer.Clear(flags, rect, color, z, stencil);
    }
    void Draw(d3dn::DrawOp& op) override
    {
        Timed t{ beSeconds };
        if (bench)
        {
            // Constants loaded through a pointer: their values now, as registers.
            BenchOp b;
            b.kind = BenchOp::kDraw;
            b.op = op;
            for (const d3dn::DrawOp::Load& l : op.loads)
                for (uint32_t k = 0; k < l.dwords; ++k) b.op.regs.emplace_back(l.reg + k, d3dn::BE32(g_mem + ((l.address + 4 * k) & (kPhysSize - 1))));
            b.op.loads.clear();
            benchOps.push_back(std::move(b));
        }
        DoDraw(op);
    }
    void DoDraw(d3dn::DrawOp& op)
    {
        for (auto [i, v] : op.regs) regs[i] = v;
        for (const d3dn::DrawOp::Load& l : op.loads)
            for (uint32_t k = 0; k < l.dwords; ++k) regs[l.reg + k] = d3dn::BE32(g_mem + ((l.address + 4 * k) & (kPhysSize - 1)));
        if (op.stateOnly) return;
        uint32_t f0 = 0, f1 = 0;
        if (!op.inlineData.empty())
        {
            // Inline vertex data: into scratch memory, vertex fetch constant 95 at it (as
            // D3D points it into the ring), for this draw only.
            uint32_t bytes = uint32_t(op.inlineData.size());
            if (scratch + bytes + 64 > kScratchEnd - kScratchBase) scratch = 0;
            uint32_t pa = kScratchBase + scratch;
            memcpy(g_mem + pa, op.inlineData.data(), bytes);
            for (uint32_t p = pa >> 12; p <= (pa + bytes) >> 12; ++p) g_pageEpoch[p] = ++g_epoch;
            scratch += (bytes + 63) & ~63u;
            f0 = regs[xenos::reg::FETCH_CONSTANTS + 190];
            f1 = regs[xenos::reg::FETCH_CONSTANTS + 191];
            regs[xenos::reg::FETCH_CONSTANTS + 190] = pa | 3;
            regs[xenos::reg::FETCH_CONSTANTS + 191] = (((bytes + 3) / 4) << 2) | 2;
        }
        xenos::SetThreadRegisterFile(regs.data());
        renderer.Draw(op.d, op.vsHash, op.vs, op.psHash, op.ps);
        xenos::SetThreadRegisterFile(nullptr);
        if (!op.inlineData.empty())
        {
            regs[xenos::reg::FETCH_CONSTANTS + 190] = f0;
            regs[xenos::reg::FETCH_CONSTANTS + 191] = f1;
        }
    }
    void Resolve(const d3dn::ResolveOp& r, const uint32_t destFetch[6]) override
    {
        Timed t{ beSeconds };
        if (bench)
        {
            BenchOp b;
            b.kind = BenchOp::kResolve;
            b.r = r;
            benchOps.push_back(std::move(b));
        }
        renderer.Resolve(r);
        ++resolves;
        if (!r.copy) return;
        uint32_t rows = (uint32_t(r.y1) + 31) & ~31u, span = r.destPitch * rows * r.destBytes;
        for (uint32_t p = r.destBase & ~0xFFFu; p < r.destBase + span; p += 0x1000) owned.insert(p & 0x1FFFF000u);
        // HC09_REPLAY_RESOLVE_PNGS=0: no resolve_*.png (resolved data stays where the
        // renderer keeps it, as live; frame.png only).
        static const bool pngs = [] { const char* e = getenv("HC09_REPLAY_RESOLVE_PNGS"); return !e || strcmp(e, "0") != 0; }();
        if (!pngs) return;
        renderer.SyncToGuest(r.destBase, span);   // the resolve's PNG is decoded on the CPU
        xenos::TextureFetch tf;
        memcpy(tf.dword, destFetch, sizeof(tf.dword));
        xenos::Image img;
        char name[64];
        snprintf(name, sizeof(name), "/resolve_%02u_%08X.png", resolves, r.destBase);
        if (xenos::DecodeTexture2D(tf, img)) xenos::WritePng(dir + name, img);
    }
    void Present(uint32_t base, uint32_t w, uint32_t h) override
    {
        Timed t{ beSeconds };
        if (bench)
        {
            BenchOp b;
            b.kind = BenchOp::kPresent;
            b.front = base;
            b.width = w;
            b.height = h;
            benchOps.push_back(std::move(b));
        }
        if (renderer.Scale() > 1)   // the presented image at resolution scale
        {
            renderer.DecodeFrontBuffer(base, w, h);
            std::vector<uint32_t> px;
            uint32_t sw = 0, sh = 0;
            if (renderer.ReadFront(px, sw, sh) && xenos::WritePng(dir + "/frame_scaled.png", xenos::Image{ sw, sh, std::move(px) }))
                fprintf(stderr, "[native] frame_scaled.png: %ux%u\n", sw, sh);
        }
        renderer.EndFrame();
        renderer.SyncToGuest(base, ((w + 31) & ~31u) * ((h + 31) & ~31u) * 4);   // the front buffer is read on the CPU
        xenos::Image img;
        if (xenos::DecodeFrontBuffer(base, w, h, img) && xenos::WritePng(dir + "/frame.png", img))
            fprintf(stderr, "[native] frame.png: front buffer 0x%08X %ux%u\n", base, w, h);
        presented = true;
    }

    // The recorded frame again, `bench` times (registers from zero each time, as the
    // first pass started).
    void Bench()
    {
        using clock = std::chrono::steady_clock;
        hc09vk::NativeProfile p0;
        if (const hc09vk::NativeProfile* p = renderer.Profile()) p0 = *p;
        renderer.SetResolveWaits(false);   // as the live renderer
        std::vector<double> ms;
        auto all0 = clock::now();
        for (int it = 0; it < bench; ++it)
        {
            auto t0 = clock::now();
            std::fill(regs.begin(), regs.end(), 0u);
            for (BenchOp& b : benchOps)
                switch (b.kind)
                {
                case BenchOp::kTargets: renderer.SetTargets(b.rt, b.depth); break;
                case BenchOp::kClear: renderer.Clear(b.flags, b.rect, b.color, b.z, b.stencil); break;
                case BenchOp::kDraw: DoDraw(b.op); break;
                case BenchOp::kResolve: renderer.Resolve(b.r); break;
                case BenchOp::kPresent:
                    renderer.DecodeFrontBuffer(b.front, b.width, b.height);
                    renderer.EndFrame();
                    break;
                }
            ms.push_back(1000.0 * std::chrono::duration<double>(clock::now() - t0).count());
        }
        renderer.Finish();
        double wall = std::chrono::duration<double>(clock::now() - all0).count();
        std::vector<double> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        fprintf(stderr, "[bench] %d frames of %zu back-end calls: %.2f ms/frame (median %.2f, min %.2f, max %.2f) = %.0f frames/s\n", bench,
                benchOps.size(), 1000.0 * wall / bench, sorted[sorted.size() / 2], sorted.front(), sorted.back(), bench / wall);
        if (const hc09vk::NativeProfile* p = renderer.Profile())
        {
            std::string line;
            char buf[128];
            double busy = 0;
            for (int i = 0; i < hc09vk::NativeProfile::kSections; ++i)
            {
                double v = 1000.0 * (p->sec[i] - p0.sec[i]) / bench;
                busy += v;
                uint64_t w = p->waits[i] - p0.waits[i];
                if (w) snprintf(buf, sizeof(buf), " %s %.2f (%.1f/frame)", hc09vk::kNativeSectionNames[i], v, double(w) / bench);
                else snprintf(buf, sizeof(buf), " %s %.2f", hc09vk::kNativeSectionNames[i], v);
                line += buf;
            }
            fprintf(stderr, "[bench] ms/frame: sections %.2f, GPU %.2f in %.1f submits/frame;%s\n", busy,
                    1000.0 * (p->gpuSeconds - p0.gpuSeconds) / bench, double(p->submits - p0.submits) / bench, line.c_str());
            std::string g;
            for (int i = 0; i < hc09vk::NativeProfile::kGpuKinds; ++i)
            {
                snprintf(buf, sizeof(buf), " %s %.2f", hc09vk::kNativeGpuKindNames[i], 1000.0 * (p->gpuKind[i] - p0.gpuKind[i]) / bench);
                g += buf;
            }
            fprintf(stderr, "[bench] GPU ms/frame by kind:%s\n", g.c_str());
        }
    }

    int Finish() override
    {
        frontend.Kick();
        renderer.EndFrame();
        renderer.Finish();
        const hc09vk::RendererStats& s = renderer.Stats();
        fprintf(stderr, "[native] %u draws (%u drawn, %u skipped), %u clears, %u resolves, %u textures, %u shaders, %u pipelines; "
                        "renderer %.2f s (textures %.2f, shaders %.2f, pipelines %.2f, waits %.2f)\n",
                frontend.draws, s.draws, s.skipped, frontend.clears, resolves, s.textures + s.texturesOnGpu, s.shaders, s.pipelines,
                s.rendererSeconds, s.textureSeconds, s.compileSeconds, s.pipelineSeconds, s.waitSeconds);
        for (auto& [why, n] : s.skipReasons) fprintf(stderr, "[native]   skipped %u: %s\n", n, why.c_str());
        if (const d3dn::RegisterState& rs = frontend.State(); rs.statDraws)
            fprintf(stderr, "[native] dirty masks (main device): %llu draws, %llu compared every block, %.1f blocks read per draw; %llu check mismatches\n",
                    (unsigned long long)rs.statDraws, (unsigned long long)rs.statFull, double(rs.statBlocks) / double(rs.statDraws),
                    (unsigned long long)rs.statMismatch);
        // Kicks (and Swap's) run the back end: the front end's own time is the total less it.
        Pass(passes);
        if (passes > 1)
            fprintf(stderr, "[native] front end per pass after the first: %.2f ms (calls %.2f, draws %.2f, kicks %.2f; back end %.2f)\n",
                    1000.0 * (fePass[0] + fePass[1] + fePass[2] - bePass) / (passes - 1), 1000.0 * fePass[0] / (passes - 1),
                    1000.0 * fePass[1] / (passes - 1), 1000.0 * fePass[2] / (passes - 1), 1000.0 * bePass / (passes - 1));
        if (bench && presented) Bench();
        if (dev.validationMessages) fprintf(stderr, "[native] validation layer: %u messages\n", dev.validationMessages);
        return presented ? 0 : 1;
    }
};

}  // namespace

ReplaySink* CreateNativeSink(const char* outDir, int bench)
{
    auto* s = new NativeSink;
    if (!s->Init(outDir)) { delete s; return nullptr; }
    s->bench = bench;
    return s;
}

#else
ReplaySink* CreateNativeSink(const char*, int) { return nullptr; }
#endif
