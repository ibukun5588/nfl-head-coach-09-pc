// PlayStation button icons for the port, made by the launcher from the player's own game files: the C++ port of
// tools/playstation_icons.py (docs/HANDOFF_M11.md step 7; the output is byte-identical to the script's). See that
// script for what the files hold. Writes <cache>\glyphs\playstation.bin: per Xbox icon texture, the FNV-1a 64 hash of
// its DXT5 blocks as they lie in memory and its replacement in RGBA (the PlayStation icon fitted to where the Xbox icon
// shows); the native renderer swaps them while a PlayStation pad is in use (runtime/glyphs.cpp). Nothing of the
// game's is written.
#include "ps_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#pragma clang fp contract(off)   // the arithmetic of the Python reference, operation for operation

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr int kMaxSide = 64;   // runtime/glyphs.cpp GlyphCandidate

// ---- zlib inflate (RFC 1950/1951) -------------------------------------------------------------------------------------
class Inflater
{
public:
    explicit Inflater(const Bytes& in) : in_(in) {}
    bool Run(Bytes& out)
    {
        if (in_.size() < 2 || (in_[0] & 0x0F) != 8 || ((in_[0] << 8) | in_[1]) % 31) return false;
        pos_ = 2;
        for (bool last = false; !last;)
        {
            last = Bits(1);
            int type = Bits(2);
            if (type == 0)
            {
                bitCount_ = 0;   // to a byte boundary
                if (pos_ + 4 > in_.size()) return false;
                uint32_t len = in_[pos_] | (in_[pos_ + 1] << 8);
                pos_ += 4;
                if (pos_ + len > in_.size()) return false;
                out.insert(out.end(), in_.begin() + ptrdiff_t(pos_), in_.begin() + ptrdiff_t(pos_ + len));
                pos_ += len;
            }
            else if (type == 1 || type == 2)
            {
                Huffman lit, dist;
                if (type == 1)
                {
                    uint8_t l[288], d[30];
                    for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                    for (int i = 0; i < 30; ++i) d[i] = 5;
                    lit.Build(l, 288);
                    dist.Build(d, 30);
                }
                else if (!Dynamic(lit, dist)) return false;
                if (!Block(lit, dist, out)) return false;
            }
            else return false;
            if (bad_) return false;
        }
        return true;
    }

private:
    struct Huffman
    {
        uint16_t count[16]{}, symbol[288]{};
        void Build(const uint8_t* lengths, int n)
        {
            memset(count, 0, sizeof count);
            for (int i = 0; i < n; ++i) ++count[lengths[i]];
            count[0] = 0;
            uint16_t offs[16]{};
            for (int i = 1; i < 16; ++i) offs[i] = uint16_t(offs[i - 1] + count[i - 1]);
            for (int i = 0; i < n; ++i) if (lengths[i]) symbol[offs[lengths[i]]++] = uint16_t(i);
        }
    };
    int Bits(int n)
    {
        while (bitCount_ < n)
        {
            if (pos_ >= in_.size()) { bad_ = true; return 0; }
            bitBuf_ |= uint32_t(in_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        int v = int(bitBuf_ & ((1u << n) - 1));
        bitBuf_ >>= n;
        bitCount_ -= n;
        return v;
    }
    int Decode(const Huffman& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len)
        {
            code |= Bits(1);
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (bad_) return -1;
        }
        bad_ = true;
        return -1;
    }
    bool Dynamic(Huffman& lit, Huffman& dist)
    {
        static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (nlen > 286 || ndist > 30) return false;
        uint8_t lengths[320]{};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(Bits(3));
        Huffman lencode;
        lencode.Build(lengths, 19);
        memset(lengths, 0, sizeof lengths);
        for (int i = 0; i < nlen + ndist;)
        {
            int sym = Decode(lencode);
            if (sym < 0) return false;
            if (sym < 16) { lengths[i++] = uint8_t(sym); continue; }
            int len = 0, rep;
            if (sym == 16) { if (!i) return false; len = lengths[i - 1]; rep = 3 + Bits(2); }
            else if (sym == 17) rep = 3 + Bits(3);
            else rep = 11 + Bits(7);
            if (i + rep > nlen + ndist) return false;
            while (rep--) lengths[i++] = uint8_t(len);
        }
        lit.Build(lengths, nlen);
        dist.Build(lengths + nlen, ndist);
        return !bad_;
    }
    bool Block(const Huffman& lit, const Huffman& dist, Bytes& out)
    {
        static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
        for (;;)
        {
            int sym = Decode(lit);
            if (sym < 0) return false;
            if (sym < 256) { out.push_back(uint8_t(sym)); continue; }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = lbase[sym] + size_t(Bits(lext[sym]));
            int ds = Decode(dist);
            if (ds < 0 || ds >= 30) return false;
            size_t d = dbase[ds] + size_t(Bits(dext[ds]));
            if (d > out.size()) return false;
            size_t from = out.size() - d;
            for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
        }
    }
    const Bytes& in_;
    size_t pos_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCount_ = 0;
    bool bad_ = false;
};

// ---- BGFA1.05 (EA .ast) ---------------------------------------------------------------------------------------------
struct Entry { uint8_t flag; uint64_t off, size; };

uint64_t Le(const uint8_t* p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float BeF(const uint8_t* p)
{
    uint32_t v = Be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

template <class Read>
bool ParseToc(Read read, std::vector<Entry>& out)
{
    Bytes h = read(0, 0x30);
    if (h.size() < 0x30 || memcmp(h.data(), "BGFA1.05", 8)) return false;
    uint32_t count = uint32_t(Le(&h[0xC], 4));
    uint64_t tocoff = Le(&h[0x10], 8), tocsz = Le(&h[0x18], 8);
    int wt = h[0x21], wh = h[0x22], wo = h[0x23], ws = h[0x24], wu = h[0x25], shift = h[0x26];
    uint32_t ntypes = uint32_t(Le(&h[0x28], 4)), namelen = uint32_t(Le(&h[0x2C], 4));
    Bytes toc = read(tocoff, tocsz);
    size_t p = size_t(ntypes) * 4, esz = size_t(1 + wt + wh + wo + ws + wu) + namelen;
    if (p + esz * count > toc.size()) return false;
    for (uint32_t i = 0; i < count; ++i, p += esz)
    {
        const uint8_t* e = &toc[p];
        size_t q = size_t(1 + wt + wh);
        out.push_back({ e[0], Le(e + q, wo) << shift, Le(e + q + wo, ws) });
    }
    return true;
}

bool Unpack(uint8_t flag, Bytes raw, Bytes& out)
{
    if (!(flag & 1)) { out = std::move(raw); return true; }
    out.clear();
    return Inflater(raw).Run(out);
}

// The decompressed entries of top-level movie `index` of an archive.
bool Movie(const fs::path& path, size_t index, std::vector<Bytes>& entries)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    auto read = [&](uint64_t o, uint64_t n) {
        Bytes b(static_cast<size_t>(n));
        f.seekg(std::streamoff(o));
        f.read(reinterpret_cast<char*>(b.data()), std::streamsize(n));
        b.resize(size_t(f.gcount()));
        f.clear();
        return b;
    };
    std::vector<Entry> toc;
    if (!ParseToc(read, toc) || index >= toc.size()) return false;
    Bytes data;
    if (!Unpack(toc[index].flag, read(toc[index].off, toc[index].size), data)) return false;
    std::vector<Entry> inner;
    auto readInner = [&](uint64_t o, uint64_t n) {
        if (o >= data.size()) return Bytes();
        return Bytes(data.begin() + ptrdiff_t(o), data.begin() + ptrdiff_t(std::min<uint64_t>(data.size(), o + n)));
    };
    if (!ParseToc(readInner, inner)) return false;
    for (const Entry& e : inner)
    {
        Bytes b;
        if (!Unpack(e.flag, readInner(e.off, e.size), b)) return false;
        entries.push_back(std::move(b));
    }
    return true;
}

// ---- textures ---------------------------------------------------------------------------------------------------------
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bppLog2)
{
    uint32_t outer = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t oi = (outer | inner) << bppLog2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return (((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) | (((oi >> 5) & 7) << 8) |
            ((oi >> 8) << 12));
}

struct Icon { int w = 0, h = 0; Bytes blocks; double qw = 0, qh = 0; };

// A small tiled DXT5 XPR2 texture's blocks as stored (guest byte order), in raster block order.
bool Texture(const Bytes& x, Icon& t)
{
    if (x.size() < 0x60 || memcmp(x.data(), "XPR2", 4)) return false;
    uint32_t hdr = Be32(&x[4]), res = Be32(&x[0x14]) + 0xC;
    if (res + 0x34 > x.size()) return false;
    uint32_t fetch[6];
    for (int i = 0; i < 6; ++i) fetch[i] = Be32(&x[res + 0x1C + 4 * i]);
    int fmt = fetch[1] & 0x3F, w = int(fetch[2] & 0x1FFF) + 1, h = int((fetch[2] >> 13) & 0x1FFF) + 1;
    int dim = (fetch[5] >> 9) & 3, packed = (fetch[5] >> 11) & 1;
    if (fmt != 20 || w > kMaxSide || h > kMaxSide || !(fetch[0] >> 31) || dim != 1 || packed) return false;
    size_t data = size_t(hdr) + 0xC + (fetch[1] & 0xFFFFF000u);
    t.w = w;
    t.h = h;
    t.blocks.clear();
    for (int by = 0; by < (h + 3) / 4; ++by)
        for (int bx = 0; bx < (w + 3) / 4; ++bx)
        {
            size_t o = data + TiledOffset2D(uint32_t(bx), uint32_t(by), 32, 4);
            for (size_t k = 0; k < 16; ++k) t.blocks.push_back(o + k < x.size() ? x[o + k] : 0);
        }
    return true;
}

uint64_t Fnv1a64(const Bytes& b)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t c : b) h = (h ^ c) * 0x100000001B3ull;
    return h;
}

