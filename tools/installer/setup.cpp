// HC09Setup.exe: installs NFL Head Coach 09 for PC from the player's own Xbox 360 game (docs/HANDOFF_M11.md step 4).
// Of the game, only the 1.2 title update package comes with it (kit\title_update, used when the player gives none):
// the player points it at their disc image (read in place) or the folder with their default.xex and qkl_*.ast files,
// and optionally their own title update; it checks them against the US retail
// hashes, builds hc09.exe for 1.0 (and 1.2) on this PC with the kit beside it (kit\hc09-build.exe: our tools only),
// installs the launcher, writes hc09.ini and makes the game images. Running it again updates an install and keeps
// the saves and settings. The saves are in Documents\NFL Head Coach 09\Saves (an older install's are moved there:
// SavesToDocuments).
//
//   HC09Setup.exe                                   the window
//   HC09Setup.exe --source <disc image or folder> --dest <install folder> [--tu <title update>] [--yes]
//                                                   unattended (console); --yes: continue past warnings;
//                                                   --no-shortcuts: none on the desktop/Start menu (tests)
//
// Decisions (2026-10-23): archives that differ from retail are a warning the player may continue past (named by
// file only: the installer recognises no particular mod); both game versions are built when the update is given.
// 2026-10-23: the release includes the title update, so both versions are built by default; without it in the
// kit (taken down on request) the player's own file is used as before.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <intrin.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---- the retail files (docs/HANDOFF_RELEASE.md 1.1a): hashes and sizes only, never the files -------------------------
constexpr const char* kXex10 = "24CFB63D2A8924FCC9B7AEE162B0ADCAC4140C05312876CBD3AB5107CB07243F";
constexpr const char* kTuPackage = "43B31C2B51E9797C6CAF02377D84203E963B977504B30B175382A4D1E17C44F2";
constexpr const char* kTuXexp = "A789D7238A58A11F71E8012345B26EC9B1F5B7B25705A6F036424EF8F0E5C043";
const wchar_t kBundledTu[] = L"title_update\\TU_12K2221_0000004000000.00000000000G1";   // in the kit (make_kit.py --tu)
struct Archive { const char* name; uint64_t size; const char* sha256; };
constexpr Archive kArchives[] = {
    { "qkl_boot.ast", 487625248ull, "F0674B39A2724E5FB1FE71C4D4DE3D75547939B703B505631714509483584A9F" },
    { "qkl_fe2ig.ast", 2289325516ull, "305BFC49886D09BE3B1F35B22805EBF3AB2F2B69A371BEC0DDDEC0DA8CFA42CD" },
    { "qkl_interface.ast", 305071742ull, "464F6BB5BDB8A37BBF9A19E3C7FC8DB8F12AB3E9FA664D65BC572B5587F12263" },
    { "qkl_misc.ast", 777209514ull, "281B5787654C74AFAE71DA862B54F67E637AFC6140E0DBCC9A0F04CE040D8812" },
    { "qkl_stream.ast", 1816207613ull, "36A09E583AA86F865072BDAA707F3BEB174CD352F221C4645F102B1531AD1BA5" },
};
const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NFLHeadCoach09PC";
const wchar_t kShortcutName[] = L"NFL Head Coach 09 (PC).lnk";
constexpr uint64_t kVersionBytes = 160ull << 20;   // one installed version (hc09.exe, hc09rt.dll, DLLs)
constexpr uint64_t kBuildBytes = 500ull << 20;     // the build's work folder at its largest (generated C++, objects)

std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
std::string Lower(std::string s)
{
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}
std::string Gb(uint64_t b)
{
    char t[32];
    snprintf(t, sizeof t, "%.1f GB", b / 1e9);
    return t;
}

