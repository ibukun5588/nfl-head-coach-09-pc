// hc09-build.exe: builds one game version's hc09.exe on the player's PC from their own default.xex (and, for 1.2,
// their Xbox 360 title update), with the kit we ship and nothing else installed: no Visual Studio, no Windows SDK,
// no Python (docs/HANDOFF_M11.md steps 2-3, the split build: runtime/split_abi.h).
//
//   hc09-build --kit <kit dir> --version 1.0|1.2 --xex <the disc's default.xex> [--tu <title update>]
//              --dest <folder for hc09.exe> [--work <temp folder>] [--jobs N] [--keep] [--patches <writes file>]
//
// --patches: the player's Xenia patches (the launcher's patch manager writes them as "<hex address> <hex bytes>"
// lines) are applied before the recompile (XenonRecomp patch_writes_file_path; code caves become part of the function
// that jumps into them). Such a build is not the tested one, so it is not compared with kit.ini.
//
// Steps: check the xex (and the update) against the retail hashes; 1.2: apply the update (kit\hc09-xexpatch.exe);
// recompile it to C++ (kit\XenonRecomp.exe, kit\config\<version>); compile that and runtime/split_glue.cpp with the
// kit's clang-cl against the kit's headers only; link with lld-link /NODEFAULTLIB against the kit's hc09rt.lib and
// ucrtbase.lib; check the result against the hash the kit publishes for it (the build is reproducible); copy hc09.exe,
// hc09rt.dll and the DLLs beside it into --dest. The generated C++ and the objects (~500 MB) are deleted afterwards
// unless --keep. Output: readable lines, and "progress <done> <total> <step>" lines for the installer's window.
// Exit code 0 = hc09.exe built and checked.
//
// The kit (tools/make_kit.py): toolchain\bin\{clang-cl,lld-link}.exe + toolchain\lib\clang\<n>\include, include\ (our C
// library headers), simde\, src\ (guest_indirect.h, ppc_context.h, split_glue.cpp, split_abi.h, split_hooks.inc,
// game_addr_v12.h), XenonRecomp.exe, hc09-xexpatch.exe, lib\ucrtbase.lib, config\<v>\{hc09.toml,jt_final.toml},
// rt\<v>\{hc09rt.dll,hc09rt.lib}, redist\*.dll, kit.ini (expected hc09.exe hash per version).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Retail files (docs/HANDOFF_RELEASE.md 1.1a): hashes only, never the files.
constexpr const char* kXex10 = "24CFB63D2A8924FCC9B7AEE162B0ADCAC4140C05312876CBD3AB5107CB07243F";
constexpr const char* kXex12 = "9EC0B8740D0B73753BE3218CCB146686476DCFB9C23801510739ADDB58801AF8";
constexpr const char* kTuPackage = "43B31C2B51E9797C6CAF02377D84203E963B977504B30B175382A4D1E17C44F2";
constexpr const char* kTuXexp = "A789D7238A58A11F71E8012345B26EC9B1F5B7B25705A6F036424EF8F0E5C043";

FILE* g_log = nullptr;

void Say(const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
}

[[noreturn]] void Fail(const std::string& msg)
{
    Say("FAILED: %s", msg.c_str());
    exit(1);
}

std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Sha256(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    std::vector<char> buf(1 << 20);
    while (in)
    {
        in.read(buf.data(), std::streamsize(buf.size()));
        if (in.gcount() > 0) BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), ULONG(in.gcount()), 0);
    }
    UCHAR d[32];
    BCryptFinishHash(h, d, 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string s;
    char x[3];
    for (UCHAR c : d) { snprintf(x, sizeof x, "%02X", c); s += x; }
    return s;
}

std::wstring Quote(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0; i < a.size(); ++i)
    {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++bs; ++i; }
        if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; }
        else { q.append(bs, L'\\'); q += a[i]; }
    }
    return q + L"\"";
}

// A child process with its output in `logFile`; no window, an environment without INCLUDE/LIB/CL (so a Visual Studio
// on this PC cannot slip into the build).
struct Proc
{
    HANDLE process = nullptr;
    fs::path logFile;
    std::string what;
};

std::vector<wchar_t> CleanEnv()
{
    std::vector<wchar_t> env;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
    {
        std::wstring v = p, k = v.substr(0, v.find(L'='));
        std::transform(k.begin(), k.end(), k.begin(), ::towupper);
        if (k == L"INCLUDE" || k == L"LIB" || k == L"CL" || k == L"_CL_" || k == L"LINK" || k == L"_LINK_") continue;
        env.insert(env.end(), v.begin(), v.end());
        env.push_back(0);
    }
    env.push_back(0);
    FreeEnvironmentStringsW(block);
    return env;
}

