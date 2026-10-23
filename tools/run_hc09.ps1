# Run build-run\hc09.exe under a timeout (Windows). stderr -> <Log> in the repo root.
#   powershell -File tools\run_hc09.ps1 -Seconds 600 -Log run-win.log -EnvVars @{ HC09_INPUT = 'f4400:start ...'; HC09_CAPTURE_FRAME = '4300' }
# -Build build-<variant>: that build dir's hc09.exe (tools/build.py --variant) instead of build-run.
# -Exe <path>: any hc09.exe, relative to the repo (e.g. builds\archive\<commit>\hc09.exe); wins over -Build.
# -Image: the image to load (default headcoach.basefile; the 1.2 build: -Build build-v12 -Image headcoach-v12.basefile).
# One instance at a time (they share hc09-cache\ and the log); stop with Stop-Process -Name hc09.
# For runs longer than a tool call allows, start this script detached:
#   Start-Process powershell -ArgumentList '-NoProfile','-File','tools\run_hc09.ps1',... -WindowStyle Hidden
param([int]$Seconds = 60, [string]$Log = 'run-win.log', [hashtable]$EnvVars = @{},
      [string]$Root = 'C:\Games\NFL Head Coach 09', [string]$Build = 'build-run', [string]$Exe = '',
      [string]$Image = 'headcoach.basefile')
$repo = Split-Path $PSScriptRoot -Parent
if (Get-Process -Name hc09 -ErrorAction SilentlyContinue) { Write-Output 'hc09.exe already running - not starting another'; exit 1 }
foreach ($k in $EnvVars.Keys) { Set-Item -Path "env:$k" -Value $EnvVars[$k] }
$exePath = if ($Exe) { if ([IO.Path]::IsPathRooted($Exe)) { $Exe } else { "$repo\$Exe" } } else { "$repo\$Build\hc09.exe" }
$p = Start-Process -FilePath $exePath -ArgumentList @($Image, '--root', "`"$Root`"") `
    -WorkingDirectory $repo -RedirectStandardError "$repo\$Log" -RedirectStandardOutput "$repo\$Log.stdout.log" `
    -NoNewWindow -PassThru
$t0 = Get-Date
if (-not $p.WaitForExit($Seconds * 1000)) {
    Stop-Process -Name hc09 -Force -ErrorAction SilentlyContinue
    Write-Output ("timeout after {0}s (killed)" -f $Seconds)
} else {
    Write-Output ("exited with code {0} after {1:N1}s" -f $p.ExitCode, ((Get-Date) - $t0).TotalSeconds)
}
