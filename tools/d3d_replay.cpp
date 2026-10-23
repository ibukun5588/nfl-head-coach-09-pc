// hc09-d3d-replay: offline reader of a D3D-level capture (runtime/hooks_native.cpp,
// format runtime/xenos/d3d_stream.h), the native renderer's test bench. The capture
// goes through the same front end as the live game (runtime/xenos/d3d_frontend).
//
//   hc09-d3d-replay <d3d.bin> --dump                 the call stream, one line per call
//   hc09-d3d-replay <d3d.bin> --surfaces             the bound surfaces' headers
//   hc09-d3d-replay <d3d.bin> --compare <replay.bin> [--verbose N]
//       every draw the front end hands to its back end in the capture's last frame
//       (command-buffer replays included), with the register file the back end
//       holds for it, compared with the register file the emulated GPU saw at the same
//       draw (the PM4 capture of that frame: HC09_CAPTURE_FRAME=<n> beside
//       HC09_D3D_CAPTURE=<n> or <a>-<n>). Draws are matched in order by primitive type,
//       index count and index address; draws inside a tiling bracket appear once per
//       tile in the PM4 stream and are matched to their first occurrence.
//   hc09-d3d-replay <d3d.bin> --render <out-dir>   (builds with Vulkan)
//       render with the native renderer (runtime/gpu_vk/vk_native) into
//       <out-dir>/frame.png (the last frame) and resolve_NN_<dest>.png (tools/d3d_render.cpp);
//       --bench N: then the frame N more times, ms/frame (HC09_NATIVE_PROF=1: sections);
//       --repeat N: the whole call stream N times through the same front end (its
//       steady-state cost per frame: the time of the passes after the first).
// Portable C++17 apart from --render.
#include "d3d_frontend.h"
#include "d3d_replay.h"
#include "d3d_state.h"
#include "d3d_stream.h"
#include "xenos.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kPhysSize = 0x20000000;
using d3dn::BE32;

std::vector<uint8_t> ReadFile(const char* path)
{
    std::vector<uint8_t> v;
    std::error_code ec;
    uintmax_t n = std::filesystem::file_size(path, ec);   // files over 2 GiB: no ftell
    FILE* f = ec ? nullptr : fopen(path, "rb");
    if (!f) return v;
    v.resize(size_t(n));
    size_t got = 0;
    while (got < v.size())
    {
        size_t r = fread(v.data() + got, 1, std::min<size_t>(v.size() - got, size_t(1) << 28), f);
        if (!r) break;
        got += r;
    }
    if (got != v.size()) v.clear();
    fclose(f);
    return v;
}

uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

// A draw as the front end handed it to its back end.
struct NDraw
{
    size_t index = 0;                  // in its frame
    bool inlineVertices = false;       // BeginVertices / DrawVerticesUP: fetch constant 95 points into the ring
    uint32_t prim = 0, count = 0, indexed = 0, dmaBase = 0;
    std::vector<uint32_t> regs;        // the back end's register file at the draw
    uint64_t vsHash = 0, psHash = 0;
    uint32_t vsBytes = 0, psBytes = 0;
    std::vector<uint8_t> vsCode;
};

std::vector<uint8_t> BigEndian(const xenos::ShaderCode* c)
{
    std::vector<uint8_t> v;
    if (!c) return v;
    for (uint32_t w : c->dw) { v.push_back(uint8_t(w >> 24)); v.push_back(uint8_t(w >> 16)); v.push_back(uint8_t(w >> 8)); v.push_back(uint8_t(w)); }
    return v;
}

// ---- the PM4 capture -------------------------------------------------------------
struct CDraw
{
    uint32_t prim = 0, sourceSelect = 0, numIndices = 0, dmaBase = 0, indexSize32 = 0, endian = 0;
    std::vector<uint32_t> regs;
    uint64_t vsHash = 0, psHash = 0;
    uint32_t vsBytes = 0, psBytes = 0;
    bool used = false;
    std::vector<uint8_t> vsCode;
};
std::vector<uint32_t> g_lastWriter;   // per CP draw x register would be big: per register, CP draw index + 1 of the last write
std::vector<std::vector<uint32_t>> g_writerAt;   // per CP draw: the writer table at that draw (only registers 0x4000-0x48FF)