void Rgb565(uint16_t c, int out[3])
{
    out[0] = ((c >> 11) * 255 + 15) / 31;
    out[1] = (((c >> 5) & 63) * 255 + 31) / 63;
    out[2] = ((c & 31) * 255 + 15) / 31;
}

// RGBA of a texture's blocks (guest 8-in-16 order).
Bytes Rgba(int w, int h, Bytes blocks)
{
    for (size_t k = 0; k + 1 < blocks.size(); k += 2) std::swap(blocks[k], blocks[k + 1]);
    Bytes px(size_t(w) * size_t(h) * 4);
    int bw = (w + 3) / 4;
    for (size_t n = 0; n < blocks.size() / 16; ++n)
    {
        const uint8_t* b = &blocks[n * 16];
        int a0 = b[0], a1 = b[1], al[8] = { a0, a1 };
        if (a0 > a1) for (int i = 0; i < 6; ++i) al[2 + i] = ((6 - i) * a0 + (1 + i) * a1) / 7;
        else
        {
            for (int i = 0; i < 4; ++i) al[2 + i] = ((4 - i) * a0 + (1 + i) * a1) / 5;
            al[6] = 0;
            al[7] = 255;
        }
        uint64_t abits = Le(b + 2, 6);
        int cl[4][3];
        Rgb565(uint16_t(Le(b + 8, 2)), cl[0]);
        Rgb565(uint16_t(Le(b + 10, 2)), cl[1]);
        for (int k = 0; k < 3; ++k)
        {
            cl[2][k] = (2 * cl[0][k] + cl[1][k]) / 3;
            cl[3][k] = (cl[0][k] + 2 * cl[1][k]) / 3;
        }
        uint32_t cbits = uint32_t(Le(b + 12, 4));
        int bx = int(n) % bw, by = int(n) / bw;
        for (int i = 0; i < 16; ++i)
        {
            int x = bx * 4 + i % 4, y = by * 4 + i / 4;
            if (x >= w || y >= h) continue;
            uint8_t* o = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
            const int* c = cl[(cbits >> (2 * i)) & 3];
            o[0] = uint8_t(c[0]);
            o[1] = uint8_t(c[1]);
            o[2] = uint8_t(c[2]);
            o[3] = uint8_t(al[(abits >> (3 * i)) & 7]);
        }
    }
    return px;
}

