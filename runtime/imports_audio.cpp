// Audio: the XAudio render-driver client pump. The game's mix is played on the
// host (host_audio.cpp). XMA decoding: imports_xma.cpp.
//
// Render driver model (Xbox 360): the title registers {callback, context};
// the hardware then invokes callback(context) every 256 samples @ 48 kHz
// (5.33 ms). Inside the callback the title mixes a frame and hands it over
// with XAudioSubmitRenderDriverFrame. We emulate the hardware timer with a
// host thread that calls the guest callback on schedule. While a host audio
// device is open, the schedule follows the device's clock: the pump keeps
// 512-1536 sample frames queued for it.
//
// HC09_AUDIO_DUMP=<file>: write the raw submitted frames (guest bytes) from the
// first non-silent one on, HC09_AUDIO_DUMP_FRAMES of them (default 3000 = 16 s).
#include "dev_env.h"
#include "kernel.h"
#include "game_addr.h"
#include "host_audio.h"
#include <cstdlib>

#include <chrono>
#include <cstring>
#include <thread>

namespace {

struct RenderClient
{
    uint32_t callback = 0, context = 0, id = 0;
    std::atomic<bool> stop{ false };
    std::thread pump;
    PPCContext* ctx = nullptr;
    uint32_t stackBase = 0;
};
std::mutex g_audioMu;
std::vector<std::shared_ptr<RenderClient>> g_clients;
std::atomic<uint64_t> g_framesSubmitted{ 0 };

// HC09-specific diagnostic: the audio core singleton (pointer at 0x83229400)
// keeps a 6-entry per-hardware-thread table of buffer pairs at +12. Print it.
static void DumpAudioCoreSlots(uint64_t call)
{
    uint32_t core = GLoad32(GameAddr(0x83229400));
    fprintf(stderr, "[audio] callback #%llu: core=0x%08X", (unsigned long long)call + 1, core);
    if (core)
        for (int c = 0; c < 6; ++c)
            fprintf(stderr, " cpu%d={%08X,%08X}", c, GLoad32(core + 12 + c * 8), GLoad32(core + 16 + c * 8));
    fprintf(stderr, "\n");
}

void PumpMain(std::shared_ptr<RenderClient> rc)
{
    HostSetThreadName(nullptr, "hc09 audio pump");
    using namespace std::chrono;
    constexpr auto kFrame = microseconds(5333); // 256 samples @ 48 kHz
    auto next = steady_clock::now();
    uint32_t sp = (rc->stackBase + 0x40000 - 0x100) & ~0xFu;
    // XAudio's processing (and its render-driver callbacks) live on hardware
    // thread 4 on a retail console; HC09's audio core keys per-CPU state off
    // KPCR.current_cpu, so the pump must look like that thread. HC09_AUDIO_CPU
    // overrides for experiments.
    uint8_t cpu = 4;
    if (const char* e = Hc09Env("HC09_AUDIO_CPU")) cpu = uint8_t(atoi(e) & 7);
    uint32_t pcr = KernelRegisterHostThread("XAudio render driver", cpu, rc->stackBase, 0x40000, rc->ctx);
    InitThreadContext(*rc->ctx, sp, pcr);
    fprintf(stderr, "[audio] render pump started: callback=0x%08X context=0x%08X cpu=%u\n", rc->callback, rc->context, cpu);
    uint64_t calls = 0;
    while (!rc->stop.load())
    {
        if (calls < 4 && Hc09Env("HC09_AUDIO_DEBUG")) DumpAudioCoreSlots(calls);
        uint64_t submittedBefore = g_framesSubmitted.load();
        rc->ctx->r3.u64 = rc->context;
        CallGuest(rc->callback, *rc->ctx);
        if (++calls == 1 || calls == 100 || (calls % 18750) == 0) // 18750 frames ≈ 100 s
            fprintf(stderr, "[audio] pump: %llu callbacks, %llu frames submitted, host queue %ld, underruns %llu\n",
                    (unsigned long long)calls, (unsigned long long)g_framesSubmitted.load(), HostAudioQueued(),
                    (unsigned long long)HostAudioUnderruns());
        next += kFrame;
        // Follow the host device's clock: nudge the period by 1/16 to keep the
        // device's queue within 512..1536 frames (only while the game submits).
        long queued = HostAudioQueued();
        if (queued >= 0 && g_framesSubmitted.load() != submittedBefore)
        {
            if (queued < 512) next -= kFrame / 16;
            else if (queued > 1536) next += kFrame / 16;
            auto now = steady_clock::now();
            if (now - next > milliseconds(100)) next = now;   // after a stall: don't burst
        }
        std::this_thread::sleep_until(next);
    }
    fprintf(stderr, "[audio] render pump stopped\n");
}

} // namespace

