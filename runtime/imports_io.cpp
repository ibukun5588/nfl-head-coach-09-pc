// Virtual filesystem: maps the game's Xbox paths onto a host directory and
// implements the NtCreateFile/NtOpenFile/NtReadFile/... family.
//
// Guest paths look like "game:\dir\file.ext", "d:\file", "\Device\Cdrom0\..."
// or "\??\game:\...". Everything after the device/drive prefix is resolved
// case-insensitively under g_vfsRoot (Xbox filesystems are case-insensitive,
// Linux is not). With no root set, every open fails with
// STATUS_OBJECT_NAME_NOT_FOUND — and is logged, which tells us what the game
// wants.
#include "dev_env.h"
#include "kernel.h"
#include "gpu.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

std::string g_vfsRoot;   // set from main(); empty = no files available
std::string g_vfsCache;  // writable scratch volume for cache:\ and the HDD device

namespace {

std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }

// Which host volume a guest path maps to.
enum class Vol { Disc, Cache, Mount, None };

// Content mounts (XamContentCreateEx): drive name (lowercase, no colon) -> host dir.
std::mutex g_mountMu;
std::map<std::string, std::string> g_mounts;
thread_local std::string t_mountRoot;   // root of the last Vol::Mount StripDevice result

// Strip the Xbox device/drive prefix, normalize separators, classify volume.
std::string StripDevice(const std::string& in, Vol* vol)
{
    std::string p = in;
    auto starts = [&](const char* pre) {
        return Lower(p.substr(0, strlen(pre))) == Lower(pre);
    };
    if (starts("\\??\\")) p = p.substr(4);
    Vol v = Vol::Disc;
    {
        size_t c = p.find(':');
        if (c != std::string::npos && c > 0 && c < 16)
        {
            std::lock_guard<std::mutex> lk(g_mountMu);
            auto it = g_mounts.find(Lower(p.substr(0, c)));
            if (it != g_mounts.end())
            {
                t_mountRoot = it->second;
                p = p.substr(c + 1);
                std::replace(p.begin(), p.end(), '\\', '/');
                while (!p.empty() && p.front() == '/') p.erase(p.begin());
                if (vol) *vol = Vol::Mount;
                return p;
            }
        }
    }
    if (starts("\\Device\\Cdrom0")) { p = p.substr(14); }
    else if (starts("\\Device\\Harddisk0\\partition1")) { p = p.substr(28); v = Vol::Cache; }
    else if (starts("\\Device\\Harddisk0\\partition0")) { p = p.substr(28); v = Vol::Cache; }
    else if (starts("cache:")) { p = p.substr(6); v = Vol::Cache; }
    else if (starts("hdd:"))   { p = p.substr(4); v = Vol::Cache; }
    else if (p.size() >= 2 && p[1] == ':') p = p.substr(2);           // game:, d:, e:, tib:
    else
    {
        size_t c = p.find(':');
        if (c != std::string::npos && c < 8) p = p.substr(c + 1);       // longer drive names
    }
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    if (vol) *vol = v;
    return p;
}

const std::string& RootFor(Vol v)
{
    return v == Vol::Cache ? g_vfsCache : v == Vol::Mount ? t_mountRoot : g_vfsRoot;
}

// Case-insensitive resolution of rel under root. Returns "" if not found.
std::string ResolveHost(const std::string& root, const std::string& rel, bool* isDir)
{
    if (root.empty()) return {};
    std::string cur = root;
    size_t pos = 0;
    while (pos <= rel.size())
    {
        size_t nxt = rel.find('/', pos);
        std::string comp = rel.substr(pos, nxt == std::string::npos ? std::string::npos : nxt - pos);
        // FATX/STFS have no "." or ".." entries: "cache:\Replays\." does not
        // exist on the console (Linux's readdir would match them).
        if (comp == "." || comp == "..") return {};
        if (!comp.empty())
        {
            std::error_code ec;
            fs::directory_iterator it(cur, ec), end;
            if (ec) return {};
            std::string match;
            std::string lc = Lower(comp);
            for (; it != end; it.increment(ec))
            {
                std::string n = it->path().filename().string();
                if (Lower(n) == lc) { match = n; break; }
            }
            if (match.empty()) return {};
            cur += "/" + match;
        }
        if (nxt == std::string::npos) break;
        pos = nxt + 1;
    }
    std::error_code ec;
    fs::file_status st = fs::status(cur, ec);
    if (ec || !fs::exists(st)) return {};
    if (isDir) *isDir = fs::is_directory(st);
    return cur;
}

