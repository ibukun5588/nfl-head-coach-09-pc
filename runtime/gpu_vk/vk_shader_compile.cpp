#include "../dev_env.h"
#include "vk_shader_compile.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#include <shaderc/shaderc.h>

namespace hc09vk {

bool CompileGlsl(const std::string& source, Stage stage, const char* name, std::vector<uint32_t>& spirv, std::string* log)
{
    static std::mutex mu;
    static shaderc_compiler_t compiler = nullptr;
    std::lock_guard<std::mutex> lk(mu);
    if (!compiler) compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    // No spirv-opt by default: the driver optimizes anyway, and spirv-opt took
    // ~0.3 s per translated shader (30 of the 37 s of an in-game GPU replay)
    // for an identical image. HC09_SHADER_OPT=1 turns it on.
    static const bool optimize = [] { const char* e = Hc09Env("HC09_SHADER_OPT"); return e && strcmp(e, "0"); }();
    shaderc_compile_options_set_optimization_level(opts, optimize ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);
    shaderc_shader_kind kind = stage == Stage::kVertex ? shaderc_vertex_shader
                             : stage == Stage::kGeometry ? shaderc_geometry_shader
                             : stage == Stage::kFragment ? shaderc_fragment_shader : shaderc_compute_shader;
    shaderc_compilation_result_t r =
        shaderc_compile_into_spv(compiler, source.data(), source.size(), kind, name ? name : "shader", "main", opts);
    bool ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
    if (ok)
    {
        const uint32_t* words = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(r));
        spirv.assign(words, words + shaderc_result_get_length(r) / 4);
    }
    if (log) *log = shaderc_result_get_error_message(r);
    shaderc_result_release(r);
    shaderc_compile_options_release(opts);
    return ok;
}

}  // namespace hc09vk
