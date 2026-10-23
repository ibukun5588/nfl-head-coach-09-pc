// xexpatch: applies a 360 title update to its base default.xex (XenonUtils XexPatcher, from Xenia).
//
//   xexpatch <base default.xex> <default.xexp | title update package> <out default.xex>
//   xexpatch --image <base default.xex> <default.xexp | title update package> <out headcoach.basefile>
//
// The update may be the LIVE/CON package itself (its default.xexp is read out of it, as tools/stfs.py does) or the
// extracted default.xexp. --image writes the patched program's PE image (what hc09.exe loads; XexPatcher's output is
// stored raw, so the image is the xex's data as it is) instead of the patched xex. Prints the result and exits 0 on
// success. The launcher runs it as hc09-xexpatch.exe (CMake target, static C runtime) to make 1.2\headcoach.basefile.
// By hand (static CRT, as XenonUtils.lib): clang-cl /std:c++20 /O2 /EHsc /MT /I<XenonRecomp-hc09>\XenonUtils
//   /I<XenonRecomp-hc09>\thirdparty\simde tools\xexpatch.cpp /link <build>\XenonUtils\XenonUtils.lib <build>\thirdparty\disasm\disasm.lib
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "xex_patcher.h"

namespace {

bool Read(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o)
{
    return o + 4 <= d.size() ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3] : 0;
}
uint32_t Le24(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16; }

// default.xexp out of an STFS package (LIVE/CON/PIRS): the file table, then the file's consecutive blocks.
bool FromStfs(const std::vector<uint8_t>& d, std::vector<uint8_t>& xexp, std::string& err)
{
    if (d.size() < 0x1000) { err = "package too small"; return false; }
    uint32_t hdr = Be32(d, 0x340);
    const uint8_t* vd = &d[0x379];
    int sep = vd[2], ftCount = vd[3] | vd[4] << 8;
    uint32_t ftBlock = Le24(vd + 5);
    size_t first = (hdr + 0xFFF) & ~size_t(0xFFF);
    auto offsetOf = [&](uint32_t block, int shift) {
        // Xenia StfsContainerDevice::BlockToOffsetSTFS: hash tables before every 0xAA and 0x70E4 blocks
        uint64_t b = block + (uint64_t((block / 0xAA) + 1) << shift);
        if (block >= 0xAA)
        {
            b += uint64_t((block / 0x70E4) + 1) << shift;
            if (block >= 0x70E4) b += uint64_t(1) << shift;
        }
        return first + size_t(b << 12);
    };
    for (int shift : { (sep & 1) ? 0 : 1, (sep & 1) ? 1 : 0 })
    {
        for (int t = 0; t < ftCount; ++t)
        {
            size_t at = offsetOf(ftBlock + t, shift);
            for (size_t e = at; e + 0x40 <= at + 0x1000 && e + 0x40 <= d.size(); e += 0x40)
            {
                if (!d[e]) break;
                int n = d[e + 0x28] & 0x3F, flags = d[e + 0x28];
                if (std::string(reinterpret_cast<const char*>(&d[e]), size_t(n)) != "default.xexp" || (flags & 0x80)) continue;
                if (!(flags & 0x40)) { err = "default.xexp is not stored in consecutive blocks"; return false; }
                uint32_t blocks = Le24(&d[e + 0x29]), start = Le24(&d[e + 0x2F]), size = Be32(d, e + 0x34);
                xexp.clear();
                for (uint32_t i = 0; i < blocks; ++i)
                {
                    size_t o = offsetOf(start + i, shift);
                    if (o + 0x1000 > d.size()) { err = "package truncated"; return false; }
                    xexp.insert(xexp.end(), d.begin() + o, d.begin() + o + 0x1000);
                }
                xexp.resize(size);
                if (xexp.size() >= 4 && !memcmp(xexp.data(), "XEX", 3)) return true;
            }
        }
    }
    err = "no default.xexp in the package";
    return false;
}

}  // namespace

// Unicode arguments (wmain): a game folder with any characters in its path works.
int wmain(int argc, wchar_t** argv)
{
    bool image = argc > 1 && !wcscmp(argv[1], L"--image");
    if (argc != 4 + image)
    {
        fprintf(stderr, "usage: xexpatch [--image] <base default.xex> <default.xexp | title update package> <out>\n");
        return 2;
    }
    std::filesystem::path basePath = argv[1 + image], patchPath = argv[2 + image], outPath = argv[3 + image];
    std::vector<uint8_t> base, patch, out;
    if (!Read(basePath, base)) { printf("FileOpenFailed: %ls\n", basePath.c_str()); return 1; }
    if (!Read(patchPath, patch)) { printf("FileOpenFailed: %ls\n", patchPath.c_str()); return 1; }
    if (patch.size() >= 4 && (!memcmp(patch.data(), "LIVE", 4) || !memcmp(patch.data(), "CON ", 4) || !memcmp(patch.data(), "PIRS", 4)))
    {
        std::vector<uint8_t> xexp;
        std::string err;
        if (!FromStfs(patch, xexp, err)) { printf("PatchFileInvalid: %s\n", err.c_str()); return 1; }
        patch.swap(xexp);
    }
    static const char* names[] = { "Success", "FileOpenFailed", "FileWriteFailed", "XexFileUnsupported", "XexFileInvalid",
                                   "PatchFileInvalid", "PatchIncompatible", "PatchFailed", "PatchUnsupported" };
    auto r = XexPatcher::apply(base.data(), base.size(), patch.data(), patch.size(), out, false);
    if (r != XexPatcher::Result::Success) { printf("%s\n", names[int(r)]); return 1; }
    if (image)
    {
        // the raw (unencrypted, uncompressed) xex XexPatcher writes: the PE image is its data as it is
        uint32_t peOff = Be32(out, 0x08), sec = Be32(out, 0x10), size = Be32(out, sec + 4);
        if (!peOff || uint64_t(peOff) + size > out.size() || out[peOff] != 'M' || out[peOff + 1] != 'Z')
        {
            printf("XexFileInvalid: the patched xex holds no plain image\n");
            return 1;
        }
        out = std::vector<uint8_t>(out.begin() + peOff, out.begin() + peOff + size);
    }
    std::ofstream f(outPath, std::ios::binary);
    if (!f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) { printf("FileWriteFailed: %ls\n", outPath.c_str()); return 1; }
    printf("Success\n");
    return 0;
}
