# perf_sample.ps1: samples a running hc09.exe every -Interval seconds into a CSV (docs/REPORT_LOWEND.md):
#   time, cpu (cores busy: process CPU seconds per second), the 4 busiest threads (cores busy each), GPU 3D engine
#   use per adapter (%, Windows' GPU Engine counters for this process), NVIDIA board power (W, nvidia-smi if present),
#   and the busiest threads by name (the runtime names its threads: the game's names, "hc09 renderer", ...).
# Runs until hc09.exe exits or -Seconds pass.
#   powershell -File tools\perf_sample.ps1 -Out I:\run\perf.csv [-Seconds 600] [-Interval 5]
param([string]$Out, [int]$Seconds = 3600, [int]$Interval = 5)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Hc09ThreadName {
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    public static string Get(int tid) {
        IntPtr h = OpenThread(0x0800, false, (uint)tid);   // THREAD_QUERY_LIMITED_INFORMATION
        if (h == IntPtr.Zero) return "";
        try {
            IntPtr d;
            if (GetThreadDescription(h, out d) < 0 || d == IntPtr.Zero) return "";
            string s = Marshal.PtrToStringUni(d); LocalFree(d); return s ?? "";
        } finally { CloseHandle(h); }
    }
}
'@
$p = $null
for ($i = 0; $i -lt 120 -and -not $p; $i++) { $p = Get-Process hc09 -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not $p) { Start-Sleep 1 } }
if (-not $p) { Write-Output 'no hc09.exe'; exit 1 }
$nv = (Get-Command nvidia-smi -ErrorAction SilentlyContinue).Source
'time,cpu_cores,thread1,thread2,thread3,thread4,threads_over_10pct,gpu3d_by_adapter,nv_watts,top_threads' | Out-File $Out -Encoding ascii
$prevCpu = $p.TotalProcessorTime.TotalSeconds
$prevT = @{}
foreach ($t in $p.Threads) { try { $prevT[$t.Id] = $t.TotalProcessorTime.TotalSeconds } catch {} }
$names = @{}
$t0 = Get-Date; $prevWall = $t0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds $Interval
    $p.Refresh()
    if ($p.HasExited) { break }
    $now = Get-Date; $dt = ($now - $prevWall).TotalSeconds; $prevWall = $now
    $cpu = $p.TotalProcessorTime.TotalSeconds
    $cores = ($cpu - $prevCpu) / $dt; $prevCpu = $cpu
    $per = @()
    $cur = @{}
    foreach ($t in $p.Threads) {
        try { $s = $t.TotalProcessorTime.TotalSeconds } catch { continue }
        $cur[$t.Id] = $s
        if ($prevT.ContainsKey($t.Id)) { $per += [pscustomobject]@{ Id = $t.Id; Cores = ($s - $prevT[$t.Id]) / $dt } }
    }
    $prevT = $cur
    $top = @($per | Sort-Object Cores -Descending | Select-Object -First 4)
    $busy = @($per | Where-Object { $_.Cores -gt 0.10 }).Count
    $gpu = ''
    try {
        $c = Get-Counter "\GPU Engine(pid_$($p.Id)_*engtype_3D)\Utilization Percentage" -ErrorAction Stop
        $by = @{}
        foreach ($s in $c.CounterSamples) {
            $a = if ($s.InstanceName -match 'luid_(0x[0-9a-f]+_0x[0-9a-f]+)') { $Matches[1] } else { '?' }
            $by[$a] = [double]($by[$a]) + $s.CookedValue
        }
        $gpu = ($by.GetEnumerator() | Where-Object { $_.Value -gt 0.5 } | ForEach-Object { '{0}={1:F0}' -f $_.Key, $_.Value }) -join ' '
    } catch {}
    $w = ''
    if ($nv) { $w = (& $nv --query-gpu=power.draw --format=csv,noheader,nounits 2>$null | Select-Object -First 1) }
    $t4 = 0..3 | ForEach-Object { if ($_ -lt $top.Count) { '{0:F2}' -f $top[$_].Cores } else { '' } }
    $named = ($top | ForEach-Object {
        if (-not $names[$_.Id]) { $names[$_.Id] = [Hc09ThreadName]::Get($_.Id) }
        $n = if ($names[$_.Id]) { $names[$_.Id] } else { "tid$($_.Id)" }
        '{0}={1:F2}' -f ($n -replace '[,;=]', ' '), $_.Cores }) -join ';'
    ('{0:F0},{1:F2},{2},{3},{4},{5},{6},{7},{8},{9}' -f ($now - $t0).TotalSeconds, $cores, $t4[0], $t4[1], $t4[2], $t4[3], $busy, $gpu, $w, $named) |
        Out-File $Out -Append -Encoding ascii
}
