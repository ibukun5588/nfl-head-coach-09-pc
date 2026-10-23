// hc09.ini -> HC09_* environment variables (see settings.h, hc09.ini.example).
//
//   [game]    root       game files folder (default.xex's folder; --root wins)
//             cache      HC09_CACHE_DIR    saves and caches (default hc09-cache; the 1.2 build hc09-cache-v12)
//             saves      HC09_SAVES_DIR    the saves instead in <saves>\1.0 or \1.2 (installs: Documents\...\Saves)
//   [display] live       HC09_GPU_LIVE     render on the GPU in a window
//             fullscreen HC09_FULLSCREEN   start in borderless fullscreen
//             vsync      HC09_VSYNC        wait for the display when presenting
//             exact      HC09_GPU_EXACT    exact (slow) shaders
//             framerate  HC09_FRAMERATE    35 (the game's own), 30, 60, ...
//             renderer   HC09_RENDERER     native (default) or emulated
//             scale      HC09_SCALE        native renderer: resolution x1 (720p) .. x4
//             msaa       HC09_NATIVE_MSAA  native renderer: 0 off, 1 as the game asks, 2 at most 2x
//   [audio]   output     HC09_AUDIO_OUT    play the game's sound
//             volume     HC09_AUDIO_VOLUME 0.0 .. 1.0
//   [input]   host       HC09_HOST_INPUT   controller and keyboard
//             controller HC09_PAD_API      auto, sdl (PlayStation and other pads) or xinput
//             glyphs     HC09_GLYPHS       button icons: auto (the pad in use), playstation, xbox
//             pad        HC09_PAD_NAME     the controller to use: auto, an SDL pad's name, or xinput:N
//   [env]     NAME = value                 any other HC09_* knob, verbatim (development build only)
//
// Booleans: 1/0, on/off, yes/no, true/false. "auto" (or an empty value)
// leaves the runtime's default. Lines starting with ; or # are comments.
#include "dev_env.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

std::string Trim(std::string s)
{
    auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), sp));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), sp).base(), s.end());
    return s;
}

std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void SetDefault(const std::string& name, const std::string& value)
{
    if (const char* e = getenv(name.c_str()); e && *e) return;   // the environment wins
#if defined(_WIN32)
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

struct Known { const char* key; const char* env; bool boolean; };
constexpr Known kKnown[] = {
    { "display.live", "HC09_GPU_LIVE", true },       { "display.fullscreen", "HC09_FULLSCREEN", true },
    { "display.vsync", "HC09_VSYNC", true },         { "display.exact", "HC09_GPU_EXACT", true },
    { "display.framerate", "HC09_FRAMERATE", false }, { "display.renderer", "HC09_RENDERER", false },
    { "display.scale", "HC09_SCALE", false },         { "display.msaa", "HC09_NATIVE_MSAA", false },
    { "audio.output", "HC09_AUDIO_OUT", true },      { "audio.volume", "HC09_AUDIO_VOLUME", false },
    { "input.host", "HC09_HOST_INPUT", true },        { "input.controller", "HC09_PAD_API", false },
    { "input.glyphs", "HC09_GLYPHS", false },         { "game.cache", "HC09_CACHE_DIR", false },
    { "input.pad", "HC09_PAD_NAME", false },          { "game.saves", "HC09_SAVES_DIR", false },
};

}  // namespace

void SettingsLoad(std::string* root)
{
    const char* path = Hc09Env("HC09_CONFIG");
    if (!path || !*path) path = "hc09.ini";
    std::ifstream in(path);
    if (!in)
    {
        fprintf(stderr, "[settings] no %s (defaults and environment only)\n", path);
        return;
    }
    std::string line, section;
    int lineNo = 0, applied = 0;
    while (std::getline(in, line))
    {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { section = Lower(Trim(line.substr(1, line.size() - 2))); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) { fprintf(stderr, "[settings] %s:%d: no '=': ignored\n", path, lineNo); continue; }
        std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (section == "env")
        {
#if !HC09_DEV_TOOLS
            // The release build has only the settings above (dev_env.h): the development knobs are not in it.
            fprintf(stderr, "[settings] %s:%d: [env] %s: development setting, ignored\n", path, lineNo, key.c_str());
            continue;
#endif
            SetDefault(key, value);
            ++applied;
            continue;
        }
        std::string full = section + "." + Lower(key);
        if (full == "game.root")
        {
            if (root && root->empty()) *root = value;
            ++applied;
            continue;
        }
        const Known* k = nullptr;
        for (const Known& c : kKnown) if (full == c.key) k = &c;
        if (!k) { fprintf(stderr, "[settings] %s:%d: unknown setting %s: ignored\n", path, lineNo, full.c_str()); continue; }
        std::string v = Lower(value);
        if (v.empty() || v == "auto") continue;
        if (k->boolean)
        {
            if (v == "1" || v == "on" || v == "yes" || v == "true") value = "1";
            else if (v == "0" || v == "off" || v == "no" || v == "false") value = "0";
            else { fprintf(stderr, "[settings] %s:%d: %s wants on/off, not \"%s\": ignored\n", path, lineNo, full.c_str(), value.c_str()); continue; }
        }
        SetDefault(k->env, value);
        ++applied;
    }
    fprintf(stderr, "[settings] %s: %d setting(s) read (the environment overrides them)\n", path, applied);
}
