// Host audio output: the game's 5.1 mix -> WASAPI shared mode (Windows).
//
// The render pump (imports_audio.cpp) calls the game's render-driver callback;
// the game submits one 256-sample frame per callback. Frames go into a ring
// here, and a thread of our own feeds the default output device from it
// (event-driven, ~20 ms device buffer). The pump paces itself on the ring
// (HostAudioQueued), so the game's audio clock follows the device's and the
// ring neither drains nor grows.
//
// The device is opened at 48 kHz float with its own channel count (WASAPI
// converts the rate if the mix format differs). 5.1 goes through unchanged to
// devices with 6+ channels, and is downmixed for stereo (L = FL + .707 C +
// .707 RL, same for R; LFE dropped) and mono.
//
// HC09_AUDIO_OUT=0/1 overrides the default (on with a visible game window);
// HC09_AUDIO_VOLUME=0..1 scales the output (default 1).
#include "dev_env.h"
#include "host_audio.h"
#include "host_input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
void HostSetThreadName(void* thread, const char* name);   // runtime.cpp

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
constexpr GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Output
{
    std::mutex mu;
    std::vector<float> ring;            // kAudioChannels floats per frame
    size_t capFrames = 16384, head = 0, count = 0;
    std::atomic<bool> open{ false };
    std::atomic<long> queued{ -1 };
    float volume = 1.0f;
    std::atomic<uint64_t> underruns{ 0 };
    bool fed = false;                   // the game has submitted at least once

    Output()
    {
        ring.resize(capFrames * kAudioChannels);
        if (const char* v = Hc09Env("HC09_AUDIO_VOLUME")) volume = std::clamp(float(atof(v)), 0.0f, 1.0f);
        std::thread([this] { Run(); }).detach();
    }

    void Push(const float* s, size_t frames)
    {
        std::lock_guard<std::mutex> lk(mu);
        fed = true;
        for (size_t f = 0; f < frames; ++f)
        {
            if (count == capFrames) { head = (head + 1) % capFrames; --count; }   // drop the oldest
            size_t tail = (head + count) % capFrames;
            memcpy(&ring[tail * kAudioChannels], s + f * kAudioChannels, sizeof(float) * kAudioChannels);
            ++count;
        }
        if (open) queued = long(count);
    }

    // `frames` frames for a device with `ch` channels; silence where the ring is empty.
    void Pull(float* out, uint32_t frames, uint32_t ch)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (count < frames && fed) ++underruns;
        for (uint32_t f = 0; f < frames; ++f, out += ch)
        {
            float s[kAudioChannels] = {};
            if (count)
            {
                memcpy(s, &ring[head * kAudioChannels], sizeof(s));
                head = (head + 1) % capFrames;
                --count;
            }
            for (float& v : s) v *= volume;
            if (ch >= 6)
            {
                memcpy(out, s, sizeof(s));
                for (uint32_t c = 6; c < ch; ++c) out[c] = 0;
                continue;
            }
            float l = s[0] + 0.7071f * (s[2] + s[4]);
            float r = s[1] + 0.7071f * (s[2] + s[5]);
            if (ch == 1) { out[0] = std::clamp(0.5f * (l + r), -1.0f, 1.0f); continue; }
            out[0] = std::clamp(l, -1.0f, 1.0f);
            out[1] = std::clamp(r, -1.0f, 1.0f);
            for (uint32_t c = 2; c < ch; ++c) out[c] = 0;
        }
        queued = long(count);
    }

    // Open the default device and feed it until it fails; then retry every second.
    void Run()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HostSetThreadName(nullptr, "hc09 audio out");
        for (int attempt = 0;; ++attempt)
        {
            if (!Session(attempt == 0)) Sleep(1000);
        }
    }

    bool Session(bool firstTime)
    {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        WAVEFORMATEX* mix = nullptr;
        HANDLE ev = nullptr;
        bool ok = false;
        auto fail = [&](const char* what, HRESULT hr) {
            if (firstTime) fprintf(stderr, "[audio-out] %s failed (0x%08lX)\n", what, (unsigned long)hr);
        };
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&en));
        if (FAILED(hr)) { fail("CoCreateInstance(MMDeviceEnumerator)", hr); goto done; }
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr)) { fail("GetDefaultAudioEndpoint", hr); goto done; }
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (FAILED(hr)) { fail("IMMDevice::Activate", hr); goto done; }
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr)) { fail("GetMixFormat", hr); goto done; }
        {
            WAVEFORMATEXTENSIBLE fmt{};
            uint32_t ch = std::max<uint32_t>(1, mix->nChannels);
            fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            fmt.Format.nChannels = WORD(ch);
            fmt.Format.nSamplesPerSec = 48000;
            fmt.Format.wBitsPerSample = 32;
            fmt.Format.nBlockAlign = WORD(4 * ch);
            fmt.Format.nAvgBytesPerSec = 48000 * 4 * ch;
            fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            fmt.Samples.wValidBitsPerSample = 32;
            fmt.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                                    ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask
                                    : (ch == 1 ? SPEAKER_FRONT_CENTER : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
            fmt.SubFormat = kSubtypeFloat;
            DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /*20 ms*/, 0, &fmt.Format, nullptr);
            if (FAILED(hr)) { fail("IAudioClient::Initialize", hr); goto done; }
            ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            UINT32 bufFrames = 0;
            if (FAILED(hr = client->SetEventHandle(ev)) || FAILED(hr = client->GetBufferSize(&bufFrames)) ||
                FAILED(hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))))
            { fail("IAudioClient setup", hr); goto done; }
            fprintf(stderr, "[audio-out] WASAPI: %u channels (mask 0x%lX, device mix %lu Hz), buffer %u frames%s\n",
                    ch, (unsigned long)fmt.dwChannelMask, (unsigned long)mix->nSamplesPerSec, bufFrames,
                    ch >= 6 ? "" : ", 5.1 downmixed");
            BYTE* data = nullptr;
            if (SUCCEEDED(render->GetBuffer(bufFrames, &data))) render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr = client->Start())) { fail("IAudioClient::Start", hr); goto done; }
            {
                std::lock_guard<std::mutex> lk(mu);
                queued = long(count);
            }
            open = true;
            ok = true;
            for (;;)
            {
                if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
                UINT32 pad = 0;
                if (FAILED(hr = client->GetCurrentPadding(&pad))) break;
                UINT32 n = bufFrames - pad;
                if (!n) continue;
                if (FAILED(hr = render->GetBuffer(n, &data))) break;
                Pull(reinterpret_cast<float*>(data), n, ch);
                if (FAILED(hr = render->ReleaseBuffer(n, 0))) break;
            }
            fprintf(stderr, "[audio-out] device lost (0x%08lX): reopening\n", (unsigned long)hr);
            client->Stop();
        }
    done:
        open = false;
        queued = -1;
        if (ev) CloseHandle(ev);
        if (mix) CoTaskMemFree(mix);
        Release(render);
        Release(client);
        Release(dev);
        Release(en);
        return ok;
    }
};

Output& TheOutput() { static Output o; return o; }

}  // namespace

void HostAudioSubmit(const float* samples, size_t frames)
{
    if (HostAudioEnabled()) TheOutput().Push(samples, frames);
}

long HostAudioQueued()
{
    return HostAudioEnabled() ? TheOutput().queued.load() : -1;
}

uint64_t HostAudioUnderruns()
{
    return HostAudioEnabled() ? TheOutput().underruns.load() : 0;
}

#else  // no host audio on this platform yet

void HostAudioSubmit(const float*, size_t) {}
long HostAudioQueued() { return -1; }
uint64_t HostAudioUnderruns() { return 0; }

#endif

bool HostAudioEnabled()
{
    static const bool enabled = [] {
        bool on;
        if (const char* e = Hc09Env("HC09_AUDIO_OUT"); e && *e)
            on = strcmp(e, "0") != 0;
        else
            on = HostWindowVisible();
        fprintf(stderr, "[audio-out] host audio output %s\n", on ? "on" : "off");
        return on;
    }();
    return enabled;
}