// The quad a texture's shape draws (texels [0, qw) x [0, qh) show); the texture's size if the entry is no such shape.
void QuadSize(const Bytes* shape, Icon& t)
{
    t.qw = t.w;
    t.qh = t.h;
    static const uint8_t magic[4] = { 0, 0, 1, 0x20 };
    if (!shape || shape->size() < 0xE8 || memcmp(shape->data(), magic, 4)) return;
    double qw = BeF(&(*shape)[0xA0]), qh = BeF(&(*shape)[0xE4]);
    if (!(qw > 0 && qw <= 4 * kMaxSide && qh > 0 && qh <= 4 * kMaxSide)) return;
    t.qw = qw;
    t.qh = qh;
}

std::vector<Icon> Icons(const std::vector<Bytes>& entries)
{
    std::vector<Icon> out;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        Icon t;
        if (!Texture(entries[i], t)) continue;
        QuadSize(i + 1 < entries.size() ? &entries[i + 1] : nullptr, t);
        out.push_back(std::move(t));
    }
    return out;
}

struct Box { double x0, y0, x1, y1; };

// The visible texels (alpha >= 24) inside the quad; the quad if none are. Python's round(): ties to even.
Box BBox(const Bytes& img, int w, int h, double qwf, double qhf)
{
    int qw = std::min(w, int(std::nearbyint(qwf))), qh = std::min(h, int(std::nearbyint(qhf)));
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int y = 0; y < qh; ++y)
        for (int x = 0; x < qw; ++x)
            if (img[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] >= 24)
            {
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    if (x1 < 0) return { 0, 0, double(qw), double(qh) };
    return { double(x0), double(y0), double(x1 + 1), double(y1 + 1) };
}