// SHA-256 over a stream of chunks.
struct Sha256
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Sha256()
    {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256()
    {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void Add(const void* p, size_t n) { BCryptHashData(h, (PUCHAR)p, ULONG(n), 0); }
    std::string Hex()
    {
        UCHAR d[32];
        BCryptFinishHash(h, d, 32, 0);
        std::string s;
        char x[3];
        for (UCHAR c : d) { snprintf(x, sizeof x, "%02X", c); s += x; }
        return s;
    }
};

// ---- the player's game: an Xbox 360 disc image (XDVDFS, read in place) or a folder ------------------------------------
// XDVDFS: the game partition at 0 (an extracted "XISO") or at the disc formats' offsets; the volume descriptor at
// sector 32 ("MICROSOFT*XBOX*MEDIA", root directory sector and size); directories are binary trees of entries
// {u16 left, u16 right (4-byte units), u32 sector, u32 size, u8 attributes (0x10 directory), u8 name length, name}.
class GameSource
{
public:
    bool Open(const fs::path& p, std::string& err)
    {
        path_ = p;
        files_.clear();
        std::error_code ec;
        if (fs::is_directory(p, ec))
        {
            iso_ = false;
            for (const auto& e : fs::directory_iterator(p, ec))
                if (e.is_regular_file()) files_[Lower(Narrow(e.path().filename().wstring()))] = { 0, e.file_size() };
            return true;
        }
        iso_ = true;
        file_ = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) { err = "cannot open " + Narrow(p.wstring()); return false; }
        for (uint64_t base : { 0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull })
        {
            uint8_t d[28];
            if (!ReadAt(base + 32 * kSector, d, sizeof d) || memcmp(d, "MICROSOFT*XBOX*MEDIA", 20)) continue;
            base_ = base;
            uint32_t rootSector, rootSize;
            memcpy(&rootSector, d + 20, 4);
            memcpy(&rootSize, d + 24, 4);
            if (!ReadDir(rootSector, rootSize, "", 0)) { err = "the disc image's directory is damaged"; return false; }
            return true;
        }
        err = "this file is not an Xbox 360 disc image (no XDVDFS volume found)";
        return false;
    }
    ~GameSource() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    bool IsImage() const { return iso_; }
    bool Has(const std::string& name) const { return files_.count(Lower(name)) != 0; }
    uint64_t Size(const std::string& name) const { auto i = files_.find(Lower(name)); return i == files_.end() ? 0 : i->second.size; }
    // Streams a file's bytes to `sink` (false from the sink stops); `done` counts bytes for progress.
    bool Read(const std::string& name, const std::function<bool(const uint8_t*, size_t)>& sink) const
    {
        auto it = files_.find(Lower(name));
        if (it == files_.end()) return false;
        std::vector<uint8_t> buf(4 << 20);
        if (!iso_)
        {
            std::ifstream in(path_ / Widen(name), std::ios::binary);
            while (in)
            {
                in.read((char*)buf.data(), std::streamsize(buf.size()));
                if (in.gcount() > 0 && !sink(buf.data(), size_t(in.gcount()))) return false;
            }
            return true;
        }
        uint64_t off = base_ + it->second.sector * kSector, left = it->second.size;
        while (left)
        {
            size_t n = size_t(std::min<uint64_t>(left, buf.size()));
            if (!ReadAt(off, buf.data(), n) || !sink(buf.data(), n)) return false;
            off += n;
            left -= n;
        }
        return true;
    }

private:
    static constexpr uint64_t kSector = 0x800;
    struct Entry { uint64_t sector, size; };
    bool ReadAt(uint64_t off, void* p, size_t n) const
    {
        OVERLAPPED o{};
        o.Offset = DWORD(off);
        o.OffsetHigh = DWORD(off >> 32);
        DWORD got = 0;
        return ReadFile(file_, p, DWORD(n), &got, &o) && got == n;
    }
    bool ReadDir(uint32_t sector, uint32_t size, const std::string& prefix, int depth)
    {
        if (depth > 8 || size == 0 || size > (64u << 20)) return depth > 0 || size == 0 ? true : false;
        std::vector<uint8_t> t(size);
        if (!ReadAt(base_ + sector * kSector, t.data(), size)) return false;
        std::vector<uint32_t> todo{ 0 };
        std::vector<bool> seen(size / 4 + 1);
        while (!todo.empty())
        {
            uint32_t off = todo.back();
            todo.pop_back();
            if (off + 14 > size || seen[off / 4]) continue;
            seen[off / 4] = true;
            uint16_t l, r;
            uint32_t sec, sz;
            memcpy(&l, &t[off], 2);
            memcpy(&r, &t[off + 2], 2);
            if (l == 0xFFFF) continue;   // padding
            memcpy(&sec, &t[off + 4], 4);
            memcpy(&sz, &t[off + 8], 4);
            uint8_t attr = t[off + 12], len = t[off + 13];
            if (off + 14 + len > size) continue;
            std::string name = prefix + Lower(std::string((const char*)&t[off + 14], len));
            if (attr & 0x10) { if (!ReadDir(sec, sz, name + "/", depth + 1)) return false; }
            else files_[name] = { sec, sz };
            if (l) todo.push_back(uint32_t(l) * 4);
            if (r) todo.push_back(uint32_t(r) * 4);
        }
        return true;
    }
    fs::path path_;
    bool iso_ = false;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t base_ = 0;
    std::map<std::string, Entry> files_;
};

std::string Upper(std::string s)
{
    for (char& c : s) c = char(toupper((unsigned char)c));
    return s;
}

std::string FileSha256(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    Sha256 h;
    std::vector<char> b(1 << 20);
    while (in)
    {
        in.read(b.data(), std::streamsize(b.size()));
        if (in.gcount() > 0) h.Add(b.data(), size_t(in.gcount()));
    }
    return h.Hex();
}

// An ini value ("" section: keys before any section); empty when missing.
std::string IniGet(const fs::path& p, const std::string& section, const std::string& key)
{
    std::ifstream in(p);
    std::string l, cur;
    auto trim = [](std::string s) { s.erase(0, s.find_first_not_of(" \t\r")); s.erase(s.find_last_not_of(" \t\r") + 1); return s; };
    while (std::getline(in, l))
    {
        std::string t = trim(l);
        if (t.size() > 2 && t.front() == '[' && t.back() == ']') { cur = Lower(trim(t.substr(1, t.size() - 2))); continue; }
        size_t eq = t.find('=');
        if (cur == section && !t.empty() && t[0] != ';' && t[0] != '#' && eq != std::string::npos && Lower(trim(t.substr(0, eq))) == key)
            return trim(t.substr(eq + 1));
    }
    return {};
}

