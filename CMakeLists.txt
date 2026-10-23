cmake_minimum_required(VERSION 3.20)
project(hc09recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Python for the generator scripts (python3 on Linux; on Windows pass
# -DPython3_EXECUTABLE=... for an interpreter that has `cryptography`).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# clang++ (Linux) or clang-cl (Windows): same compiler, MSVC-style driver flags on Windows.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(HC09_CL ON)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)   # fopen/getenv are fine here
else()
  set(HC09_CL OFF)
endif()

# Path to the XenonRecomp checkout (for ppc_context.h and the SIMDe submodule).
set(XENON_DIR "${CMAKE_SOURCE_DIR}/../XenonRecomp" CACHE PATH "XenonRecomp root")

# HC09_GAME_VERSION: which executable the build is for (docs/HANDOFF_M10_5.md): 1.0, the disc's default.xex
# (out/, build-run/), or 1.2, the Xbox 360 title update applied to it (default-v12.xex, out-v12/, build-v12/;
# tools/build.py --game-version 1.2). One source tree: the runtime sees HC09_GAME_V12 (0 or 1).
set(HC09_GAME_VERSION "1.0" CACHE STRING "Game version the build is for: 1.0 (the disc) or 1.2 (title update)")
set_property(CACHE HC09_GAME_VERSION PROPERTY STRINGS 1.0 1.2)
if(HC09_GAME_VERSION STREQUAL "1.0")
  set(HC09_GAME_V12 0)
  set(_hc09_out "out")
  set(_hc09_xex "default.xex")
elseif(HC09_GAME_VERSION STREQUAL "1.2")
  set(HC09_GAME_V12 1)
  set(_hc09_out "out-v12")
  set(_hc09_xex "default-v12.xex")
else()
  message(FATAL_ERROR "HC09_GAME_VERSION=${HC09_GAME_VERSION}: 1.0 or 1.2")
endif()
# HC09_XEX: the xex the import slots are read from (the one out/ was recompiled from).
set(HC09_XEX "${CMAKE_SOURCE_DIR}/${_hc09_xex}" CACHE FILEPATH "The game's xex for this version (import slots)")
message(STATUS "hc09: game version ${HC09_GAME_VERSION} (${HC09_XEX})")

# HC09_OUT: the recompiled code's folder. `out` is the build; tools/build.py --variant <name> recompiles
# with other recompiler options into out-<name> and builds it in build-<name> (experiments side by side).
set(HC09_OUT "${_hc09_out}" CACHE STRING "Folder of the recompiled code (out, out-v12, or out-<variant>)")
if(HC09_GAME_V12 AND HC09_OUT STREQUAL "out")
  message(FATAL_ERROR "HC09_GAME_VERSION=1.2 with HC09_OUT=out: out/ is 1.0's recompiled code (use out-v12)")
endif()
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${HC09_OUT}/ppc_recomp.*.cpp")

# Hand-written import implementations: every runtime/imports_*.cpp except the
# generated one. The generated stub file is rebuilt whenever the function
# mapping or any hand-written import file changes, so a plain `ninja` can
# never link stale stubs against new manual implementations.
set(IMPORTS_GEN "${CMAKE_SOURCE_DIR}/runtime/imports_gen.cpp")
set(IMPORTS_GEN_OUT "")
if(HC09_GAME_V12)   # its own stubs in its build folder; runtime/imports_gen.cpp stays 1.0's
  set(IMPORTS_GEN "${CMAKE_BINARY_DIR}/imports_gen.cpp")
  set(IMPORTS_GEN_OUT "${CMAKE_SOURCE_DIR}/runtime/imports_manual.cpp" "${IMPORTS_GEN}")
endif()
# CONFIGURE_DEPENDS: re-scan at build time so a newly added imports_*.cpp is
# picked up by a plain `ninja` without an explicit re-configure.
file(GLOB MANUAL_IMPORTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/imports_*.cpp")
list(REMOVE_ITEM MANUAL_IMPORTS "${CMAKE_SOURCE_DIR}/runtime/imports_gen.cpp")
add_custom_command(
    OUTPUT "${IMPORTS_GEN}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/runtime/gen_imports.py"
            "${CMAKE_SOURCE_DIR}/${HC09_OUT}/ppc_func_mapping.cpp" ${IMPORTS_GEN_OUT}
    DEPENDS "${CMAKE_SOURCE_DIR}/${HC09_OUT}/ppc_func_mapping.cpp"
            "${CMAKE_SOURCE_DIR}/runtime/gen_imports.py"
            ${MANUAL_IMPORTS}
    COMMENT "Generating import stubs (gen_imports.py)")

# XEX import-slot table (variable imports such as VdGpuClockInMHz), derived
# from the original XEX header.
set(XEX_IMPORTS "${CMAKE_SOURCE_DIR}/${HC09_OUT}/xex_imports.cpp")
add_custom_command(
    OUTPUT "${XEX_IMPORTS}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/xeximports.py"
            "${HC09_XEX}" "${XEX_IMPORTS}"
    DEPENDS "${HC09_XEX}" "${CMAKE_SOURCE_DIR}/tools/xeximports.py"
    COMMENT "Extracting XEX import slots (xeximports.py)")

set(RUNTIME_SOURCES
    runtime/runtime.cpp
    runtime/kernel_vars.cpp
    runtime/gpu.cpp
    runtime/hooks_diag.cpp
    runtime/hooks_music.cpp
    runtime/hooks_sim.cpp
    runtime/hooks_autowin.cpp
    runtime/hooks_simlog.cpp
    runtime/hooks_gameplay.cpp
    runtime/hooks_pacing.cpp
    runtime/interp.cpp
    runtime/hooks_d3d.cpp
    runtime/hooks_native.cpp
    runtime/xenos/d3d_state.cpp
    runtime/xenos/d3d_frontend.cpp
    runtime/host_input.cpp
    runtime/glyphs.cpp
    runtime/host_audio.cpp
    runtime/settings.cpp
    runtime/crash_report.cpp
    runtime/profiler.cpp
    runtime/xenos/xenos_capture.cpp
    runtime/xenos/xenos_texture.cpp
    runtime/xenos/xenos_shader.cpp
    runtime/xenos/xenos_raster.cpp
    "${IMPORTS_GEN}"
    "${XEX_IMPORTS}"
    ${MANUAL_IMPORTS}
    ${HC09_OUT}/ppc_func_mapping.cpp)
# HC09_DEV_TOOLS (docs/HANDOFF_M11.md step 1): OFF is the release build. It leaves the development tools out of
# hc09.exe at compile time: the gameplay tools (live tweaker and overlay, loggers, auto-win), the generated trace
# hooks, the profiler, and every HC09_* switch except hc09.ini's settings (runtime/dev_env.h: the code behind the
# others is compiled out). The test benches (hc09-replay, hc09-d3d-replay, xe-*-test) are not built either.
option(HC09_DEV_TOOLS "Development tools in hc09.exe (OFF: the release build)" ON)
message(STATUS "hc09: development tools ${HC09_DEV_TOOLS}")
if(HC09_GAME_V12 OR NOT HC09_DEV_TOOLS)
  # 1.2: the gameplay tools stay out until their addresses are mapped, the generated diagnostics (1.0's function
  # lists) too; the release build leaves them out altogether. runtime/tools_stubs.cpp answers for them.
  # tools/gen_addr.py's EXCLUDED is the same list.
  list(REMOVE_ITEM RUNTIME_SOURCES runtime/hooks_gameplay.cpp runtime/hooks_simlog.cpp runtime/hooks_autowin.cpp
       runtime/hooks_sim.cpp runtime/hooks_music.cpp runtime/hooks_d3d.cpp)
  list(APPEND RUNTIME_SOURCES runtime/tools_stubs.cpp)
endif()
if(NOT HC09_DEV_TOOLS)
  list(REMOVE_ITEM RUNTIME_SOURCES runtime/profiler.cpp)
endif()
# HC09_SPLIT (docs/HANDOFF_M11.md step 2, runtime/split_abi.h): the release runtime as a DLL we build and ship,
# hc09rt.dll, without the recompiled code; the player's PC compiles out/ and links it with runtime/split_glue.cpp into
# hc09.exe using clang-cl and lld-link alone (tools/player_build.py). Release build only.
option(HC09_SPLIT "Build the runtime as hc09rt.dll (the recompiled code is linked on the player's PC)" OFF)
if(HC09_SPLIT)
  list(REMOVE_ITEM RUNTIME_SOURCES ${HC09_OUT}/ppc_func_mapping.cpp)
  list(APPEND RUNTIME_SOURCES runtime/split_dll.cpp)   # a hand-written source (1.2: game_addr_v12.h names)
endif()
# The hand-written sources: the runtime minus the generated files.
set(HC09_HAND_SOURCES ${RUNTIME_SOURCES})
list(REMOVE_ITEM HC09_HAND_SOURCES "${IMPORTS_GEN}" "${XEX_IMPORTS}" ${HC09_OUT}/ppc_func_mapping.cpp)

if(HC09_SPLIT)
  if(HC09_DEV_TOOLS OR NOT HC09_CL)
    message(FATAL_ERROR "HC09_SPLIT needs the release build (HC09_DEV_TOOLS=OFF) and clang-cl")
  endif()
  add_library(hc09 SHARED ${RUNTIME_SOURCES})
  set_target_properties(hc09 PROPERTIES OUTPUT_NAME hc09rt)
  target_compile_definitions(hc09 PRIVATE HC09_SPLIT=1)
  target_link_libraries(hc09 PRIVATE shell32)   # CommandLineToArgvW
  find_program(HC09_LLVM_NM llvm-nm HINTS "$ENV{ProgramFiles}/LLVM/bin" REQUIRED)
  add_custom_command(TARGET hc09 PRE_LINK
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/split_def.py" "${HC09_LLVM_NM}"
              "${CMAKE_SOURCE_DIR}/runtime/split_hooks.inc" "${CMAKE_BINARY_DIR}/hc09rt.def"
              "${CMAKE_BINARY_DIR}/CMakeFiles/hc09.dir" $<$<BOOL:${HC09_GAME_V12}>:${CMAKE_SOURCE_DIR}/runtime/game_addr_v12.h>
      COMMENT "hc09rt.dll exports (split_def.py)")
  target_link_options(hc09 PRIVATE "/DEF:${CMAKE_BINARY_DIR}/hc09rt.def")
  message(STATUS "hc09: split build: hc09rt.dll (runtime only)")
else()
  add_executable(hc09 ${PPC_SOURCES} ${RUNTIME_SOURCES})
endif()
# The runtime's sources only (the recompiled TUs' command line stays as it was: no rebuild of them in build-run).
set_property(SOURCE ${RUNTIME_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS HC09_GAME_V12=${HC09_GAME_V12})
if(NOT HC09_DEV_TOOLS)   # runtime/dev_env.h defaults to 1: the development build's command lines stay as they were
  target_compile_definitions(hc09 PRIVATE HC09_DEV_TOOLS=0)
endif()
if(HC09_GAME_V12)
  # runtime/game_addr_v12.h (tools/gen_addr.py) #defines the 1.0 function names the runtime uses to 1.2's: into the
  # hand-written sources only (in ppc_func_mapping.cpp it would point guest addresses at the wrong functions).
  if(HC09_CL)
    set(_hc09_fi "/FI${CMAKE_SOURCE_DIR}/runtime/game_addr_v12.h")
  else()
    set(_hc09_fi "-include;${CMAKE_SOURCE_DIR}/runtime/game_addr_v12.h")
  endif()
  set_property(SOURCE ${HC09_HAND_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${_hc09_fi})
endif()
# The recompiled TUs only: guest_indirect.h maps mftb to the 50 MHz guest time base there.
set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_DEFINITIONS HC09_GUEST_TU)
# HC09_GUEST_ARCH: -march for the recompiled code only. Default x86-64-v3 (AVX2, BMI1/2, FMA, MOVBE; Intel
# Haswell 2013+, AMD Excavator/Zen; decided 2026-10-07): SIMDe then uses real SSSE3/SSE4 instructions (pshufb
# for every vector load/store byte swap) instead of scalar emulation, and byte-swapped loads become movbe; the
# game's code takes half the time. The way back for older CPUs: -DHC09_GUEST_ARCH=x86-64-v2 (SSE4.2) or
# x86-64 (SSE2), or empty (the compiler's default, no flags). The value is cached per build dir: reconfigure an
# existing one to change it. FP contraction off: the generated `a*b + c` (fmadds) must not become an FMA, so the
# arithmetic stays bit-identical to the plain build. runtime.cpp (compiled without -march) checks the CPU at
# startup for the level the guest code needs (HC09_GUEST_LEVEL) and exits with a message instead of faulting.
set(HC09_GUEST_ARCH "x86-64-v3" CACHE STRING "-march for the recompiled code (x86-64-v3 default; x86-64-v2, x86-64 or empty for older CPUs)")
set(HC09_GUEST_LEVEL 0)
if(HC09_GUEST_ARCH MATCHES "^x86-64-v([234])$")
  set(HC09_GUEST_LEVEL ${CMAKE_MATCH_1})
elseif(HC09_GUEST_ARCH AND NOT HC09_GUEST_ARCH STREQUAL "x86-64")
  message(WARNING "HC09_GUEST_ARCH=${HC09_GUEST_ARCH}: no startup CPU check for this -march")
endif()
message(STATUS "hc09: guest code -march=${HC09_GUEST_ARCH} (startup CPU check level ${HC09_GUEST_LEVEL})")
set_property(SOURCE runtime/runtime.cpp APPEND PROPERTY COMPILE_DEFINITIONS HC09_GUEST_LEVEL=${HC09_GUEST_LEVEL})
if(HC09_GUEST_ARCH)
  if(HC09_CL)
    set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_OPTIONS
        "/clang:-march=${HC09_GUEST_ARCH};/clang:-ffp-contract=off")
  else()
    set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_OPTIONS "-march=${HC09_GUEST_ARCH};-ffp-contract=off")
  endif()
endif()

# Build id banner: the runtime prints the git commit it was built from, so a
# run log always says which build produced it.
add_custom_target(hc09_build_id ALL
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_SOURCE_DIR}/runtime/build_id.h -DSRC=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/build_id.cmake
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/runtime/build_id.h
    COMMENT "Updating build id")
add_dependencies(hc09 hc09_build_id)

target_include_directories(hc09 PRIVATE
    "${CMAKE_SOURCE_DIR}/${HC09_OUT}"
    "${CMAKE_SOURCE_DIR}/runtime"
    "${XENON_DIR}/thirdparty/simde")

# The generated code relies on Clang-specific intrinsics/codegen.
if(HC09_CL)
  target_compile_options(hc09 PRIVATE
      /clang:-fno-strict-aliasing
      /w                     # the generated TUs are machine output; silence noise
      "/FI${CMAKE_SOURCE_DIR}/runtime/guest_indirect.h")  # safe indirect calls
  # Guest code runs native call chains on host stacks: 8 MiB like Linux's default
  # (std::thread on Windows takes the executable's default, 1 MiB otherwise).
  target_link_options(hc09 PRIVATE /STACK:8388608)
  target_link_libraries(hc09 PRIVATE winmm ole32 ws2_32)   # timeBeginPeriod; COM for WASAPI; the live tweaker
else()
  target_compile_options(hc09 PRIVATE
      -fno-strict-aliasing
      -Wno-everything        # the generated TUs are machine output; silence noise
      "-include${CMAKE_SOURCE_DIR}/runtime/guest_indirect.h")  # safe indirect calls
endif()

if(HC09_CL)
  set(HC09_O0 /Od)
  set(HC09_O2 /O2)
else()
  set(HC09_O0 -O0)
  set(HC09_O2 -O2)
endif()
# HC09_GUEST_OPT: optimization of the recompiled guest code (and the rest of
# the runtime). O2 since 2026-10-05 (BACKEND_PLAN milestone 6, decided): at -O0 the game's own code limits live play to ~23 fps, at -O2 30-36.
# -DHC09_GUEST_OPT=O0 is the bring-up/debugging build (HANDOFF §5: the -O0
# rule existed for correctness while bring-up continued; use it to rule the
# optimizer out when chasing a guest-code bug). An existing build dir keeps its
# cached value: reconfigure with -DHC09_GUEST_OPT=O2 to switch it.
set(HC09_GUEST_OPT "O2" CACHE STRING "Guest code optimization: O0 or O2")
# Gamepads through SDL3 (runtime/host_input.cpp: PlayStation, Switch and Xbox
# pads). Windows: the official SDL3-devel-<ver>-VC.zip from
# github.com/libsdl-org/SDL/releases unpacked to thirdparty/SDL3 (gitignored);
# only the headers are used at build time, SDL3.dll is loaded at run time and
# copied next to hc09.exe. Without it, controllers come through XInput alone.
set(HC09_SDL3_DIR "${CMAKE_SOURCE_DIR}/thirdparty/SDL3" CACHE PATH "SDL3 VC development package (include/, lib/x64/)")
if(WIN32 AND EXISTS "${HC09_SDL3_DIR}/include/SDL3/SDL_gamepad.h")
  target_include_directories(hc09 PRIVATE "${HC09_SDL3_DIR}/include")
  target_compile_definitions(hc09 PRIVATE HC09_HAVE_SDL3)
  add_custom_command(TARGET hc09 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HC09_SDL3_DIR}/lib/x64/SDL3.dll" "$<TARGET_FILE_DIR:hc09>")
  message(STATUS "hc09: gamepads through SDL3 (${HC09_SDL3_DIR})")
endif()
# XMA decoding (runtime/imports_xma.cpp) with FFmpeg's xma2 decoder, linked as
# shared libraries (LGPL; approved by 2026-10-05). Windows: the LGPL
# "shared" build from github.com/BtbN/FFmpeg-Builds unpacked to
# thirdparty/ffmpeg (gitignored); its DLLs are copied next to hc09.exe.
# Linux: pkg-config libavcodec/libavutil. Without FFmpeg, XMA decodes to silence.
set(HC09_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg" CACHE PATH "FFmpeg shared build (include/, lib/, bin/)")
if(EXISTS "${HC09_FFMPEG_DIR}/include/libavcodec/avcodec.h")
  target_include_directories(hc09 PRIVATE "${HC09_FFMPEG_DIR}/include")
  target_compile_definitions(hc09 PRIVATE HC09_HAVE_FFMPEG)
  if(WIN32)
    target_link_libraries(hc09 PRIVATE "${HC09_FFMPEG_DIR}/lib/avcodec.lib" "${HC09_FFMPEG_DIR}/lib/avutil.lib")
    file(GLOB HC09_FFMPEG_DLLS "${HC09_FFMPEG_DIR}/bin/avcodec-*.dll" "${HC09_FFMPEG_DIR}/bin/avutil-*.dll"
         "${HC09_FFMPEG_DIR}/bin/swresample-*.dll")
    add_custom_command(TARGET hc09 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HC09_FFMPEG_DLLS} "$<TARGET_FILE_DIR:hc09>")
  else()
    target_link_directories(hc09 PRIVATE "${HC09_FFMPEG_DIR}/lib")
    target_link_libraries(hc09 PRIVATE avcodec avutil)
  endif()
  message(STATUS "hc09: XMA decoding with FFmpeg (${HC09_FFMPEG_DIR})")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavcodec libavutil)
  endif()
  if(FFMPEG_FOUND)
    target_link_libraries(hc09 PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(hc09 PRIVATE HC09_HAVE_FFMPEG)
    message(STATUS "hc09: XMA decoding with FFmpeg (pkg-config)")
  else()
    message(STATUS "hc09: no FFmpeg found: XMA decodes to silence")
  endif()
endif()

if(HC09_GUEST_OPT STREQUAL "O2")
  target_compile_options(hc09 PRIVATE ${HC09_O2})
else()
  target_compile_options(hc09 PRIVATE ${HC09_O0})
endif()

# runtime/xenos/ is host-only (the offline software renderer for frame
# captures, no guest code), so it is always optimized: at -O0 a 4,000-draw
# frame takes ~100 s to render and stalls the GPU thread.
# runtime/gpu.cpp (the command processor) is host-only too: at -O0 the packet
# loop cost ~150-250 ns per packet, ~14 ms per in-game frame (HC09_GPU_CPUPROF).
# Guest memory and GPU registers are read through volatile loads (GLoad32), so
# its polls (ring write pointer, WAIT_REG_MEM) still see the CPU's writes.
set_source_files_properties(
    runtime/xenos/xenos_capture.cpp runtime/xenos/xenos_texture.cpp
    runtime/xenos/xenos_shader.cpp runtime/xenos/xenos_raster.cpp
    runtime/gpu.cpp
    PROPERTIES COMPILE_OPTIONS "${HC09_O2}")

# Vulkan backend (docs/BACKEND_PLAN.md) for the native and emulated renderers. Window/surface code exists for
# Win32 so far.
find_package(Vulkan QUIET OPTIONAL_COMPONENTS shaderc_combined)
if(Vulkan_FOUND AND WIN32)
  # hc09.exe: live rendering in a window (HC09_GPU_LIVE=1, runtime/gpu_live.cpp). Host-only code: optimized.
  set(HC09_LIVE_SOURCES runtime/gpu_live.cpp runtime/gpu_vk/vk_device.cpp runtime/gpu_vk/vk_edram.cpp runtime/gpu_vk/vk_native.cpp
      runtime/gpu_vk/vk_swapchain.cpp runtime/gpu_vk/vk_platform_win32.cpp
      runtime/gpu_vk/xe_glsl.cpp runtime/gpu_vk/vk_shader_compile.cpp)
  target_sources(hc09 PRIVATE ${HC09_LIVE_SOURCES})
  set_source_files_properties(${HC09_LIVE_SOURCES} PROPERTIES COMPILE_OPTIONS "${HC09_O2}")
  target_include_directories(hc09 PRIVATE "${CMAKE_SOURCE_DIR}/runtime/gpu_vk" "${CMAKE_SOURCE_DIR}/runtime/xenos")
  target_compile_definitions(hc09 PRIVATE HC09_HAVE_VULKAN)
  target_link_libraries(hc09 PRIVATE Vulkan::Vulkan Vulkan::shaderc_combined)
  if(NOT HC09_DEV_TOOLS AND HC09_CL)
    # The release build: vulkan-1.dll loaded on first use, so a PC without a Vulkan driver gets runtime/gpu_live.cpp's
    # message instead of the Windows loader's.
    target_link_options(hc09 PRIVATE /DELAYLOAD:vulkan-1.dll)
    target_link_libraries(hc09 PRIVATE delayimp)
  endif()
else()
  message(STATUS "hc09: Vulkan backend disabled (no Vulkan SDK / platform layer)")
  # The native renderer's entry points, reporting it off (the D3D hooks call them).
  target_sources(hc09 PRIVATE runtime/native_live_stub.cpp)
  target_include_directories(hc09 PRIVATE "${CMAKE_SOURCE_DIR}/runtime/xenos")
endif()

# The test benches are development tools: the release build (HC09_DEV_TOOLS=OFF) has none.
if(HC09_DEV_TOOLS)
  # Offline frame replay: re-renders captures/frame_N/replay.bin without the game
  # (portable: runtime/xenos plus one file, no OS headers).
  add_executable(hc09-replay tools/xenos_replay.cpp tools/replay_gpu.cpp
      runtime/xenos/xenos_capture.cpp runtime/xenos/xenos_texture.cpp
      runtime/xenos/xenos_shader.cpp runtime/xenos/xenos_raster.cpp)
  target_include_directories(hc09-replay PRIVATE "${CMAKE_SOURCE_DIR}/runtime/xenos")
  target_compile_options(hc09-replay PRIVATE ${HC09_O2})

  # Native renderer test bench (docs/NATIVE_RENDERER.md): D3D-level captures.
  add_executable(hc09-d3d-replay tools/d3d_replay.cpp tools/d3d_render.cpp runtime/xenos/d3d_state.cpp runtime/xenos/d3d_frontend.cpp
      runtime/xenos/xenos_capture.cpp runtime/xenos/xenos_texture.cpp
      runtime/xenos/xenos_shader.cpp runtime/xenos/xenos_raster.cpp)
  target_include_directories(hc09-d3d-replay PRIVATE "${CMAKE_SOURCE_DIR}/runtime/xenos")
  target_compile_options(hc09-d3d-replay PRIVATE ${HC09_O2})

  # `hc09-replay --gpu` and `hc09-d3d-replay --render` when the Vulkan SDK is found.
  if(Vulkan_FOUND AND WIN32)
    target_sources(hc09-replay PRIVATE
        runtime/gpu_vk/vk_device.cpp runtime/gpu_vk/vk_swapchain.cpp runtime/gpu_vk/vk_platform_win32.cpp
        runtime/gpu_vk/vk_edram.cpp runtime/gpu_vk/xe_glsl.cpp
        runtime/gpu_vk/vk_shader_compile.cpp)
    target_include_directories(hc09-replay PRIVATE "${CMAKE_SOURCE_DIR}/runtime/gpu_vk")
    target_compile_definitions(hc09-replay PRIVATE HC09_HAVE_VULKAN)
    target_link_libraries(hc09-replay PRIVATE Vulkan::Vulkan Vulkan::shaderc_combined)
    message(STATUS "hc09-replay: Vulkan backend enabled (${Vulkan_LIBRARY})")
    # hc09-d3d-replay --render: the native renderer (runtime/gpu_vk/vk_native).
    target_sources(hc09-d3d-replay PRIVATE
        runtime/gpu_vk/vk_device.cpp runtime/gpu_vk/vk_native.cpp runtime/gpu_vk/xe_glsl.cpp runtime/gpu_vk/vk_shader_compile.cpp)
    target_include_directories(hc09-d3d-replay PRIVATE "${CMAKE_SOURCE_DIR}/runtime/gpu_vk")
    target_compile_definitions(hc09-d3d-replay PRIVATE HC09_HAVE_VULKAN)
    target_link_libraries(hc09-d3d-replay PRIVATE Vulkan::Vulkan Vulkan::shaderc_combined)

    # Translator check: Xenos microcode -> GLSL -> SPIR-V for a capture's shaders.
    add_executable(xe-glsl-test tools/xe_glsl_test.cpp runtime/gpu_vk/xe_glsl.cpp runtime/gpu_vk/vk_shader_compile.cpp
        runtime/xenos/xenos_capture.cpp runtime/xenos/xenos_texture.cpp
        runtime/xenos/xenos_shader.cpp runtime/xenos/xenos_raster.cpp)
    target_include_directories(xe-glsl-test PRIVATE "${CMAKE_SOURCE_DIR}/runtime/xenos" "${CMAKE_SOURCE_DIR}/runtime/gpu_vk")
    target_link_libraries(xe-glsl-test PRIVATE Vulkan::shaderc_combined)
    target_compile_options(xe-glsl-test PRIVATE ${HC09_O2})

    # EDRAM library check: the GLSL render-target arithmetic against xenos_raster.cpp, bit for bit.
    add_executable(xe-edram-test tools/xe_edram_test.cpp runtime/gpu_vk/xe_glsl.cpp runtime/gpu_vk/vk_shader_compile.cpp
        runtime/gpu_vk/vk_device.cpp runtime/xenos/xenos_capture.cpp runtime/xenos/xenos_texture.cpp
        runtime/xenos/xenos_shader.cpp runtime/xenos/xenos_raster.cpp)
    target_include_directories(xe-edram-test PRIVATE "${CMAKE_SOURCE_DIR}/runtime/xenos" "${CMAKE_SOURCE_DIR}/runtime/gpu_vk")
    target_link_libraries(xe-edram-test PRIVATE Vulkan::Vulkan Vulkan::shaderc_combined)
    target_compile_options(xe-edram-test PRIVATE ${HC09_O2})
  endif()
endif()

# HC09_RUNTIME_PDB (diagnostics, default off; e.g. a build-pdb dir for profiling sessions): debug
# info for the runtime's own sources (not the recompiled code) and a PDB, so HC09_PROFILE names the
# runtime's functions. Codegen is unchanged; /OPT:REF,ICF are passed because /DEBUG turns them off.
option(HC09_RUNTIME_PDB "Debug info and a PDB for the runtime sources (profiling)" OFF)
if(HC09_RUNTIME_PDB AND HC09_CL)
  get_target_property(_hc09_srcs hc09 SOURCES)
  foreach(_s ${_hc09_srcs})
    if(NOT _s MATCHES "ppc_recomp\.")
      set_property(SOURCE ${_s} APPEND PROPERTY COMPILE_OPTIONS /Z7)
    endif()
  endforeach()
  target_link_options(hc09 PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# hc09-launcher.exe (Windows): the settings window of a portable folder (tools/launcher, tools/make_portable.ps1). It
# checks the user's own default.xex against the one this build was made from and builds headcoach.basefile from it:
# the hashes of both are taken here, at configure time. Static C runtime: it starts on a PC without the VC++ runtime.
if(WIN32)
  add_executable(hc09-launcher WIN32 tools/launcher/hc09_launcher.cpp tools/launcher/ps_icons.cpp tools/launcher/ps3_saves.cpp tools/launcher/pad_test.cpp)
  add_dependencies(hc09-launcher hc09_build_id)
  target_include_directories(hc09-launcher PRIVATE "${CMAKE_SOURCE_DIR}/runtime")
  target_link_libraries(hc09-launcher PRIVATE bcrypt comctl32 shell32 ole32 winhttp)
  # HC09_RELEASE_VERSION (e.g. 0.1.0): the version a release is published as (GitHub tag v<version>); the launcher's
  # optional update check compares it with the latest release. Empty (development builds): no check.
  set(HC09_RELEASE_VERSION "" CACHE STRING "Release version (GitHub tag without the v); empty: a development build")
  if(HC09_RELEASE_VERSION)
    target_compile_definitions(hc09-launcher PRIVATE HC09_RELEASE_VERSION="${HC09_RELEASE_VERSION}")
  endif()
  set_property(TARGET hc09-launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  if(EXISTS "${HC09_SDL3_DIR}/include/SDL3/SDL_gamepad.h")   # the controller test panel (SDL3.dll loaded at run time)
    target_include_directories(hc09-launcher PRIVATE "${HC09_SDL3_DIR}/include")
    target_compile_definitions(hc09-launcher PRIVATE HC09_HAVE_SDL3)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/default.xex" AND EXISTS "${CMAKE_SOURCE_DIR}/headcoach.basefile")
    file(SHA256 "${CMAKE_SOURCE_DIR}/default.xex" HC09_XEX_SHA256)
    file(SHA256 "${CMAKE_SOURCE_DIR}/headcoach.basefile" HC09_BASEFILE_SHA256)
    string(TOUPPER "${HC09_XEX_SHA256}" HC09_XEX_SHA256)
    string(TOUPPER "${HC09_BASEFILE_SHA256}" HC09_BASEFILE_SHA256)
    target_compile_definitions(hc09-launcher PRIVATE HC09_XEX_SHA256="${HC09_XEX_SHA256}" HC09_BASEFILE_SHA256="${HC09_BASEFILE_SHA256}")
  endif()
  target_link_options(hc09-launcher PRIVATE "/MANIFEST:EMBED"
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

  # hc09-build.exe (tools/installer/hc09_build.cpp): the player's build of hc09.exe in the split build, from the kit
  # (tools/make_kit.py). Static C runtime: it runs on a PC with nothing installed.
  add_executable(hc09-build tools/installer/hc09_build.cpp)
  target_link_libraries(hc09-build PRIVATE bcrypt)
  set_property(TARGET hc09-build PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

  # HC09Setup.exe (tools/installer/setup.cpp): the installer; it runs the kit's hc09-build.exe. Static C runtime.
  add_executable(HC09Setup WIN32 tools/installer/setup.cpp)
  target_link_libraries(HC09Setup PRIVATE bcrypt comctl32 shell32 ole32 uuid advapi32)
  set_property(TARGET HC09Setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_link_options(HC09Setup PRIVATE "/MANIFEST:EMBED"
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

  # The exe icon of hc09.exe and the launcher: res/hc09.ico when present. It is an optional private icon for
  # development builds (never shipped), gitignored;
  # the release build (HC09_DEV_TOOLS=OFF) has none until there is an icon of our own.
  if(HC09_DEV_TOOLS AND EXISTS "${CMAKE_SOURCE_DIR}/res/hc09.ico")
    enable_language(RC)
    file(WRITE "${CMAKE_BINARY_DIR}/hc09_icon.rc" "1 ICON \"${CMAKE_SOURCE_DIR}/res/hc09.ico\"\n")
    target_sources(hc09 PRIVATE "${CMAKE_BINARY_DIR}/hc09_icon.rc")
    target_sources(hc09-launcher PRIVATE "${CMAKE_BINARY_DIR}/hc09_icon.rc")
    message(STATUS "hc09: exe icon res/hc09.ico")
  endif()

  # hc09-xexpatch.exe: applies the user's 1.2 title update to their default.xex for the launcher (tools/xexpatch.cpp,
  # XenonUtils' XexPatcher from the recompiler's build; static C runtime like XenonUtils.lib).
  set(_hc09_xu "${XENON_DIR}/build/XenonUtils/XenonUtils.lib")
  if(EXISTS "${_hc09_xu}")
    add_executable(hc09-xexpatch tools/xexpatch.cpp)
    target_include_directories(hc09-xexpatch PRIVATE "${XENON_DIR}/XenonUtils" "${XENON_DIR}/thirdparty/simde")
    target_link_libraries(hc09-xexpatch PRIVATE "${_hc09_xu}" "${XENON_DIR}/build/thirdparty/disasm/disasm.lib")
    set_property(TARGET hc09-xexpatch PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endif()
endif()
