# Screenshot of the hc09-replay --gpu window, kept only if it shows the expected frame.
#   powershell -File tools\window_shot.ps1 -Replay captures\st\frame_4690\replay.bin -Out shot.png -Expected <frame.png>
# It brings the window to the foreground.
# The client area is compared with -Expected (tools/imgdiff.py, tolerance 16: an
# overlay on the PC turns colour (13,11,12) black on screen); on mismatch the
# screenshot is deleted, so nothing else on the desktop can end up in a file.
# The saved image is the visible window frame (title bar, no resize borders).
param([string]$Replay, [string]$Out, [string]$Expected, [int]$Seconds = 12,
      [string]$Python = 'python')
$repo = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static void Front(IntPtr h) {
    uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero), me = GetCurrentThreadId();
    AttachThreadInput(me, fg, true);
    ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
    AttachThreadInput(me, fg, false);
  }
}
'@
[W]::SetProcessDPIAware() | Out-Null
$p = Start-Process -FilePath "$repo\build-run\hc09-replay.exe" -ArgumentList @($Replay, 'captures\window_shot', '--gpu', '--seconds', "$Seconds") `
     -WorkingDirectory $repo -PassThru -NoNewWindow -RedirectStandardError "$repo\captures\window_shot.log"
for ($i = 0; $i -lt 300 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh() }
if ($p.MainWindowHandle -eq 0) { 'no window'; exit 1 }
$h = $p.MainWindowHandle
[W]::Front($h)
Start-Sleep -Seconds 2
$grab = {
  param($x, $y, $w, $hh, $file)
  $bmp = New-Object System.Drawing.Bitmap $w, $hh, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
  $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}
$cr = New-Object W+RECT; [W]::GetClientRect($h, [ref]$cr) | Out-Null
$pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
$fr = New-Object W+RECT; [W]::DwmGetWindowAttribute($h, 9, [ref]$fr, 16) | Out-Null   # DWMWA_EXTENDED_FRAME_BOUNDS
$client = [IO.Path]::ChangeExtension($Out, '.client.png')
& $grab $pt.X $pt.Y ($cr.R - $cr.L) ($cr.B - $cr.T) $client
& $grab $fr.L $fr.T ($fr.R - $fr.L) ($fr.B - $fr.T) $Out
Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
$res = & $Python "$repo\tools\imgdiff.py" $Expected $client --tol 16 2>&1
$ok = $LASTEXITCODE -eq 0
Remove-Item $client -ErrorAction SilentlyContinue
if (-not $ok) { Remove-Item $Out -ErrorAction SilentlyContinue; "screenshot REJECTED: $res"; exit 1 }
"screenshot OK: $($res | Select-Object -First 1)"