// ---- hc09.ini: change [section] key = value, keep everything else -----------------------------------------------------
void IniSet(const fs::path& p, const std::string& section, const std::string& key, const std::string& value)
{
    std::vector<std::string> lines;
    {
        std::ifstream in(p);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    auto trim = [](std::string s) { s.erase(0, s.find_first_not_of(" \t\r")); s.erase(s.find_last_not_of(" \t\r") + 1); return s; };
    std::string cur;
    int sectionEnd = -1, found = -1;
    for (int i = 0; i < int(lines.size()); ++i)
    {
        std::string t = trim(lines[size_t(i)]);
        if (t.size() > 2 && t.front() == '[' && t.back() == ']') { cur = Lower(trim(t.substr(1, t.size() - 2))); continue; }
        if (cur != section) continue;
        sectionEnd = i;
        size_t eq = t.find('=');
        if (!t.empty() && t[0] != ';' && t[0] != '#' && eq != std::string::npos && Lower(trim(t.substr(0, eq))) == key) found = i;
    }
    std::string line = key + " = " + value;
    if (found >= 0) lines[size_t(found)] = line;
    else
    {
        bool has = false;
        for (auto& l : lines) if (Lower(trim(l)) == "[" + section + "]") has = true;
        if (!has) { if (!lines.empty()) lines.push_back(""); lines.push_back("[" + section + "]"); lines.push_back(line); }
        else if (sectionEnd >= 0) lines.insert(lines.begin() + sectionEnd + 1, line);
        else
            for (size_t i = 0; i < lines.size(); ++i)
                if (Lower(trim(lines[i])) == "[" + section + "]") { lines.insert(lines.begin() + ptrdiff_t(i) + 1, line); break; }
    }
    std::ofstream out(p, std::ios::binary);
    for (auto& l : lines) out << l << "\r\n";
}

bool CpuHasAvx2()
{
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1, fma = (r[2] >> 12) & 1, movbe = (r[2] >> 22) & 1;
    if (!osxsave || !avx || !fma || !movbe || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(r, 7, 0);
    return ((r[1] >> 5) & 1) && ((r[1] >> 3) & 1) && ((r[1] >> 8) & 1);   // AVX2, BMI1, BMI2
}

// ---- the installation ---------------------------------------------------------------------------------------------
struct Ui
{
    std::function<void(const std::string&)> log;
    std::function<void(double, const std::string&)> progress;   // 0..1, step
    std::function<bool(const std::string&)> confirm;           // a warning: true = continue
};

class Installer
{
public:
    fs::path setupDir, source, tu, dest;
    bool shortcuts = true;
    Ui ui;
    std::atomic<bool> cancel{ false };

    bool Run()
    {
        // Every log line also goes to <dest>\hc09-setup.log (the log a player can send); kept until the folder exists.
        auto shown = ui.log;
        auto file = std::make_shared<std::ofstream>();
        auto early = std::make_shared<std::vector<std::string>>();
        ui.log = [shown, file, early](const std::string& s) {
            shown(s);
            if (file->is_open()) { *file << s << "\r\n"; file->flush(); }
            else early->push_back(s);
        };
        bool ok = RunSteps(*file, *early);
        ui.log = shown;
        if (!file->is_open() && !early->empty())   // stopped before the install folder existed: the log goes to %TEMP%
        {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            fs::path p = fs::path(tmp) / L"hc09-setup.log";
            std::ofstream out(p, std::ios::binary | std::ios::app);
            out << "\r\n==== HC09Setup ====\r\n";
            for (auto& l : *early) out << l << "\r\n";
            ui.log("The log is in " + Narrow(p.wstring()));
        }
        return ok;
    }

private:
    bool RunSteps(std::ofstream& logFile, std::vector<std::string>& early)
    {
        std::string err;
        dest = fs::absolute(dest);
        const fs::path kit = setupDir / L"kit";
        if (!fs::exists(kit / L"hc09-build.exe")) return Fail("the kit folder is missing beside HC09Setup.exe");
        if (!CpuHasAvx2())
            return Fail("this computer's processor lacks AVX2 (Intel Haswell 2013 or later, AMD Ryzen or later are needed)");

        // 1. The player's files.
        Step(0.00, "Checking your game files");
        GameSource src;
        if (!src.Open(source, err)) return Fail(err);
        ui.log(src.IsImage() ? "Disc image: " + Narrow(source.wstring()) : "Game folder: " + Narrow(source.wstring()));
        // Reading a disc image is tested only with images made by tools/make_test_xiso.py (no real one at hand yet).
        if (src.IsImage()) ui.log("(disc images are in beta: if this one fails, use the folder of your dump instead)");
        if (!src.Has("default.xex")) return Fail("default.xex is not in " + Narrow(source.wstring()));
        std::vector<std::string> missing, differ;
        for (const auto& a : kArchives) if (!src.Has(a.name)) missing.push_back(a.name);
        if (!missing.empty())
        {
            std::string m;
            for (auto& s : missing) m += " " + s;
            return Fail("these game archives are missing:" + m);
        }
        uint64_t total = src.Size("default.xex"), done = 0;
        for (const auto& a : kArchives) total += src.Size(a.name);
        auto hashOf = [&](const std::string& name, std::string& out) {
            Sha256 h;
            bool ok = src.Read(name, [&](const uint8_t* p, size_t n) {
                h.Add(p, n);
                done += n;
                Step(0.25 * double(done) / double(total), "Checking your game files");
                return !cancel.load();
            });
            out = h.Hex();
            return ok;
        };
        std::string h;
        if (!hashOf("default.xex", h)) return Fail(cancel ? "cancelled" : "cannot read default.xex");
        if (h != kXex10)
            return Fail("default.xex is not the US retail Xbox 360 NFL Head Coach 09 (it is needed as it shipped)");
        ui.log("default.xex: US retail, OK");
        for (const auto& a : kArchives)
        {
            if (!hashOf(a.name, h)) return Fail(cancel ? "cancelled" : std::string("cannot read ") + a.name);
            if (h != a.sha256) differ.push_back(a.name);
            ui.log(std::string(a.name) + (h == a.sha256 ? ": retail, OK" : ": differs from the retail file"));
        }
        if (!differ.empty())
        {
            std::string m = "These game archives differ from the US retail files:\n";
            for (auto& s : differ) m += "    " + s + "\n";
            m += "\nThe game may not work correctly with them. Continue anyway?";
            if (!ui.confirm(m)) return Fail("stopped: game archives differ from retail");
            ui.log("continuing with archives that differ from retail (the player's choice)");
        }
        bool bundled = tu.empty() && fs::exists(kit / kBundledTu);
        if (bundled) tu = kit / kBundledTu;
        bool v12 = !tu.empty();
        if (v12)
        {
            Sha256 th;
            std::ifstream in(tu, std::ios::binary);
            if (!in) return Fail("cannot read the title update " + Narrow(tu.wstring()));
            std::vector<char> b(1 << 20);
            while (in) { in.read(b.data(), std::streamsize(b.size())); if (in.gcount() > 0) th.Add(b.data(), size_t(in.gcount())); }
            std::string t = th.Hex();
            if (t != kTuPackage && t != kTuXexp)
                return Fail("the title update is not NFL Head Coach 09 Title Update #2 for Xbox 360 (the 1.2 update)");
            ui.log(std::string("title update: Title Update #2 (game version 1.2), OK") + (bundled ? " (included with the installer)" : ""));
        }

        // 2. Room on the disk.
        std::error_code ec;
        fs::create_directories(dest, ec);
        if (ec) return Fail("cannot create " + Narrow(dest.wstring()));
        logFile.open(dest / L"hc09-setup.log", std::ios::binary | std::ios::app);
        logFile << "\r\n==== HC09Setup ====\r\n";
        for (auto& l : early) logFile << l << "\r\n";
        early.clear();
        ULARGE_INTEGER freeBytes{};
        GetDiskFreeSpaceExW(dest.c_str(), &freeBytes, nullptr, nullptr);
        uint64_t kitBytes = 0;
        for (const auto& e : fs::recursive_directory_iterator(kit)) if (e.is_regular_file()) kitBytes += e.file_size();
        uint64_t need = kitBytes + (v12 ? 2 : 1) * kVersionBytes + kBuildBytes + (src.IsImage() ? total : 0);
        ui.log("space needed: " + Gb(need) + ", free on that drive: " + Gb(freeBytes.QuadPart));
        if (freeBytes.QuadPart < need)
            return Fail("not enough free space on the install drive: " + Gb(need) + " needed, " + Gb(freeBytes.QuadPart) + " free");

        // 3. The game files: from a disc image into <dest>\game; a folder stays where it is.
        fs::path root = source;
        if (src.IsImage())
        {
            root = dest / L"game";
            fs::create_directories(root);
            done = 0;
            std::vector<std::string> names{ "default.xex" };
            for (const auto& a : kArchives) names.push_back(a.name);
            for (const auto& n : names)
            {
                fs::path tmp = root / Widen(n + ".part");
                std::ofstream out(tmp, std::ios::binary);
                bool ok = src.Read(n, [&](const uint8_t* p, size_t k) {
                    out.write((const char*)p, std::streamsize(k));
                    done += k;
                    Step(0.25 + 0.15 * double(done) / double(total), "Copying the game files from the disc image");
                    return bool(out) && !cancel.load();
                });
                out.close();
                if (!ok) return Fail(cancel ? "cancelled" : "copying " + n + " failed");
                fs::rename(tmp, root / Widen(n), ec);
                if (ec) return Fail("cannot write " + n);
            }
            ui.log("game files copied to " + Narrow(root.wstring()));
        }
        if (v12)
        {
            fs::create_directories(dest / L"game" / L"title_update");
            fs::path keep = dest / L"game" / L"title_update" / tu.filename();
            if (fs::absolute(tu) != keep) fs::copy_file(tu, keep, fs::copy_options::overwrite_existing, ec);
            tu = keep;
        }

        // 4. The kit (kept: the launcher and a later run rebuild with it).
        Step(0.40, "Copying the build tools");
        if (fs::absolute(kit) != fs::absolute(dest / L"kit"))
        {
            fs::remove_all(dest / L"kit", ec);   // the new kit as it is: no files of an older one left over
            fs::copy(kit, dest / L"kit", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) return Fail("copying the kit failed: " + ec.message());
        }

        // 5. Build hc09.exe per version on this PC.
        std::vector<std::string> versions{ "1.0" };
        if (v12) versions.push_back("1.2");
        double span = 0.5 / versions.size(), at = 0.42;
        for (const auto& v : versions)
        {
            // Already built and still the tested build (a re-run, or an update of the runtime alone: hc09.exe does not
            // change with it): keep hc09.exe, refresh hc09rt.dll and the DLLs from the kit.
            std::string want = IniGet(dest / L"kit" / L"kit.ini", "", "hc09_" + v + "_sha256");
            if (!want.empty() && FileSha256(dest / Widen(v) / L"hc09.exe") == Upper(want))
            {
                fs::copy_file(dest / L"kit" / L"rt" / Widen(v) / L"hc09rt.dll", dest / Widen(v) / L"hc09rt.dll", fs::copy_options::overwrite_existing, ec);
                for (const auto& e : fs::directory_iterator(dest / L"kit" / L"redist"))
                    fs::copy_file(e.path(), dest / Widen(v) / e.path().filename(), fs::copy_options::overwrite_existing, ec);
                ui.log("game version " + v + ": hc09.exe already built and checked; runtime refreshed");
                at += span;
                continue;
            }
            std::vector<std::wstring> args{ (dest / L"kit" / L"hc09-build.exe").wstring(), L"--kit", (dest / L"kit").wstring(),
                                            L"--version", Widen(v), L"--xex", (root / L"default.xex").wstring(),
                                            L"--dest", (dest / Widen(v)).wstring(), L"--work", (dest / (L"build-work-" + Widen(v))).wstring() };
            if (v == "1.2") { args.push_back(L"--tu"); args.push_back(tu.wstring()); }
            std::string label = "Building game version " + v;
            if (!RunTool(args, [&](const std::string& line) {
                    int n, t;
                    unsigned c, ct;
                    if (sscanf(line.c_str(), "progress %d %d", &n, &t) == 2 && t > 0) Step(at + span * 0.15 * n / t, label);
                    else if (sscanf(line.c_str(), "compiled %u of %u", &c, &ct) == 2 && ct)
                        Step(at + span * (0.25 + 0.7 * c / ct), label + " (compiling)");
                    else ui.log("  " + line);
                }))
                return Fail(cancel ? "cancelled" : "building game version " + v + " failed (see the log)");
            at += span;
        }

        // 6. The launcher and its settings; the game images.
        Step(0.93, "Setting up the launcher");
        for (const wchar_t* exe : { L"hc09-launcher.exe", L"hc09-xexpatch.exe" })   // the launcher runs hc09-xexpatch (1.2)
        {
            fs::copy_file(dest / L"kit" / exe, dest / exe, fs::copy_options::overwrite_existing, ec);
            if (ec) return Fail("cannot install " + Narrow(exe));
        }
        // Steam Deck / AMD on Linux: the renderer's memory fix, loaded through Steam's launch options (README).
        if (fs::exists(dest / L"kit" / L"libhc09_userptr.so"))
            fs::copy_file(dest / L"kit" / L"libhc09_userptr.so", dest / L"libhc09_userptr.so", fs::copy_options::overwrite_existing, ec);
        // The player's documentation and the third-party licenses.
        for (const wchar_t* doc : { L"README.txt", L"THIRD-PARTY-NOTICES.txt" })
            fs::copy_file(dest / L"kit" / doc, dest / doc, fs::copy_options::overwrite_existing, ec);
        fs::copy(dest / L"kit" / L"licenses", dest / L"licenses", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        fs::path ini = dest / L"hc09.ini";
        IniSet(ini, "game", "root", Narrow(root.wstring()));
        if (v12) IniSet(ini, "game", "title_update", Narrow(tu.wstring()));
        if (IniGet(ini, "game", "saves").empty()) SavesToDocuments(ini);
        // First install: the launcher's defaults, so hc09.exe shows the game even when started without the launcher
        // (settings the player has chosen are never changed): 60 fps by default.
        for (auto [sec, key, val] : { std::tuple{ "display", "live", "on" }, std::tuple{ "display", "renderer", "native" },
                                   std::tuple{ "display", "framerate", "60" } })   // 60 fps by default (2026-10-23)
            if (IniGet(ini, sec, key).empty()) IniSet(ini, sec, key, val);
        for (const auto& v : versions)
        {
            IniSet(ini, "game", "version", v);
            if (!RunTool({ (dest / L"hc09-launcher.exe").wstring(), L"--prepare" }, [&](const std::string& l) { ui.log("  " + l); }))
                return Fail("making the game image for " + v + " failed (see the log)");
        }
        IniSet(ini, "game", "version", versions.back());   // 1.2 when there is one (the better one for careers)

        // 7. Shortcuts.
        if (shortcuts) { Shortcuts(); Register(); }
        Step(1.0, "Done");
        ui.log("Installed in " + Narrow(dest.wstring()) + ". Start the game with hc09-launcher.exe" +
               std::string(shortcuts ? " (shortcuts on the desktop and in the Start menu)." : "."));
        return true;
    }

    bool Fail(const std::string& m)
    {
        ui.log("FAILED: " + m);
        return false;
    }
    void Step(double f, const std::string& s) { ui.progress(std::clamp(f, 0.0, 1.0), s); }

    // The saves go to Documents\NFL Head Coach 09\Saves\1.0 and \1.2 (2026-10-23; hc09.ini [game] saves, read by
    // the game and the launcher). An older install's saves (<dest>\hc09-cache*\content\E000C0AC4E000009\00000001) are
    // copied there first; only when every copy worked is the setting written and the old content folder renamed to
    // content-moved-to-Documents (kept as a backup). Nothing in Documents is replaced. If something fails, the saves
    // stay where they were and the game keeps using them there.
    void SavesToDocuments(const fs::path& ini)
    {
        PWSTR docs = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)))
        {
            ui.log("saves: no Documents folder found; they stay in the install folder");
            return;
        }
        fs::path base = fs::path(docs) / L"NFL Head Coach 09" / L"Saves";
        CoTaskMemFree(docs);
        std::error_code ec;
        fs::create_directories(base, ec);
        if (ec) { ui.log("saves: cannot create " + Narrow(base.wstring()) + "; they stay in the install folder"); return; }
        std::vector<fs::path> moved;
        for (auto [cache, sub] : { std::pair{ L"hc09-cache", L"1.0" }, std::pair{ L"hc09-cache-v12", L"1.2" } })
        {
            fs::path old = dest / cache / L"content" / L"E000C0AC4E000009" / L"00000001", now = base / sub;
            if (!fs::is_directory(old) || fs::is_empty(old, ec)) continue;
            if (fs::is_directory(now) && !fs::is_empty(now, ec))
            {
                ui.log("saves of " + Narrow(sub) + ": " + Narrow(now.wstring()) + " already has saves; the ones in " +
                       Narrow(old.wstring()) + " are left there");
                continue;
            }
            fs::create_directories(now, ec);
            fs::copy(old, now, fs::copy_options::recursive, ec);
            if (ec)
            {
                ui.log("saves of " + Narrow(sub) + ": copying to Documents failed (" + ec.message() + "); they stay in the install folder");
                return;
            }
            moved.push_back(dest / cache);
            ui.log("saves of " + Narrow(sub) + " copied to " + Narrow(now.wstring()));
        }
        IniSet(ini, "game", "saves", Narrow(base.wstring()));
        for (const fs::path& c : moved)
        {
            fs::rename(c / L"content", c / L"content-moved-to-Documents", ec);
            if (!ec) ui.log("  the old copy is kept in " + Narrow((c / L"content-moved-to-Documents").wstring()));
        }
        ui.log("saves folder: " + Narrow(base.wstring()));
    }

    // A tool's output line by line; true when it exits with 0.
    bool RunTool(const std::vector<std::wstring>& args, const std::function<void(const std::string&)>& onLine)
    {
        std::wstring cmd;
        for (const auto& a : args) cmd += (cmd.empty() ? L"\"" : L" \"") + a + L"\"";
        SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
        HANDLE rd, wr;
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{ sizeof si };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 dest.c_str(), &si, &pi);
        CloseHandle(wr);
        if (!ok) { CloseHandle(rd); ui.log("cannot start " + Narrow(args[0])); return false; }
        std::string pending;
        char buf[4096];
        DWORD got;
        while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got)
        {
            pending.append(buf, got);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pending.erase(0, nl + 1);
                if (!line.empty()) onLine(line);
            }
            if (cancel) TerminateProcess(pi.hProcess, 1);
        }
        if (!pending.empty()) onLine(pending);
        CloseHandle(rd);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return code == 0;
    }

    // Windows' "Apps & features" entry (per user: no administrator rights), and setup kept in the kit to uninstall.
    void Register()
    {
        std::error_code ec;
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        fs::path keep = dest / L"kit" / L"HC09Setup.exe";
        if (fs::absolute(self) != keep) fs::copy_file(self, keep, fs::copy_options::overwrite_existing, ec);
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
        auto str = [&](const wchar_t* name, const std::wstring& v) {
            RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
        };
        auto dword = [&](const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v); };
        str(L"DisplayName", L"NFL Head Coach 09 for PC");
        str(L"Publisher", L"NFL Head Coach 09 for PC (unofficial fan project)");
        str(L"DisplayIcon", (dest / L"hc09-launcher.exe").wstring());
        str(L"InstallLocation", dest.wstring());
        str(L"UninstallString", L"\"" + keep.wstring() + L"\" --uninstall \"" + dest.wstring() + L"\"");
        dword(L"NoModify", 1);
        dword(L"NoRepair", 1);
        uint64_t bytes = 0;
        for (const auto& e : fs::recursive_directory_iterator(dest, ec)) if (e.is_regular_file(ec)) bytes += e.file_size(ec);
        dword(L"EstimatedSize", DWORD(bytes / 1024));
        RegCloseKey(k);
        ui.log("registered in Apps & features (uninstall from there)");
    }

    void Shortcuts()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (int where : { CSIDL_DESKTOPDIRECTORY, CSIDL_PROGRAMS })
        {
            wchar_t dir[MAX_PATH];
            if (FAILED(SHGetFolderPathW(nullptr, where, nullptr, 0, dir))) continue;
            IShellLinkW* link = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) continue;
            link->SetPath((dest / L"hc09-launcher.exe").c_str());
            link->SetWorkingDirectory(dest.c_str());
            link->SetDescription(L"NFL Head Coach 09 for PC");
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))))
            {
                file->Save((fs::path(dir) / kShortcutName).c_str(), TRUE);
                file->Release();
            }
            link->Release();
        }
        CoUninitialize();
        ui.log("shortcuts made (desktop, Start menu)");
    }
};

