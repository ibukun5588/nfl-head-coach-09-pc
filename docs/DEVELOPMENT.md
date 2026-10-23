# Building from source

Players do not need this: download the release and run `HC09Setup.exe` (see the [README](../README.md)).

This is a static recompilation of the Xbox 360 game with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp):
the game's PowerPC program is translated to C++ and compiled for x86-64, and a hand-written runtime (`runtime/`)
stands in for the console's kernel, GPU (a Vulkan renderer of the game's Direct3D calls, plus an emulated renderer
of the console's command stream), audio (XMA through FFmpeg) and controllers (SDL3, XInput).

**No game files are in this repository, and none may be added**: not `default.xex`, not the decrypted image, not the
recompiler's output (`out/`), not anything made from the game's archives. Bring your own copy.

## Layout

| Path | What |
|---|---|
| `runtime/` | the runtime: kernel and system imports, GPU front end, Vulkan renderers, audio, input, settings |
| `config/` | the recompiler's configuration for game version 1.0 (`config/`) and 1.2 (`config/v12/`): addresses only |
| `tools/build.py` | the developer's build: decrypt the xex, recompile, build with CMake + Ninja |
| `tools/launcher/` | `hc09-launcher.exe` |
| `tools/installer/` | `HC09Setup.exe` and `hc09-build.exe` (the player's build of `hc09.exe`) |
| `tools/make_kit.py` | assembles the release: the build kit (clang-cl, lld-link, headers, runtime DLLs) and the zip |
| `toolchain/include/` | the C library headers the recompiled code is compiled against on the player's PC |
| `release/` | the player's README, third-party notices and licenses |

## Requirements (developer build)

- Windows 10/11 x64, Visual Studio 2022 Build Tools (MSVC libraries and Windows SDK), LLVM 23 (`clang-cl`,
  `lld-link`), CMake, Ninja, Python 3.11 with `cryptography`, the Vulkan SDK (with `shaderc_combined`).
- `thirdparty/SDL3` (the SDL3 VC development package) and `thirdparty/ffmpeg` (an LGPL shared build); both optional.
- The project's fork of XenonRecomp beside this folder (`../XenonRecomp-hc09`): switch-table analysis fixes, guest
  registers as C++ locals and the title-update patcher. It is not public yet; upstream XenonRecomp alone does not
  produce the code the configs expect.

## Building

From a shell with the MSVC environment (`tools\winenv.cmd`):

```
py -3.11 tools\build.py <path to your default.xex>                        # game version 1.0: build-run\hc09.exe
py -3.11 tools\build.py <default.xex> --title-update <your TU file>       # 1.0 and 1.2 (build-v12\)
py -3.11 tools\build.py <default.xex> --release --build-dir <folder>     # the release runtime (no developer tools)
```

The release is a split build: the runtime is built here as `hc09rt.dll` (CMake `-DHC09_SPLIT=ON -DHC09_DEV_TOOLS=OFF`),
and the player's PC compiles only the recompiled code with the shipped clang-cl and links it with lld-link
(`tools/installer/hc09_build.cpp`; `runtime/split_abi.h` is the whole interface). The result is reproducible:
the kit lists the expected SHA-256 of each `hc09.exe`, and the installer checks the player's build against it.
`tools/make_kit.py` assembles the kit and the release zip; `tools/release_check.py` checks that a release binary
holds none of the developer switches.
