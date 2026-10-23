// Xam content (save games and other title content) on one virtual hard drive.
//
// Device: a single HDD, id kDeviceId, always present (the device selector
// "picks" it at once). Content lives on the host under
//   <cache>/content/<xuid hex or 0>/<content type hex>/<file name>/
// (the profile's saved games in <saves>/<file name>/ instead when hc09.ini [game] saves is set: g_vfsSaves)
// and XamContentCreateEx mounts that directory as the requested drive
// ("save:\..." etc.) in the VFS. A sidecar "<file name>.name" keeps the
// display name for enumeration.
//
// Asynchronous Xam calls take an XOVERLAPPED; we complete them immediately
// (result in InternalLow, length in InternalHigh, event signaled) and return
// ERROR_IO_PENDING, which is what the console returns for a queued request.
#include "kernel.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <atomic>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t ERROR_SUCCESS              = 0;
constexpr uint32_t ERROR_PATH_NOT_FOUND       = 3;
constexpr uint32_t ERROR_NO_MORE_FILES        = 0x12;
constexpr uint32_t ERROR_ALREADY_EXISTS       = 0xB7;
constexpr uint32_t ERROR_IO_PENDING           = 0x3E5;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;

constexpr uint32_t kDeviceId   = 0x00000001;
constexpr uint32_t kDeviceType = 1;              // XCONTENTDEVICETYPE_HDD
constexpr uint64_t kXuid       = 0xE000C0AC4E000009ull;   // same profile as imports_user.cpp
constexpr uint32_t kContentDataSize = 0x134;     // XCONTENT_DATA (padded)

// XOVERLAPPED { InternalLow; InternalHigh; InternalContext; hEvent;
//               pCompletionRoutine; dwCompletionContext; dwExtendedError; }
uint32_t Complete(uint32_t ov, uint32_t result, uint32_t length = 0)
{
    if (!ov) return result;
    GStore32(ov + 0x00, result);
    GStore32(ov + 0x04, length);
    GStore32(ov + 0x18, result);
    uint32_t ev = GLoad32(ov + 0x0C);
    if (ev)
    {
        std::shared_ptr<KObject> o;
        { std::lock_guard<std::mutex> lk(g_dispMu); o = ByHandle(ev); }
        SetEventObj(o);
    }
    if (GLoad32(ov + 0x10))
        fprintf(stderr, "[content] XOVERLAPPED %08X has a completion routine 0x%08X (not called)\n", ov, GLoad32(ov + 0x10));
    return ERROR_IO_PENDING;
}

std::string Hex(uint64_t v, int w) { char b[24]; snprintf(b, sizeof(b), "%0*llX", w, (unsigned long long)v); return b; }

void MkdirP(const std::string& path)
{
    std::error_code ec;
    fs::create_directories(HostPath(path), ec);
}

std::string TypeDir(uint32_t user, uint32_t contentType)
{
    uint64_t xuid = (user < 4) ? kXuid : 0;
    // The profile's saved games (XCONTENTTYPE_SAVEDGAME) in their own folder when one is set (runtime.cpp g_vfsSaves).
    if (!g_vfsSaves.empty() && xuid == kXuid && contentType == 1) return g_vfsSaves;
    return g_vfsCache + "/content/" + Hex(xuid, 16) + "/" + Hex(contentType, 8);
}

// XCONTENT_DATA { DWORD DeviceID; DWORD dwContentType; WCHAR szDisplayName[128]; CHAR szFileName[42]; }
struct ContentData { uint32_t device = 0, type = 0; std::string display, file; };

ContentData ReadContentData(uint32_t p)
{
    ContentData d;
    d.device = GLoad32(p + 0);
    d.type = GLoad32(p + 4);
    for (int i = 0; i < 128; ++i) { uint16_t c = GLoad16(p + 8 + i * 2); if (!c) break; d.display += char(c < 0x80 ? c : '?'); }
    for (int i = 0; i < 42; ++i) { uint8_t c = GLoad8(p + 0x108 + i); if (!c) break; d.file += char(c); }
    return d;
}

