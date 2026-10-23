#pragma once
// hc09-d3d-replay: shared types of the D3D-capture reader (tools/d3d_replay.cpp)
// and the native renderer front end (tools/d3d_render.cpp).
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "d3d_state.h"

// Sparse guest physical memory built from the capture's 'P' records.
struct Memory : d3dn::GuestMemory
{
    std::unordered_map<uint32_t, std::vector<uint8_t>> pages;
    uint32_t Load32(uint32_t va) const override { return U32(va); }
    void Put(uint32_t page, const uint8_t* data) { pages[page & 0x1FFFF000u].assign(data, data + 4096); }
    uint8_t Byte(uint32_t va) const
    {
        auto it = pages.find(va & 0x1FFFF000u);
        return it == pages.end() ? 0 : it->second[va & 0xFFF];
    }
    uint32_t U32(uint32_t va) const
    {
        return (uint32_t(Byte(va)) << 24) | (uint32_t(Byte(va + 1)) << 16) | (uint32_t(Byte(va + 2)) << 8) | Byte(va + 3);
    }
    void Write(uint32_t va, const uint8_t* data, uint32_t n)
    {
        for (uint32_t i = 0; i < n; ++i)
        {
            auto& pg = pages[(va + i) & 0x1FFFF000u];
            if (pg.empty()) pg.assign(4096, 0);
            pg[(va + i) & 0xFFF] = data[i];
        }
    }
    std::vector<uint8_t> Bytes(uint32_t va, uint32_t n) const
    {
        std::vector<uint8_t> v(n);
        for (uint32_t i = 0; i < n; ++i) v[i] = Byte(va + i);
        return v;
    }
};

// One recorded call ('C').
struct Call
{
    uint32_t fn = 0, depth = 0, lr = 0, r[8] = {}, ret = 0;
    double f1 = 0, f2 = 0;
    std::map<uint32_t, std::vector<uint8_t>> blobs;
    const std::vector<uint8_t>* Blob(uint32_t tag) const
    {
        auto it = blobs.find(tag);
        return it == blobs.end() ? nullptr : &it->second;
    }
};

// Receives the frame as the reader walks it.
struct ReplaySink
{
    virtual ~ReplaySink() = default;
    virtual void Page(uint32_t page, const uint8_t* data) = 0;          // a 'P' record
    virtual void Write(uint32_t va, const uint8_t* data, uint32_t n) = 0;   // memory the reader patched (shader microcode)
    // A call by the game other than a draw (depth 0), with the device before it.
    virtual void Event(const Call& c, const uint8_t* device, const Memory& mem) = 0;
    // A draw call (depth 0): the device before and after it.
    virtual void Draw(const Call& c, const uint8_t* before, const uint8_t* after, const Memory& mem) = 0;
    // A 'V' record: sub_8232BD98 patched a vertex shader (in place or into a ring copy).
    // `device`: the device of the draw being flushed (guest address).
    virtual void Patched(uint32_t device, uint32_t vs, uint32_t variant, uint32_t dst, const uint8_t* code, uint32_t bytes,
                         const Memory& mem) = 0;
    // The main device (physical address, from the capture header).
    virtual void MainDevice(uint32_t pa) = 0;
    // D3D submitted the commands so far ('K'): memory now holds what the GPU reads for them.
    virtual void Kick() = 0;
    virtual int Finish() = 0;   // end of the capture: write the outputs; exit code
    virtual void Pass(int) {}   // --repeat: pass n (1, 2, ...) of the call stream starts
};

// tools/d3d_render.cpp (nullptr without Vulkan or on failure).
// bench > 0: replay the frame bench more times and print ms/frame (d3d_render.cpp).
ReplaySink* CreateNativeSink(const char* outDir, int bench = 0);
