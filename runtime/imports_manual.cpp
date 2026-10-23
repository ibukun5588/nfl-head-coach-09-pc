// Hand-implemented imports — the minimal real kernel behavior needed to get
// past CRT startup. Everything else is a logging stub in imports_gen.cpp.
//
// Guest calling convention: integer args in r3,r4,r5,r6,...; NTSTATUS/return
// in r3. Guest memory is big-endian, so all loads/stores through `base` use
// the PPC_LOAD_*/PPC_STORE_* byteswapping macros.
#include "crash_report.h"
#include "runtime.h"
#include <cstring>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

// ---- Memory allocation ---------------------------------------------------

// NTSTATUS NtAllocateVirtualMemory(PPVOID BaseAddress, PSIZE_T RegionSize,
//                                  DWORD AllocationType, DWORD Protect)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    ImportLog("NtAllocateVirtualMemory");
    uint32_t pBase = ctx.r3.u32;
    uint32_t pSize = ctx.r4.u32;
    uint32_t size  = pSize ? PPC_LOAD_U32(pSize) : 0;
    if (size == 0) size = 0x1000;
    uint32_t addr = GuestAlloc(size, 0x1000);        // page-aligned
    if (pBase) PPC_STORE_U32(pBase, addr);
    if (pSize) PPC_STORE_U32(pSize, (size + 0xFFF) & ~0xFFFu);
    ctx.r3.u64 = addr ? 0 : 0xC0000017u;             // STATUS_NO_MEMORY on fail
}

PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    ImportLog("NtFreeVirtualMemory");
    ctx.r3.u64 = 0; // bump allocator never frees; report success
}

// PVOID MmAllocatePhysicalMemoryEx(DWORD flags r3, SIZE_T size r4, DWORD protect r5,
//                                  DWORD minAddr r6, DWORD maxAddr r7, DWORD alignment r8)
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    ImportLog("MmAllocatePhysicalMemoryEx");
    uint32_t size = ctx.r4.u32 ? ctx.r4.u32 : 0x1000;
    uint32_t align = ctx.r8.u32 > 0x1000 ? ctx.r8.u32 : 0x1000;
    uint32_t r = PhysicalAlloc(size, align);
    fprintf(stderr, "[kernel] MmAllocatePhysicalMemoryEx(flags 0x%X, %u bytes, protect 0x%X, range 0x%08X-0x%08X, align 0x%X) -> 0x%08X (pa 0x%08X) from 0x%08X\n",
            ctx.r3.u32, size, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, r, r & 0x1FFFFFFF, uint32_t(ctx.lr));
    ctx.r3.u64 = r;
}

// PVOID MmAllocatePhysicalMemory(DWORD flags r3, SIZE_T size r4, DWORD protect r5)
PPC_FUNC(__imp__MmAllocatePhysicalMemory)
{
    ImportLog("MmAllocatePhysicalMemory");
    uint32_t size = ctx.r4.u32 ? ctx.r4.u32 : 0x1000;
    uint32_t r = PhysicalAlloc(size, 0x1000);
    fprintf(stderr, "[kernel] MmAllocatePhysicalMemory(%u bytes) -> 0x%08X from 0x%08X\n", size, r, uint32_t(ctx.lr));
    ctx.r3.u64 = r;
}

