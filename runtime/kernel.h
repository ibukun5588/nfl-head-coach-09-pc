#pragma once
// Shared kernel-object model: threads, dispatcher objects, files, handles.
// Defined in imports_threads.cpp; used by imports_io.cpp and others.
#include "runtime.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

constexpr uint32_t STATUS_SUCCESS               = 0x00000000;
constexpr uint32_t STATUS_TIMEOUT               = 0x00000102;
constexpr uint32_t STATUS_PENDING               = 0x00000103;
constexpr uint32_t STATUS_END_OF_FILE           = 0xC0000011;
constexpr uint32_t STATUS_INVALID_HANDLE        = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER     = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE          = 0xC000000F;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_NOT_IMPLEMENTED       = 0xC0000002;
constexpr uint32_t STATUS_ACCESS_DENIED         = 0xC0000022;
constexpr uint32_t STATUS_NO_MEMORY             = 0xC0000017;

// DISPATCHER_HEADER.Type values (Xbox 360 / NT), plus runtime-private types.
enum ObjType : uint8_t {
    T_NotificationEvent    = 0,
    T_SynchronizationEvent = 1,
    T_Mutant               = 2,
    T_Semaphore            = 5,
    T_Thread               = 6,
    T_NotificationTimer    = 8,
    T_SynchronizationTimer = 9,
    T_File                 = 0x80,   // runtime-private: an open host file
    T_Unknown              = 0xFF,
};

struct KThread
{
    uint32_t  startAddr = 0, param = 0, xapiStartup = 0;
    uint32_t  stackBase = 0, stackSize = 0, pcr = 0;
    uint8_t   cpu = 0;                 // hardware thread (KPCR.current_cpu)
    uint64_t  tid = 0;
    std::atomic<int> suspendCount{ 0 };
    std::atomic<bool> finished{ false };
    uint32_t  exitCode = 0;
    std::thread host;
    PPCContext* ctx = nullptr;
};

struct KFile
{
    FILE*       fp = nullptr;
    std::string hostPath;
    std::string guestPath;
    uint64_t    size = 0;
    uint64_t    pos = 0;
    bool        isDir = false;
    bool        writable = false;        // cache:/HDD/content mount (never the disc)
    bool        deleteOnClose = false;   // FileDispositionInformation
    // NtQueryDirectoryFile cursor (directories only).
    std::vector<std::string> dirEntries;
    size_t      dirPos = 0;
    bool        dirListed = false;
};

// NtClose of a file handle: honours delete-on-close (writable volumes only).
void FileOnClose(const std::shared_ptr<KFile>& f);

struct KObject
{
    ObjType  type = T_Unknown;
    uint32_t guest = 0;            // object pointer in guest memory
    std::atomic<int> refs{ 1 };

    // Dispatcher state (guarded by g_dispMu).
    bool manualReset = false;
    bool signaled    = false;
    int32_t count    = 0;
    int32_t limit    = 0x7FFFFFFF;
    uint64_t ownerTid = 0;
    int32_t recursion = 0;

    std::shared_ptr<KThread> thread;   // T_Thread
    std::shared_ptr<KFile>   file;     // T_File
};

extern std::mutex g_dispMu;
extern std::condition_variable g_dispCv;
extern std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_byGuest;
extern std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_byHandle;
extern thread_local std::shared_ptr<KObject> t_self;

// All take g_dispMu held unless stated.
std::shared_ptr<KObject> NewObject(ObjType t, uint32_t guest = 0);
uint32_t                 NewHandle(const std::shared_ptr<KObject>& o);
std::shared_ptr<KObject> ByHandle(uint32_t h);
std::shared_ptr<KObject> ByGuest(uint32_t p);
void                     MirrorHeader(const std::shared_ptr<KObject>& o);
uint32_t SetEventObj(const std::shared_ptr<KObject>& o);    // locks internally
uint32_t ResetEventObj(const std::shared_ptr<KObject>& o);  // locks internally

// KPCR layout (what r13 points at). The game reads these directly:
//   +0x070 stack base, +0x074 stack limit, +0x100 current KTHREAD*,
//   +0x10C current hardware thread index (0..5).
constexpr uint32_t KPCR_STACK_BASE  = 0x070;
constexpr uint32_t KPCR_STACK_LIMIT = 0x074;
constexpr uint32_t KPCR_CUR_THREAD  = 0x100;
constexpr uint32_t KPCR_CUR_CPU     = 0x10C;
void    SetupPcr(uint32_t pcr, uint32_t threadObj, uint8_t cpu, uint32_t stackBase, uint32_t stackSize);
uint8_t CpuFromAffinityMask(uint32_t mask);   // highest set bit, 0 if none

// Give a runtime-created host thread (audio pump, ...) a guest thread identity:
// a KTHREAD object, tid, name, PCR. Returns the PCR address for InitThreadContext.
uint32_t KernelRegisterHostThread(const char* name, uint8_t cpu, uint32_t stackBase, uint32_t stackSize, PPCContext* ctx = nullptr);

// Diagnostics: every guest thread with its state and a backtrace (racy for
// running threads, exact for blocked ones; good enough to see who waits on what).
void KernelDumpAllThreads();

// Current guest thread identity for diagnostics (safe from any host thread).
uint64_t    KernelCurrentTid();
std::string KernelCurrentThreadName();
void        KernelSetThreadName(uint64_t tid, const std::string& name);
std::string KernelThreadName(uint64_t tid);   // "" if unnamed

// VFS content mounts: "<drive>:\..." resolves under hostDir (writable).
void VfsMount(const std::string& drive, const std::string& hostDir);
bool VfsUnmount(const std::string& drive);
extern std::string g_vfsCache;
extern std::string g_vfsSaves;

// Host file paths are UTF-8 strings in the runtime (hc09.ini, the command line); these hand them to the OS (UTF-16 on
// Windows), so folders with any characters work: a user name in Documents, a game folder (imports_io.cpp).
std::filesystem::path HostPath(const std::string& utf8);
std::string HostUtf8(const std::filesystem::path& p);
FILE* HostFopen(const std::string& utf8, const char* mode);

// Read a guest ANSI_STRING {u16 len, u16 max, u32 buf} into a host string.
std::string ReadAnsiString(uint32_t pStr);
// Read a guest OBJECT_ATTRIBUTES {u32 rootDir, u32 pObjectName, u32 attrs}.
std::string ReadObjectName(uint32_t pObjAttr, uint32_t* rootDirOut = nullptr);