void WriteContentData(uint32_t p, const ContentData& d)
{
    for (uint32_t i = 0; i < kContentDataSize; i += 4) GStore32(p + i, 0);
    GStore32(p + 0, d.device);
    GStore32(p + 4, d.type);
    for (size_t i = 0; i < d.display.size() && i < 127; ++i) GStore16(p + 8 + uint32_t(i) * 2, uint8_t(d.display[i]));
    for (size_t i = 0; i < d.file.size() && i < 41; ++i) GStore8(p + 0x108 + uint32_t(i), uint8_t(d.file[i]));
}

std::string ReadGuestString(uint32_t p, size_t max = 64)
{
    std::string s;
    for (size_t i = 0; p && i < max; ++i) { char c = char(GLoad8(p + uint32_t(i))); if (!c) break; s += c; }
    return s;
}

struct Enumerator { std::vector<ContentData> items; size_t next = 0; uint32_t perCall = 1; };
std::mutex g_enumMu;
std::map<uint32_t, Enumerator> g_enums;

} // namespace

// DWORD XamShowDeviceSelectorUI(DWORD user r3, DWORD contentType r4, DWORD contentFlags r5,
//                               ULONGLONG bytesRequested r6, DWORD* deviceId r7, XOVERLAPPED* r8)
PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    ImportLog("XamShowDeviceSelectorUI");
    fprintf(stderr, "[content] XamShowDeviceSelectorUI(user %u, type 0x%X, flags 0x%X, %llu bytes) -> HDD 0x%X\n",
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (unsigned long long)ctx.r6.u64, kDeviceId);
    if (ctx.r7.u32) GStore32(ctx.r7.u32, kDeviceId);
    ctx.r3.u64 = Complete(ctx.r8.u32, ERROR_SUCCESS);
}

// DWORD XamContentGetDeviceState(DWORD deviceId r3, XOVERLAPPED* r4)
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    ImportLog("XamContentGetDeviceState");
    uint32_t r = ctx.r3.u32 == kDeviceId ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    ctx.r3.u64 = Complete(ctx.r4.u32, r);
}

// DWORD XamContentGetDeviceData(DWORD deviceId r3, XDEVICE_DATA* r4)
// { DWORD DeviceID; DWORD DeviceType; ULONGLONG ulDeviceBytes; ULONGLONG ulDeviceFreeBytes;
//   WCHAR wszFriendlyName[28]; }
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    ImportLog("XamContentGetDeviceData");
    uint32_t id = ctx.r3.u32, out = ctx.r4.u32;
    if (id != kDeviceId) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    if (out)
    {
        for (int i = 0; i < 0x50; i += 4) GStore32(out + i, 0);
        GStore32(out + 0x00, kDeviceId);
        GStore32(out + 0x04, kDeviceType);
        GStore64(out + 0x08, 20ull << 30);       // 20 GB
        GStore64(out + 0x10, 16ull << 30);       // 16 GB free
        const char* name = "Hard Drive";
        for (int i = 0; name[i]; ++i) GStore16(out + 0x18 + i * 2, uint8_t(name[i]));
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamContentCreateEnumerator(DWORD user r3, DWORD deviceId r4, DWORD contentType r5,
//     DWORD contentFlags r6, DWORD itemsPerEnumerate r7, DWORD* bufferSize r8, HANDLE* handle r9)
PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    ImportLog("XamContentCreateEnumerator");
    uint32_t user = ctx.r3.u32, dev = ctx.r4.u32, type = ctx.r5.u32, per = ctx.r7.u32;
    Enumerator e;
    e.perCall = per ? per : 1;
    if (dev == 0 || dev == kDeviceId)
    {
        std::string dir = TypeDir(user, type);
        std::error_code ec;
        fs::directory_iterator it(HostPath(dir), ec), end;
        if (!ec)
        {
            for (; it != end; it.increment(ec))
            {
                std::string n = HostUtf8(it->path().filename());
                if (!it->is_directory(ec)) continue;
                ContentData c{ kDeviceId, type, n, n };
                if (FILE* f = HostFopen(dir + "/" + n + ".name", "rb"))
                {
                    char buf[256] = {}; size_t r = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
                    c.display.assign(buf, r);
                }
                e.items.push_back(c);
            }
        }
    }
    uint32_t h;
    {
        std::lock_guard<std::mutex> lk(g_dispMu);
        h = NewHandle(NewObject(T_Unknown));
    }
    fprintf(stderr, "[content] enumerator 0x%X: user %u device 0x%X type 0x%X -> %zu item(s)\n", h, user, dev, type, e.items.size());
    { std::lock_guard<std::mutex> lk(g_enumMu); g_enums[h] = e; }
    if (ctx.r8.u32) GStore32(ctx.r8.u32, e.perCall * kContentDataSize);
    if (ctx.r9.u32) GStore32(ctx.r9.u32, h);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// DWORD XamEnumerate(HANDLE h r3, DWORD flags r4, PVOID buffer r5, DWORD bufferSize r6,
//                    DWORD* itemsReturned r7, XOVERLAPPED* r8)
PPC_FUNC(__imp__XamEnumerate)
{
    ImportLog("XamEnumerate");
    uint32_t h = ctx.r3.u32, buf = ctx.r5.u32, size = ctx.r6.u32, pCount = ctx.r7.u32, ov = ctx.r8.u32;
    uint32_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_enumMu);
        auto it = g_enums.find(h);
        if (it != g_enums.end())
        {
            Enumerator& e = it->second;
            while (e.next < e.items.size() && n < e.perCall && (n + 1) * kContentDataSize <= size)
                WriteContentData(buf + n++ * kContentDataSize, e.items[e.next++]);
        }
        else fprintf(stderr, "[content] XamEnumerate on unknown handle 0x%X\n", h);
    }
    if (pCount) GStore32(pCount, n);
    uint32_t r = n ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
    ctx.r3.u64 = Complete(ov, r, n);
}