bool ReadPm4(const char* path, std::vector<CDraw>& out)
{
    std::vector<uint8_t> file = ReadFile(path);
    if (file.size() < 16 || memcmp(file.data(), "XRPL", 4) != 0) return false;
    std::vector<uint32_t> regs(xenos::kRegisterCount, 0);
    g_lastWriter.assign(xenos::kRegisterCount, 0);
    uint64_t vs = 0, ps = 0;
    uint32_t vsBytes = 0, psBytes = 0;
    std::vector<uint8_t> vsCode;
    size_t p = 16;
    auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, &file[at], 4); return v; };
    while (p < file.size())
    {
        char tag = char(file[p++]);
        if (tag == 'P') p += 4 + 4096;
        else if (tag == 'R')
        {
            uint32_t n = u32(p);
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t r = u32(p + 4 + 8 * i);
                if (r < regs.size()) { regs[r] = u32(p + 8 + 8 * i); g_lastWriter[r] = uint32_t(out.size()); }
            }
            p += 4 + 8 * size_t(n);
        }
        else if (tag == 'S')
        {
            uint32_t type = u32(p), dw = u32(p + 8);
            uint64_t h = Fnv1a(&file[p + 12], size_t(dw) * 4);
            if (type == 0) { vs = h; vsBytes = dw * 4; vsCode.assign(&file[p + 12], &file[p + 12] + size_t(dw) * 4); }
            else { ps = h; psBytes = dw * 4; }
            p += 12 + size_t(dw) * 4;
        }
        else if (tag == 'D')
        {
            CDraw d;
            d.prim = u32(p); d.sourceSelect = u32(p + 4); d.indexSize32 = u32(p + 8); d.numIndices = u32(p + 12);
            d.dmaBase = u32(p + 16); d.endian = u32(p + 24);
            d.regs = regs;
            g_writerAt.emplace_back(g_lastWriter.begin() + 0x4000, g_lastWriter.begin() + 0x4900);
            d.vsHash = vs; d.psHash = ps; d.vsBytes = vsBytes; d.psBytes = psBytes; d.vsCode = vsCode;
            out.push_back(std::move(d));
            p += 32;
        }
        else if (tag == 'F') p += 12;
        else if (tag == 'E') break;
        else { fprintf(stderr, "pm4: bad tag 0x%02X at %zu\n", uint8_t(tag), p - 1); return false; }
    }
    return true;
}

// Register ranges compared, with names for the report.
struct Range { uint32_t lo, hi; const char* name; };
const Range kCompared[] = {
    { 0x2000, 0x2005, "surface/colour/depth info" }, { 0x2080, 0x2082, "window offset/scissor" },
    { 0x2100, 0x2114, "VGT/blend/stencil/viewport" }, { 0x2180, 0x2182, "SQ program" },
    { 0x2200, 0x220B, "depth/blend/colour control" }, { 0x2280, 0x2294, "point/line/misc" },
    { 0x2300, 0x2317, "AA/SQ consts" }, { 0x2320, 0x2325, "misc" }, { 0x2380, 0x239F, "0x2380 block" },
    { 0x4800, 0x48BF, "fetch constants" }, { 0x4900, 0x4927, "bool/loop constants" },
};

