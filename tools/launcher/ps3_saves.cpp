// PS3 (RPCS3) saves <-> the port's saves: see ps3_saves.h and tools/ps3save.py (the formats, verified 2026-10-20).
#include "ps3_saves.h"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

const wchar_t kTitle[] = L"BLUS30128";
const wchar_t kXuidDir[] = L"E000C0AC4E000009";   // runtime/imports_content.cpp kXuid
const wchar_t kTypeDir[] = L"00000001";           // XCONTENTTYPE_SAVEDGAME
const wchar_t kProfile[] = L"PROFILE-HeadCoach";  // the 360 build's one profile save

bool Read(const fs::path& p, Bytes& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
bool Write(const fs::path& p, const Bytes& b)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(f);
}
std::wstring Upper(std::wstring s)
{
    for (wchar_t& c : s) c = wchar_t(towupper(c));
    return s;
}

// EA's CRC (polynomial 0x04C11DB7, MSB first, the first four bytes inverted as the seed; sub_82F0D220).
uint32_t EaCrc(const uint8_t* d, size_t n)
{
    static uint32_t table[256];
    static bool made = false;
    if (!made)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            table[i] = c;
        }
        made = true;
    }
    if (n < 4) return 0;
    uint32_t c = ~(uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3]);
    for (size_t i = 4; i < n; ++i) c = table[c >> 24] ^ ((c << 8) | d[i]);
    return ~c;
}
void PutBe(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
uint32_t GetBe(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

Bytes PackMc02(const Bytes& hed, const Bytes& usr)
{
    Bytes b = { 'M', 'C', '0', '2' };
    PutBe(b, uint32_t(0x1C + hed.size() + usr.size()));
    PutBe(b, uint32_t(hed.size()));
    PutBe(b, uint32_t(usr.size()));
    PutBe(b, EaCrc(hed.data(), hed.size()));
    PutBe(b, EaCrc(usr.data(), usr.size()));
    PutBe(b, EaCrc(b.data(), b.size()));
    b.insert(b.end(), hed.begin(), hed.end());
    b.insert(b.end(), usr.begin(), usr.end());
    return b;
}

bool UnpackMc02(const Bytes& d, Bytes& hed, Bytes& usr, std::wstring& msg)
{
    if (d.size() < 0x1C || memcmp(d.data(), "MC02", 4)) { msg = L"not a save of this game (no MC02 header)"; return false; }
    uint32_t total = GetBe(&d[4]), hs = GetBe(&d[8]), us = GetBe(&d[12]);
    if (total != d.size() || 0x1Cull + hs + us != total) { msg = L"the save's sizes do not add up"; return false; }
    hed.assign(d.begin() + 0x1C, d.begin() + 0x1C + hs);
    usr.assign(d.begin() + 0x1C + hs, d.end());
    if (GetBe(&d[0x18]) != EaCrc(d.data(), 0x18) || GetBe(&d[0x10]) != EaCrc(hed.data(), hed.size()) ||
        GetBe(&d[0x14]) != EaCrc(usr.data(), usr.size()))
    {
        msg = L"the save's checksums are wrong (damaged?)";
        return false;
    }
    return true;
}

bool Ps3Parts(const fs::path& folder, Bytes& hed, Bytes& usr)
{
    for (const fs::path& d : { folder, folder / folder.filename() })   // a copied folder inside itself
        if (Read(d / L"HED-DATA", hed) && Read(d / L"USR-DATA", usr)) return true;
    return false;
}

fs::path PortDir(const std::wstring& cache) { return fs::path(cache) / L"content" / kXuidDir / kTypeDir; }

// PARAM.SFO: overwrite a string value in place (keys/types stay).
void SfoSetString(Bytes& sfo, const char* key, const std::string& value)
{
    if (sfo.size() < 20 || memcmp(sfo.data(), "\0PSF", 4)) return;
    auto le32 = [&](size_t o) { return uint32_t(sfo[o]) | uint32_t(sfo[o + 1]) << 8 | uint32_t(sfo[o + 2]) << 16 | uint32_t(sfo[o + 3]) << 24; };
    uint32_t kt = le32(8), dt = le32(12), n = le32(16);
    for (uint32_t i = 0; i < n; ++i)
    {
        size_t e = 20 + 16 * size_t(i);
        if (e + 16 > sfo.size()) return;
        uint32_t ko = uint32_t(sfo[e]) | uint32_t(sfo[e + 1]) << 8, mx = le32(e + 8), off = dt + le32(e + 12);
        const char* k = reinterpret_cast<const char*>(&sfo[kt + ko]);
        if (strcmp(k, key) || off + mx > sfo.size() || mx == 0) continue;
        std::string v = value.substr(0, mx - 1);
        memset(&sfo[off], 0, mx);
        memcpy(&sfo[off], v.data(), v.size());
        uint32_t len = uint32_t(v.size() + 1);
        for (int s = 0; s < 4; ++s) sfo[e + 4 + size_t(s)] = uint8_t(len >> (8 * s));
        return;
    }
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? char(c) : '?';
    return s;
}

}  // namespace

std::vector<Ps3SaveInfo> Ps3ListSaves(const std::wstring& folder)
{
    std::vector<Ps3SaveInfo> out;
    std::error_code ec;
    auto add = [&](const fs::path& d) {
        std::wstring base = d.filename().wstring(), prefix = std::wstring(kTitle) + L"-";
        if (Upper(base).rfind(prefix, 0) != 0) return;
        Ps3SaveInfo s;
        s.folder = d.wstring();
        s.name = base.substr(prefix.size());
        if (Upper(s.name) == Upper(kProfile)) s.name = kProfile;
        Bytes h, u;
        s.usable = Ps3Parts(d, h, u);
        out.push_back(s);
    };
    fs::path f = folder;
    add(f);
    if (out.empty())
        for (const auto& e : fs::directory_iterator(f, ec))
            if (e.is_directory()) add(e.path());
    return out;
}

Ps3Import Ps3ImportSave(const Ps3SaveInfo& save, const std::wstring& saves, const std::wstring& backups, bool replace,
                        std::wstring& msg)
{
    Bytes hed, usr;
    if (!Ps3Parts(save.folder, hed, usr))
    {
        msg = save.name + L": no HED-DATA + USR-DATA (a save copied off a console is still encrypted)";
        return Ps3Import::Failed;
    }
    std::error_code ec;
    fs::path root = saves, dir = root / save.name;
    if (fs::exists(dir))
    {
        if (!replace) { msg = save.name + L": already in the port"; return Ps3Import::Exists; }
        char stamp[32];
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        fs::path b = fs::path(backups) / stamp;
        fs::create_directories(b, ec);
        fs::copy(dir, b / save.name, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        fs::copy_file(root / (save.name + L".name"), b / (save.name + L".name"), fs::copy_options::overwrite_existing, ec);
    }
    fs::create_directories(dir, ec);
    fs::path file = dir / save.name, tmp = file.wstring() + L".tmp";
    if (!Write(tmp, PackMc02(hed, usr))) { msg = save.name + L": cannot write"; return Ps3Import::Failed; }
    fs::rename(tmp, file, ec);
    std::string display = Narrow(save.name);
    Write(root / (save.name + L".name"), Bytes(display.begin(), display.end()));
    msg = save.name + L": imported";
    return Ps3Import::Imported;
}

std::wstring PortSaveDir(const std::wstring& cache) { return PortDir(cache).wstring(); }

std::vector<std::wstring> PortListSaves(const std::wstring& saves)
{
    std::vector<std::wstring> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(saves), ec))
        if (e.is_directory() && fs::is_regular_file(e.path() / e.path().filename())) out.push_back(e.path().filename().wstring());
    return out;
}

