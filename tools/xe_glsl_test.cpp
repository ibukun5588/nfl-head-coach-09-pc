// xe-glsl-test: translate every captured shader in a directory to GLSL and
// compile it to SPIR-V (checks the translator without rendering).
//
//   xe-glsl-test <captures/frame_N/shaders> [--dump <dir>]
//
// Reads {vs,ps}_<hash>.bin (big-endian microcode as captured); with --dump,
// writes <hash>.glsl for every shader (and always for failures).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vk_shader_compile.h"
#include "xe_glsl.h"
#include "xenos.h"

namespace xenos {
uint32_t HostReg(uint32_t) { return 0; }
const uint32_t* HostRegFile(bool*) { return nullptr; }
const uint8_t* HostPhys(uint32_t) { static uint8_t zero[4096]; return zero; }
uint64_t HostSwapCount() { return 0; }
void HostWatchWrites(uint32_t, uint32_t) {}
uint64_t HostTrackWrites(uint32_t, uint32_t) { return 0; }   // replay memory never changes
bool HostWrittenSince(uint32_t, uint32_t, uint64_t&) { return false; }
}  // namespace xenos

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <shaders-dir> [--dump <dir>]\n", argv[0]); return 2; }
    std::string dump = argc > 3 && !strcmp(argv[2], "--dump") ? argv[3] : "";
    namespace fs = std::filesystem;
    int ok = 0, failed = 0, unsupported = 0, dispatch = 0;
    for (const auto& e : fs::directory_iterator(argv[1]))
    {
        std::string name = e.path().filename().string();
        if (e.path().extension() != ".bin" || name.size() < 4) continue;
        bool vs = name.rfind("vs_", 0) == 0;
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        xenos::ShaderCode code = xenos::ShaderFromBigEndian(vs ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel,
                                                            bytes.data(), uint32_t(bytes.size() / 4));
        hc09vk::TranslatedShader t = hc09vk::TranslateShader(code);
        std::vector<uint32_t> spirv;
        std::string log;
        bool compiled = t.error.empty() &&
                        hc09vk::CompileGlsl(t.glsl, vs ? hc09vk::Stage::kVertex : hc09vk::Stage::kFragment, name.c_str(), spirv, &log);
        if (compiled) ++ok; else ++failed;
        if (t.unsupported) ++unsupported;
        if (t.dispatcher) ++dispatch;
        if (!compiled || !dump.empty())
        {
            std::string out = (dump.empty() ? "." : dump) + "/" + e.path().stem().string() + ".glsl";
            std::ofstream(out) << t.glsl << "\n/* disassembly:\n" << xenos::Disassemble(code) << "*/\n";
        }
        if (!compiled)
            fprintf(stderr, "FAIL %s: %s\n", name.c_str(), t.error.empty() ? log.substr(0, 600).c_str() : t.error.c_str());
        else if (t.unsupported)
            fprintf(stderr, "note %s: unsupported features 0x%X\n", name.c_str(), t.unsupported);
    }
    fprintf(stderr, "%d compiled, %d failed (%d with unsupported features, %d with non-linear control flow)\n", ok, failed,
            unsupported, dispatch);
    return failed ? 1 : 0;
}
