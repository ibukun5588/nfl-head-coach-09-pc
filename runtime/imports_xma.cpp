// XMA decoder contexts: the Xenos audio hardware's XMA decoder, emulated with
// FFmpeg's xma2 decoder (libavcodec, LGPL, loaded as DLLs; chosen
// 2026-10-05). Without FFmpeg (HC09_HAVE_FFMPEG undefined) or with HC09_XMA=0
// every frame decodes to silence at the same pace.
//
// Hardware model (as the XMA* kernel exports present it; layout and
// semantics cross-checked against Xenia's xboxkrnl_audio_xma.cc and
// apu/xma_context.cc, BSD — reference only, nothing copied):
// - Input: two buffers of 2048-byte packets, validated by the title. The
//   decoder consumes the current one, clears its valid bit and moves to the
//   other. A packet header's skip count says how many packets of other
//   streams follow before this stream's next one (5.1 music is three stereo
//   streams interleaved in one buffer, one context each).
// - Output: a ring of 256-byte blocks (output_buffer_block_count of them) of
//   16-bit big-endian PCM, stereo interleaved. The decoder writes from the
//   write offset up to the title's read offset. Output valid = the decoder may
//   write; it clears it when the ring is full or the input has run dry, and
//   the title sets it again after consuming.
// Decoding runs on a host thread ("XMA decoder"), like the hardware, kicked by
// the title's calls and every 2 ms. FFmpeg decodes whole packets; samples that
// do not fit the ring yet wait in a per-context FIFO.
//
// HC09_XMA_DEBUG=1 logs context setup and the first packets of each context.
// HC09_XMA_DUMP=<dir>: per context, the input packets in decode order
// (xma_<ctx>.bin) and the decoded PCM (xma_<ctx>.pcm, 16-bit big-endian).
#include "dev_env.h"
#include "kernel.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(HC09_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}
#endif

namespace {

constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kSamplesPerFrame = 512;
constexpr int kSampleRates[4] = { 24000, 32000, 44100, 48000 };

bool g_xmaDebug = false;
bool g_xmaDecode = true;
std::string g_xmaDumpDir;

struct XmaCtx
{
    std::mutex mu;
    uint32_t guest = 0;             // the context's guest address
    FILE* dumpIn = nullptr;
    FILE* dumpPcm = nullptr;
    bool allocated = false, enabled = false;
    // Input buffers (guest virtual addresses) and the decode position.
    uint32_t inPtr[2] = {}, inPackets[2] = {};
    bool inValid[2] = {};
    int cur = 0;                    // buffer being decoded
    uint32_t nextPacket = 0;        // next packet of this stream in buffer `cur`
    uint32_t skipCarry = 0;         // packets to skip at the start of the next buffer
    // Output ring.
    uint32_t outPtr = 0, outBlocks = 0, outRead = 0, outWrite = 0;
    bool outValid = false;
    // Format.
    bool stereo = false;
    int rateCode = 3;
    // Decoded samples (big-endian int16, interleaved) not yet in the ring.
    std::vector<uint8_t> fifo;
    size_t fifoHead = 0;
    // Statistics.
    uint64_t packets = 0, frames = 0, errors = 0;
#if defined(HC09_HAVE_FFMPEG)
    AVCodecContext* av = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int avRate = 0, avChannels = 0;
#endif

    size_t FifoBytes() const { return fifo.size() - fifoHead; }
    void FifoClear() { fifo.clear(); fifoHead = 0; }
};

std::mutex g_xmaMu;                                         // the map only
std::unordered_map<uint32_t, std::unique_ptr<XmaCtx>> g_xma;   // never erased: released contexts are reused
std::condition_variable g_xmaKick;
std::mutex g_kickMu;
bool g_kicked = false;

XmaCtx* Find(uint32_t p)
{
    std::lock_guard<std::mutex> lk(g_xmaMu);
    auto it = g_xma.find(p);
    if (it != g_xma.end()) return it->second.get();
    auto& slot = g_xma[p];
    slot = std::make_unique<XmaCtx>();
    slot->guest = p;
    return slot.get();
}

void Kick()
{
    { std::lock_guard<std::mutex> lk(g_kickMu); g_kicked = true; }
    g_xmaKick.notify_one();
}

// Append one decoded frame: `ch` planar float channels of `n` samples.
void PushPcm(XmaCtx& c, const float* const* planes, int ch, int n)
{
    int outCh = c.stereo ? 2 : 1;
    size_t at = c.fifo.size();
    c.fifo.resize(at + size_t(n) * outCh * 2);
    uint8_t* o = c.fifo.data() + at;
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < outCh; ++k)
        {
            float v = planes[k < ch ? k : 0][i];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            int16_t s = int16_t(v * 32767.0f);
            *o++ = uint8_t(uint16_t(s) >> 8);
            *o++ = uint8_t(s);
        }
    ++c.frames;
}