// void MmFreePhysicalMemory(DWORD type r3, PVOID address r4)
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    ImportLog("MmFreePhysicalMemory");
    fprintf(stderr, "[kernel] MmFreePhysicalMemory(0x%08X) from 0x%08X\n", ctx.r4.u32, uint32_t(ctx.lr));
    PhysicalFree(ctx.r4.u32);
    ctx.r3.u64 = 0;
}
// Physical address of a virtual one. The physical views (0xA/0xC/0xE) are
// aliases of the low 512 MiB, so masking is exact for them; anything else
// (our virtual heap) is returned unchanged and logged once — the GPU would
// read the wrong bytes if the game pointed it there.
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    ImportLog("MmGetPhysicalAddress");
    uint32_t va = ctx.r3.u32;
    bool backed = va >= 0xA0000000u                                    // physical views
               || (va >= 0x40000000u && va < 0x60000000u)             // virtual heap (pa = va & 0x1FFFFFFF)
               || (va >= PPC_IMAGE_BASE && va < PPC_IMAGE_BASE + PPC_IMAGE_SIZE); // image (pa 0x02000000..)
    if (backed) ctx.r3.u64 = va & 0x1FFFFFFF;
    else
    {
        static std::atomic<int> warned{ 0 };
        if (warned.fetch_add(1) < 3)
            fprintf(stderr, "[kernel] MmGetPhysicalAddress(0x%08X): not physical memory, returning as-is\n", va);
        ctx.r3.u64 = va;
    }
}

// PVOID ExAllocatePoolTypeWithTag(SIZE_T size r3, DWORD tag r4, DWORD type r5)
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)
{
    ImportLog("ExAllocatePoolTypeWithTag");
    uint32_t size = ctx.r3.u32 ? ctx.r3.u32 : 0x10;
    ctx.r3.u64 = GuestAlloc(size, 16);
}

PPC_FUNC(__imp__ExAllocatePoolWithTag)
{
    ImportLog("ExAllocatePoolWithTag");
    uint32_t size = ctx.r3.u32 ? ctx.r3.u32 : 0x10;
    ctx.r3.u64 = GuestAlloc(size, 16);
}

PPC_FUNC(__imp__ExFreePool) { ImportLog("ExFreePool"); ctx.r3.u64 = 0; }

// ---- Strings -------------------------------------------------------------
// VOID RtlInitAnsiString(PANSI_STRING dst r3, PCSZ src r4)
// ANSI_STRING { USHORT Length; USHORT MaximumLength; CHAR* Buffer; }
PPC_FUNC(__imp__RtlInitAnsiString)
{
    ImportLog("RtlInitAnsiString");
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    if (!dst) { return; }
    uint16_t len = 0;
    if (src) { while (PPC_LOAD_U8(src + len) != 0 && len < 0xFFFE) ++len; }
    PPC_STORE_U16(dst + 0, len);
    PPC_STORE_U16(dst + 2, src ? len + 1 : 0);
    PPC_STORE_U32(dst + 4, src);
}

// ---- Time ----------------------------------------------------------------
// VOID KeQuerySystemTime(PLARGE_INTEGER r3)  — 100 ns ticks since 1601.
PPC_FUNC(__imp__KeQuerySystemTime)
{
    ImportLog("KeQuerySystemTime");
    uint32_t p = ctx.r3.u32;
    if (p)
    {
        uint64_t unixNs = uint64_t(time(nullptr)) * 10000000ull;
        uint64_t ft = unixNs + 116444736000000000ull; // 1601→1970 offset
        PPC_STORE_U32(p + 0, uint32_t(ft >> 32));      // high dword first (BE)
        PPC_STORE_U32(p + 4, uint32_t(ft & 0xFFFFFFFF));
    }
}

// ULONGLONG KeQueryPerformanceFrequency(void)
// The Xenon timebase ticks at CPU clock / 64 = 3.2 GHz / 64 = 50 MHz. The
// game divides its 3.2e9 clock constant by this to get cycles-per-tick (64);
// returning 0 here is what caused the M3 divdu-by-zero.
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    ImportLog("KeQueryPerformanceFrequency");
    ctx.r3.u64 = 50000000ull;
}