fs::path ExeDir()
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return fs::path(p).parent_path();
}

// ---- unattended (console) -----------------------------------------------------------------------------------------
int RunConsole(int argc, wchar_t** argv)
{
    Installer in;
    in.setupDir = ExeDir();
    bool yes = false;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--source") in.source = next();
        else if (a == L"--dest") in.dest = next();
        else if (a == L"--tu") in.tu = next();
        else if (a == L"--yes") yes = true;
        else if (a == L"--no-shortcuts") in.shortcuts = false;
    }
    if (in.source.empty() || in.dest.empty())
    {
        fprintf(stderr, "HC09Setup --source <disc image or game folder> --dest <install folder> [--tu <title update>] [--yes]\n");
        return 2;
    }
    std::string last;
    in.ui.log = [](const std::string& s) { printf("%s\n", s.c_str()); fflush(stdout); };
    in.ui.progress = [&](double f, const std::string& s) {
        static int lastPct = -1;
        int pct = int(f * 100);
        if (s != last || pct / 10 != lastPct / 10) { printf("[%3d%%] %s\n", pct, s.c_str()); fflush(stdout); last = s; lastPct = pct; }
    };
    in.ui.confirm = [&](const std::string& m) { printf("WARNING: %s\n%s\n", m.c_str(), yes ? "(--yes: continuing)" : "(stopping; --yes continues)"); return yes; };
    return in.Run() ? 0 : 1;
}

