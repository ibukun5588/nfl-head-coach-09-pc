#pragma once
// Button prompt icons for the controller in use. The game's icon library
// (qkl_boot.ast 83) holds Xbox and PlayStation icons, small DXT5 APT bitmaps;
// the 360 build only shows the Xbox ones. tools/playstation_icons.py pairs them
// into <cache>/glyphs/playstation.bin, keyed by a hash of each Xbox icon's DXT
// blocks. The native renderer asks here after decoding a texture.
//
// HC09_GLYPHS (hc09.ini [input] glyphs): auto (default: PlayStation icons while
// the last controller used is a PlayStation pad), playstation, xbox.
#include <cstdint>

#include "xenos/xenos_texture.h"

// Registered with the native renderer's texture override (vk_native.h) at start.
//
// The glyph set textures are drawn with now: 0 = the game's own (Xbox), 1 =
// PlayStation. Part of the native texture cache key for glyph candidates, so a
// change re-decodes only those. Cheap; any thread.
uint32_t GlyphSetActive();
// Whether the texture could be an icon (2D DXT5, tiled, at most 64x64): only
// these are hashed.
bool GlyphCandidate(const xenos::TextureFetch& tf);
// Replace `img` (mip 0 of `tf`, as decoded) with glyph set `set`'s icon when the
// texture is one of the Xbox icons; true if it did.
bool GlyphReplace(const xenos::TextureFetch& tf, uint32_t set, xenos::Image& img);