#if defined(HC09_HAVE_FFMPEG)
bool OpenDecoder(XmaCtx& c)
{
    int rate = kSampleRates[c.rateCode & 3], ch = c.stereo ? 2 : 1;
    if (c.av && c.avRate == rate && c.avChannels == ch) return true;
    if (c.av) avcodec_free_context(&c.av);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec) return false;
    c.av = avcodec_alloc_context3(codec);
    c.av->sample_rate = rate;
    av_channel_layout_default(&c.av->ch_layout, ch);
    c.av->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX tail: one stream; channel mask 0 (unspecified order).
    c.av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    c.av->extradata_size = 34;
    c.av->extradata[0] = 1;
    int r = avcodec_open2(c.av, codec, nullptr);
    if (r < 0)
    {
        fprintf(stderr, "[xma] avcodec_open2 failed (%d): rate %d, %d ch\n", r, rate, ch);
        avcodec_free_context(&c.av);
        return false;
    }
    if (!c.pkt) c.pkt = av_packet_alloc();
    if (!c.frame) c.frame = av_frame_alloc();
    c.avRate = rate;
    c.avChannels = ch;
    return true;
}
#endif

// Decode one packet of this stream into the FIFO.
void DecodePacketTo(XmaCtx& c, const uint8_t* packet);

void DecodePacket(XmaCtx& c, const uint8_t* packet)
{
    if (g_xmaDumpDir.empty()) { DecodePacketTo(c, packet); return; }
    if (!c.dumpIn)
    {
        char name[64];
        snprintf(name, sizeof(name), "/xma_%08X.bin", c.guest);
        c.dumpIn = fopen((g_xmaDumpDir + name).c_str(), "wb");
        snprintf(name, sizeof(name), "/xma_%08X.pcm", c.guest);
        c.dumpPcm = fopen((g_xmaDumpDir + name).c_str(), "wb");
    }
    size_t before = c.fifo.size();
    DecodePacketTo(c, packet);
    if (c.dumpIn) { fwrite(packet, 1, kPacketBytes, c.dumpIn); fflush(c.dumpIn); }
    if (c.dumpPcm && c.fifo.size() > before) { fwrite(c.fifo.data() + before, 1, c.fifo.size() - before, c.dumpPcm); fflush(c.dumpPcm); }
}

void DecodePacketTo(XmaCtx& c, const uint8_t* packet)
{
    ++c.packets;
#if defined(HC09_HAVE_FFMPEG)
    if (g_xmaDecode && OpenDecoder(c))
    {
        // The packet must stay readable past its end (AV_INPUT_BUFFER_PADDING_SIZE).
        uint8_t buf[kPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE] = {};
        memcpy(buf, packet, kPacketBytes);
        c.pkt->data = buf;
        c.pkt->size = kPacketBytes;
        int r = avcodec_send_packet(c.av, c.pkt);
        if (r < 0 && c.errors++ < 8)
            fprintf(stderr, "[xma] context: avcodec_send_packet error %d (packet %llu)\n", r,
                    (unsigned long long)c.packets);
        while (r >= 0)
        {
            r = avcodec_receive_frame(c.av, c.frame);
            if (r < 0) break;
            PushPcm(c, reinterpret_cast<const float* const*>(c.frame->extended_data), c.frame->ch_layout.nb_channels,
                    c.frame->nb_samples);
            av_frame_unref(c.frame);
        }
        return;
    }
#endif
    // Silence: as many frames as begin in this packet (6-bit count in the header).
    static const float zeros[kSamplesPerFrame] = {};
    const float* planes[2] = { zeros, zeros };
    for (int f = packet[0] >> 2; f > 0; --f) PushPcm(c, planes, 2, kSamplesPerFrame);
}