// DWORD XAudioRegisterRenderDriverClient(XAUDIO_RENDER_DRIVER_CLIENT* r3 {callback, context}, DWORD* pClientId r4)
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    ImportLog("XAudioRegisterRenderDriverClient");
    uint32_t p = ctx.r3.u32;
    auto rc = std::make_shared<RenderClient>();
    rc->callback = GLoad32(p + 0);
    rc->context  = GLoad32(p + 4);
    rc->ctx = new PPCContext();
    rc->stackBase = GuestAlloc(0x40000, 0x1000);
    {
        std::lock_guard<std::mutex> lk(g_audioMu);
        rc->id = 0x41550000u | uint32_t(g_clients.size());
        g_clients.push_back(rc);
    }
    if (ctx.r4.u32) GStore32(ctx.r4.u32, rc->id);
    rc->pump = std::thread(PumpMain, rc);
    rc->pump.detach();
    ctx.r3.u64 = 0;
}

// DWORD XAudioUnregisterRenderDriverClient(DWORD clientId r3)
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    ImportLog("XAudioUnregisterRenderDriverClient");
    std::lock_guard<std::mutex> lk(g_audioMu);
    for (auto& rc : g_clients) if (rc->id == ctx.r3.u32) rc->stop = true;
    ctx.r3.u64 = 0;
}

// DWORD XAudioSubmitRenderDriverFrame(DWORD clientId r3, const void* samples r4)
// 256 samples of 6 channels, big-endian float, planar: channel c's samples are
// floats [c*256, c*256+256) (FL FR C LFE RL RR).
namespace {
constexpr uint32_t kSubmitSamples = 256;

struct AudioDump
{
    FILE* f = nullptr;
    uint64_t left = 0;
    bool started = false;
    AudioDump()
    {
        const char* path = Hc09Env("HC09_AUDIO_DUMP");
        if (!path || !*path) return;
        f = fopen(path, "wb");
        left = 3000;
        if (const char* n = Hc09Env("HC09_AUDIO_DUMP_FRAMES")) left = strtoull(n, nullptr, 10);
        fprintf(stderr, "[audio] dumping submitted frames to %s%s\n", path, f ? "" : " (cannot open)");
    }
    void Write(const uint8_t* p, size_t n, bool silent)
    {
        if (!f || !left) return;
        if (!started && silent) return;
        if (!started) fprintf(stderr, "[audio] dump starts at frame #%llu\n", (unsigned long long)g_framesSubmitted.load());
        started = true;
        fwrite(p, 1, n, f);
        if (--left == 0) { fclose(f); f = nullptr; fprintf(stderr, "[audio] dump complete\n"); }
    }
};
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    ImportLog("XAudioSubmitRenderDriverFrame");
    static AudioDump dump;
    static uint64_t nonSilent = 0;
    uint32_t src = ctx.r4.u32;
    uint64_t n = g_framesSubmitted.fetch_add(1) + 1;
    if (src)
    {
        float frame[kSubmitSamples * kAudioChannels];   // interleaved, host order
        bool silent = true;
        for (uint32_t c = 0; c < kAudioChannels; ++c)
            for (uint32_t i = 0; i < kSubmitSamples; ++i)
            {
                uint32_t bits = GLoad32(src + 4 * (c * kSubmitSamples + i));
                silent &= (bits & 0x7FFFFFFF) == 0;
                float v;
                memcpy(&v, &bits, 4);
                frame[i * kAudioChannels + c] = v;
            }
        if (!silent && nonSilent++ == 0)
            fprintf(stderr, "[audio] first non-silent frame from the game: #%llu\n", (unsigned long long)n);
        if ((n % 18750) == 0)
            fprintf(stderr, "[audio] %llu of %llu submitted frames non-silent\n", (unsigned long long)nonSilent,
                    (unsigned long long)n);
        dump.Write(g_base + src, sizeof(frame), silent);
        HostAudioSubmit(frame, kSubmitSamples);
    }
    ctx.r3.u64 = 0;
}

// DWORD XAudioGetSpeakerConfig(DWORD* pConfig r3) — stereo.
PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    ImportLog("XAudioGetSpeakerConfig");
    if (ctx.r3.u32) GStore32(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}
// DWORD XAudioGetVoiceCategoryVolume(DWORD category r3, float* pVolume r4)
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    ImportLog("XAudioGetVoiceCategoryVolume");
    if (ctx.r4.u32) GStore32(ctx.r4.u32, 0x3F800000); // 1.0f
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    ImportLog("XAudioGetVoiceCategoryVolumeChangeMask");
    if (ctx.r4.u32) GStore32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

// ---- misc that showed up alongside audio init -------------------------------
PPC_FUNC(__imp__KeEnableFpuExceptions) { ImportLog("KeEnableFpuExceptions"); ctx.r3.u64 = 0; } // never trap on host
// DWORD MmQueryAddressProtect(PVOID r3) -> PAGE_READWRITE for anything mapped
PPC_FUNC(__imp__MmQueryAddressProtect) { ImportLog("MmQueryAddressProtect"); ctx.r3.u64 = 0x04; }