// IO_STATUS_BLOCK { NTSTATUS Status; ULONG_PTR Information; }
void WriteIosb(uint32_t p, uint32_t status, uint32_t info)
{
    if (!p) return;
    GStore32(p + 0, status);
    GStore32(p + 4, info);
}

// 64-bit file positioning and truncation (fseeko/ftruncate on POSIX).
int FileSeek(FILE* fp, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(fp, int64_t(off), SEEK_SET);
#else
    return fseeko(fp, off_t(off), SEEK_SET);
#endif
}

bool FileTruncate(FILE* fp, uint64_t size)
{
#ifdef _WIN32
    return _chsize_s(_fileno(fp), int64_t(size)) == 0;
#else
    return ftruncate(fileno(fp), off_t(size)) == 0;
#endif
}

uint32_t OpenCommon(const char* api, PPCContext& ctx, uint32_t pHandle, uint32_t pObjAttr,
                    uint32_t pIosb, uint32_t createDisposition, uint32_t createOptions)
{
    std::string name = ReadObjectName(pObjAttr);
    Vol vol;
    std::string rel = StripDevice(name, &vol);
    const std::string& root = RootFor(vol);
    bool isDir = false;
    std::string host = ResolveHost(root, rel, &isDir);
    bool writable = (vol == Vol::Cache || vol == Vol::Mount);

    // FILE_SUPERSEDE=0 FILE_OPEN=1 FILE_CREATE=2 FILE_OPEN_IF=3 FILE_OVERWRITE=4 FILE_OVERWRITE_IF=5
    bool mayCreate = writable && (createDisposition == 0 || createDisposition == 2 ||
                                  createDisposition == 3 || createDisposition == 5);
    bool truncate  = writable && (createDisposition == 0 || createDisposition == 4 || createDisposition == 5);
    bool wantDir   = (createOptions & 0x1) != 0;   // FILE_DIRECTORY_FILE

    if (host.empty() && mayCreate && !root.empty())
    {
        // Create: resolve the parent case-insensitively, keep the leaf as given.
        size_t slash = rel.find_last_of('/');
        std::string parentRel = slash == std::string::npos ? "" : rel.substr(0, slash);
        std::string leaf = slash == std::string::npos ? rel : rel.substr(slash + 1);
        bool pdir = true;
        std::string parent = parentRel.empty() ? root : ResolveHost(root, parentRel, &pdir);
        if (!parent.empty() && pdir && !leaf.empty())
        {
            host = parent + "/" + leaf;
            std::error_code ec;
            if (wantDir) { fs::create_directory(host, ec); isDir = true; }
            else { FILE* f = fopen(host.c_str(), "wb"); if (f) fclose(f); }
            if (!fs::exists(host, ec)) host.clear();
        }
    }

    if (host.empty())
    {
        fprintf(stderr, "[vfs] %s \"%s\" -> NOT FOUND (%s)\n", api, name.c_str(), rel.c_str());
        WriteIosb(pIosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
        if (pHandle) GStore32(pHandle, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    auto kf = std::make_shared<KFile>();
    kf->hostPath = host; kf->guestPath = name; kf->isDir = isDir; kf->writable = writable;
    if (!isDir)
    {
        kf->fp = fopen(host.c_str(), writable ? (truncate ? "w+b" : "r+b") : "rb");
        if (!kf->fp && writable) kf->fp = fopen(host.c_str(), "w+b");
        if (!kf->fp)
        {
            fprintf(stderr, "[vfs] %s \"%s\" -> open failed: %s\n", api, name.c_str(), strerror(errno));
            WriteIosb(pIosb, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
        std::error_code ec; kf->size = uint64_t(fs::file_size(host, ec)); if (ec) kf->size = 0;
    }

    uint32_t h;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        auto o = NewObject(T_File);
        o->file = kf;
        o->signaled = true;          // a file handle is always signaled for waits
        h = NewHandle(o);
    }
    if (pHandle) GStore32(pHandle, h);
    WriteIosb(pIosb, STATUS_SUCCESS, 1 /* FILE_OPENED */);
    fprintf(stderr, "[vfs] %s \"%s\" -> %s%s (%llu bytes) handle 0x%X\n", api, name.c_str(),
            isDir ? "dir " : "", host.c_str() + root.size(), (unsigned long long)kf->size, h);
    return STATUS_SUCCESS;
}

std::shared_ptr<KFile> FileByHandle(uint32_t h)
{
    std::lock_guard<std::mutex> lk(g_dispMu);
    auto o = ByHandle(h);
    return (o && o->type == T_File) ? o->file : nullptr;
}

} // namespace

// NTSTATUS NtCreateFile(PHANDLE r3, ACCESS_MASK r4, POBJECT_ATTRIBUTES r5, PIO_STATUS_BLOCK r6,
//                       PLARGE_INTEGER allocSize r7, ULONG attrs r8, ULONG share r9,
//                       ULONG disposition r10, ULONG options [stack])
PPC_FUNC(__imp__NtCreateFile)
{
    ImportLog("NtCreateFile");
    // 9th arg lives on the stack: r1 + 0x54 in the Xbox 360 ABI (home space 0x50 + 1 slot).
    uint32_t options = GLoad32(ctx.r1.u32 + 0x54);
    ctx.r3.u64 = OpenCommon("NtCreateFile", ctx, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options);
}

// NTSTATUS NtOpenFile(PHANDLE r3, ACCESS_MASK r4, POBJECT_ATTRIBUTES r5, PIO_STATUS_BLOCK r6,
//                     ULONG share r7, ULONG options r8)
PPC_FUNC(__imp__NtOpenFile)
{
    ImportLog("NtOpenFile");
    ctx.r3.u64 = OpenCommon("NtOpenFile", ctx, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32);
}

// NTSTATUS NtReadFile(HANDLE r3, HANDLE event r4, PIO_APC_ROUTINE r5, PVOID apcCtx r6,
//                     PIO_STATUS_BLOCK r7, PVOID buffer r8, ULONG length r9, PLARGE_INTEGER offset r10)
PPC_FUNC(__imp__NtReadFile)
{
    ImportLog("NtReadFile");
    auto kf = FileByHandle(ctx.r3.u32);
    uint32_t pIosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, pOff = ctx.r10.u32;
    if (!kf || !kf->fp)
    {
        WriteIosb(pIosb, STATUS_INVALID_HANDLE, 0);
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    uint64_t off = pOff ? GLoad64(pOff) : kf->pos;
    // Xbox passes 0xFFFFFFFFFFFFFFFE/-1 style "use current position" sentinels.
    if (pOff && int64_t(off) < 0) off = kf->pos;
    if (FileSeek(kf->fp, off) != 0) { WriteIosb(pIosb, STATUS_END_OF_FILE, 0); ctx.r3.u64 = STATUS_END_OF_FILE; return; }
    GuestHostWrite(buf, len);   // the kernel cannot write into write-tracked (read-only) pages
    size_t rd = fread(g_base + buf, 1, len, kf->fp);
    kf->pos = off + rd;
    uint32_t status = (rd == 0 && len > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    // HC09_TRACE_READS=1: every read with its offset, size, completion event/APC and thread.
    static const bool traceReads = Hc09Env("HC09_TRACE_READS") != nullptr;
    if (traceReads)
        fprintf(stderr, "[vfs] read %s @0x%llX len 0x%X -> 0x%zX event 0x%X apc 0x%08X tid %llu (%s) swap %llu\n",
                kf->guestPath.c_str(), (unsigned long long)off, len, rd, ctx.r4.u32, ctx.r5.u32,
                (unsigned long long)KernelCurrentTid(), KernelCurrentThreadName().c_str(), (unsigned long long)GpuSwapCount());
    WriteIosb(pIosb, status, uint32_t(rd));
    if (ctx.r4.u32)   // completion event
    {
        std::shared_ptr<KObject> ev; { std::lock_guard<std::mutex> lk(g_dispMu); ev = ByHandle(ctx.r4.u32); }
        SetEventObj(ev);
    }
    ctx.r3.u64 = status;
}

// NTSTATUS NtWriteFile(HANDLE r3, HANDLE event r4, apc r5, apcCtx r6, PIO_STATUS_BLOCK r7,
//                      PVOID buffer r8, ULONG length r9, PLARGE_INTEGER offset r10)
PPC_FUNC(__imp__NtWriteFile)
{
    ImportLog("NtWriteFile");
    auto kf = FileByHandle(ctx.r3.u32);
    uint32_t pIosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, pOff = ctx.r10.u32;
    if (!kf || !kf->fp) { WriteIosb(pIosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint64_t off = pOff ? GLoad64(pOff) : kf->pos;
    if (pOff && int64_t(off) < 0) off = kf->pos;
    FileSeek(kf->fp, off);
    size_t wr = fwrite(g_base + buf, 1, len, kf->fp);
    fflush(kf->fp);
    kf->pos = off + wr;
    if (kf->pos > kf->size) kf->size = kf->pos;
    uint32_t status = (wr == len) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    WriteIosb(pIosb, status, uint32_t(wr));
    if (ctx.r4.u32)
    {
        std::shared_ptr<KObject> ev; { std::lock_guard<std::mutex> lk(g_dispMu); ev = ByHandle(ctx.r4.u32); }
        SetEventObj(ev);
    }
    ctx.r3.u64 = status;
}

// FILE_INFORMATION_CLASS values we answer.
enum : uint32_t {
    FileBasicInformation         = 4,
    FileStandardInformation      = 5,
    FileInternalInformation      = 6,
    FileEaInformation            = 7,
    FileNameInformation          = 9,
    FileDispositionInformation   = 13,
    FilePositionInformation      = 14,
    FileEndOfFileInformation     = 20,
    FileAlignmentInformation     = 17,
    FileNetworkOpenInformation   = 34,
    FileAttributeTagInformation  = 35,
    FileSectorInformation        = 26,   // Xbox: the sector the file starts on (Pipe/Mailslot classes renumbered)
};

// NTSTATUS NtQueryInformationFile(HANDLE r3, PIO_STATUS_BLOCK r4, PVOID info r5, ULONG len r6, CLASS r7)
PPC_FUNC(__imp__NtQueryInformationFile)
{
    ImportLog("NtQueryInformationFile");
    auto kf = FileByHandle(ctx.r3.u32);
    uint32_t pIosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!kf) { WriteIosb(pIosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t attrs = kf->isDir ? 0x10 /*DIRECTORY*/ : 0x80 /*NORMAL*/;
    uint32_t written = 0, status = STATUS_SUCCESS;
    switch (cls)
    {
    case FileBasicInformation:          // 4 times + attrs (0x28)
        for (int i = 0; i < 0x20; i += 4) GStore32(info + i, 0);
        GStore32(info + 0x20, attrs); written = 0x28; break;
    case FileStandardInformation:       // AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory (0x18)
        GStore64(info + 0x00, (kf->size + 0xFFF) & ~0xFFFull);
        GStore64(info + 0x08, kf->size);
        GStore32(info + 0x10, 1);
        GStore8 (info + 0x14, 0);
        GStore8 (info + 0x15, kf->isDir ? 1 : 0);
        written = 0x18; break;
    case FileInternalInformation:       // IndexNumber (8)
        GStore64(info, 1); written = 8; break;
    case FileEaInformation:
        GStore32(info, 0); written = 4; break;
    case FilePositionInformation:
        GStore64(info, kf->pos); written = 8; break;
    case FileAlignmentInformation:
        GStore32(info, 0); written = 4; break;
    case FileNetworkOpenInformation:    // 4 times, AllocationSize, EndOfFile, FileAttributes (0x38)
        for (int i = 0; i < 0x20; i += 4) GStore32(info + i, 0);
        GStore64(info + 0x20, (kf->size + 0xFFF) & ~0xFFFull);
        GStore64(info + 0x28, kf->size);
        GStore32(info + 0x30, attrs);
        written = 0x38; break;
    case FileAttributeTagInformation:
        GStore32(info, attrs); GStore32(info + 4, 0); written = 8; break;
    case FileSectorInformation:         // where the file starts on its device: stable per path
    {                                   // (HC09 keys its cache:\mcache validity on it)
        uint32_t h = 2166136261u;
        for (char c : kf->guestPath) h = (h ^ uint8_t(tolower((unsigned char)c))) * 16777619u;
        GStore32(info, h & 0x00FFFFFF); written = 4; break;
    }
    default:
        fprintf(stderr, "[vfs] NtQueryInformationFile: unhandled class %u for \"%s\"\n", cls, kf->guestPath.c_str());
        status = STATUS_NOT_IMPLEMENTED; break;
    }
    WriteIosb(pIosb, status, written);
    ctx.r3.u64 = status;
}

// NTSTATUS NtSetInformationFile(HANDLE r3, PIO_STATUS_BLOCK r4, PVOID info r5, ULONG len r6, CLASS r7)
PPC_FUNC(__imp__NtSetInformationFile)
{
    ImportLog("NtSetInformationFile");
    auto kf = FileByHandle(ctx.r3.u32);
    uint32_t pIosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!kf) { WriteIosb(pIosb, STATUS_INVALID_HANDLE, 0); ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t status = STATUS_SUCCESS;
    switch (cls)
    {
    case FilePositionInformation: kf->pos = GLoad64(info); break;
    case FileEndOfFileInformation:          // LARGE_INTEGER EndOfFile
        if (kf->writable && kf->fp)
        {
            uint64_t eof = GLoad64(info);
            fflush(kf->fp);
            if (FileTruncate(kf->fp, eof)) kf->size = eof;
            else status = STATUS_ACCESS_DENIED;
        }
        break;
    case FileDispositionInformation:        // BOOLEAN DeleteFile
        if (!kf->writable) { status = STATUS_ACCESS_DENIED; break; }
        kf->deleteOnClose = GLoad8(info) != 0;
        fprintf(stderr, "[vfs] %s marked %s\n", kf->guestPath.c_str(), kf->deleteOnClose ? "delete-on-close" : "keep");
        break;
    default:
        fprintf(stderr, "[vfs] NtSetInformationFile: unhandled class %u\n", cls);
        status = STATUS_NOT_IMPLEMENTED; break;
    }
    WriteIosb(pIosb, status, 0);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE r3, PIO_STATUS_BLOCK r4, PVOID info r5, ULONG len r6, FS_CLASS r7)
PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    ImportLog("NtQueryVolumeInformationFile");
    uint32_t pIosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t written = 0, status = STATUS_SUCCESS;
    switch (cls)
    {
    case 1:  // FileFsVolumeInformation: creation time, serial, label len, supports objects, label
        GStore64(info, 0); GStore32(info + 8, 0x12345678); GStore32(info + 12, 0); GStore8(info + 16, 0);
        written = 0x12; break;
    case 3:  // FileFsSizeInformation: total units, avail units, sectors/unit, bytes/sector
        GStore64(info, 0x100000); GStore64(info + 8, 0x80000); GStore32(info + 16, 1); GStore32(info + 20, 2048);
        written = 0x18; break;
    case 4:  // FileFsDeviceInformation: device type (7 = FILE_DEVICE_CD_ROM), characteristics
        GStore32(info, 7); GStore32(info + 4, 0x1 /*removable*/ | 0x2 /*read-only*/);
        written = 8; break;
    case 5:  // FileFsAttributeInformation: attrs, max name len, name len, name
        GStore32(info, 0x1 /*case-sensitive search*/ | 0x2 /*case preserved*/); GStore32(info + 4, 255);
        GStore32(info + 8, 4); GStore8(info + 12, 'U'); GStore8(info + 13, 'D'); GStore8(info + 14, 'F'); GStore8(info + 15, 'X');
        written = 0x10; break;
    default:
        fprintf(stderr, "[vfs] NtQueryVolumeInformationFile: unhandled class %u\n", cls);
        status = STATUS_NOT_IMPLEMENTED; break;
    }
    WriteIosb(pIosb, status, written);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES r3, PFILE_NETWORK_OPEN_INFORMATION r4)
PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    ImportLog("NtQueryFullAttributesFile");
    std::string name = ReadObjectName(ctx.r3.u32);
    bool isDir = false;
    Vol vol;
    std::string rel = StripDevice(name, &vol);
    std::string host = ResolveHost(RootFor(vol), rel, &isDir);
    if (host.empty())
    {
        fprintf(stderr, "[vfs] NtQueryFullAttributesFile \"%s\" -> NOT FOUND\n", name.c_str());
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND; return;
    }
    std::error_code ec;
    uint64_t size = isDir ? 0 : uint64_t(fs::file_size(host, ec));
    if (ec) size = 0;
    uint32_t info = ctx.r4.u32;
    for (int i = 0; i < 0x20; i += 4) GStore32(info + i, 0);
    GStore64(info + 0x20, (size + 0xFFF) & ~0xFFFull);
    GStore64(info + 0x28, size);
    GStore32(info + 0x30, isDir ? 0x10 : 0x80);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE r3, HANDLE event r4, PVOID apc r5, PVOID apcCtx r6,
//                               PIO_STATUS_BLOCK r7, PVOID info r8, ULONG len r9, POBJECT_STRING mask r10,
//                               BOOLEAN restart [stack]) — the Xbox kernel's form: no information class.
// One matching entry per call, as FILE_DIRECTORY_INFORMATION:
//   +00 NextEntryOffset, +04 FileIndex, +08 Creation/+10 LastAccess/+18 LastWrite/
//   +20 Change time, +28 EndOfFile, +30 AllocationSize, +38 FileAttributes,
//   +3C FileNameLength, +40 FileName (ANSI, not terminated).
// mask: ANSI_STRING* {u16 Length, u16 MaximumLength, char* Buffer}, * and ? wildcards,
// case-insensitive; null or empty = everything. The listing is taken on the first
// call (or RestartScan) and sorted, so repeated scans see the same order.
namespace {
bool WildMatch(const char* m, const char* s)
{
    if (!*m) return !*s;
    if (*m == '*') return WildMatch(m + 1, s) || (*s && WildMatch(m, s + 1));
    if (!*s) return false;
    if (*m != '?' && tolower((unsigned char)*m) != tolower((unsigned char)*s)) return false;
    return WildMatch(m + 1, s + 1);
}
}

PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    ImportLog("NtQueryDirectoryFile");
    constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006, STATUS_NO_SUCH_FILE = 0xC000000F,
                       STATUS_BUFFER_OVERFLOW = 0x80000005, STATUS_INVALID_HANDLE = 0xC0000008;
    uint32_t pIosb = ctx.r7.u32, info = ctx.r8.u32, len = ctx.r9.u32, pMask = ctx.r10.u32;
    bool restart = (GLoad32(ctx.r1.u32 + 0x54) & 0xFF) != 0;
    std::string mask;
    if (pMask)
    {
        uint16_t n = GLoad16(pMask);
        uint32_t b = GLoad32(pMask + 4);
        for (uint16_t i = 0; i < n && b; ++i) mask += char(GLoad8(b + i));
    }
    if (mask.empty()) mask = "*";
    auto f = FileByHandle(ctx.r3.u32);
    if (!f || !f->isDir)
    {
        WriteIosb(pIosb, STATUS_INVALID_HANDLE, 0);
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    if (!f->dirListed || restart)
    {
        f->dirEntries.clear();
        f->dirPos = 0;
        f->dirListed = true;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(f->hostPath, ec))
        {
            std::string name = e.path().filename().string();
            if (WildMatch(mask.c_str(), name.c_str())) f->dirEntries.push_back(name);
        }
        std::sort(f->dirEntries.begin(), f->dirEntries.end(),
                  [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
        static int logged = 0;
        if (logged++ < 32)
            fprintf(stderr, "[vfs] NtQueryDirectoryFile %s mask \"%s\"%s: %zu entr%s\n", f->guestPath.c_str(),
                    mask.c_str(), restart ? " (restart)" : "", f->dirEntries.size(), f->dirEntries.size() == 1 ? "y" : "ies");
        if (f->dirEntries.empty())
        {
            WriteIosb(pIosb, STATUS_NO_SUCH_FILE, 0);
            ctx.r3.u64 = STATUS_NO_SUCH_FILE;
            return;
        }
    }
    if (f->dirPos >= f->dirEntries.size())
    {
        WriteIosb(pIosb, STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = STATUS_NO_MORE_FILES;
        return;
    }
    const std::string& name = f->dirEntries[f->dirPos];
    uint32_t need = 0x40 + uint32_t(name.size());
    if (len < need)
    {
        WriteIosb(pIosb, STATUS_BUFFER_OVERFLOW, 0);
        ctx.r3.u64 = STATUS_BUFFER_OVERFLOW;
        return;
    }
    std::error_code ec;
    fs::path host = fs::path(f->hostPath) / name;
    bool dir = fs::is_directory(host, ec);
    uint64_t size = dir ? 0 : uint64_t(fs::file_size(host, ec));
    if (ec) size = 0;
    for (uint32_t i = 0; i < 0x40; i += 4) GStore32(info + i, 0);
    GStore32(info + 0x04, uint32_t(f->dirPos));
    for (uint32_t t = 0x08; t <= 0x20; t += 8)   // a fixed timestamp (2008-08-12): the disc's files never change
    {
        GStore32(info + t, 0x01C8FC5D);
        GStore32(info + t + 4, 0x00000000);
    }
    GStore32(info + 0x28, uint32_t(size >> 32));
    GStore32(info + 0x2C, uint32_t(size));
    uint64_t alloc = (size + 0xFFF) & ~uint64_t(0xFFF);
    GStore32(info + 0x30, uint32_t(alloc >> 32));
    GStore32(info + 0x34, uint32_t(alloc));
    GStore32(info + 0x38, dir ? 0x10 : (f->writable ? 0x80 : 0x01));   // DIRECTORY / NORMAL / READONLY (disc)
    GStore32(info + 0x3C, uint32_t(name.size()));
    for (size_t i = 0; i < name.size(); ++i) GStore8(info + 0x40 + uint32_t(i), uint8_t(name[i]));
    ++f->dirPos;
    WriteIosb(pIosb, STATUS_SUCCESS, need);
    ctx.r3.u64 = STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtFlushBuffersFile) { ImportLog("NtFlushBuffersFile"); WriteIosb(ctx.r4.u32, STATUS_SUCCESS, 0); ctx.r3.u64 = STATUS_SUCCESS; }

// Xam input lives in imports_input.cpp (virtual pad + scripted presses).

// ---- delete-on-close (called from NtClose) ------------------------------------
void FileOnClose(const std::shared_ptr<KFile>& f)
{
    if (!f || !f->deleteOnClose || !f->writable) return;
    f->deleteOnClose = false;
    if (f->fp) { fclose(f->fp); f->fp = nullptr; }
    std::error_code ec;
    bool removed = fs::remove(f->hostPath, ec);   // a file or an empty directory
    fprintf(stderr, "[vfs] delete-on-close %s -> %s\n", f->guestPath.c_str(),
            removed ? "deleted" : ec ? ec.message().c_str() : "not found");
}

// ---- content mounts (used by imports_content.cpp) --------------------------
void VfsMount(const std::string& drive, const std::string& hostDir)
{
    std::lock_guard<std::mutex> lk(g_mountMu);
    g_mounts[Lower(drive)] = hostDir;
    fprintf(stderr, "[vfs] mount %s: -> %s\n", drive.c_str(), hostDir.c_str());
}
bool VfsUnmount(const std::string& drive)
{
    std::lock_guard<std::mutex> lk(g_mountMu);
    bool had = g_mounts.erase(Lower(drive)) > 0;
    fprintf(stderr, "[vfs] unmount %s:%s\n", drive.c_str(), had ? "" : " (was not mounted)");
    return had;
}
