// What the rest of the runtime calls in the development tools when a build leaves them out (CMakeLists.txt):
//   - the release build (HC09_DEV_TOOLS=OFF, docs/HANDOFF_M11.md step 1): the gameplay tools, the generated
//     diagnostics and the profiler are not compiled at all;
//   - the 1.2 build (HC09_GAME_VERSION=1.2): the gameplay tools and the generated diagnostics name 1.0's functions.
// Everything here answers "off".
#include "runtime.h"
#include "dev_env.h"
#include "overlay.h"
#include "profiler.h"

#include <cstdio>
#include <cstdlib>

// hooks_d3d.cpp (HC09_TRACE_D3D, 1.0's D3D function list): no D3D call census or timing.
int D3DTraceEnter(uint32_t, PPCContext&) { return -1; }
void D3DTraceLeave(int) {}

// hooks_gameplay.cpp (HC09_TWEAK live tweaker, its in-game overlay): nothing to draw.
void OverlayBuild(std::vector<OvVtx>&, float, float, float, float) {}

#if !HC09_DEV_TOOLS
// profiler.cpp (HC09_PROFILE): no sampler.
void ProfilerRegisterThread(uint64_t, const char*) {}
void ProfilerStart() {}
#endif

#if HC09_DEV_TOOLS && HC09_GAME_V12
namespace {
// Say so once when one of the left-out tools is asked for.
struct V12Note
{
    V12Note()
    {
        for (const char* e : { "HC09_TWEAK", "HC09_BLOCK_LOG", "HC09_SLIDERS", "HC09_SKILL", "HC09_SIM_LOG",
                               "HC09_AUTOWIN_SIM", "HC09_TRACE_D3D", "HC09_TRACE_SIM", "HC09_TRACE_MUSIC" })
            if (getenv(e))
                fprintf(stderr, "[runtime] %s: not in the 1.2 build yet (its 1.0 addresses are not mapped)\n", e);
    }
} g_note;
}
#endif
