#pragma once
#include <string>

// <cache>\glyphs\playstation.bin from the game folder's qkl_boot.ast and qkl_interface.ast (tools/launcher/ps_icons.cpp,
// the port of tools/playstation_icons.py). `count`: icons written.
bool MakePlayStationIcons(const std::wstring& root, const std::wstring& outFile, std::string& err, int* count = nullptr);
