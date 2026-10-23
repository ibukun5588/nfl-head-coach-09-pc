# Make a portable folder of the PC port: hc09-launcher.exe and the game versions, with what they need to run on
# another Windows PC.
#   powershell -File tools\make_portable.ps1 [-Build build-run] [-Build12 build-v12] [-Out dist] [-WithSaves] [-WithGlyphs] [-Zip]
# Layout (docs/HANDOFF_M10_5.md step 7):
#   hc09-launcher.exe, hc09-xexpatch.exe, hc09.ini, README.txt
#   1.0\hc09.exe (+ DLLs)        the original game (-Build, default build-run)
#   1.2\hc09.exe (+ DLLs)        the Xbox 360 title update (-Build12, default build-v12 when it has hc09.exe;
#                                -Build12 '' leaves it out)
# The folder holds NO game files: on the target PC the launcher asks for the folder with the user's own default.xex
# and qkl_*.ast archives (and, for 1.2, their own title update file) and builds each version's headcoach.basefile
# from them (checked against this build's hashes).
# -WithSaves also copies this PC's saves (hc09-cache\content, hc09-cache-v12\content: career, profile, rosters).
# -WithGlyphs also copies hc09-cache\glyphs (the PlayStation button icons, tools/playstation_icons.py: made from the
# game files, so only for a folder that stays with the owner of the game), for both versions.
# Needs on the target PC: Windows 10/11 x64, a CPU with AVX2 (x86-64-v3; hc09.exe says so if not), a Vulkan driver.
param([string]$Build = 'build-run', [string]$Build12 = 'build-v12', [string]$Out = 'dist', [switch]$WithSaves,
      [switch]$WithGlyphs, [switch]$Zip)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
$id = (Select-String -Path runtime\build_id.h -Pattern '"([^"]+)"').Matches[0].Groups[1].Value.TrimEnd('+')
$dir = Join-Path $Out "HC09-PC-$id"
foreach ($f in 'hc09.exe', 'hc09-launcher.exe') { if (-not (Test-Path "$Build\$f")) { throw "$Build\$f is missing: build it first (ninja -C $Build hc09 hc09-launcher)" } }
$v12 = $Build12 -and (Test-Path "$Build12\hc09.exe")
if ($v12 -and -not (Test-Path "$Build\hc09-xexpatch.exe")) { throw "$Build\hc09-xexpatch.exe is missing (ninja -C $Build hc09-xexpatch)" }
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force $dir | Out-Null

# The Visual C++ runtime next to each hc09.exe (app-local deployment; the target PC may not have the redistributable).
$crt = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw 'Microsoft.VC143.CRT (VS 2022 redistributable DLLs) not found' }
# Each version: its hc09.exe, the FFmpeg (XMA audio; LGPL shared build, thirdparty/ffmpeg) and SDL3 DLLs, the CRT.
$versions = @(@{ Name = '1.0'; Build = $Build })
if ($v12) { $versions += @{ Name = '1.2'; Build = $Build12 } }
foreach ($v in $versions) {
    $vd = Join-Path $dir $v.Name
    New-Item -ItemType Directory -Force $vd | Out-Null
    Copy-Item "$($v.Build)\hc09.exe", "$($v.Build)\*.dll" $vd
    Copy-Item "$($crt.FullName)\*.dll" $vd
}
# The launcher (static C runtime) and, for 1.2, the title-update patcher beside it.
Copy-Item "$Build\hc09-launcher.exe" $dir
if ($v12) { Copy-Item "$Build\hc09-xexpatch.exe" $dir }

# Settings: the documented template, game folder left for the launcher to ask.
(Get-Content hc09.ini.example) -replace '^root = .*$', 'root =' -replace '^framerate = .*$', 'framerate = 240' | Set-Content -Encoding ascii "$dir\hc09.ini"

foreach ($c in 'hc09-cache', 'hc09-cache-v12') {
    if ($c -eq 'hc09-cache-v12' -and -not $v12) { continue }
    if ($WithSaves -and (Test-Path "$c\content")) {
        New-Item -ItemType Directory -Force "$dir\$c" | Out-Null
        Copy-Item -Recurse "$c\content" "$dir\$c\content"
    }
    if ($WithGlyphs -and (Test-Path hc09-cache\glyphs)) {   # the same icons for both versions (same game files)
        New-Item -ItemType Directory -Force "$dir\$c" | Out-Null
        Copy-Item -Recurse hc09-cache\glyphs "$dir\$c\glyphs"
    }
}

$v12text = if ($v12) { @"

Game version: the launcher offers 1.0 (the original game) and 1.2 (the Xbox 360 title update #2, which plays
differently and is better for careers). For 1.2, choose YOUR title update file too (the TU_... file from your
Xbox 360's drive, or its default.xexp): the first Play with 1.2 applies it to your default.xex (1.2\headcoach.basefile).
Saves: hc09-cache\ for 1.0 and hc09-cache-v12\ for 1.2 ("Copy saves" copies them across), or tick "Both versions use
the same saves".
"@ } else { '' }
@"
NFL Head Coach 09 - PC port (build $id)

1. Start hc09-launcher.exe.
2. Game files: choose the folder with YOUR copy of the game (default.xex and the five qkl_*.ast files from your
   Xbox 360 disc). The first Play builds headcoach.basefile from that default.xex (a few seconds).
3. Pick the settings and press Play. The game's log goes to hc09.log in this folder (send it along with any report).
$v12text
Needs: Windows 10/11 64-bit, a CPU with AVX2 (Intel Haswell 2013+ / AMD Zen), a GPU with a Vulkan driver.
Saves and the shader cache go to hc09-cache\ in this folder. Settings are in hc09.ini (the launcher writes it;
the file explains every setting). Keyboard: arrows = D-pad, WASD = left stick, Space = A, Esc = B, Enter = START,
Alt+Enter or F11 = fullscreen. An Xbox controller works too.

This folder contains no game data. Do not share headcoach.basefile, hc09.exe or your game files: they are EA's
property. The VC++ runtime DLLs are Microsoft's (redistributable); avcodec/avutil/swresample are FFmpeg (LGPL).
"@ | Set-Content -Encoding ascii "$dir\README.txt"

$size = (Get-ChildItem -Recurse -File $dir | Measure-Object Length -Sum).Sum / 1MB
Write-Output ("{0}: versions {1}; {2} files, {3:N0} MB" -f $dir, (($versions | ForEach-Object { $_.Name }) -join ', '),
              (Get-ChildItem -Recurse -File $dir).Count, $size)
if ($Zip) {
    $z = "$dir.zip"
    if (Test-Path $z) { Remove-Item $z }
    Compress-Archive -Path "$dir\*" -DestinationPath $z
    Write-Output ("{0}: {1:N0} MB" -f $z, ((Get-Item $z).Length / 1MB))
}