const char* RegName(uint32_t r)
{
    using namespace xenos::reg;
    switch (r)
    {
    case RB_SURFACE_INFO: return "RB_SURFACE_INFO"; case RB_COLOR_INFO: return "RB_COLOR_INFO"; case RB_DEPTH_INFO: return "RB_DEPTH_INFO";
    case PA_SC_WINDOW_OFFSET: return "PA_SC_WINDOW_OFFSET"; case PA_SC_WINDOW_SCISSOR_TL: return "WINDOW_SCISSOR_TL";
    case PA_SC_WINDOW_SCISSOR_BR: return "WINDOW_SCISSOR_BR"; case VGT_MAX_VTX_INDX: return "VGT_MAX_VTX_INDX";
    case VGT_MIN_VTX_INDX: return "VGT_MIN_VTX_INDX"; case VGT_INDX_OFFSET: return "VGT_INDX_OFFSET"; case RB_COLOR_MASK: return "RB_COLOR_MASK";
    case SQ_PROGRAM_CNTL: return "SQ_PROGRAM_CNTL"; case SQ_CONTEXT_MISC: return "SQ_CONTEXT_MISC"; case RB_DEPTHCONTROL: return "RB_DEPTHCONTROL";
    case RB_BLENDCONTROL0: return "RB_BLENDCONTROL0"; case RB_COLORCONTROL: return "RB_COLORCONTROL"; case PA_CL_CLIP_CNTL: return "PA_CL_CLIP_CNTL";
    case PA_SU_SC_MODE_CNTL: return "PA_SU_SC_MODE_CNTL"; case PA_CL_VTE_CNTL: return "PA_CL_VTE_CNTL"; case RB_MODECONTROL: return "RB_MODECONTROL";
    case PA_SU_VTX_CNTL: return "PA_SU_VTX_CNTL"; case 0x2307: return "SQ_VS_CONST"; case 0x2308: return "SQ_PS_CONST";
    default: return nullptr;
    }
}