// Move FIFO samples into the guest's output ring; true if the ring is now full.
bool DrainFifo(XmaCtx& c)
{
    if (!c.outBlocks) return false;
    // write == read means empty while output is valid; the decoder stops (and
    // clears valid) when its write offset catches up with the title's read offset.
    while (c.FifoBytes() >= kBlockBytes)
    {
        memcpy(g_base + c.outPtr + c.outWrite * kBlockBytes, c.fifo.data() + c.fifoHead, kBlockBytes);
        c.fifoHead += kBlockBytes;
        c.outWrite = (c.outWrite + 1) % c.outBlocks;
        if (c.outWrite == c.outRead) return true;   // caught up with the title: full
    }
    if (c.fifoHead == c.fifo.size()) c.FifoClear();
    return false;
}

// One decoder pass over a context: fill its output ring as far as input allows.
void Work(XmaCtx& c)
{
    if (!c.allocated || !c.enabled || !c.outValid) return;
    for (;;)
    {
        if (DrainFifo(c))
        {
            c.outValid = false;   // full: the title consumes and validates again
            return;
        }
        // The FIFO is short of a block: decode the next packet of this stream.
        if (!c.inValid[c.cur])
        {
            if (!c.inValid[c.cur ^ 1])
            {
                if (c.FifoBytes() == 0) c.outValid = false;   // input ran dry
                return;
            }
            c.cur ^= 1;
            c.nextPacket = c.skipCarry;
            c.skipCarry = 0;
        }
        if (c.nextPacket >= c.inPackets[c.cur])
        {
            c.skipCarry = c.nextPacket - c.inPackets[c.cur];
            c.inValid[c.cur] = false;
            c.cur ^= 1;
            c.nextPacket = c.skipCarry;
            c.skipCarry = 0;
            continue;
        }
        const uint8_t* packet = g_base + c.inPtr[c.cur] + c.nextPacket * kPacketBytes;
        size_t before = c.FifoBytes();
        uint64_t framesBefore = c.frames;
        DecodePacket(c, packet);
        if (g_xmaDebug && c.packets <= 6)
        {
            int peak = 0;
            for (size_t i = c.fifoHead + before; i + 1 < c.fifo.size(); i += 2)
                peak = std::max(peak, std::abs(int(int16_t(uint16_t(c.fifo[i] << 8 | c.fifo[i + 1])))));
            uint32_t off = ((packet[0] & 3u) << 13 | packet[1] << 5 | packet[2] >> 3) + 32;
            fprintf(stderr, "[xma] ctx 0x%08X packet #%llu (buffer %d, index %u of %u): header %02X%02X%02X%02X = %u frames, "
                    "first frame at bit %u, metadata %u, skip %u -> %llu frames decoded, peak %d\n",
                    c.guest, (unsigned long long)c.packets, c.cur, c.nextPacket, c.inPackets[c.cur], packet[0], packet[1],
                    packet[2], packet[3], packet[0] >> 2, off, packet[2] & 7, packet[3],
                    (unsigned long long)(c.frames - framesBefore), peak);
        }
        c.nextPacket += packet[3] + 1u;   // skip the other streams' packets
        if (c.nextPacket >= c.inPackets[c.cur])
        {
            // Buffer consumed: hand it back and continue in the other one.
            c.skipCarry = c.nextPacket - c.inPackets[c.cur];
            c.inValid[c.cur] = false;
            c.cur ^= 1;
            c.nextPacket = c.skipCarry;
            c.skipCarry = 0;
        }
    }
}

void DecoderThread()
{
    fprintf(stderr, "[xma] decoder thread started (%s)\n",
#if defined(HC09_HAVE_FFMPEG)
            g_xmaDecode ? "FFmpeg xma2" : "silence (HC09_XMA=0)");
#else
            "silence: built without FFmpeg");
#endif
    std::vector<XmaCtx*> list;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lk(g_kickMu);
            g_xmaKick.wait_for(lk, std::chrono::milliseconds(2), [] { return g_kicked; });
            g_kicked = false;
        }
        list.clear();
        {
            std::lock_guard<std::mutex> lk(g_xmaMu);
            for (auto& [p, c] : g_xma) list.push_back(c.get());
        }
        for (XmaCtx* c : list)
        {
            std::lock_guard<std::mutex> lk(c->mu);
            Work(*c);
        }
    }
}

