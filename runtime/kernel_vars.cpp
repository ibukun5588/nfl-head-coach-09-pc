// Kernel variable exports and XEX import-slot patching.
//
// The game reads these through the import slots in its image header, e.g.
//   lwz r11, slot(VdGpuClockInMHz) ; lwz r10, 0(r11)
// so each one needs real guest storage with believable contents.
#include "kernel.h"
#include "xex_imports.h"

#include <chrono>
#include <cstring>
#include <map>
#include <thread>

namespace {

// xboxkrnl.exe variable exports by ordinal (Xenia's xboxkrnl_table.inc).
struct KVar { const char* name; uint32_t size; void (*init)(uint32_t addr); };

uint32_t g_timeStampBundle = 0;

void InitTimeStampBundle(uint32_t a)
{
    g_timeStampBundle = a;
    // {u64 InterruptTime (100 ns), u64 SystemTime (100 ns since 1601), u32 TickCount (ms), u32 pad}
    // Updated by a 1 ms host thread below; games poll this instead of calling
    // KeQuerySystemTime in hot paths.
}

void InitXboxHardwareInfo(uint32_t a)
{
    GStore32(a + 0x0, 0x00000000);  // Flags (retail)
    GStore8 (a + 0x4, 6);           // NumberOfProcessors
    GStore8 (a + 0x5, 0);           // PCIBridgeRevisionID
    GStore16(a + 0xC, 0x0000);      // BldrMagic
    GStore16(a + 0xE, 0x0000);      // BldrFlags
}

void InitKrnlVersion(uint32_t a)
{
    GStore16(a + 0, 2);             // Major
    GStore16(a + 2, 0);             // Minor
    GStore16(a + 4, 17559);         // Build (2.0.17559 era kernel)
    GStore16(a + 6, 0);             // QFE
}

void InitCommandLine(uint32_t a)
{
    const char* cmd = "default.xex";
    for (size_t i = 0; cmd[i]; ++i) GStore8(a + uint32_t(i), uint8_t(cmd[i]));
}

void InitGpuClock(uint32_t a) { GStore32(a, 500); }   // Xenos core clock, MHz

void InitModuleHandle(uint32_t a)
{
    // XexExecutableModuleHandle: pointer to the executable's LDR entry. We
    // only need the handle to be a stable non-null pointer whose +0x58 field
    // (image base, read by XexGetModuleHandle users) is sane.
    uint32_t ldr = GuestAlloc(0x100, 16);
    GStore32(ldr + 0x58, PPC_IMAGE_BASE);
    GStore32(a, ldr);
}

void InitZero(uint32_t) {}

const std::map<uint32_t, KVar> kKernelVars = {
    { 0x00E, { "ExEventObjectType",          0x40,  InitZero } },
    { 0x01B, { "ExThreadObjectType",         0x40,  InitZero } },
    { 0x059, { "KeDebugMonitorData",         0x40,  InitZero } },
    { 0x0AD, { "KeTimeStampBundle",          0x20,  InitTimeStampBundle } },
    { 0x156, { "XboxHardwareInfo",           0x10,  InitXboxHardwareInfo } },
    { 0x158, { "XboxKrnlVersion",            0x08,  InitKrnlVersion } },
    { 0x193, { "XexExecutableModuleHandle",  0x04,  InitModuleHandle } },
    { 0x1AE, { "ExLoadedCommandLine",        0x400, InitCommandLine } },
    { 0x1BE, { "VdGlobalDevice",             0x04,  InitZero } },
    { 0x1C0, { "VdGpuClockInMHz",            0x04,  InitGpuClock } },
    { 0x1C1, { "VdHSIOCalibrationLock",      0x1C,  InitZero } },
    { 0x266, { "KeCertMonitorData",          0x40,  InitZero } },
};

std::chrono::steady_clock::time_point g_timeBase = std::chrono::steady_clock::now();

void TimeStampThread()
{
    using namespace std::chrono;
    const auto t0 = g_timeBase;   // set at startup, shared with KernelInterruptTimeNs
    for (;;)
    {
        auto now = steady_clock::now();
        uint64_t interrupt100ns = duration_cast<nanoseconds>(now - t0).count() / 100;
        uint64_t sys100ns = uint64_t(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100)
                            + 116444736000000000ULL;   // Unix epoch -> 1601 epoch
        GStore64(g_timeStampBundle + 0x0, interrupt100ns);
        GStore64(g_timeStampBundle + 0x8, sys100ns);
        GStore32(g_timeStampBundle + 0x10, uint32_t(interrupt100ns / 10000));
        std::this_thread::sleep_for(milliseconds(1));
    }
}

} // namespace

// Nanoseconds on the KeTimeStampBundle clock (InterruptTime / TickCount epoch).
uint64_t KernelInterruptTimeNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_timeBase).count());
}

void PatchXexImports()
{
    // Pass 1: which (lib, ordinal) pairs have a code thunk (= functions).
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> thunkOf;
    for (uint32_t i = 0; i < g_xexImportSlotCount; ++i)
    {
        const auto& s = g_xexImportSlots[i];
        uint32_t v = GLoad32(s.va);
        if ((v >> 24) == 1) thunkOf[{ s.lib, v & 0xFFFF }] = s.va;
    }
    // Pass 2: fill every address slot.
    int vars = 0, funcs = 0, unknown = 0;
    for (uint32_t i = 0; i < g_xexImportSlotCount; ++i)
    {
        const auto& s = g_xexImportSlots[i];
        uint32_t v = GLoad32(s.va);
        if ((v >> 24) != 0) continue;                 // code thunk, handled by the recompiler
        uint32_t ordinal = v & 0xFFFF;
        const char* lib = s.lib < g_xexImportLibCount ? g_xexImportLibs[s.lib] : "?";
        auto th = thunkOf.find({ s.lib, ordinal });
        if (th != thunkOf.end()) { GStore32(s.va, th->second); ++funcs; continue; }

        // A variable import.
        uint32_t addr; const char* name = "(unknown variable)";
        bool krnl = std::strcmp(lib, "xboxkrnl.exe") == 0;
        auto it = krnl ? kKernelVars.find(ordinal) : kKernelVars.end();
        if (it != kKernelVars.end())
        {
            addr = GuestAlloc(it->second.size, 16);
            it->second.init(addr);
            name = it->second.name; ++vars;
        }
        else { addr = GuestAlloc(0x100, 16); ++unknown; }
        GStore32(s.va, addr);
        fprintf(stderr, "[xex] variable import %s!%u (ordinal 0x%03X) -> %s @ 0x%08X (slot 0x%08X)\n",
                lib, ordinal, ordinal, name, addr, s.va);
    }
    fprintf(stderr, "[xex] import slots patched: %d functions, %d variables, %d unknown\n", funcs, vars, unknown);
    if (g_timeStampBundle) std::thread(TimeStampThread).detach();
}
