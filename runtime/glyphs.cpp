// Button prompt icons for the controller in use (see glyphs.h).
//
// <cache>/glyphs/playstation.bin (tools/playstation_icons.py): "HCGL", u32 version 1,
// u32 count, then per icon u64 key, u32 width, u32 height, width*height RGBA8.
// The key is FNV-1a 64 over the Xbox icon's DXT blocks as they lie in guest
// memory, in raster block order, each read at its tiled offset; the image has
// the Xbox texture's size, so the game's shapes (which take texels 1:1) frame
// the PlayStation glyph where the Xbox one was.
#include "dev_env.h"
#include "glyphs.h"

#include "host_input.h"
#include "kernel.h"
#include "xenos/xenos.h"
#if defined(HC09_HAVE_VULKAN)
#include "gpu_vk/vk_native.h"
#endif

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kFmtDxt45 = 20;

struct Glyph { uint32_t width = 0, height = 0; std::vector<uint32_t> rgba; };

enum class Mode { Auto, PlayStation, Xbox };

Mode ModeSetting()
{
    std::string v;
    if (const char* e = Hc09Env("HC09_GLYPHS")) for (const char* c = e; *c; ++c) v += char(tolower((unsigned char)*c));
    if (v == "playstation" || v == "ps" || v == "ps3") return Mode::PlayStation;
    if (v == "xbox") return Mode::Xbox;
    return Mode::Auto;
}

struct Table
{
    std::unordered_map<uint64_t, Glyph> icons;

    Table()
    {
        std::string path = g_vfsCache + "/glyphs/playstation.bin";
        FILE* f = fopen(path.c_str(), "rb");
        if (!f)
        {
            fprintf(stderr, "[glyphs] no %s (tools/playstation_icons.py makes it): Xbox icons only\n", path.c_str());
            return;
        }
        char magic[4] = {};
        uint32_t version = 0, count = 0;
        bool ok = fread(magic, 4, 1, f) == 1 && !memcmp(magic, "HCGL", 4) && fread(&version, 4, 1, f) == 1 &&
                  version == 1 && fread(&count, 4, 1, f) == 1 && count <= 256;
        for (uint32_t i = 0; ok && i < count; ++i)
        {
            uint64_t key = 0;
            Glyph g;
            ok = fread(&key, 8, 1, f) == 1 && fread(&g.width, 4, 1, f) == 1 && fread(&g.height, 4, 1, f) == 1 &&
                 g.width && g.height && g.width <= 64 && g.height <= 64;
            if (!ok) break;
            g.rgba.resize(size_t(g.width) * g.height);
            ok = fread(g.rgba.data(), 4, g.rgba.size(), f) == g.rgba.size();
            if (ok) icons[key] = std::move(g);
        }
        fclose(f);
        if (!ok)
        {
            fprintf(stderr, "[glyphs] %s is damaged: Xbox icons only\n", path.c_str());
            icons.clear();
            return;
        }
        fprintf(stderr, "[glyphs] %zu PlayStation button icons loaded\n", icons.size());
    }
};

Table& TheTable() { static Table t; return t; }

// HC09_GLYPHS_DEBUG=1 logs each candidate texture with its key; =2 also writes every 2D texture up to
// 128x128 the native renderer decodes to captures/glyphs/<format>_<w>x<h>_<address>.png, once (to
// find which texture a prompt is drawn from).
int DebugLevel()
{
    static const int level = [] { const char* e = Hc09Env("HC09_GLYPHS_DEBUG"); return e && *e ? atoi(e) : 0; }();
    return level;
}

uint64_t BlockHash(const xenos::TextureFetch& tf)
{
    const uint8_t* base = xenos::HostPhys(tf.baseAddress() & 0x1FFFFFFFu);
    if (!base) return 0;
    uint32_t bw = (tf.width() + 3) / 4, bh = (tf.height() + 3) / 4;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const uint8_t* b = base + xenos::TiledOffset2D(bx, by, 32, 4);
            for (int i = 0; i < 16; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
        }
    return h;
}

}  // namespace

uint32_t GlyphSetActive()
{
    static const Mode mode = [] {
        Mode m = ModeSetting();
        fprintf(stderr, "[glyphs] button icons: %s\n",
                m == Mode::PlayStation ? "PlayStation" : m == Mode::Xbox ? "Xbox" : "auto (the controller in use)");
        return m;
    }();
    if (mode == Mode::Xbox) return 0;
    if (mode == Mode::Auto && HostInputPadKind() != HostPadKind::PlayStation) return 0;
    return TheTable().icons.empty() ? 0 : 1;
}

bool GlyphCandidate(const xenos::TextureFetch& tf)
{
    return tf.dimension() == 1 && !tf.stacked() && tf.format() == kFmtDxt45 && tf.tiled() &&
           tf.width() <= 64 && tf.height() <= 64 && !tf.packedMips();
}

bool GlyphReplace(const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img)
{
    if (set != 1 || !GlyphCandidate(tf)) return false;
    Table& t = TheTable();
    uint64_t key = BlockHash(tf);
    auto it = t.icons.find(key);
    if (DebugLevel() >= 1)
        fprintf(stderr, "[glyphs] candidate %ux%u at %08X pitch %u: key %016llX %s\n", tf.width(), tf.height(),
                tf.baseAddress(), tf.pitchTexels(), (unsigned long long)key, it != t.icons.end() ? "ICON" : "-");
    if (it == t.icons.end()) return false;
    const Glyph& g = it->second;
    if (g.width != img.width || g.height != img.height) return false;
    img.rgba = g.rgba;
    return true;
}

#if defined(HC09_HAVE_VULKAN)
namespace {
// The native renderer asks for each texture: glyph candidates get the active set.
constexpr uint32_t kDumpBit = 0x80000000u;

const bool kRegistered = [] {
    hc09vk::TextureOverride o;
    o.set = [](const xenos::TextureFetch& tf) -> uint32_t {
        uint32_t set = GlyphCandidate(tf) ? GlyphSetActive() : 0;
        if (DebugLevel() >= 2 && tf.dimension() == 1 && !tf.stacked() && tf.width() <= 128 && tf.height() <= 128)
            set |= kDumpBit;
        return set;
    };
    o.replace = [](const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img) -> bool {
        if (set & kDumpBit)
        {
            static std::mutex mu;
            static std::unordered_map<uint64_t, bool> seen;
            uint64_t id = (uint64_t(tf.baseAddress()) << 32) | (tf.format() << 26) | (tf.width() << 13) | tf.height();
            std::lock_guard<std::mutex> lk(mu);
            if (!seen[id])
            {
                seen[id] = true;
                char name[160];
                snprintf(name, sizeof(name), "captures/glyphs/%s_%ux%u_%08X.png", xenos::TextureFormatName(tf.format()),
                         tf.width(), tf.height(), tf.baseAddress());
                xenos::WritePng(name, img);
            }
        }
        return GlyphReplace(tf, set & ~kDumpBit, img);
    };
    hc09vk::SetTextureOverride(o);
    return true;
}();
}  // namespace
#endif