void StartDecoder()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* d = Hc09Env("HC09_XMA_DEBUG")) g_xmaDebug = strcmp(d, "0") != 0;
        if (const char* x = Hc09Env("HC09_XMA")) g_xmaDecode = strcmp(x, "0") != 0;
        if (const char* d = Hc09Env("HC09_XMA_DUMP")) g_xmaDumpDir = d;
#if defined(HC09_HAVE_FFMPEG)
        av_log_set_level(g_xmaDebug ? AV_LOG_WARNING : AV_LOG_ERROR);   // "skipped samples" notes are harmless
#endif
        std::thread(DecoderThread).detach();
    });
}

// Run `f` on the context under its lock.
template <class F> auto With(uint32_t p, F f)
{
    XmaCtx* c = Find(p);
    std::lock_guard<std::mutex> lk(c->mu);
    return f(*c);
}

}  // namespace

// DWORD XMACreateContext(void** ppContext r3)
PPC_FUNC(__imp__XMACreateContext)
{
    ImportLog("XMACreateContext");
    StartDecoder();
    uint32_t p = GuestAlloc(0x40, 0x40);
    for (uint32_t i = 0; i < 0x40; i += 4) GStore32(p + i, 0);
    With(p, [](XmaCtx& c) { c.allocated = true; return 0; });
    if (ctx.r3.u32) GStore32(ctx.r3.u32, p);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMAReleaseContext)
{
    ImportLog("XMAReleaseContext");
    With(ctx.r3.u32, [](XmaCtx& c) {
        c.allocated = c.enabled = false;
        c.inValid[0] = c.inValid[1] = c.outValid = false;
        c.FifoClear();
        return 0;
    });
    ctx.r3.u64 = 0;
}

// DWORD XMAInitializeContext(void* ctx r3, XMA_CONTEXT_INIT* init r4)
// XMA_CONTEXT_INIT (big-endian): +00 in0 ptr, +04 in0 packets, +08 in1 ptr,
// +0C in1 packets, +10 input read offset (bits), +14 out ptr, +18 out blocks,
// +1C work buffer, +20 subframe decode count, +24 channel count - 1, +28 sample
// rate code (0 24k, 1 32k, 2 44.1k, 3 48k), +2C loop start/+30 end (bits),
// +34 loop count, +35 loop subframe end, +36 loop subframe skip.
PPC_FUNC(__imp__XMAInitializeContext)
{
    ImportLog("XMAInitializeContext");
    uint32_t p = ctx.r3.u32, init = ctx.r4.u32;
    for (uint32_t i = 0; i < 0x40; i += 4) GStore32(p + i, 0);
    With(p, [&](XmaCtx& c) {
        c.allocated = true;
        c.enabled = false;
        c.inValid[0] = c.inValid[1] = c.outValid = false;
        c.cur = 0;
        c.skipCarry = 0;
        c.outRead = c.outWrite = 0;
        c.FifoClear();
#if defined(HC09_HAVE_FFMPEG)
        if (c.av) avcodec_flush_buffers(c.av);   // a new stream: no bits carried over
#endif
        if (!init) return 0;
        c.inPtr[0] = GLoad32(init + 0x00);
        c.inPackets[0] = GLoad32(init + 0x04) & 0xFFF;
        c.inPtr[1] = GLoad32(init + 0x08);
        c.inPackets[1] = GLoad32(init + 0x0C) & 0xFFF;
        uint32_t readOff = GLoad32(init + 0x10);
        c.nextPacket = readOff / (kPacketBytes * 8);
        c.outPtr = GLoad32(init + 0x14);
        c.outBlocks = GLoad32(init + 0x18) & 0x1F;
        c.stereo = GLoad32(init + 0x24) >= 1;
        c.rateCode = int(GLoad32(init + 0x28) & 3);
        uint8_t loopCount = GLoad8(init + 0x34);
        static int logged = 0;
        if (g_xmaDebug || (loopCount && logged < 4) || (readOff && logged < 4))
        {
            ++logged;
            fprintf(stderr,
                    "[xma] init 0x%08X: in0 0x%08X x%u, in1 0x%08X x%u, read offset %u bits, out 0x%08X x%u blocks, "
                    "%s %d Hz, subframes %u, loop %u..%u x%u\n",
                    p, c.inPtr[0], c.inPackets[0], c.inPtr[1], c.inPackets[1], readOff, c.outPtr, c.outBlocks,
                    c.stereo ? "stereo" : "mono", kSampleRates[c.rateCode], GLoad32(init + 0x20),
                    GLoad32(init + 0x2C), GLoad32(init + 0x30), loopCount);
        }
        return 0;
    });
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMAEnableContext)
{
    ImportLog("XMAEnableContext");
    With(ctx.r3.u32, [](XmaCtx& c) { c.enabled = true; return 0; });
    Kick();
    ctx.r3.u64 = 0;
}

