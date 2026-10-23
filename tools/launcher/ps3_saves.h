#pragma once
// PS3 (RPCS3) saves <-> the port's saves, in the launcher (docs/HANDOFF_M11.md step 7): the C++ port of tools/ps3save.py
// (see it for the formats). Inputs are only read; a port save that is replaced is backed up first to
// <cache>\content-backup\<time>\, and an existing RPCS3 save folder is never replaced.
#include <string>
#include <vector>

struct Ps3SaveInfo
{
    std::wstring folder;   // the BLUS30128-<NAME> folder
    std::wstring name;     // the port's name for it (<TYPE>-<NAME>)
    bool usable = false;   // HED-DATA + USR-DATA present (a console copy with PARAM.PFD is still encrypted)
};

// The NFL Head Coach 09 saves in an RPCS3 savedata folder, or the one save `folder` is.
std::vector<Ps3SaveInfo> Ps3ListSaves(const std::wstring& folder);

// One PS3 save into <cache>\content\...\<NAME>\<NAME> (MC02). exists: a port save of that name is there (nothing is
// written unless `replace`, and then the old one is backed up first).
enum class Ps3Import { Imported, Exists, Failed };
Ps3Import Ps3ImportSave(const Ps3SaveInfo& save, const std::wstring& cache, bool replace, std::wstring& msg);

// The port's saves in <cache> (names).
std::vector<std::wstring> PortListSaves(const std::wstring& cache);

// A port save into <outParent>\BLUS30128-<NAME>\ (HED-DATA, USR-DATA, and PARAM.SFO/ICON0.PNG from `templ`, one of the
// player's own PS3 saves of the game).
bool Ps3ExportSave(const std::wstring& cache, const std::wstring& name, const std::wstring& outParent,
                   const std::wstring& templ, std::wstring& msg);
