#pragma once
// Xenia game patches (<title id> - <name>.patch.toml, the format of Xenia Canary's game-patches) for the launcher's
// patch manager (2026-10-23). A patch is a list of writes to the game's memory image; Xenia applies them while
// the game runs, the PC version applies them before the recompile (hc09-build --patches, XenonRecomp
// patch_writes_file_path) and to the game image the game loads. Supported write types: be8, be16, be32, be64, f32,
// f64, string, u16string, array.
#include <cstdint>
#include <string>
#include <vector>

struct XeniaWrite
{
    uint32_t address = 0;
    std::vector<uint8_t> bytes;   // big-endian, as they land in memory
};

struct XeniaPatch
{
    std::string name, desc, author;
    bool enabled = true;          // the file's is_enabled (the default before the player chooses)
    std::vector<XeniaWrite> writes;
};

struct XeniaPatchFile
{
    std::wstring path, fileName;
    std::string titleName, titleId;
    std::vector<std::string> hashes;
    std::vector<XeniaPatch> patches;
    std::string error;            // empty: read fine
};

// Reads one .patch.toml; false (and out.error) when it is not a readable patch file.
bool LoadXeniaPatchFile(const std::wstring& path, XeniaPatchFile& out);

// The writes as hc09-build and XenonRecomp take them: "<hex address> <hex bytes>" lines, '#' comments.
std::string XeniaWritesText(const std::vector<std::pair<const XeniaPatchFile*, const XeniaPatch*>>& patches);

// Applies the writes in that text to a memory image loaded at `base` (headcoach.basefile: 0x82000000).
bool ApplyXeniaWrites(const std::string& text, std::vector<uint8_t>& image, uint32_t base, std::string& err);