// ---- Fatal errors: stop the loop and show the true first failure ---------
static void FatalHalt(const char* which, PPCContext& ctx)
{
    fprintf(stderr,
            "\n[runtime] *** %s called — guest requested a fatal halt ***\n"
            "          r3=0x%llX r4=0x%llX r5=0x%llX\n",
            which,
            (unsigned long long)ctx.r3.u64, (unsigned long long)ctx.r4.u64,
            (unsigned long long)ctx.r5.u64);
    fflush(stderr);
    char details[160];
    snprintf(details, sizeof details, "          %s r3=0x%llX r4=0x%llX r5=0x%llX\n", which, (unsigned long long)ctx.r3.u64,
             (unsigned long long)ctx.r4.u64, (unsigned long long)ctx.r5.u64);
    CrashReportWrite("the game asked for a fatal halt", details, nullptr);
    _exit(42);
}
PPC_FUNC(__imp__KeBugCheck)   { ImportLog("KeBugCheck");   FatalHalt("KeBugCheck", ctx); }
PPC_FUNC(__imp__KeBugCheckEx) { ImportLog("KeBugCheckEx"); FatalHalt("KeBugCheckEx", ctx); }
PPC_FUNC(__imp__HalReturnToFirmware)
{
    ImportLog("HalReturnToFirmware");
    fprintf(stderr, "\n[runtime] *** HalReturnToFirmware — guest exiting ***\n");
    fflush(stderr);
    _exit(0);
}

// ---- Title relaunch/exit: the game's "fatal, go back to dashboard" path ------
PPC_FUNC(__imp__XamLoaderSetLaunchData)
{
    ImportLog("XamLoaderSetLaunchData");
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamLoaderLaunchTitle)
{
    ImportLog("XamLoaderLaunchTitle");
    fprintf(stderr, "\n[runtime] *** XamLoaderLaunchTitle called: the game asked to relaunch/exit to the dashboard.\n"
                    "          This is its fatal-error path (typically: required files missing). Halting. ***\n");
    fflush(stderr);
    _exit(43);
}
PPC_FUNC(__imp__XamLoaderTerminateTitle)
{
    ImportLog("XamLoaderTerminateTitle");
    fprintf(stderr, "\n[runtime] *** XamLoaderTerminateTitle called: game is exiting. ***\n");
    fflush(stderr);
    _exit(0);
}

// ---- XMsgInProcessCall(app r3, message r4, buffer r5, length r6) -------------
// Synchronous calls into XAM's built-in apps. The game asks the music player
// (XMP, app 0xFA) whether the user is playing their own soundtrack before it
// starts a song, with the state word pre-set to 1 = playing: answering without
// writing it kept the front-end score off. The answer is "no user music", as on
// a console with the guide's player idle (message ids and layouts as Xenia's
// xmp_app). Other apps (0xFC, content/marketplace) keep the plain success.
PPC_FUNC(__imp__XMsgInProcessCall)
{
    ImportLog("XMsgInProcessCall");
    const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, buf = ctx.r5.u32;
    if (app == 0xFA && buf != 0) {
        switch (msg) {
        case 0x00070009:   // XMPGetStatus(client, &state): 0 idle, 1 playing, 2 paused
            if (uint32_t p = GLoad32(buf + 4)) GStore32(p, 0);
            break;
        case 0x0007001B:   // XMPGetPlaybackController(client, &controller, &locked): the title
            if (uint32_t p = GLoad32(buf + 4)) GStore32(p, 0);
            if (uint32_t p = GLoad32(buf + 8)) GStore32(p, 0);
            break;
        default: {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 8)
                fprintf(stderr, "[xmsg] XMP message 0x%08X not handled (buffer 0x%08X) -> success\n", msg, buf);
        }
        }
    }
    ctx.r3.u64 = 0;
}

// ---- Winsock events: a network thread polls these; don't let it spin --------
// DWORD WSAWaitForMultipleEvents(cEvents r3, lphEvents r4, fWaitAll r5, dwTimeout r6, fAlertable r7)
PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents)
{
    ImportLog("NetDll_WSAWaitForMultipleEvents");
    uint32_t timeoutMs = ctx.r6.u32;
    uint32_t sleepMs = timeoutMs == 0xFFFFFFFFu ? 50 : (timeoutMs > 50 ? 50 : timeoutMs);
    if (sleepMs == 0) sleepMs = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    ctx.r3.u64 = 0x102; // WSA_WAIT_TIMEOUT
}
