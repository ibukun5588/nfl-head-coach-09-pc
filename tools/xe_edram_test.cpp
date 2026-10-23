// xe-edram-test: the EDRAM renderer's GLSL render-target arithmetic
// (xe_glsl.cpp EdramGlslLibrary) against the software rasterizer's
// (xenos_raster.cpp), bit for bit, on the GPU.
//
//   xe-edram-test [--quick]
//
// Every colour format decode (raw and linear) and clamp+encode, D24S8 and
// D24FS8 depth decode (all 2^24 values) and encode, the blend formula for
// every factor/op, resolve packing for every destination format, and the
// shader ALU's exp2/log2/sin/cos/div/sqrt/rsq (xenos_shader.h). Before that:
// D24FS8 depth decode (all 2^24 values) and encode, the blend formula for
// every factor/op, and resolve packing for every destination format. The game's
// captured frames only use 8888 and D24S8, so this is where the other formats
// are verified. Exit code 1 if anything differs (NaN equals NaN). Clamp and
// blend results that differ only by -0 vs +0 or a flushed denormal (GPU float
// arithmetic) are counted but not failures: they never change an encoded value
// except in raw float targets, where they are the sign of a zero. Blends of
// infinite or denormal operands (inf * 0, flushed denormals) are counted too.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vk_device.h"
#include "vk_shader_compile.h"
#include "xe_glsl.h"
#include "xenos.h"
#include "xenos_raster.h"
#include "xenos_shader.h"

namespace xenos {
uint32_t HostReg(uint32_t) { return 0; }
const uint32_t* HostRegFile(bool*) { return nullptr; }
const uint8_t* HostPhys(uint32_t) { static uint8_t zero[4096]; return zero; }
uint64_t HostSwapCount() { return 0; }
void HostWatchWrites(uint32_t, uint32_t) {}
uint64_t HostTrackWrites(uint32_t, uint32_t) { return 0; }   // replay memory never changes
bool HostWrittenSince(uint32_t, uint32_t, uint64_t&) { return false; }
}  // namespace xenos

namespace {

using namespace hc09vk;

// Case layout: 16 dwords in, 4 dwords out.
enum Mode : uint32_t { kDecode, kEncode, kClamp, kDepthDecode, kDepthEncode, kBlend, kPack, kTranscendental, kDivSqrt };
const char* kModeNames[] = { "decodeRt", "clampRt+encodeRt", "clampRt", "decodeDepth", "encodeDepth", "blend", "packResolve",
                             "exp2/log2/sin/cos", "div/sqrt/rsq" };

const char* kShader = R"GLSL(
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer In { uint inp[]; };
layout(std430, set = 0, binding = 1) writeonly buffer Out { uint outp[]; };
layout(push_constant) uniform Params { uint mode, count, base, pad; } P;
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= P.count) return;
    uint b = i * 16u;
    uvec4 r = uvec4(0u);
    vec4 c = uintBitsToFloat(uvec4(inp[b], inp[b + 1u], inp[b + 2u], inp[b + 3u]));
    switch (P.mode)
    {
    case 0u: r = floatBitsToUint(decodeRt(inp[b + 4u], inp[b], inp[b + 1u], inp[b + 5u] != 0u)); break;
    case 1u: r.xy = encodeRt(inp[b + 4u], clampRt(inp[b + 4u], c), inp[b + 5u] != 0u); break;
    case 2u: r = floatBitsToUint(clampRt(inp[b + 4u], c)); break;
    case 3u: r.x = floatBitsToUint(decodeDepth((P.base + i) & 0xFFFFFFu, P.pad != 0u)); break;
    case 4u: r.x = encodeDepth(c.x, inp[b + 5u] != 0u); break;
    case 5u:
    {
        vec4 d = uintBitsToFloat(uvec4(inp[b + 4u], inp[b + 5u], inp[b + 6u], inp[b + 7u]));
        vec4 k = uintBitsToFloat(uvec4(inp[b + 8u], inp[b + 9u], inp[b + 10u], inp[b + 11u]));
        for (int j = 0; j < 4; ++j) r[j] = floatBitsToUint(blendChannel(inp[b + 12u], c, d, k, j));
        break;
    }
    case 6u: r.xy = packResolve(inp[b + 4u], inp[b + 5u], c); break;
    case 7u: r = floatBitsToUint(vec4(xexp2(c.x), xlog2(c.x), xsin(c.x), xcos(c.x))); break;
    case 8u: r = floatBitsToUint(vec4(xdiv(c.x, c.y), xsqrtf(c.x), xdiv(1.0, xsqrtf(c.x)), 0.0)); break;
    }
    outp[i * 4u] = r.x; outp[i * 4u + 1u] = r.y; outp[i * 4u + 2u] = r.z; outp[i * 4u + 3u] = r.w;
}
)GLSL";