// ---- the window ---------------------------------------------------------------------------------------------------
enum : int { kSource = 100, kImageBtn, kFolderBtn, kTu, kTuBtn, kDest, kDestBtn, kInstall, kPlay, kClose, kBar, kStatus, kLog };
enum : UINT { kMsgLog = WM_APP + 1, kMsgProgress, kMsgDone, kMsgConfirm };

struct
{
    HWND wnd = nullptr;
    HFONT font = nullptr, bold = nullptr;
    int dpi = 96;
    std::thread worker;
    Installer* job = nullptr;
    std::string logText;
    int S(int v) const { return MulDiv(v, dpi, 96); }
    HWND Ctl(int id) const { return GetDlgItem(wnd, id); }
} g;

std::wstring GetText(int id)
{
    int n = GetWindowTextLengthW(g.Ctl(id));
    std::wstring s(size_t(n), L'\0');
    GetWindowTextW(g.Ctl(id), s.data(), n + 1);
    return s;
}

std::wstring Pick(bool folder, const wchar_t* title, const std::vector<COMDLG_FILTERSPEC>& filters)
{
    std::wstring r;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    d->SetTitle(title);
    if (!folder && !filters.empty()) d->SetFileTypes(UINT(filters.size()), filters.data());
    if (SUCCEEDED(d->Show(g.wnd)))
    {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it)))
        {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return r;
}

