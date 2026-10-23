@echo off
rem Windows build environment for hc09-recomp: MSVC x64 libraries/SDK (for
rem clang-cl and lld-link), LLVM, CMake and Ninja on PATH.
rem   call tools\winenv.cmd            (from cmd)
rem   cmd /c "call tools\winenv.cmd && python tools\build.py ..."   (from PowerShell)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo winenv: Visual Studio C++ build tools not found & exit /b 1)
rem vcvars64 runs vswhere.exe through PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
where ninja >nul 2>nul || for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_*") do set "PATH=%%d;%PATH%"
if not defined VULKAN_SDK for /d %%d in ("C:\VulkanSDK\*") do set "VULKAN_SDK=%%d"
if defined VULKAN_SDK set "PATH=%VULKAN_SDK%\Bin;%PATH%"
exit /b 0
