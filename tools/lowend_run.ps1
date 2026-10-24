# lowend_run.ps1: one measurement run for docs/REPORT_LOWEND.md (1.0, a copy of the CAREER-TEST career).
# -Scene career: by wall time (seconds after start) main menu 45-90, career hub 400-455, Sim To (simulating the
#   season) 530-700; frame dumps at 60, 430 and 650 s.
# -Scene game: a Play Now game (the blocking logger's script, auto-resume at stalls), in play from ~500 s: 540-700;
#   frame dumps at 300, 560 and 690 s.
# -Scene menu: the main menu only (3D city), no frame dumps (A/B comparisons).
# The run folder <Dir> needs hc09.ini and hc09-cache\ (with the career); the log is <Dir>\run.log, the samples
# <Dir>\perf.csv (tools/perf_sample.ps1), the dumps in <Dir>\dumps.
#   powershell -File tools\lowend_run.ps1 -Dir I:\m12-measure\A -Env @{ HC09_FRAMERATE = '60' } [-Affinity 0xFF]
# -Affinity: the CPUs hc09.exe may use (a mask; 0xFF = 4 cores / 8 threads on this PC, like the Steam Deck).
param([string]$Dir, [string]$Exe = 'C:\Dev\hc09-recomp\build-run\hc09.exe', [hashtable]$Env = @{}, [long]$Affinity = 0,
      [int]$Seconds = 720, [string]$Scene = 'career')
$inp = '27:start 29:start 31:start 33:start 95:a 99:dup 101:a 460:a 470:dleft 480:a 487:ddown 489:ddown 491:ddown ' +
       '493:ddown 495:ddown 497:ddown 499:ddown 503:a 509:dup 511:a'
$dumpsAt = 60, 430, 650
if ($Scene -eq 'menu') { $inp = '27:start 29:start 31:start 33:start'; $dumpsAt = @() }
if ($Scene -eq 'game') {
    $inp = '130:start 145:a 159:start 175:a 190:a 205:a 219:a 271:ddown 280:a 294:a 308:a 325:start 340:a 354:a ' +
           '369:start 383:a 398:a 412:start 427:a 447:a 462:a 476:a 491:a'
    $dumpsAt = 300, 560, 690
    $env:HC09_BLOCK_AUTORESUME = '20'
}
New-Item -ItemType Directory -Force "$Dir\dumps" | Out-Null
Set-Content -Path "$Dir\input.txt" -Value '' -Encoding ascii
$env:HC09_HOST_INPUT = '0'
$env:HC09_INPUT = $inp
$env:HC09_INPUT_FILE = "$Dir\input.txt"
$env:HC09_CACHE_DIR = "$Dir\hc09-cache"
$env:HC09_GPU_LIVE_DIR = "$Dir\dumps"
foreach ($k in $Env.Keys) { Set-Item -Path "env:$k" -Value $Env[$k] }
$p = Start-Process -FilePath $Exe -ArgumentList @('C:\Dev\hc09-recomp\headcoach.basefile', '--root', '"C:\Games\NFL Head Coach 09"') `
    -WorkingDirectory $Dir -RedirectStandardError "$Dir\run.log" -RedirectStandardOutput "$Dir\run.out.log" -PassThru
if ($Affinity) { $p.ProcessorAffinity = [IntPtr]$Affinity }
$sampler = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    "$PSScriptRoot\perf_sample.ps1", '-Out', "$Dir\perf.csv", '-Seconds', "$Seconds" -WindowStyle Hidden -PassThru
$t0 = Get-Date
foreach ($at in $dumpsAt) {
    while (((Get-Date) - $t0).TotalSeconds -lt $at -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
    Add-Content -Path "$Dir\input.txt" -Value 'dump' -Encoding ascii
}
if (-not $p.WaitForExit([int](($Seconds - ((Get-Date) - $t0).TotalSeconds) * 1000))) { Stop-Process -Id $p.Id -Force }
$sampler.WaitForExit(30000) | Out-Null
"done after $([int]((Get-Date) - $t0).TotalSeconds) s" | Out-File "$Dir\result.txt"