void UpdateSpace()
{
    std::wstring d = GetText(kDest);
    if (d.empty()) return;
    fs::path p = d;
    while (!p.empty() && !fs::exists(p)) p = p.parent_path();
    ULARGE_INTEGER f{};
    if (!p.empty() && GetDiskFreeSpaceExW(p.c_str(), &f, nullptr, nullptr))
        SetWindowTextW(g.Ctl(kStatus), Widen("Free on that drive: " + Gb(f.QuadPart) + ". Needed: ~1 GB from a game folder, ~6.5 GB from a disc image.").c_str());
}

void Busy(bool busy)
{
    for (int id : { kSource, kImageBtn, kFolderBtn, kTu, kTuBtn, kDest, kDestBtn, kInstall }) EnableWindow(g.Ctl(id), !busy);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case kImageBtn:
        {
            std::wstring p = Pick(false, L"Your NFL Head Coach 09 disc image", { { L"Disc images (*.iso)", L"*.iso;*.xiso" }, { L"All files", L"*.*" } });
            if (!p.empty()) SetWindowTextW(g.Ctl(kSource), p.c_str());
            return 0;
        }
        case kFolderBtn:
        {
            std::wstring p = Pick(true, L"The folder with your default.xex and qkl_*.ast files", {});
            if (!p.empty()) SetWindowTextW(g.Ctl(kSource), p.c_str());
            return 0;
        }
        case kTuBtn:
        {
            std::wstring p = Pick(false, L"Your title update (optional): the TU package or default.xexp", { { L"All files", L"*.*" } });
            if (!p.empty()) SetWindowTextW(g.Ctl(kTu), p.c_str());
            return 0;
        }
        case kDestBtn:
        {
            std::wstring p = Pick(true, L"Install folder", {});
            if (!p.empty()) { SetWindowTextW(g.Ctl(kDest), (fs::path(p) / L"NFL Head Coach 09").c_str()); UpdateSpace(); }
            return 0;
        }
        case kDest:
            if (HIWORD(wp) == EN_CHANGE) UpdateSpace();
            return 0;
        case kInstall:
        {
            if (GetText(kSource).empty()) { MessageBoxW(hwnd, L"Choose your disc image or game folder first.", L"HC09 Setup", MB_ICONINFORMATION); return 0; }
            if (g.worker.joinable()) g.worker.join();
            delete g.job;
            g.job = new Installer;
            g.job->setupDir = ExeDir();
            g.job->source = GetText(kSource);
            g.job->tu = GetText(kTu);
            g.job->dest = GetText(kDest);
            g.job->ui.log = [](const std::string& s) { PostMessageW(g.wnd, kMsgLog, 0, LPARAM(new std::string(s))); };
            g.job->ui.progress = [](double f, const std::string& s) { PostMessageW(g.wnd, kMsgProgress, WPARAM(f * 1000), LPARAM(new std::string(s))); };
            g.job->ui.confirm = [](const std::string& m) { return SendMessageW(g.wnd, kMsgConfirm, 0, LPARAM(&m)) == IDYES; };
            Busy(true);
            EnableWindow(g.Ctl(kPlay), FALSE);
            SetWindowTextW(g.Ctl(kLog), L"");
            g.logText.clear();
            g.worker = std::thread([] { bool ok = g.job->Run(); PostMessageW(g.wnd, kMsgDone, ok, 0); });
            return 0;
        }
        case kPlay:
        {
            fs::path launcher = fs::path(GetText(kDest)) / L"hc09-launcher.exe";
            ShellExecuteW(hwnd, L"open", launcher.c_str(), nullptr, launcher.parent_path().c_str(), SW_SHOWNORMAL);
            return 0;
        }
        case kClose:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case kMsgLog:
    {
        std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(lp));
        g.logText += *s + "\r\n";
        SetWindowTextW(g.Ctl(kLog), Widen(g.logText).c_str());
        SendMessageW(g.Ctl(kLog), EM_SETSEL, g.logText.size(), g.logText.size());
        SendMessageW(g.Ctl(kLog), EM_SCROLLCARET, 0, 0);
        return 0;
    }
    case kMsgProgress:
    {
        std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(lp));
        SendMessageW(g.Ctl(kBar), PBM_SETPOS, wp, 0);
        SetWindowTextW(g.Ctl(kStatus), Widen(*s).c_str());
        return 0;
    }
    case kMsgConfirm:
        return MessageBoxW(hwnd, Widen(*reinterpret_cast<const std::string*>(lp)).c_str(), L"HC09 Setup",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    case kMsgDone:
        Busy(false);
        EnableWindow(g.Ctl(kPlay), wp != 0);
        SetWindowTextW(g.Ctl(kStatus), wp ? L"Installed. Press Play to open the launcher." : L"Not installed: see the log.");
        if (!wp) SendMessageW(g.Ctl(kBar), PBM_SETPOS, 0, 0);
        return 0;
    case WM_CLOSE:
        if (g.worker.joinable())
        {
            if (MessageBoxW(hwnd, L"Stop the installation?", L"HC09 Setup", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            g.job->cancel = true;
            g.worker.join();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC:   // labels on the window's own background
        SetBkMode(HDC(wp), TRANSPARENT);
        return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             g.S(x), g.S(y), g.S(w), g.S(h), g.wnd, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(f ? f : g.font), TRUE);
    return c;
}

int RunWindow(HINSTANCE inst)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    g.dpi = int(GetDpiForSystem());
    NONCLIENTMETRICSW m{ sizeof m };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0);
    m.lfMessageFont.lfHeight = -g.S(15);
    g.font = CreateFontIndirectW(&m.lfMessageFont);
    m.lfMessageFont.lfWeight = FW_BOLD;
    m.lfMessageFont.lfHeight = -g.S(20);
    g.bold = CreateFontIndirectW(&m.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"HC09Setup";
    RegisterClassW(&wc);
    RECT r{ 0, 0, g.S(640), g.S(560) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g.wnd = CreateWindowW(wc.lpszClassName, L"NFL Head Coach 09 for PC - Setup", style, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);

    Add(L"STATIC", L"NFL Head Coach 09 for PC", 0, 20, 14, 600, 28, 0, g.bold);
    Add(L"STATIC", L"Builds the game on this PC from your own Xbox 360 copy (US). Choose the folder of your dump: "
                   L"default.xex and the five qkl_*.ast files.", 0, 20, 46, 600, 40, 0);
    Add(L"STATIC", L"Your game (the folder of your dump; a disc image is in beta):", 0, 20, 96, 600, 20, 0);
    Add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 20, 118, 372, 26, kSource);
    Add(L"BUTTON", L"Folder...", WS_TABSTOP, 400, 118, 92, 26, kFolderBtn);
    Add(L"BUTTON", L"ISO (beta)...", WS_TABSTOP, 498, 118, 122, 26, kImageBtn);
    Add(L"STATIC", fs::exists(ExeDir() / L"kit" / kBundledTu)
                       ? L"Title update: included, game version 1.2 is installed beside 1.0 (or choose your own copy)"
                       : L"Title update (optional; adds game version 1.2): the TU file from your console's drive", 0, 20, 154, 600, 20, 0);
    Add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 20, 176, 500, 26, kTu);
    Add(L"BUTTON", L"Browse...", WS_TABSTOP, 526, 176, 94, 26, kTuBtn);
    Add(L"STATIC", L"Install to:", 0, 20, 212, 600, 20, 0);
    wchar_t local[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    Add(L"EDIT", (fs::path(local) / L"Programs" / L"NFL Head Coach 09").c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 20, 234, 500, 26, kDest);
    Add(L"BUTTON", L"Browse...", WS_TABSTOP, 526, 234, 94, 26, kDestBtn);
    Add(PROGRESS_CLASSW, L"", 0, 20, 276, 600, 20, kBar);
    SendMessageW(g.Ctl(kBar), PBM_SETRANGE32, 0, 1000);
    Add(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 20, 300, 600, 20, kStatus);
    Add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 20, 326, 600, 180, kLog);
    Add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 320, 518, 96, 30, kInstall);
    Add(L"BUTTON", L"Play", WS_TABSTOP | WS_DISABLED, 422, 518, 96, 30, kPlay);
    Add(L"BUTTON", L"Close", WS_TABSTOP, 524, 518, 96, 30, kClose);
    UpdateSpace();
    ShowWindow(g.wnd, SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(g.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}

// ---- uninstalling (Apps & features runs "<install>\kit\HC09Setup.exe --uninstall <install>") ----------------------------
// It runs from a copy in %TEMP% (the install folder holds the running setup), asks whether to keep the saves, then
// removes the install folder, the shortcuts and the registration. Saves kept: the Documents folder ([game] saves) is
// left alone (an older install: its hc09-cache* folders stay); deleted: its 1.0, 1.2 and Backups folders go too.
//   --keep-saves / --delete-saves: no question (unattended).
int Uninstall(int argc, wchar_t** argv)
{
    fs::path dest;
    int saves = 0;   // 0 ask, 1 keep, 2 delete
    bool fromTemp = false;
    DWORD parent = 0;   // the copy in the kit that started this one: it must have exited before its folder goes
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--uninstall" && i + 1 < argc) dest = argv[++i];
        else if (a == L"--keep-saves") saves = 1;
        else if (a == L"--delete-saves") saves = 2;
        else if (a == L"--from-temp") fromTemp = true;
        else if (a == L"--parent" && i + 1 < argc) parent = DWORD(_wtoi(argv[++i]));
    }
    if (dest.empty() || !fs::exists(dest / L"hc09-launcher.exe")) { MessageBoxW(nullptr, L"Not an NFL Head Coach 09 for PC install folder.", L"HC09", MB_ICONERROR); return 1; }
    if (!fromTemp)
    {
        wchar_t self[MAX_PATH], tmp[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        GetTempPathW(MAX_PATH, tmp);
        fs::path copy = fs::path(tmp) / L"hc09-uninstall.exe";
        std::error_code ec;
        fs::copy_file(self, copy, fs::copy_options::overwrite_existing, ec);
        std::wstring cmd = L"\"" + copy.wstring() + L"\" --uninstall \"" + dest.wstring() + L"\" --from-temp --parent " +
                           std::to_wstring(GetCurrentProcessId()) + (saves == 1 ? L" --keep-saves" : saves == 2 ? L" --delete-saves" : L"");
        STARTUPINFOW si{ sizeof si };
        PROCESS_INFORMATION pi{};
        if (ec || !CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, tmp, &si, &pi)) return 1;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 0;   // at once: this exe is in the folder to remove
    }
    if (parent)
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, parent)) { WaitForSingleObject(h, 30000); CloseHandle(h); }
    // The saves: in Documents (hc09.ini [game] saves, set by setup) or, in an older install, in the hc09-cache folders.
    fs::path savesDir = Widen(IniGet(dest / L"hc09.ini", "game", "saves"));
    std::wstring where = savesDir.empty() ? L"in the hc09-cache folders in " + dest.wstring() : L"in " + savesDir.wstring();
    if (!saves)
    {
        TASKDIALOG_BUTTON buttons[] = { { 100, L"Uninstall, keep my saves" }, { 101, L"Uninstall and delete my saves too" } };
        std::wstring text = L"Remove NFL Head Coach 09 for PC from " + dest.wstring() + L"?\n\nYour careers, rosters and profile are " +
                            where + L". Keep them to install again later.";
        TASKDIALOGCONFIG c{ sizeof c };
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = L"HC09 Setup";
        c.pszMainInstruction = L"Uninstall NFL Head Coach 09 for PC";
        c.pszContent = text.c_str();
        c.cButtons = 2;
        c.pButtons = buttons;
        int pressed = IDCANCEL;
        TaskDialogIndirect(&c, &pressed, nullptr, nullptr);
        if (pressed != 100 && pressed != 101) return 1;
        saves = pressed == 100 ? 1 : 2;
    }
    std::error_code ec;
    std::vector<std::wstring> left;
    for (const auto& e : fs::directory_iterator(dest, ec))
    {
        std::wstring n = e.path().filename().wstring();
        std::wstring low = n;
        for (wchar_t& ch : low) ch = wchar_t(towlower(ch));
        if (saves == 1 && savesDir.empty() && low.rfind(L"hc09-cache", 0) == 0) continue;
        fs::remove_all(e.path(), ec);
        if (ec) left.push_back(n);
    }
    if (saves == 2 || !savesDir.empty()) fs::remove(dest, ec);
    if (saves == 2 && !savesDir.empty())
    {
        // Only what the game and launcher make there; the folders above go only when empty.
        for (const wchar_t* sub : { L"1.0", L"1.2", L"Backups" }) fs::remove_all(savesDir / sub, ec);
        fs::remove(savesDir, ec);
        fs::remove(savesDir.parent_path(), ec);
    }
    for (int where : { CSIDL_DESKTOPDIRECTORY, CSIDL_PROGRAMS })
    {
        wchar_t dir[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, where, nullptr, 0, dir))) fs::remove(fs::path(dir) / kShortcutName, ec);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    std::wstring done = L"NFL Head Coach 09 for PC was removed.";
    if (saves == 1) done += L"\n\nYour saves are still " + where + L".";
    if (!left.empty()) done += L"\n\nSome files were in use and stay: close the game and delete " + dest.wstring() + L" by hand.";
    bool quiet = false;
    for (int i = 1; i < argc; ++i) quiet = quiet || !wcscmp(argv[i], L"--keep-saves") || !wcscmp(argv[i], L"--delete-saves");
    if (!quiet) MessageBoxW(nullptr, done.c_str(), L"HC09 Setup", MB_ICONINFORMATION);
    return left.empty() ? 0 : 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--uninstall")) return Uninstall(argc, argv);
    if (argc > 1)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
        {
            FILE* f;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
        }
        return RunConsole(argc, argv);
    }
    return RunWindow(inst);
}