bool Ps3ExportSave(const std::wstring& saves, const std::wstring& name, const std::wstring& outParent,
                   const std::wstring& templ, std::wstring& msg)
{
    Bytes d, hed, usr, sfo;
    if (!Read(fs::path(saves) / name / name, d)) { msg = name + L": not found"; return false; }
    if (!UnpackMc02(d, hed, usr, msg)) { msg = name + L": " + msg; return false; }
    if (!Read(fs::path(templ) / L"PARAM.SFO", sfo)) { msg = L"no PARAM.SFO in " + templ; return false; }
    std::wstring upper = Upper(name);   // PS3 savedata folder names are upper case
    fs::path folder = fs::path(outParent) / (std::wstring(kTitle) + L"-" + upper);
    if (fs::exists(folder)) { msg = folder.wstring() + L" exists: an RPCS3 save is never replaced (move it away first)"; return false; }
    std::string dirName = Narrow(std::wstring(kTitle) + L"-" + upper), n = Narrow(upper);
    SfoSetString(sfo, "SAVEDATA_DIRECTORY", dirName);
    SfoSetString(sfo, "SAVEDATA_LIST_PARAM", n.find('-') == std::string::npos ? n : n.substr(n.find('-') + 1));
    SfoSetString(sfo, "SUB_TITLE", n);
    SfoSetString(sfo, "DETAIL", "Converted from the PC port");
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec || !Write(folder / L"HED-DATA", hed) || !Write(folder / L"USR-DATA", usr) || !Write(folder / L"PARAM.SFO", sfo))
    {
        msg = L"cannot write " + folder.wstring();
        return false;
    }
    fs::copy_file(fs::path(templ) / L"ICON0.PNG", folder / L"ICON0.PNG", ec);
    msg = name + L" -> " + folder.wstring();
    return true;
}
