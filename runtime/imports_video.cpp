// Video driver (Vd*) and config surface — enough truthful answers for the
// renderer to initialize. No GPU command processing yet: VdSwap just counts
// frames so we can see the game reach its render loop.
#include "kernel.h"
#include "gpu.h"

namespace {
std::atomic<uint64_t> g_swaps{ 0 };

// Log the guest arguments of a Vd call (r3..r8) — the driver hands the kernel
// several pointers during bring-up and we want to see every one of them.
void LogVdArgs(const char* name, const PPCContext& ctx)
{
    // Several of these are per-frame once the game is running; keep the first
    // few of each so bring-up stays readable without flooding the frame loop.
    static std::unordered_map<std::string, int> seen;
    static std::mutex mu;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (++seen[name] > 4) return;
    }
    fprintf(stderr, "[video] %s(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X) from 0x%08X\n", name,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, uint32_t(ctx.lr));
}

// XVIDEO_MODE { DWORD dwDisplayWidth, dwDisplayHeight; BOOL fIsInterlaced, fIsWideScreen,
//               fIsHiDef; float RefreshRate; DWORD VideoStandard; DWORD ReservedA, ReservedB; }
void WriteVideoMode(uint32_t p)
{
    if (!p) return;
    GStore32(p + 0x00, 1280);
    GStore32(p + 0x04, 720);
    GStore32(p + 0x08, 0);          // interlaced
    GStore32(p + 0x0C, 1);          // widescreen
    GStore32(p + 0x10, 1);          // hi-def
    GStore32(p + 0x14, 0x42700000); // 60.0f
    GStore32(p + 0x18, 1);          // NTSC-M
    // The two "reserved" words are not reserved to the game: the renderer
    // divides VdGpuClockInMHz by ReservedA to size a buffer ring. A retail
    // kernel reports 0x4A / 1 here (same values Xenia uses).
    GStore32(p + 0x1C, 0x4A);
    GStore32(p + 0x20, 1);
}
}

PPC_FUNC(__imp__VdQueryVideoMode) { ImportLog("VdQueryVideoMode"); WriteVideoMode(ctx.r3.u32); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XGetVideoMode)    { ImportLog("XGetVideoMode");    WriteVideoMode(ctx.r3.u32); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdQueryVideoFlags){ ImportLog("VdQueryVideoFlags"); ctx.r3.u64 = 0x7; /* widescreen|hidef|... */ }
PPC_FUNC(__imp__XGetAVPack)       { ImportLog("XGetAVPack");       ctx.r3.u64 = 0x6; /* XC_AV_PACK_HDMI */ }

// VdGetCurrentDisplayInformation(X_DISPLAY_INFO* r3), 0x58 bytes:
//   +0x00 u16 front buffer width   +0x02 u16 front buffer height
//   +0x04 u8  front buffer color format, +0x05 u8 pixel format
//   +0x08 scaler parameters (56 bytes): source rect x1,y1,x2,y2 (u32 each),
//         scaled output width/height (u32), vertical filter type + 3 params,
//         horizontal filter type + 3 params
//   +0x40 u16 overscan left/top/right/bottom
//   +0x48 u16 display width        +0x4A u16 display height
//   +0x4C f32 refresh rate         +0x50 u32 interlaced
//   +0x54 u8  display color format +0x56 u16 actual display width
// D3D copies display width/height/actual width (+0x48/+0x4A/+0x56) into the
// device and sizes the per-present scaler from them; zeros there end in a
// divide by zero (sub_823350C8) once the front end changes the scaler.
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    ImportLog("VdGetCurrentDisplayInformation");
    uint32_t p = ctx.r3.u32;
    if (p)
    {
        for (int i = 0; i < 0x58; i += 4) GStore32(p + i, 0);
        GStore16(p + 0x00, 1280);           // front buffer
        GStore16(p + 0x02, 720);
        GStore32(p + 0x08 + 0x08, 1280);    // scaler source rect (0,0)-(1280,720)
        GStore32(p + 0x08 + 0x0C, 720);
        GStore32(p + 0x08 + 0x10, 1280);    // scaled output size
        GStore32(p + 0x08 + 0x14, 720);
        GStore16(p + 0x48, 1280);           // display
        GStore16(p + 0x4A, 720);
        GStore32(p + 0x4C, 0x42700000);     // 60.0f
        GStore32(p + 0x50, 0);              // progressive
        GStore16(p + 0x56, 1280);           // actual display width
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    ImportLog("VdGetCurrentDisplayGamma");
    if (ctx.r3.u32) GStore32(ctx.r3.u32, 2);                 // gamma type
    if (ctx.r4.u32) GStore32(ctx.r4.u32, 0x40000000);        // 2.0f ... approximate
    ctx.r3.u64 = 0;
}

// Engine/ring-buffer bring-up: acknowledge. Real command processing is M5+.
PPC_FUNC(__imp__VdInitializeEngines)        { ImportLog("VdInitializeEngines"); LogVdArgs("VdInitializeEngines", ctx); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdShutdownEngines)          { ImportLog("VdShutdownEngines");          ctx.r3.u64 = 0; }
// void VdInitializeRingBuffer(PVOID physicalBase r3, DWORD sizeLog2 r4)
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    ImportLog("VdInitializeRingBuffer");
    GpuInitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}