// DWORD XamContentCreateEx(DWORD user r3, LPCSTR rootName r4, XCONTENT_DATA* r5, DWORD flags r6,
//     DWORD* disposition r7, DWORD* licenseMask r8, DWORD cacheSize r9, ULONGLONG contentSize r10,
//     XOVERLAPPED* [sp+0x54])
// flags (low byte): 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING.
PPC_FUNC(__imp__XamContentCreateEx)
{
    ImportLog("XamContentCreateEx");
    uint32_t user = ctx.r3.u32, flags = ctx.r6.u32, pDisp = ctx.r7.u32, pLic = ctx.r8.u32;
    uint32_t ov = GLoad32(ctx.r1.u32 + 0x54);
    std::string root = ReadGuestString(ctx.r4.u32);
    ContentData c = ReadContentData(ctx.r5.u32);
    std::string dir = TypeDir(user, c.type) + "/" + c.file;
    std::error_code ec;
    bool exists = fs::is_directory(HostPath(dir), ec);
    uint32_t mode = flags & 0xFF, result = ERROR_SUCCESS, disposition = exists ? 2 : 1;   // 1 created, 2 opened
    if (mode == 1 && exists) result = ERROR_ALREADY_EXISTS;
    else if ((mode == 3 || mode == 5) && !exists) result = ERROR_PATH_NOT_FOUND;
    else
    {
        if (!exists) MkdirP(dir);
        if (!c.display.empty())
            if (FILE* f = HostFopen(dir + ".name", "wb")) { fwrite(c.display.data(), 1, c.display.size(), f); fclose(f); }
        VfsMount(root, dir);
    }
    fprintf(stderr, "[content] XamContentCreateEx(user %u, \"%s\", type 0x%X, \"%s\" / \"%s\", flags 0x%X) -> %s, result 0x%X\n",
            user, root.c_str(), c.type, c.file.c_str(), c.display.c_str(), flags,
            result ? "failed" : (disposition == 1 ? "created" : "opened"), result);
    if (pDisp) GStore32(pDisp, result ? 0 : disposition);
    if (pLic) GStore32(pLic, 0);
    ctx.r3.u64 = ov ? Complete(ov, result) : result;
}

