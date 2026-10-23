# Drives a running hc09.exe with keyboard presses posted to its window (WM_KEYDOWN/WM_KEYUP): the way a player's
# keyboard reaches the game, so it works on the release build, which has no scripted input (docs/HANDOFF_M11.md).
# The window is not brought to the front; the game needs host input on (HC09_HOST_INPUT=1 or hc09.ini).
#   .\tools\post_keys.ps1 -Exe build-rel\hc09.exe -ArgList 'headcoach.basefile' -Dir <run folder> -Seconds 600 `
#       -Keys '130:start 145:a 159:start' -Log run.log [-Env @{ HC09_CACHE_DIR = '...' }]
# -Keys: <seconds after start>:<button>, the HC09_INPUT button names: a b x y start back up down left right
# (also dup/ddown/dleft/dright), lb rb lt rt. The process is stopped after -Seconds.
# -Sample N: every N seconds, print the game's frame count (the log's last "VdSwap #n") for fps over a window.
param([string]$Exe, [string]$ArgList = '', [string]$Dir = '.', [int]$Seconds = 600, [string]$Keys = '',
      [string]$Log = 'run.log', [hashtable]$Env = @{}, [int]$Sample = 0)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PK {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
  public static void Press(IntPtr h, uint vk, int holdMs) {
    uint sc = MapVirtualKeyW(vk, 0) << 16;
    PostMessageW(h, 0x0100, (IntPtr)vk, (IntPtr)(1 | sc));                          // WM_KEYDOWN
    System.Threading.Thread.Sleep(holdMs);
    PostMessageW(h, 0x0101, (IntPtr)vk, (IntPtr)unchecked((int)(0xC0000001u | sc)));  // WM_KEYUP
  }
}
'@
# host_input.cpp's keyboard map
$vk = @{ a = 0x20; b = 0x4C; x = 0x4A; y = 0x49; start = 0x0D; back = 0x08; lb = 0x51; rb = 0x45; lt = 0x5A; rt = 0x43
         up = 0x26; down = 0x28; left = 0x25; right = 0x27; dup = 0x26; ddown = 0x28; dleft = 0x25; dright = 0x27 }
$steps = @()
foreach ($t in ($Keys -split '\s+' | Where-Object { $_ })) {
    $at, $name = $t -split ':', 2
    if (-not $vk.ContainsKey($name)) { throw "unknown button '$name' in '$t'" }
    $steps += [pscustomobject]@{ At = [double]$at; Name = $name; Vk = $vk[$name] }
}
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
$logPath = Join-Path (Resolve-Path $Dir) $Log
$p = Start-Process -FilePath (Resolve-Path $Exe) -ArgumentList $ArgList -WorkingDirectory $Dir -PassThru `
     -RedirectStandardError $logPath -RedirectStandardOutput "$logPath.out"
$t0 = Get-Date
$h = [IntPtr]::Zero
while ($h -eq [IntPtr]::Zero -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 120) {
    Start-Sleep -Milliseconds 500; $p.Refresh(); $h = $p.MainWindowHandle
}
if ($h -eq [IntPtr]::Zero) { Write-Output 'no game window'; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Write-Output ("window after {0:n1} s" -f ((Get-Date) - $t0).TotalSeconds)
$nextSample = $Sample
function Wait-Until([double]$at) {
    while (((Get-Date) - $t0).TotalSeconds -lt $at -and -not $p.HasExited) {
        $now = ((Get-Date) - $t0).TotalSeconds
        if ($Sample -gt 0 -and $now -ge $script:nextSample) {
            $script:nextSample += $Sample
            $fs = [IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
            $fs.Seek([Math]::Max(0, $fs.Length - 65536), 'Begin') | Out-Null
            $tail = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
            $m = [regex]::Matches($tail, 'VdSwap #(\d+)')
            if ($m.Count) { Write-Output ("sample t={0:n1} swaps={1}" -f $now, $m[$m.Count - 1].Groups[1].Value) }
        }
        Start-Sleep -Milliseconds 100
    }
}
foreach ($s in $steps) {
    Wait-Until $s.At
    if ($p.HasExited) { break }
    [PK]::Press($h, [uint32]$s.Vk, 150)
    Write-Output ("t={0,6:n1} s  {1}" -f ((Get-Date) - $t0).TotalSeconds, $s.Name)
}
Wait-Until $Seconds
if ($p.HasExited) { Write-Output "hc09 exited by itself (code $($p.ExitCode))" } else { Stop-Process -Id $p.Id -Force; Write-Output 'stopped' }