struct Rng
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint32_t U() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 32); }
    float F(float lo, float hi) { return lo + (hi - lo) * float(U() >> 8) / 16777216.0f; }
};
inline uint32_t Bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float Flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
bool SameFloat(uint32_t a, uint32_t b) { return a == b || (std::isnan(Flt(a)) && std::isnan(Flt(b))); }
// GPU float arithmetic flushes denormals to zero and may give +0 for -0; equal up to that.
bool SameFloatFtz(uint32_t a, uint32_t b)
{
    auto zeroish = [](uint32_t v) { return (v & 0x7F800000u) == 0; };
    return SameFloat(a, b) || (zeroish(a) && zeroish(b));
}

struct Gpu
{
    Device dev;
    Buffer in, out;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    static constexpr uint32_t kMax = 1u << 22;

    bool Init(std::string* err)
    {
        if (!dev.CreateInstance({}, err) || !dev.CreateDevice(VK_NULL_HANDLE, err)) return false;
        VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!dev.CreateBuffer(VkDeviceSize(kMax) * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, in) ||
            !dev.CreateBuffer(VkDeviceSize(kMax) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, out))
        { *err = "buffers"; return false; }
        VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                              { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
        VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lci.bindingCount = 2;
        lci.pBindings = b;
        vkCreateDescriptorSetLayout(dev.device, &lci, nullptr, &setLayout);
        VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pci.setLayoutCount = 1;
        pci.pSetLayouts = &setLayout;
        pci.pushConstantRangeCount = 1;
        pci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(dev.device, &pci, nullptr, &layout);
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
        VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        vkCreateDescriptorPool(dev.device, &dpi, nullptr, &pool);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout;
        vkAllocateDescriptorSets(dev.device, &ai, &set);
        VkDescriptorBufferInfo bi[2] = { { in.buffer, 0, VK_WHOLE_SIZE }, { out.buffer, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet w[2]{};
        for (int i = 0; i < 2; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = uint32_t(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(dev.device, 2, w, 0, nullptr);
        std::vector<uint32_t> spirv;
        std::string log;
        std::string src = "#version 450\n" + EdramGlslLibrary() + kShader;
        if (!CompileGlsl(src, Stage::kCompute, "xe_edram_test", spirv, &log)) { *err = log; return false; }
        VkShaderModuleCreateInfo mci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mci.codeSize = spirv.size() * 4;
        mci.pCode = spirv.data();
        VkShaderModule m;
        vkCreateShaderModule(dev.device, &mci, nullptr, &m);
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main" };
        cpi.layout = layout;
        VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);
        vkDestroyShaderModule(dev.device, m, nullptr);
        return r == VK_SUCCESS;
    }
    uint32_t* Input() { return static_cast<uint32_t*>(in.mapped); }
    const uint32_t* Run(uint32_t mode, uint32_t count, uint32_t base = 0, uint32_t extra = 0)
    {
        VkCommandBuffer cb = dev.BeginOneShot();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        uint32_t pc[4] = { mode, count, base, extra };
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
        vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
        VkMemoryBarrier hb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
        dev.EndOneShot(cb);
        return static_cast<const uint32_t*>(out.mapped);
    }
};

struct Tally
{
    uint64_t cases = 0, bad = 0, ftz = 0;   // ftz: differs only by -0 vs +0 or a flushed denormal
    uint64_t special = 0;                   // differs, with an infinite or denormal operand (blend)
    int shown = 0;
    void Fail(const char* what, const std::string& detail)
    {
        ++bad;
        if (shown++ < 4) fprintf(stderr, "  MISMATCH %s: %s\n", what, detail.c_str());
    }
};

std::string Hex4(const uint32_t* v, int n)
{
    char b[96];
    std::string s;
    for (int i = 0; i < n; ++i) { snprintf(b, sizeof(b), "%s%08X(%.9g)", i ? " " : "", v[i], double(Flt(v[i]))); s += b; }
    return s;
}

// Interesting floats around a format's representable values.
float Interesting(Rng& g, uint32_t fmt)
{
    switch (g.U() % 8)
    {
    case 0: return g.F(-0.2f, 1.2f);
    case 1: { float k = float(g.U() % 256) / 255.0f; return Flt(Bits(k) + (g.U() % 5) - 2); }       // 8-bit steps +-2 ulp
    case 2: { float k = (float(g.U() % 256) + 0.5f) / 255.0f; return Flt(Bits(k) + (g.U() % 5) - 2); }   // 8-bit rounding points
    case 3: { float k = (float(g.U() % 1024) + 0.5f) / 1023.0f; return Flt(Bits(k) + (g.U() % 5) - 2); }
    case 4: return fmt == 3 || fmt == 12 ? g.F(0.0f, 33.0f) : g.F(-40.0f, 40.0f);
    case 5: return g.F(-70000.0f, 70000.0f);
    case 6: { static const float s[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 31.875f, 32.0f, -32.0f, 65504.0f, 65520.0f, 1e-8f, 6.1e-5f, NAN, INFINITY, -INFINITY };
              return s[g.U() % (sizeof(s) / sizeof(s[0]))]; }
    default: return std::ldexp(g.F(0.5f, 1.0f), int(g.U() % 40) - 30);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    bool quick = argc > 1 && !strcmp(argv[1], "--quick");
    Gpu gpu;
    std::string err;
    if (!gpu.Init(&err)) { fprintf(stderr, "init: %s\n", err.c_str()); return 2; }
    const uint32_t N = quick ? (1u << 16) : (1u << 20);
    Rng g;
    bool allOk = true;
    auto report = [&](const char* name, const Tally& t) {
        fprintf(stderr, "%-40s %10llu cases, %llu mismatches%s\n", name, (unsigned long long)t.cases, (unsigned long long)t.bad,
                (std::string(t.ftz ? " (" + std::to_string(t.ftz) + " equal up to -0/denormal flush)" : "") +
                 (t.special ? " (" + std::to_string(t.special) + " differ with an infinite/denormal operand or |x| >= 2^24 for sin/cos)" : "")).c_str());
        allOk &= t.bad == 0;
    };
    const uint32_t kFormats[] = { 0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15 };
    char name[96];

    // Colour decode (raw and linear).
    for (uint32_t fmt : kFormats)
        for (uint32_t linear = 0; linear < 2; ++linear)
        {
            uint32_t* in = gpu.Input();
            for (uint32_t i = 0; i < N; ++i)
            {
                uint32_t* c = in + i * 16;
                c[0] = i < 256 ? i * 0x01010101u : g.U();   // every 8-bit value first
                c[1] = g.U();
                c[4] = fmt;
                c[5] = linear;
            }
            const uint32_t* out = gpu.Run(kDecode, N);
            Tally t;
            for (uint32_t i = 0; i < N; ++i)
            {
                float c[4];
                xenos::RasterDecodeRt(fmt, in + i * 16, c, linear != 0);
                ++t.cases;
                for (int k = 0; k < 4; ++k)
                    if (!SameFloat(Bits(c[k]), out[i * 4 + k]))
                    {
                        uint32_t want[4] = { Bits(c[0]), Bits(c[1]), Bits(c[2]), Bits(c[3]) };
                        t.Fail("decode", "in " + Hex4(in + i * 16, 2) + " cpu " + Hex4(want, 4) + " gpu " + Hex4(out + i * 4, 4));
                        break;
                    }
            }
            snprintf(name, sizeof(name), "decodeRt fmt %u%s", fmt, linear ? " linear" : "");
            report(name, t);
        }

    // Clamp, and clamp + encode (linear and raw).
    for (uint32_t fmt : kFormats)
        for (uint32_t mode : { uint32_t(kClamp), uint32_t(kEncode) })
            for (uint32_t linear = 0; linear < (mode == kEncode ? 2u : 1u); ++linear)
            {
                uint32_t* in = gpu.Input();
                for (uint32_t i = 0; i < N; ++i)
                {
                    uint32_t* c = in + i * 16;
                    for (int k = 0; k < 4; ++k) c[k] = Bits(Interesting(g, fmt));
                    c[4] = fmt;
                    c[5] = linear;
                }
                const uint32_t* out = gpu.Run(mode, N);
                Tally t;
                for (uint32_t i = 0; i < N; ++i)
                {
                    float c[4];
                    for (int k = 0; k < 4; ++k) c[k] = Flt(in[i * 16 + k]);
                    xenos::RasterClampRt(fmt, c);
                    ++t.cases;
                    if (mode == kClamp)
                    {
                        bool ftzOnly = true, exact = true;
                        for (int k = 0; k < 4; ++k)
                        {
                            exact &= SameFloat(Bits(c[k]), out[i * 4 + k]);
                            ftzOnly &= SameFloatFtz(Bits(c[k]), out[i * 4 + k]);
                        }
                        if (!exact && ftzOnly) ++t.ftz;
                        for (int k = 0; k < 4; ++k)
                            if (!SameFloatFtz(Bits(c[k]), out[i * 4 + k]))
                            {
                                uint32_t want[4] = { Bits(c[0]), Bits(c[1]), Bits(c[2]), Bits(c[3]) };
                                t.Fail("clamp", "in " + Hex4(in + i * 16, 4) + " cpu " + Hex4(want, 4) + " gpu " + Hex4(out + i * 4, 4));
                                break;
                            }
                    }
                    else
                    {
                        uint32_t p[2] = { 0, 0 };
                        xenos::RasterEncodeRt(fmt, c, p, linear != 0);
                        bool wide = fmt == 5 || fmt == 7 || fmt == 15;
                        if (p[0] != out[i * 4] || (wide && p[1] != out[i * 4 + 1]))
                            t.Fail("encode", "in " + Hex4(in + i * 16, 4) + " cpu " + Hex4(p, 2) + " gpu " + Hex4(out + i * 4, 2));
                    }
                }
                snprintf(name, sizeof(name), "%s fmt %u%s", kModeNames[mode], fmt, linear ? " linear" : "");
                report(name, t);
            }

    // Depth decode: every 24-bit value, both formats.
    for (uint32_t isFloat = 0; isFloat < 2; ++isFloat)
    {
        Tally t;
        uint32_t total = quick ? (1u << 20) : (1u << 24), chunk = Gpu::kMax;
        for (uint32_t base = 0; base < total; base += chunk)
        {
            uint32_t n = std::min(chunk, total - base);
            const uint32_t* out = gpu.Run(kDepthDecode, n, base, isFloat);
            for (uint32_t i = 0; i < n; ++i)
            {
                float z = xenos::RasterDecodeDepth(base + i, isFloat != 0);
                ++t.cases;
                if (!SameFloat(Bits(z), out[i * 4]))
                {
                    uint32_t want = Bits(z), d = base + i;
                    t.Fail("decodeDepth", "d24 " + Hex4(&d, 1) + " cpu " + Hex4(&want, 1) + " gpu " + Hex4(out + i * 4, 1));
                }
            }
        }
        report(isFloat ? "decodeDepth D24FS8 (all 2^24)" : "decodeDepth D24S8 (all 2^24)", t);
    }

    // Depth encode: random z, and the neighbourhood of every kind of representable depth.
    for (uint32_t isFloat = 0; isFloat < 2; ++isFloat)
    {
        uint32_t* in = gpu.Input();
        for (uint32_t i = 0; i < N; ++i)
        {
            uint32_t* c = in + i * 16;
            float z = (i & 1) ? g.F(-0.1f, 1.1f) : Flt(Bits(xenos::RasterDecodeDepth(g.U() & 0xFFFFFF, isFloat != 0)) + (g.U() % 9) - 4);
            if ((i & 7) == 2) z = std::ldexp(g.F(0.5f, 1.0f), -int(g.U() % 30));
            c[0] = Bits(z);
            c[5] = isFloat;
        }
        const uint32_t* out = gpu.Run(kDepthEncode, N);
        Tally t;
        for (uint32_t i = 0; i < N; ++i)
        {
            uint32_t want = xenos::RasterEncodeDepth(Flt(in[i * 16]), isFloat != 0);
            ++t.cases;
            if (want != out[i * 4]) t.Fail("encodeDepth", "z " + Hex4(in + i * 16, 1) + " cpu " + Hex4(&want, 1) + " gpu " + Hex4(out + i * 4, 1));
        }
        report(isFloat ? "encodeDepth D24FS8" : "encodeDepth D24S8", t);
    }

    // Blending: every factor (0, 1, 4-16) and op (0-4), colour and alpha.
    {
        static const uint32_t kFactors[] = { 0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        uint32_t* in = gpu.Input();
        for (uint32_t i = 0; i < N; ++i)
        {
            uint32_t* c = in + i * 16;
            for (int k = 0; k < 12; ++k) c[k] = Bits((g.U() & 3) ? g.F(0.0f, 1.0f) : Interesting(g, 0));
            auto f = [&] { return kFactors[g.U() % 15]; };
            c[12] = f() | ((g.U() % 5) << 5) | (f() << 8) | (f() << 16) | ((g.U() % 5) << 21) | (f() << 24);
        }
        const uint32_t* out = gpu.Run(kBlend, N);
        Tally t;
        for (uint32_t i = 0; i < N; ++i)
        {
            const uint32_t* c = in + i * 16;
            float s[4], d[4], k[4];
            for (int j = 0; j < 4; ++j) { s[j] = Flt(c[j]); d[j] = Flt(c[4 + j]); k[j] = Flt(c[8 + j]); }
            ++t.cases;
            bool exact = true;
            for (int j = 0; j < 4; ++j) exact &= SameFloat(Bits(xenos::RasterBlend(c[12], s, d, k, j)), out[i * 4 + j]);
            if (!exact) ++t.ftz;   // undone below if a component differs beyond -0/denormal
            for (int j = 0; j < 4; ++j)
            {
                float want = xenos::RasterBlend(c[12], s, d, k, j);
                if (!SameFloatFtz(Bits(want), out[i * 4 + j]))
                {
                    // Infinite or denormal operands: inf * 0 and flushed denormals follow the GPU's float rules.
                    bool special = false;
                    for (int q = 0; q < 12; ++q)
                        special |= !std::isfinite(Flt(c[q])) || (Flt(c[q]) != 0.0f && std::fabs(Flt(c[q])) < 1.17549435e-38f);
                    if (special) { ++t.special; --t.ftz; break; }
                    uint32_t w = Bits(want);
                    char b[32];
                    snprintf(b, sizeof(b), "blend %08X ch %d: ", c[12], j);
                    t.Fail("blend", b + std::string("src ") + Hex4(c, 4) + " dst " + Hex4(c + 4, 4) + " cpu " + Hex4(&w, 1) + " gpu " +
                                        Hex4(out + i * 4 + j, 1));
                    --t.ftz;
                    break;
                }
            }
        }
        report("blend (all factors and ops)", t);
    }

    // Resolve packing, every destination format.
    for (uint32_t destFmt : { 2u, 4u, 10u, 24u, 6u, 7u, 25u, 26u, 31u, 32u, 36u, 37u })
        for (uint32_t srcFmt : { 0u, 4u })
        {
            uint32_t* in = gpu.Input();
            for (uint32_t i = 0; i < N; ++i)
            {
                uint32_t* c = in + i * 16;
                for (int k = 0; k < 4; ++k) c[k] = Bits(Interesting(g, srcFmt));
                c[4] = destFmt;
                c[5] = srcFmt;
            }
            const uint32_t* out = gpu.Run(kPack, N);
            Tally t;
            for (uint32_t i = 0; i < N; ++i)
            {
                float c[4];
                for (int k = 0; k < 4; ++k) c[k] = Flt(in[i * 16 + k]);
                uint32_t p[2];
                xenos::RasterPackResolve(destFmt, srcFmt, c, p);
                ++t.cases;
                if (p[0] != out[i * 4] || p[1] != out[i * 4 + 1])
                    t.Fail("pack", "in " + Hex4(in + i * 16, 4) + " cpu " + Hex4(p, 2) + " gpu " + Hex4(out + i * 4, 2));
            }
            snprintf(name, sizeof(name), "packResolve dest %u src %u", destFmt, srcFmt);
            report(name, t);
        }

    // The shader ALU's correctly rounded math (xenos_shader.h Exp2/Log2/Sin/Cos, float / and sqrt).
    for (uint32_t mode : { uint32_t(kTranscendental), uint32_t(kDivSqrt) })
    {
        uint32_t* in = gpu.Input();
        for (uint32_t i = 0; i < N; ++i)
        {
            uint32_t* c = in + i * 16;
            float x;
            switch (i % 6)
            {
            case 0: x = g.F(-10.0f, 10.0f); break;
            case 1: x = g.F(-200.0f, 200.0f); break;
            case 2: x = std::ldexp(g.F(0.5f, 1.0f), int(g.U() % 60) - 30); break;
            case 3: x = g.F(0.0f, 1.0f); break;
            case 4: x = -std::ldexp(g.F(0.5f, 1.0f), int(g.U() % 40) - 20); break;
            default: x = Interesting(g, 0); break;
            }
            c[0] = Bits(x);
            c[1] = Bits((i & 1) ? g.F(-100.0f, 100.0f) : std::ldexp(g.F(0.5f, 1.0f), int(g.U() % 40) - 20));
        }
        const uint32_t* out = gpu.Run(mode, N);
        Tally t;
        for (uint32_t i = 0; i < N; ++i)
        {
            float x = Flt(in[i * 16]), y = Flt(in[i * 16 + 1]);
            float want[4];
            if (mode == kTranscendental) { want[0] = xenos::Exp2(x); want[1] = xenos::Log2(x); want[2] = xenos::Sin(x); want[3] = xenos::Cos(x); }
            else { want[0] = x / y; want[1] = std::sqrt(x); want[2] = 1.0f / std::sqrt(x); want[3] = 0.0f; }
            ++t.cases;
            for (int k = 0; k < 4; ++k)
                if (!SameFloatFtz(Bits(want[k]), out[i * 4 + k]))
                {
                    // Denormal operands (flushed by the GPU) and sin/cos beyond the exact range reduction (|x| >= 2^24).
                    auto denorm = [](float v) { return v != 0.0f && std::fabs(v) < 1.17549435e-38f; };
                    if (denorm(x) || denorm(y) || (mode == kTranscendental && k >= 2 && std::fabs(x) >= 16777216.0f)) { ++t.special; break; }
                    uint32_t w[4] = { Bits(want[0]), Bits(want[1]), Bits(want[2]), Bits(want[3]) };
                    t.Fail(kModeNames[mode], "x " + Hex4(in + i * 16, 2) + " cpu " + Hex4(w, 4) + " gpu " + Hex4(out + i * 4, 4));
                    break;
                }
        }
        report(kModeNames[mode], t);
    }

    fprintf(stderr, allOk ? "all identical\n" : "MISMATCHES\n");
    vkDeviceWaitIdle(gpu.dev.device);
    return allOk ? 0 : 1;
}