int Compare(const std::vector<NDraw>& nd, std::vector<CDraw>& cd, int verbose)
{
    // Match: same primitive, count and (indexed) index address.
    auto sig = [](const NDraw& a, const CDraw& b) {
        return a.prim == b.prim && a.count == b.numIndices && (a.indexed ? (b.sourceSelect == 0 && a.dmaBase == b.dmaBase)
                                                                          : b.sourceSelect != 0);
    };
    size_t cursor = 0;
    std::map<uint32_t, uint32_t> regDiff;      // register -> draws differing
    std::map<std::string, uint32_t> other;
    uint32_t matched = 0, unmatched = 0, printed = 0;
    for (const NDraw& d : nd)
    {
        while (cursor < cd.size() && cd[cursor].used) ++cursor;
        size_t j = cursor;
        for (; j < cd.size() && j < cursor + 40000; ++j)
            if (!cd[j].used && sig(d, cd[j])) break;
        if (j >= cd.size() || j >= cursor + 40000)
        {
            ++unmatched;
            if (printed++ < size_t(verbose) + 10)
                printf("draw %zu (prim %u count %u%s): no PM4 draw matches\n", d.index, d.prim, d.count, d.indexed ? " indexed" : "");
            continue;
        }
        CDraw& c = cd[j];
        c.used = true;
        ++matched;
        std::vector<std::string> diffs;
        if (d.vsHash != c.vsHash)
        {
            ++other["vertex shader microcode"];
            char b[160];
            snprintf(b, sizeof(b), "VS microcode: native %u bytes, pm4 %u bytes", d.vsBytes, c.vsBytes);
            diffs.push_back(b);
            static int shown = 0;
            if (shown++ < 12)
                for (size_t k = 0; k + 4 <= std::min(d.vsCode.size(), c.vsCode.size()); k += 4)
                    if (memcmp(&d.vsCode[k], &c.vsCode[k], 4) != 0)
                        printf("      VS dword %zu: native %08X pm4 %08X\n", k / 4, BE32(&d.vsCode[k]), BE32(&c.vsCode[k]));
        }
        if (d.psHash != c.psHash) { ++other["pixel shader microcode"]; diffs.push_back("PS microcode"); }
        auto cmp = [&](uint32_t r) {
            if (d.regs[r] == c.regs[r]) return;
            if (r >= 0x4000 && r < 0x4800)
            {
                // Constants D3D's own draws (clears, resolves, its internal command buffer)
                // left on the GPU: scratch values the game's shaders do not set.
                uint32_t w = g_writerAt[j][r - 0x4000];
                if (w > 0 && w <= cd.size() && !cd[w - 1].used) { ++other["constants left by D3D's own draws"]; return; }
            }
            ++regDiff[r];
            char b[200];
            const char* n = RegName(r);
            int o = snprintf(b, sizeof(b), "%s%s0x%04X: native %08X pm4 %08X", n ? n : "", n ? " " : "", r, d.regs[r], c.regs[r]);
            if (r >= 0x4000 && r < 0x4900)
            {
                uint32_t w = g_writerAt[j][r - 0x4000];   // written before PM4 draw w (0 = before the first)
                if (w > 0 && w <= cd.size())
                    snprintf(b + o, sizeof(b) - o, " (pm4 value set after draw %u: prim %u count %u%s)", w - 1, cd[w - 1].prim,
                             cd[w - 1].numIndices, cd[w - 1].regs[xenos::reg::RB_MODECONTROL] == 6 ? " resolve" : "");
            }
            diffs.push_back(b);
        };
        for (const Range& rg : kCompared)
            for (uint32_t r = rg.lo; r <= rg.hi; ++r)
            {
                if (r >= 0x48BA && r <= 0x48BF && d.inlineVertices)
                    continue;   // slot 95: inline vertex data (in the ring)
                cmp(r);
            }
        // ALU constants inside the windows the shaders use (SQ_VS_CONST / SQ_PS_CONST of the PM4 side).
        for (uint32_t w : { 0x2307u, 0x2308u })
        {
            uint32_t v = c.regs[w], b = v & 0x1FF, n = ((v >> 12) & 0x1FF) + 1;
            for (uint32_t k = b; k < b + n && k < 512; ++k)
                for (uint32_t i = 0; i < 4; ++i) cmp(xenos::reg::ALU_CONSTANTS + 4 * k + i);
        }
        if (!diffs.empty() && printed++ < size_t(verbose))
        {
            printf("draw %zu (prim %u count %u%s) = pm4 draw %zu: %zu differences\n", d.index, d.prim, d.count,
                   d.inlineVertices ? " inline" : "", j, diffs.size());
            for (size_t k = 0; k < diffs.size() && k < size_t(getenv("HC09_DIFF_LINES") ? atoi(getenv("HC09_DIFF_LINES")) : 12); ++k) printf("    %s\n", diffs[k].c_str());
        }
    }
    uint32_t unusedPm4 = 0;
    std::map<std::pair<uint64_t, uint64_t>, std::pair<uint32_t, uint64_t>> unusedBy;   // (vs, ps) -> draws, indices
    for (const CDraw& c : cd)
        if (!c.used)
        {
            ++unusedPm4;
            if (c.prim != 1 && c.regs[xenos::reg::RB_MODECONTROL] != 6)   // not D3D's points / resolves
            {
                auto& e = unusedBy[{ c.vsHash, c.psHash }];
                ++e.first;
                e.second += c.numIndices;
            }
        }
    if (verbose)
    {
        std::vector<std::pair<std::pair<uint32_t, uint64_t>, std::pair<uint64_t, uint64_t>>> v;
        for (auto& [k, e] : unusedBy) v.push_back({ e, k });
        std::sort(v.rbegin(), v.rend());
        printf("PM4 draws without a native counterpart (no points/resolves), by shaders:\n");
        for (size_t i = 0; i < v.size() && i < 15; ++i)
            printf("  vs %016llx ps %016llx: %u draws, %llu indices\n", (unsigned long long)v[i].second.first,
                   (unsigned long long)v[i].second.second, v[i].first.first, (unsigned long long)v[i].first.second);
    }
    printf("\n%zu native draws: %u matched, %u unmatched; %zu PM4 draws, %u not matched (resolves, clears, tile passes)\n",
           nd.size(), matched, unmatched, cd.size(), unusedPm4);
    for (auto& [what, n] : other) printf("  %-28s differs in %u draws\n", what.c_str(), n);
    std::map<std::string, std::pair<uint32_t, uint32_t>> byGroup;   // group -> registers, max draws
    for (auto& [r, n] : regDiff)
    {
        const char* name = RegName(r);
        if (name || r < 0x4000) printf("  %-20s 0x%04X differs in %u draws\n", name ? name : "", r, n);
        else
        {
            const char* g = r < 0x4800 ? "ALU constants" : r < 0x4900 ? "fetch constants" : "bool/loop constants";
            auto& e = byGroup[g];
            ++e.first;
            e.second = std::max(e.second, n);
        }
    }
    for (auto& [g, e] : byGroup) printf("  %-20s %u registers differ (in up to %u draws)\n", g.c_str(), e.first, e.second);
    return 0;
}


