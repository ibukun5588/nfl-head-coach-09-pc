// hc09-replay: render a captured frame again from its replay.bin, offline.
//
//   hc09-replay <captures/frame_N/replay.bin> [out-dir]   (default: replay_out)
//               [--gpu-render] [--gpu [--seconds N] [--gpu-dump presented.png]]
//
// Feeds the recorded register writes, shader loads and draws through the same
// runtime/xenos entry points the command processor uses, with the host hooks
// served from the recorded memory pages. Writes the usual capture output
// (frame.png, draws.txt, resolves, textures) to <out-dir>/frame_N/. Portable
// C++17, no OS headers: builds wherever runtime/xenos builds.
// --gpu (builds with Vulkan): also show the presented frame in a window
// (tools/replay_gpu.cpp); --seconds closes it after N seconds, --gpu-dump
// writes what the window actually presented. --gpu-render draws the frame
// with the Vulkan renderer instead of the software rasterizer (resolves stay
// in software), so frame.png and the resolves are the GPU's output.
// File format: see runtime/xenos/xenos_capture.cpp.
#include "xenos.h"
#include "xenos_texture.h"
#include "replay_gpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kPhysSize = 0x20000000;
uint8_t* g_mem = nullptr;
std::vector<uint32_t> g_regs(xenos::kRegisterCount, 0);
uint64_t g_swaps = 0;
}  // namespace

namespace xenos {
uint32_t HostReg(uint32_t index) { return index < kRegisterCount ? g_regs[index] : 0; }
const uint32_t* HostRegFile(bool* bigEndian) { *bigEndian = false; return g_regs.data(); }
const uint8_t* HostPhys(uint32_t pa) { return g_mem + (pa & (kPhysSize - 1)); }
uint64_t HostSwapCount() { return g_swaps; }
void HostWatchWrites(uint32_t, uint32_t) {}
uint64_t HostTrackWrites(uint32_t, uint32_t) { return 0; }   // replay memory never changes
bool HostWrittenSince(uint32_t, uint32_t, uint64_t&) { return false; }
}  // namespace xenos

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    bool gpu = false, gpuRender = false;
    double seconds = -1.0;
    std::string gpuDump;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--gpu")) gpu = true;
        else if (!strcmp(argv[i], "--gpu-render")) gpuRender = true;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-dump") && i + 1 < argc) gpuDump = argv[++i];
        else args.push_back(argv[i]);
    }
    if (args.empty())
    {
        fprintf(stderr, "usage: %s <replay.bin> [out-dir] [--gpu-render] [--gpu [--seconds N] [--gpu-dump presented.png]]\n", argv[0]);
        return 2;
    }
    std::string outDir = args.size() > 1 ? args[1] : "replay_out";
    FILE* f = fopen(args[0].c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", args[0].c_str()); return 1; }
    std::vector<uint8_t> file;
    {
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
        fclose(f);
    }
    if (file.size() < 16 || memcmp(file.data(), "XRPL", 4) != 0) { fprintf(stderr, "not a replay file\n"); return 1; }
    uint32_t version;
    uint64_t swap;
    memcpy(&version, &file[4], 4);
    memcpy(&swap, &file[8], 8);
    if (version != 1) { fprintf(stderr, "unsupported replay version %u\n", version); return 1; }
    g_mem = gpuRender ? GpuRenderBegin(kPhysSize)
                      : static_cast<uint8_t*>(calloc(kPhysSize, 1));   // untouched pages stay unbacked
    if (!g_mem) { fprintf(stderr, "out of memory\n"); return 1; }

    // Pass 1: memory pages (first-touch contents). Pass 2: everything else in order.
    // HC09_REPLAY_REPEAT=N (with --gpu-render): N more passes feeding the GPU
    // renderer like a running game (live observer, no capture): steady-state cost.
    int repeat = 0;
    if (const char* e = getenv("HC09_REPLAY_REPEAT")) repeat = gpuRender ? atoi(e) : 0;
    uint32_t pages = 0, draws = 0, shaders = 0;
    uint32_t front = 0, frontW = 0, frontH = 0;   // the XE_SWAP that ends the frame
    for (int pass = 0; pass < 2 + repeat; ++pass)
    {
        if (pass == 2) GpuRenderRepeatBegin();
        if (pass == 1)
        {
            long target = long(swap);
            xenos::CaptureConfigure(&target, 1, outDir, false);
            g_swaps = swap;
            xenos::FrameEnd(0, 0, 0);   // opens the capture of frame `swap`
        }
        size_t p = 16;
        auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, &file[at], 4); return v; };
        bool end = false;
        while (!end && p < file.size())
        {
            char tag = char(file[p++]);
            switch (tag)
            {
            case 'P':
                if (pass == 0) { memcpy(g_mem + (u32(p) & (kPhysSize - 1)), &file[p + 4], 4096); ++pages; }
                p += 4 + 4096;
                break;
            case 'R':
            {
                uint32_t n = u32(p);
                if (pass >= 1)
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        uint32_t r = u32(p + 4 + i * 8);
                        if (r < g_regs.size()) g_regs[r] = u32(p + 8 + i * 8);
                    }
                p += 4 + size_t(n) * 8;
                break;
            }
            case 'S':
            {
                uint32_t type = u32(p), addr = u32(p + 4), dwords = u32(p + 8);
                if (pass >= 1) { xenos::LoadShader(xenos::ShaderType(type), &file[p + 12], dwords, addr); ++shaders; }
                p += 12 + size_t(dwords) * 4;
                break;
            }
            case 'D':
            {
                if (pass >= 1)
                {
                    xenos::DrawPacket d;
                    d.primType = u32(p); d.sourceSelect = u32(p + 4); d.indexSize32 = u32(p + 8); d.numIndices = u32(p + 12);
                    d.dmaBase = u32(p + 16); d.dmaWords = u32(p + 20); d.dmaEndian = u32(p + 24); d.packetAddr = u32(p + 28);
                    d.where = "replay";
                    xenos::Draw(d);
                    ++draws;
                }
                p += 32;
                break;
            }
            case 'F':
                if (pass >= 1)
                {
                    g_swaps = swap + uint64_t(pass);
                    front = u32(p); frontW = u32(p + 4); frontH = u32(p + 8);
                    xenos::FrameEnd(front, frontW, frontH);
                }
                p += 12;
                break;
            case 'E': end = true; break;
            default: fprintf(stderr, "bad record tag 0x%02X at %zu\n", uint8_t(tag), p - 1); return 1;
            }
        }
    }
    fprintf(stderr, "replayed frame %llu: %u pages, %u shader loads, %u draws -> %s/frame_%llu\n",
            (unsigned long long)swap, pages, shaders, draws, outDir.c_str(), (unsigned long long)swap);
    xenos::Image img;
    bool haveImage = gpu && xenos::DecodeFrontBuffer(front, frontW, frontH, img);
    if (gpuRender && repeat > 0) GpuRenderRepeatEnd(repeat);
    if (gpuRender) GpuRenderEnd();   // releases g_mem
    int rc = 0;
    if (gpu)
    {
        if (!haveImage) { fprintf(stderr, "no front buffer to present\n"); rc = 1; }
        else rc = ReplayPresent(img, ("hc09-replay frame " + std::to_string(swap)).c_str(), seconds, gpuDump);
    }
    if (!gpuRender) free(g_mem);
    return rc;
}