Proc Start(const std::vector<std::wstring>& args, const fs::path& logFile, const std::string& what, const fs::path& cwd = {})
{
    static std::vector<wchar_t> env = CleanEnv();
    std::wstring cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + Quote(a);
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE out = CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(out);
    if (!ok) Fail("cannot start " + Narrow(args[0]));
    CloseHandle(pi.hThread);
    return { pi.hProcess, logFile, what };
}

DWORD Finish(Proc& p)
{
    WaitForSingleObject(p.process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p.process, &code);
    CloseHandle(p.process);
    return code;
}

std::string ReadText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

DWORD RunOne(const std::vector<std::wstring>& args, const fs::path& logFile, const std::string& what, const fs::path& cwd = {})
{
    Proc p = Start(args, logFile, what, cwd);
    DWORD code = Finish(p);
    if (code) Say("%s failed (exit %lu):\n%s", what.c_str(), code, ReadText(logFile).c_str());
    return code;
}

std::string KitValue(const fs::path& ini, const std::string& key)
{
    std::ifstream in(ini);
    std::string line;
    while (std::getline(in, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return {};
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    fs::path kit, xex, tu, dest, work, patches;
    std::wstring version;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    bool keep = false;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { if (i + 1 >= argc) Fail("missing value after " + Narrow(a)); return argv[++i]; };
        if (a == L"--kit") kit = next();
        else if (a == L"--version") version = next();
        else if (a == L"--xex") xex = next();
        else if (a == L"--tu") tu = next();
        else if (a == L"--dest") dest = next();
        else if (a == L"--work") work = next();
        else if (a == L"--jobs") jobs = std::max(1, _wtoi(next().c_str()));
        else if (a == L"--keep") keep = true;
        else if (a == L"--patches") patches = next();
        else Fail("unknown argument " + Narrow(a));
    }
    if (kit.empty() || xex.empty() || dest.empty() || (version != L"1.0" && version != L"1.2"))
    {
        fputs("hc09-build --kit <kit> --version 1.0|1.2 --xex <default.xex> [--tu <title update>] --dest <folder> "
              "[--work <folder>] [--jobs N] [--keep] [--patches <writes file>]\n", stderr);
        return 2;
    }
    const bool v12 = version == L"1.2";
    const std::string ver = Narrow(version);
    kit = fs::absolute(kit);
    dest = fs::absolute(dest);
    if (work.empty()) work = dest / L"build-work";
    work = fs::absolute(work);
    fs::create_directories(work);
    fs::create_directories(dest);
    g_log = _wfopen((work / L"build.log").c_str(), L"w");
    Say("hc09-build: game version %s, %u parallel jobs, work folder %s", ver.c_str(), jobs, Narrow(work.wstring()).c_str());
    const int kSteps = 6;
    Say("progress 0 %d checking your files", kSteps);

    // 1. The player's files.
    std::string h = Sha256(xex);
    if (h != kXex10)
        Fail("default.xex is not the US retail NFL Head Coach 09 (1.0) for Xbox 360 (SHA-256 " + (h.empty() ? "unreadable" : h) + ")");
    fs::path srcXex = xex;
    if (v12)
    {
        if (tu.empty()) Fail("game version 1.2 needs your title update (--tu)");
        std::string th = Sha256(tu);
        if (th != kTuPackage && th != kTuXexp)
            Fail("the title update is not NFL Head Coach 09 Title Update #2 (SHA-256 " + (th.empty() ? "unreadable" : th) + ")");
        srcXex = work / L"default-v12.xex";
        Say("progress 1 %d applying the title update", kSteps);
        if (RunOne({ (kit / L"hc09-xexpatch.exe").wstring(), xex.wstring(), tu.wstring(), srcXex.wstring() },
                   work / L"xexpatch.log", "applying the title update"))
            Fail("the title update could not be applied");
        if (Sha256(srcXex) != kXex12) Fail("the patched default.xex is not the expected 1.2");
    }

    // 2. Recompile to C++ (XenonRecomp, our configs: addresses only).
    Say("progress 2 %d translating the game's code to C++", kSteps);
    fs::path out = work / L"out", cfgDir = kit / L"config" / version;
    fs::remove_all(out);
    fs::create_directories(out);
    fs::copy_file(cfgDir / L"jt_final.toml", work / L"jt_final.toml", fs::copy_options::overwrite_existing);
    std::string cfg = ReadText(cfgDir / L"hc09.toml");
    auto setKey = [&](const char* key, const fs::path& value) {   // the first line "key = ..." gets the path
        const std::string prefix = std::string(key) + " = ";
        for (size_t at = 0; at < cfg.size();)
        {
            size_t end = cfg.find('\n', at);
            if (end == std::string::npos) end = cfg.size();
            if (cfg.compare(at, prefix.size(), prefix) == 0)
            {
                size_t stop = end > at && cfg[end - 1] == '\r' ? end - 1 : end;
                cfg.replace(at, stop - at, prefix + "\"" + value.generic_string() + "\"");
                return;
            }
            at = end + 1;
        }
        Fail(std::string("the config has no ") + key);
    };
    // XenonRecomp takes narrow (ANSI) paths: it gets only names relative to the work folder it runs in, so the game
    // folder and the install folder (the user's name in it) may hold any characters. The 1.0 xex and ppc_context.h are
    // copied in for that (1.2's patched xex is made there already).
    if (srcXex != work / L"default-v12.xex")
    {
        fs::copy_file(srcXex, work / L"default.xex", fs::copy_options::overwrite_existing);
        srcXex = work / L"default.xex";
    }
    fs::copy_file(kit / L"src" / L"ppc_context.h", work / L"ppc_context.h", fs::copy_options::overwrite_existing);
    setKey("file_path", srcXex.filename());
    setKey("out_directory_path", out.filename());
    setKey("switch_table_file_path", L"jt_final.toml");
    if (!patches.empty())
    {
        fs::copy_file(patches, work / L"patch_writes.txt", fs::copy_options::overwrite_existing);
        size_t at = cfg.find("[main]");
        if (at == std::string::npos) Fail("the config has no [main] section");
        cfg.insert(at + 6, "\npatch_writes_file_path = \"patch_writes.txt\"");
        Say("with your patches (%s)", Narrow(patches.filename().wstring()).c_str());
    }
    { std::ofstream(work / L"hc09.toml", std::ios::binary) << cfg; }
    DWORD t0 = GetTickCount();
    // Run in the work folder with the config named relative to it: XenonRecomp joins the config's folder to the paths
    // in it, so they stay relative to the work folder.
    if (RunOne({ (kit / L"XenonRecomp.exe").wstring(), L"hc09.toml", L"ppc_context.h" },
               work / L"recomp.log", "XenonRecomp", work))
        Fail("the recompiler failed (see recomp.log)");
    std::vector<fs::path> sources;
    for (const auto& e : fs::directory_iterator(out))
    {
        std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"ppc_recomp.", 0) == 0 && e.path().extension() == L".cpp") sources.push_back(e.path());
    }
    // The link order decides the exe's bytes (kit.ini's hash): the recompiled files by name, then the function table,
    // then the glue (as tools/player_build.py).
    std::sort(sources.begin(), sources.end());
    sources.push_back(out / L"ppc_func_mapping.cpp");
    if (sources.size() < 300) Fail("the recompiler wrote only " + std::to_string(sources.size()) + " files");
    Say("recompiled to %zu C++ files in %.0f s", sources.size(), (GetTickCount() - t0) / 1000.0);

    // 3. Compile: the recompiled code and the glue, with the kit's clang-cl and headers only (/X: no system headers).
    Say("progress 3 %d compiling (%zu files)", kSteps, sources.size() + 1);
    fs::path obj = work / L"obj", logs = work / L"logs";
    fs::create_directories(obj);
    fs::create_directories(logs);
    const std::wstring clang = (kit / L"toolchain" / L"bin" / L"clang-cl.exe").wstring();
    const std::vector<std::wstring> inc = { L"-I" + (kit / L"include").wstring(), L"-I" + out.wstring(),
                                            L"-I" + (kit / L"src").wstring(), L"-I" + (kit / L"simde").wstring() };
    struct Job { std::vector<std::wstring> args; std::string name; fs::path obj; };
    std::vector<Job> todo;
    for (const auto& s : sources)
    {
        Job j{ { clang, L"/c", L"/X", L"/DWIN32", L"/D_WINDOWS", L"/EHsc", L"/O2", L"/Ob2", L"/DNDEBUG", L"-std:c++20", L"-MD",
                 L"/clang:-fno-strict-aliasing", L"/w", L"/clang:-march=x86-64-v3", L"/clang:-ffp-contract=off",
                 L"-DHC09_GUEST_TU", L"/FI" + (kit / L"src" / L"guest_indirect.h").wstring() },
               Narrow(s.filename().wstring()), obj / (s.stem().wstring() + L".obj") };
        j.args.insert(j.args.end(), inc.begin(), inc.end());
        j.args.push_back(s.wstring());
        j.args.push_back(L"/Fo" + j.obj.wstring());
        todo.push_back(std::move(j));
    }
    {
        Job g{ { clang, L"/c", L"/X", L"/O2", L"-std:c++20", L"-MD", L"/clang:-fno-strict-aliasing", L"/w" },
               "split_glue.cpp", obj / L"split_glue.obj" };
        if (v12) g.args.push_back(L"/FI" + (kit / L"src" / L"game_addr_v12.h").wstring());
        g.args.insert(g.args.end(), inc.begin(), inc.end());
        g.args.push_back((kit / L"src" / L"split_glue.cpp").wstring());
        g.args.push_back(L"/Fo" + g.obj.wstring());
        todo.push_back(std::move(g));
    }
    t0 = GetTickCount();
    size_t next = 0, done = 0, failed = 0;
    std::vector<Proc> running;
    while (done < todo.size())
    {
        while (running.size() < jobs && next < todo.size())
        {
            const Job& j = todo[next++];
            running.push_back(Start(j.args, logs / (j.name + ".log"), j.name));
        }
        std::vector<HANDLE> hs;
        for (auto& p : running) hs.push_back(p.process);
        DWORD w = WaitForMultipleObjects(DWORD(hs.size()), hs.data(), FALSE, INFINITE);
        size_t k = w - WAIT_OBJECT_0;
        if (k >= running.size()) Fail("waiting for the compiler failed");
        Proc p = running[k];
        running.erase(running.begin() + ptrdiff_t(k));
        if (Finish(p)) { ++failed; Say("compiling %s failed:\n%s", p.what.c_str(), ReadText(p.logFile).c_str()); }
        ++done;
        if (done % 10 == 0 || done == todo.size()) Say("compiled %zu of %zu", done, todo.size());
    }
    if (failed) Fail(std::to_string(failed) + " file(s) did not compile (see the logs folder)");
    Say("compiled in %.0f s", (GetTickCount() - t0) / 1000.0);

    // 4. Link: no MSVC library; the runtime's import library and Windows' ucrtbase.dll only. /Brepro: reproducible.
    Say("progress 4 %d linking", kSteps);
    fs::path rsp = work / L"link.rsp";
    {
        // UTF-8, as LLVM reads response files: the install folder's path may hold any characters (the user's name).
        std::ofstream r(rsp, std::ios::binary);
        for (const auto& j : todo) r << Narrow(Quote(j.obj.wstring())) << "\n";
    }
    fs::path exe = work / L"hc09.exe";
    if (RunOne({ (kit / L"toolchain" / L"bin" / L"lld-link.exe").wstring(), L"/OUT:" + exe.wstring(), L"/ENTRY:Hc09Entry",
                 L"/SUBSYSTEM:CONSOLE", L"/STACK:8388608", L"/MACHINE:X64", L"/NODEFAULTLIB", L"/INCREMENTAL:NO", L"/Brepro",
                 L"@" + rsp.wstring(), (kit / L"rt" / version / L"hc09rt.lib").wstring(), (kit / L"lib" / L"ucrtbase.lib").wstring() },
               work / L"link.log", "linking"))
        Fail("linking failed (see link.log)");

    // 5. The result must be the hc09.exe the kit was tested with.
    Say("progress 5 %d checking the result", kSteps);
    std::string got = Sha256(exe), want = KitValue(kit / L"kit.ini", "hc09_" + ver + "_sha256");
    std::transform(want.begin(), want.end(), want.begin(), ::toupper);
    if (!patches.empty()) Say("patched build (not compared with the tested build): hc09.exe is %s", got.c_str());
    else if (want.empty()) Say("note: the kit lists no hash for %s; hc09.exe is %s", ver.c_str(), got.c_str());
    else if (got != want) Fail("hc09.exe differs from the tested build (" + got + ", expected " + want + ")");
    else Say("hc09.exe matches the tested build");

    // 6. Into place: hc09.exe beside the runtime and its DLLs.
    Say("progress 6 %d installing", kSteps);
    fs::copy_file(exe, dest / L"hc09.exe", fs::copy_options::overwrite_existing);
    fs::copy_file(kit / L"rt" / version / L"hc09rt.dll", dest / L"hc09rt.dll", fs::copy_options::overwrite_existing);
    if (fs::exists(kit / L"redist"))
        for (const auto& e : fs::directory_iterator(kit / L"redist"))
            fs::copy_file(e.path(), dest / e.path().filename(), fs::copy_options::overwrite_existing);
    if (!keep)
    {
        if (g_log) { fclose(g_log); g_log = nullptr; }
        fs::copy_file(work / L"build.log", dest / L"hc09-build.log", fs::copy_options::overwrite_existing);
        std::error_code ec;
        if (!patches.empty())   // which code caves the recompiler found: what to look at when a patch misbehaves
            fs::copy_file(work / L"recomp.log", dest / L"hc09-recomp.log", fs::copy_options::overwrite_existing, ec);
        fs::remove_all(work, ec);
    }
    Say("done: %s", Narrow((dest / L"hc09.exe").wstring()).c_str());
    return 0;
}