// The texels in sbox of src scaled uniformly to fit dbox of a transparent dw x dh image, centred (bilinear on
// premultiplied alpha): playstation_icons.py fit(), operation for operation.
Bytes Fit(const Bytes& src, int sw, int sh, Box s, int dw, int dh, Box d)
{
    double sq0 = s.x1 - s.x0, sq1 = s.y1 - s.y0, dq0 = d.x1 - d.x0, dq1 = d.y1 - d.y0;
    Bytes out(size_t(dw) * size_t(dh) * 4);
    auto texel = [&](int x, int y, double t[4]) {
        x = std::min(std::max(x, 0), sw - 1);
        y = std::min(std::max(y, 0), sh - 1);
        const uint8_t* o = &src[(size_t(y) * size_t(sw) + size_t(x)) * 4];
        int a = o[3];
        t[0] = double(o[0] * a); t[1] = double(o[1] * a); t[2] = double(o[2] * a); t[3] = double(a);
    };
    double scale = std::min(dq0 / sq0, dq1 / sq1);
    double ox = d.x0 + (dq0 - sq0 * scale) / 2, oy = d.y0 + (dq1 - sq1 * scale) / 2;
    for (int y = 0; y < dh; ++y)
    {
        double fy = (y + 0.5 - oy) / scale - 0.5;
        if (fy < -0.5 || fy > sq1 - 0.5) continue;
        int y0 = int(std::floor(fy));
        double ty = fy - y0;
        y0 += int(s.y0);
        for (int x = 0; x < dw; ++x)
        {
            double fx = (x + 0.5 - ox) / scale - 0.5;
            if (fx < -0.5 || fx > sq0 - 0.5) continue;
            fx += s.x0;
            int x0 = int(std::floor(fx));
            double tx = fx - x0;
            double c[4] = { 0, 0, 0, 0 }, t[4];
            const int xs[4] = { x0, x0 + 1, x0, x0 + 1 }, ys[4] = { y0, y0, y0 + 1, y0 + 1 };
            const double wg[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };
            for (int k = 0; k < 4; ++k)
            {
                texel(xs[k], ys[k], t);
                for (int j = 0; j < 4; ++j) c[j] += t[j] * wg[k];
            }
            double a = c[3];
            if (a > 0.5)
            {
                uint8_t* o = &out[(size_t(y) * size_t(dw) + size_t(x)) * 4];
                for (int j = 0; j < 3; ++j) o[j] = uint8_t(std::min(255, int(c[j] / a + 0.5)));
                o[3] = uint8_t(std::min(255, int(a + 0.5)));
            }
        }
    }
    return out;
}

}  // namespace