// void VdEnableRingBufferRPtrWriteBack(PVOID physicalAddr r3, DWORD blockSizeLog2 r4)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    ImportLog("VdEnableRingBufferRPtrWriteBack");
    GpuEnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}
// void VdSetSystemCommandBufferGpuIdentifierAddress(PVOID addr r3)
// Address the kernel's system-command-buffer packets write their progress
// identifier to. D3D programs the scratch write-back itself (SCRATCH_ADDR/
// SCRATCH_UMSK via type-0 packets), so nothing else is needed here yet.
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    ImportLog("VdSetSystemCommandBufferGpuIdentifierAddress");
    LogVdArgs("VdSetSystemCommandBufferGpuIdentifierAddress", ctx);
    if (ctx.r3.u32) GpuSetSystemIdentifierAddress(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) { ImportLog("VdInitializeScalerCommandBuffer"); LogVdArgs("VdInitializeScalerCommandBuffer", ctx); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdEnableDisableClockGating) { ImportLog("VdEnableDisableClockGating"); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)  { ImportLog("VdIsHSIOTrainingSucceeded");  ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdRetrainEDRAM)             { ImportLog("VdRetrainEDRAM");             ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker)       { ImportLog("VdRetrainEDRAMWorker");       ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdPersistDisplay)           { ImportLog("VdPersistDisplay"); LogVdArgs("VdPersistDisplay", ctx); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdSetDisplayMode)           { ImportLog("VdSetDisplayMode"); LogVdArgs("VdSetDisplayMode", ctx); ctx.r3.u64 = 0; }
// void VdSetGraphicsInterruptCallback(PFN callback r3, PVOID userData r4)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    ImportLog("VdSetGraphicsInterruptCallback");
    GpuSetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) { ImportLog("VdCallGraphicsNotificationRoutines"); LogVdArgs("VdCallGraphicsNotificationRoutines", ctx); ctx.r3.u64 = 0; }

// void VdGetSystemCommandBuffer(PVOID state r3, DWORD* value r4)
// `state` is a 0x94-byte structure in D3D's present path (sub_82333410) that
// is then passed to VdSwap; D3D reads flags and a 56-byte scaler-parameter
// block out of it (+0x1C flag, +0x20 block, +0x90). The kernel leaves it
// zeroed apart from a tag, like Xenia: state = {0xBEEF0000, 0...}, *value =
// 0xBEEF0001. (Treating `state` as a pointer-to-pointer left 144 bytes of
// stack garbage there, which became a degenerate scaler rect and a divide by
// zero in sub_823350C8 once the front end changed screens.)
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    ImportLog("VdGetSystemCommandBuffer");
    LogVdArgs("VdGetSystemCommandBuffer", ctx);
    if (uint32_t p = ctx.r3.u32)
    {
        for (uint32_t i = 0; i < 0x94; i += 4) GStore32(p + i, 0);
        GStore32(p, 0xBEEF0000);
    }
    if (ctx.r4.u32) GStore32(ctx.r4.u32, 0xBEEF0001);
    ctx.r3.u64 = 0;
}

// VdSwap(PVOID ringSpace r3, PVOID fetchConstant r4, r5, r6, r7,
//        DWORD* frontBuffer r8, DWORD* textureFormat r9, DWORD* colorSpace r10,
//        DWORD* width [sp+0x54], DWORD* height [sp+0x5C])
// Stack arguments 9+ use 8-byte slots starting at sp+0x54 (low word of each).
// The kernel formats the present into the 64 dwords of ring space the caller
// reserved; the GPU executes it as a PM4_XE_SWAP when it gets there.
PPC_FUNC(__imp__VdSwap)
{
    ImportLog("VdSwap");
    uint32_t buf = ctx.r3.u32;
    auto deref = [](uint32_t p) { return p ? GLoad32(p) : 0u; };
    uint32_t pWidth = GLoad32(ctx.r1.u32 + 0x54), pHeight = GLoad32(ctx.r1.u32 + 0x5C);
    uint32_t frontBuffer = deref(ctx.r8.u32), texFormat = deref(ctx.r9.u32), colorSpace = deref(ctx.r10.u32);
    uint32_t width = deref(pWidth), height = deref(pHeight);
    uint64_t n = g_swaps.fetch_add(1);
    if (buf) GpuWriteSwapPacket(buf, frontBuffer, texFormat, colorSpace, width, height, uint32_t(n));
    if (n < 5 || (n % 600) == 0)
        fprintf(stderr, "[video] VdSwap #%llu (frontbuffer 0x%08X fmt %u %ux%u; packet @0x%08X; gpu swaps so far %llu)\n",
                (unsigned long long)n, frontBuffer, texFormat, width, height, buf, (unsigned long long)GpuSwapCount());
    ctx.r3.u64 = 0;
}