// DWORD XamContentClose(LPCSTR rootName r3, XOVERLAPPED* r4)
PPC_FUNC(__imp__XamContentClose)
{
    ImportLog("XamContentClose");
    VfsUnmount(ReadGuestString(ctx.r3.u32));
    ctx.r3.u64 = ctx.r4.u32 ? Complete(ctx.r4.u32, ERROR_SUCCESS) : ERROR_SUCCESS;
}

// DWORD XamContentGetCreator(DWORD user r3, XCONTENT_DATA* r4, BOOL* isCreator r5, XUID* creator r6, XOVERLAPPED* r7)
PPC_FUNC(__imp__XamContentGetCreator)
{
    ImportLog("XamContentGetCreator");
    if (ctx.r5.u32) GStore32(ctx.r5.u32, 1);
    if (ctx.r6.u32) GStore64(ctx.r6.u32, kXuid);
    ctx.r3.u64 = ctx.r7.u32 ? Complete(ctx.r7.u32, ERROR_SUCCESS) : ERROR_SUCCESS;
}

// DWORD XamContentDelete(DWORD user r3, XCONTENT_DATA* r4, XOVERLAPPED* r5)
PPC_FUNC(__imp__XamContentDelete)
{
    ImportLog("XamContentDelete");
    ContentData c = ReadContentData(ctx.r4.u32);
    fprintf(stderr, "[content] XamContentDelete(\"%s\") — left on disk (not deleting host files)\n", c.file.c_str());
    ctx.r3.u64 = ctx.r5.u32 ? Complete(ctx.r5.u32, ERROR_SUCCESS) : ERROR_SUCCESS;
}

// DWORD XamContentSetThumbnail(LPCSTR rootName r3, PVOID image r4, DWORD size r5, XOVERLAPPED* r6)
PPC_FUNC(__imp__XamContentSetThumbnail)
{
    ImportLog("XamContentSetThumbnail");
    ctx.r3.u64 = ctx.r6.u32 ? Complete(ctx.r6.u32, ERROR_SUCCESS) : ERROR_SUCCESS;
}

// ---- XMsgStartIORequest(app r3, message r4, XOVERLAPPED* r5, buffer r6, length r7) ----------------
// (Ex adds r8.) Asynchronous calls into XAM's built-in apps. Were generated stubs: success without
// completing the XOVERLAPPED, so an asynchronous request stayed pending for good. The game uses
// XGIUserWriteAchievements (app 0xFB, 0x000B0008; buffer { count, XUSER_ACHIEVEMENT* } with entries
// { user index, achievement id }) when the player earns an achievement, and XMP 0x0007001A
// (XMPSetPlaybackController, synchronous). Requests complete at once with success, as Xenia does.
static uint32_t XMsgStartIO(uint32_t app, uint32_t msg, uint32_t ov, uint32_t buf)
{
    if (app == 0xFB && msg == 0x000B0008 && buf) {
        const uint32_t n = GLoad32(buf), list = GLoad32(buf + 4);
        for (uint32_t i = 0; i < n && i < 16 && list; ++i)
            fprintf(stderr, "[xgi] achievement unlocked: id %u (user %u)\n", GLoad32(list + 8 * i + 4), GLoad32(list + 8 * i));
    } else if (!(app == 0xFA && msg == 0x0007001A)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 16)
            fprintf(stderr, "[xmsg] XMsgStartIORequest app 0x%X message 0x%08X (overlapped 0x%08X) -> success\n", app, msg, ov);
    }
    return ov ? Complete(ov, ERROR_SUCCESS) : ERROR_SUCCESS;
}
PPC_FUNC(__imp__XMsgStartIORequest)
{
    ImportLog("XMsgStartIORequest");
    ctx.r3.u64 = XMsgStartIO(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
PPC_FUNC(__imp__XMsgStartIORequestEx)
{
    ImportLog("XMsgStartIORequestEx");
    ctx.r3.u64 = XMsgStartIO(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