bool MakePlayStationIcons(const std::wstring& root, const std::wstring& outFile, std::string& err, int* count)
{
    // qkl_boot.ast entry 83: the button icon library (44 textures); its order (playstation_icons.py XBOX / PS).
    static const char* const kXbox[19] = { "B", "A", "Back", "D-pad", "LB", "press LS", "LS", "LS", "LS left/right", "LT",
                                           "RB", "press RS", "RS", "RS", "RS left/right", "RT", "Start", "X", "Y" };
    static const std::map<std::string, int> kPs = {
        { "B", 19 }, { "A", 20 }, { "Back", 32 }, { "D-pad", 21 }, { "LB", 22 }, { "LT", 23 }, { "press LS", 24 }, { "LS", 25 },
        { "LS left/right", 26 }, { "LS up/down", 25 }, { "RB", 27 }, { "RT", 28 }, { "press RS", 29 }, { "RS", 30 },
        { "RS left/right", 31 }, { "RS up/down", 30 }, { "Start", 34 }, { "X", 33 }, { "Y", 35 } };
    // qkl_interface.ast 0x20..0x34: standalone Xbox icon movies.
    static const char* const kInterface[21] = { "A", "B", "Back", "D-pad", "LB", "press LS", "LS", "LS left/right", "LS",
                                                "LS up/down", "LT", "RB", "press RS", "RS", "RS left/right", "RS",
                                                "RS up/down", "RT", "Start", "X", "Y" };
    fs::path r = root;
    std::vector<Bytes> entries;
    if (!Movie(r / L"qkl_boot.ast", 83, entries)) { err = "cannot read the button icons from qkl_boot.ast"; return false; }
    std::vector<Icon> lib = Icons(entries);
    if (lib.size() != 44) { err = "qkl_boot.ast entry 83 has " + std::to_string(lib.size()) + " icon textures, expected 44"; return false; }

    std::vector<std::pair<std::string, Icon>> targets;
    for (int k = 0; k < 19; ++k) targets.push_back({ kXbox[k], lib[size_t(k)] });
    for (int i = 0; i < 21; ++i)
    {
        std::vector<Bytes> m;
        if (!Movie(r / L"qkl_interface.ast", size_t(0x20 + i), m)) { err = "cannot read qkl_interface.ast"; return false; }
        std::vector<Icon> t = Icons(m);
        if (t.size() != 1) { err = "qkl_interface.ast entry " + std::to_string(0x20 + i) + " is not an icon movie"; return false; }
        targets.push_back({ kInterface[i], t[0] });
    }

    Bytes out = { 'H', 'C', 'G', 'L', 1, 0, 0, 0, 0, 0, 0, 0 };
    std::vector<uint64_t> seen;
    for (const auto& [name, t] : targets)
    {
        uint64_t key = Fnv1a64(t.blocks);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        const Icon& s = lib[size_t(kPs.at(name))];
        Bytes simg = Rgba(s.w, s.h, s.blocks), dimg = Rgba(t.w, t.h, t.blocks);
        Bytes rep = Fit(simg, s.w, s.h, BBox(simg, s.w, s.h, s.qw, s.qh), t.w, t.h, BBox(dimg, t.w, t.h, t.qw, t.qh));
        for (int i = 0; i < 8; ++i) out.push_back(uint8_t(key >> (8 * i)));
        for (int v : { t.w, t.h }) for (int i = 0; i < 4; ++i) out.push_back(uint8_t(uint32_t(v) >> (8 * i)));
        out.insert(out.end(), rep.begin(), rep.end());
    }
    uint32_t n = uint32_t(seen.size());
    memcpy(&out[8], &n, 4);
    std::error_code ec;
    fs::create_directories(fs::path(outFile).parent_path(), ec);
    fs::path tmp = fs::path(outFile).wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        if (!f) { err = "cannot write the icon file"; return false; }
    }
    fs::rename(tmp, outFile, ec);
    if (ec) { err = "cannot write the icon file"; return false; }
    if (count) *count = int(n);
    return true;
}