// --compare: the front end's output kept as NDraws (the last frame's).
struct CompareSink : ReplaySink, d3dn::Backend
{
    d3dn::Frontend frontend{ *this };
    std::unordered_map<uint32_t, std::unique_ptr<d3dn::Frontend>> others;
    uint32_t mainDevice = 0;
    const Memory* mem = nullptr;
    std::vector<uint32_t> regs = std::vector<uint32_t>(xenos::kRegisterCount, 0);
    std::vector<NDraw> frame, last;
    uint32_t frames = 0;

    d3dn::Frontend& For(uint32_t dev)
    {
        if (!dev || (dev & 0x1FFFFFFF) == mainDevice) return frontend;
        auto& f = others[dev];
        if (!f) f = std::make_unique<d3dn::Frontend>(*this, false);
        return *f;
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
    // ReplaySink
    void Page(uint32_t, const uint8_t*) override {}
    void Write(uint32_t, const uint8_t*, uint32_t) override {}
    void MainDevice(uint32_t pa) override { mainDevice = pa; }
    void Event(const Call& c, const uint8_t* device, const Memory& m) override { mem = &m; For(c.r[0]).Call(Args(c), device, m); }
    void Draw(const Call& c, const uint8_t* before, const uint8_t* after, const Memory& m) override
    {
        mem = &m;
        For(c.r[0]).Draw(Args(c), before, after, m);
    }
    void Patched(uint32_t device, uint32_t vs, uint32_t variant, uint32_t dst, const uint8_t* code, uint32_t bytes, const Memory& m) override
    {
        For(device).VertexShaderPatched(vs, variant, dst, code, bytes, m);
    }
    void Kick() override { frontend.Kick(); }
    int Finish() override { frontend.Kick(); return 0; }
    // d3dn::Backend
    void SetTargets(const d3dn::Surface*, const d3dn::Surface&) override {}
    void Clear(uint32_t, const int32_t*, const float*, float, uint32_t) override {}
    void Resolve(const d3dn::ResolveOp&, const uint32_t*) override {}
    void Present(uint32_t, uint32_t, uint32_t) override
    {
        last = std::move(frame);
        frame.clear();
        ++frames;
    }
    void Draw(d3dn::DrawOp& op) override
    {
        for (auto [i, v] : op.regs) regs[i] = v;
        if (mem)
            for (const d3dn::DrawOp::Load& l : op.loads)
                for (uint32_t k = 0; k < l.dwords; ++k) regs[l.reg + k] = mem->U32(l.address + 4 * k);
        if (op.stateOnly) return;
        NDraw d;
        d.index = frame.size();
        d.inlineVertices = !op.inlineData.empty();
        d.prim = op.d.primType;
        d.count = op.d.numIndices;
        d.indexed = op.d.sourceSelect == 0;
        d.dmaBase = op.d.dmaBase;
        d.regs = regs;
        d.vsHash = op.vsHash;
        d.psHash = op.psHash;
        d.vsCode = BigEndian(op.vs);
        d.vsBytes = uint32_t(d.vsCode.size());
        d.psBytes = op.ps ? uint32_t(op.ps->dw.size() * 4) : 0;
        frame.push_back(std::move(d));
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <d3d.bin> --dump | --surfaces | --compare <replay.bin> [--verbose N] | --render <out-dir> [--bench N]\n", argv[0]);
        return 2;
    }
    bool dump = false, surfaces = false;
    std::set<uint32_t> seenSurf;
    const char* pm4 = nullptr;
    const char* renderDir = nullptr;
    int verbose = 0, bench = 0, repeat = 1;
    for (int i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--dump")) dump = true;
        else if (!strcmp(argv[i], "--surfaces")) surfaces = true;
        else if (!strcmp(argv[i], "--compare") && i + 1 < argc) pm4 = argv[++i];
        else if (!strcmp(argv[i], "--verbose") && i + 1 < argc) verbose = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--render") && i + 1 < argc) renderDir = argv[++i];
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::max(1, atoi(argv[++i]));
    }
    std::unique_ptr<ReplaySink> sink;
    CompareSink* cmp = nullptr;
    if (renderDir)
    {
        sink.reset(CreateNativeSink(renderDir, bench));
        if (!sink) { fprintf(stderr, "native renderer unavailable\n"); return 1; }
    }
    else if (pm4) { cmp = new CompareSink; sink.reset(cmp); }

    std::vector<uint8_t> file = ReadFile(argv[1]);
    if (file.size() < 20 || memcmp(file.data(), "XD3D", 4) != 0) { fprintf(stderr, "not a D3D capture: %s\n", argv[1]); return 1; }
    uint32_t version, devPa;
    uint64_t swap;
    memcpy(&version, &file[4], 4);
    memcpy(&swap, &file[8], 8);
    memcpy(&devPa, &file[16], 4);
    if (version != d3dcap::kVersion && version != 4 && version != 5)   // 4: before 'N' records, 5: before 'S'
    {
        fprintf(stderr, "unsupported version %u\n", version);
        return 1;
    }
    printf("D3D capture: from the frame after swap %llu, device pa 0x%08X\n", (unsigned long long)swap, devPa);
    if (sink) sink->MainDevice(devPa);

    Memory mem;
    std::vector<uint8_t> device(d3dcap::kDeviceBytes, 0), pre;
    std::unordered_map<uint32_t, std::vector<uint8_t>> others;   // other devices ('N'), by physical address
    auto deviceOf = [&](uint32_t addr) -> std::vector<uint8_t>& {
        uint32_t pa = addr & 0x1FFFFFFF;
        if (pa == devPa || !addr) return device;
        auto& v = others[pa];
        if (v.empty()) v.assign(d3dcap::kDeviceBytes, 0);
        return v;
    };
    std::vector<Call> calls;
    size_t p = 20, swaps = 0, drawCalls = 0;
    auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, &file[at], 4); return v; };
    uint32_t pendingDraw = 0;   // call index + 1 of a draw waiting for its 'A'
    uint32_t pendingAfter = 0;  // ... of a call the front end takes after it ran (a resolve into a mip level or slice)
    std::map<uint32_t, uint32_t> callCounts;
    for (int pass = 0; pass < repeat; ++pass)
    {
    if (pass)
    {
        if (sink) sink->Pass(pass);
        p = 20;
        pendingDraw = pendingAfter = 0;
        calls.clear();
    }
    while (p < file.size())
    {
        char tag = char(file[p++]);
        if (tag == 'P')
        {
            mem.Put(u32(p), &file[p + 4]);
            if (sink) sink->Page(u32(p), &file[p + 4]);
            p += 4 + 4096;
        }
        else if (tag == 'M' || tag == 'N' || tag == 'S')
        {
            std::vector<uint8_t>* dv = &device;
            if (tag != 'M') { dv = &deviceOf(u32(p) | 0x80000000u); p += 4; }
            if (tag == 'S') std::fill(dv->begin(), dv->end(), 0);
            uint32_t n = u32(p);
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t off = u32(p + 4 + 8 * i);
                if (off + 4 <= dv->size()) memcpy(&(*dv)[off], &file[p + 8 + 8 * i], 4);
            }
            p += 4 + 8 * size_t(n);
        }
        else if (tag == 'C')
        {
            Call c;
            c.fn = u32(p); c.depth = u32(p + 4); c.lr = u32(p + 8);
            for (int i = 0; i < 8; ++i) c.r[i] = u32(p + 12 + 4 * i);
            memcpy(&c.f1, &file[p + 44], 8);
            memcpy(&c.f2, &file[p + 52], 8);
            c.ret = u32(p + 60);
            uint32_t nb = u32(p + 64);
            p += 68;
            for (uint32_t b = 0; b < nb; ++b)
            {
                uint32_t tagb = u32(p), bytes = u32(p + 4);
                c.blobs[tagb].assign(&file[p + 8], &file[p + 8] + bytes);
                p += 8 + bytes;
            }
            ++callCounts[c.fn | (c.depth ? 0x100u : 0u)];
            if (dump)
            {
                printf("#%zu%s %s lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X f1=%g", calls.size(),
                       c.depth ? " (inner)" : "", d3dcap::CallName(c.fn), c.lr, c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.f1);
                for (auto& [t, b] : c.blobs) printf(" blob%u:%zuB", t, b.size());
                printf("\n");
            }
            if (c.fn == d3dcap::kSwap && c.depth == 0) ++swaps;
            bool isDraw = c.depth == 0 && (c.fn == d3dcap::kDrawIndexedVertices || c.fn == d3dcap::kDrawVertices ||
                                           c.fn == d3dcap::kBeginVertices || c.fn == d3dcap::kDrawVerticesUP);
            std::vector<uint8_t>& cdev = deviceOf(c.r[0]);
            if (isDraw) { pre = cdev; pendingDraw = uint32_t(calls.size()) + 1; ++drawCalls; }
            bool after = c.depth == 0 && c.fn == d3dcap::kResolve && (c.r[5] || c.r[6]);   // d3dn::Frontend::ResolveAfterCall
            if (after) pendingAfter = uint32_t(calls.size()) + 1;
            if (sink && c.depth == 0 && !isDraw && !after) sink->Event(c, cdev.data(), mem);
            calls.push_back(std::move(c));
        }
        else if (tag == 'A')
        {
            uint32_t fn = u32(p);
            p += 4;
            if (pendingAfter && calls[pendingAfter - 1].fn == fn)
            {
                const Call& c = calls[pendingAfter - 1];
                if (sink) sink->Event(c, deviceOf(c.r[0]).data(), mem);
                pendingAfter = 0;
            }
            if (pendingDraw && calls[pendingDraw - 1].fn == fn)
            {
                const Call& c = calls[pendingDraw - 1];
                const uint8_t* dv = deviceOf(c.r[0]).data();
                if (sink) sink->Draw(c, pre.data(), dv, mem);
                if (surfaces)   // the headers of the bound targets, once each
                    for (uint32_t i = 0; i < 5; ++i)
                        if (uint32_t sf = BE32(dv + d3dcap::dev::kRenderTarget + 4 * i); sf && seenSurf.insert(sf).second)
                        {
                            printf("surface %08X (%s%u, first at call #%u):", sf, i < 4 ? "RT" : "depth", i < 4 ? i : 0, pendingDraw - 1);
                            for (uint32_t k = 0; k < 64; k += 4) printf(" %08X", mem.U32(sf + k));
                            printf("\n");
                        }
                pendingDraw = 0;
            }
        }
        else if (tag == 'R')
        {
            if (dump) printf("    -> returned %08X\n", u32(p + 4));
            p += 8;
        }
        else if (tag == 'V')
        {
            // In place (the shader's microcode): memory changes for good; a ring copy:
            // the draw being flushed uses it once (the front end decides).
            uint32_t vs = u32(p), variant = u32(p + 4), dst = u32(p + 8), bytes = u32(p + 12);
            const uint8_t* data = &file[p + 16];
            uint32_t off = mem.U32(vs + 896 + 8 * variant);
            uint32_t own = mem.U32(vs + 32) + mem.U32(vs + 872 + off);
            if ((dst & 0x1FFFFFFF) == (own & 0x1FFFFFFF)) { mem.Write(dst, data, bytes); if (sink) sink->Write(dst, data, bytes); }
            if (sink) sink->Patched(pendingDraw ? calls[pendingDraw - 1].r[0] : 0, vs, variant, dst, data, bytes, mem);
            p += 16 + bytes;
        }
        else if (tag == 'K') { if (sink) sink->Kick(); }
        else if (tag == 'E') break;
        else { fprintf(stderr, "bad tag 0x%02X at %zu\n", uint8_t(tag), p - 1); return 1; }
    }
    }
    printf("%zu calls, %zu draw calls, %zu frames, %zu pages\n", calls.size(), drawCalls, swaps, mem.pages.size());
    for (auto& [k, n] : callCounts) printf("  %-32s %s %u\n", d3dcap::CallName(k & 0xFF), (k & 0x100) ? "(inner)" : "       ", n);
    int rc = sink ? sink->Finish() : 0;
    if (cmp)
    {
        std::vector<CDraw> cd;
        if (!ReadPm4(pm4, cd)) { fprintf(stderr, "cannot read %s\n", pm4); return 1; }
        printf("comparing the last frame: %zu draws handed to the back end\n", cmp->last.size());
        return Compare(cmp->last, cd, verbose);
    }
    return rc;
}
