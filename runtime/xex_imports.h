#pragma once
// XEX import slots, generated into out/xex_imports.cpp by tools/xeximports.py.
// Every imported symbol owns one address slot in the image header (the XEX
// "thunk data"): the kernel writes the export's address there at load.
// Function imports additionally have a code thunk that XenonRecomp turned
// into the __imp__ functions; variable imports (VdGpuClockInMHz, ...) only
// have the slot, and the game dereferences it directly.
#include <cstdint>

struct XexImportSlot
{
    uint32_t va;    // guest address of the 4-byte slot
    uint32_t lib;   // index into g_xexImportLibs
};
extern const char* const g_xexImportLibs[];
extern const uint32_t    g_xexImportLibCount;
extern const XexImportSlot g_xexImportSlots[];
extern const uint32_t      g_xexImportSlotCount;

// Resolve all slots: variables get runtime-backed storage, function slots
// get the address of their code thunk. Call after the image is loaded.
void PatchXexImports();
