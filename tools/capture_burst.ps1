# Capture N frames of the game window in quick succession (window-only, DPI-aware).
# Usage: powershell -File tools\capture_burst.ps1 <process> <out prefix> <count> [delay ms]
param([string]$ProcessName, [string]$Prefix, [int]$Count = 10, [int]$DelayMs = 50)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class WinB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[WinB]::SetProcessDPIAware() | Out-Null
$p = Get-Process $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$r = New-Object WinB+RECT; [WinB]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$pt = New-Object WinB+POINT; [WinB]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
$size = New-Object System.Drawing.Size ($r.R - $r.L), ($r.B - $r.T)
for ($i = 0; $i -lt $Count; $i++) {
    $bmp = New-Object System.Drawing.Bitmap $size.Width, $size.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $size)
    $bmp.Save(("{0}_{1:D2}.png" -f $Prefix, $i)); $g.Dispose(); $bmp.Dispose()
    Start-Sleep -Milliseconds $DelayMs
}
"captured $Count frames"