// DWORD XMADisableContext(void* ctx r3, BOOL wait r4). The decoder holds the
// context's lock while it works on it, so taking the lock is the wait.
PPC_FUNC(__imp__XMADisableContext)
{
    ImportLog("XMADisableContext");
    With(ctx.r3.u32, [](XmaCtx& c) { c.enabled = false; return 0; });
    ctx.r3.u64 = 0;
}

// DWORD XMASetInputBuffer0/1(void* ctx r3, void* buffer r4, DWORD packets r5)
PPC_FUNC(__imp__XMASetInputBuffer0)
{
    ImportLog("XMASetInputBuffer0");
    With(ctx.r3.u32, [&](XmaCtx& c) { c.inPtr[0] = ctx.r4.u32; c.inPackets[0] = ctx.r5.u32 & 0xFFF; return 0; });
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMASetInputBuffer1)
{
    ImportLog("XMASetInputBuffer1");
    With(ctx.r3.u32, [&](XmaCtx& c) { c.inPtr[1] = ctx.r4.u32; c.inPackets[1] = ctx.r5.u32 & 0xFFF; return 0; });
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMASetInputBuffer0Valid)
{
    ImportLog("XMASetInputBuffer0Valid");
    With(ctx.r3.u32, [](XmaCtx& c) { c.inValid[0] = true; return 0; });
    Kick();
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMASetInputBuffer1Valid)
{
    ImportLog("XMASetInputBuffer1Valid");
    With(ctx.r3.u32, [](XmaCtx& c) { c.inValid[1] = true; return 0; });
    Kick();
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMAIsInputBuffer0Valid)
{
    ImportLog("XMAIsInputBuffer0Valid");
    ctx.r3.u64 = With(ctx.r3.u32, [](XmaCtx& c) { return c.inValid[0] ? 1 : 0; });
}
PPC_FUNC(__imp__XMAIsInputBuffer1Valid)
{
    ImportLog("XMAIsInputBuffer1Valid");
    ctx.r3.u64 = With(ctx.r3.u32, [](XmaCtx& c) { return c.inValid[1] ? 1 : 0; });
}
PPC_FUNC(__imp__XMAIsOutputBufferValid)
{
    ImportLog("XMAIsOutputBufferValid");
    ctx.r3.u64 = With(ctx.r3.u32, [](XmaCtx& c) { return c.outValid ? 1 : 0; });
}
PPC_FUNC(__imp__XMASetOutputBufferValid)
{
    ImportLog("XMASetOutputBufferValid");
    With(ctx.r3.u32, [](XmaCtx& c) { c.outValid = true; return 0; });
    Kick();
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMAGetOutputBufferWriteOffset)
{
    ImportLog("XMAGetOutputBufferWriteOffset");
    ctx.r3.u64 = With(ctx.r3.u32, [](XmaCtx& c) { return c.outWrite; });
}
PPC_FUNC(__imp__XMAGetOutputBufferReadOffset)
{
    ImportLog("XMAGetOutputBufferReadOffset");
    ctx.r3.u64 = With(ctx.r3.u32, [](XmaCtx& c) { return c.outRead; });
}
// DWORD XMASetOutputBufferReadOffset(void* ctx r3, DWORD offset r4)
PPC_FUNC(__imp__XMASetOutputBufferReadOffset)
{
    ImportLog("XMASetOutputBufferReadOffset");
    With(ctx.r3.u32, [&](XmaCtx& c) { c.outRead = ctx.r4.u32 & 0x1F; return 0; });
    Kick();
    ctx.r3.u64 = 0;
}
