# Capture only the game's window (not the desktop) to a PNG.
# Usage: powershell -File tools\capture_window.ps1 <process name> <out.png>
param([string]$ProcessName, [string]$OutPath, [string]$WindowClass = "")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[Win]::SetProcessDPIAware() | Out-Null   # physical pixels, so the whole client area is captured
Add-Type -Name FW -Namespace U -MemberDefinition '[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);'
$p = Get-Process $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($WindowClass) {   # capture another top-level window of the app by its window class
    $h = [U.FW]::FindWindow($WindowClass, [NullString]::Value)
    $p = [pscustomobject]@{ MainWindowHandle = $h; MainWindowTitle = $WindowClass }
}
$r = New-Object Win+RECT
[Win]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R - $r.L)), ([Math]::Max(1, $r.B - $r.T))
# GPU swapchain content isn't visible to PrintWindow, so bring the window to the
# front and copy just its client rectangle from the screen.
[Win]::ShowWindow($p.MainWindowHandle, 9) | Out-Null   # SW_RESTORE
[Win]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
$pt = New-Object Win+POINT
[Win]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
$bmp.Save($OutPath)
"captured $($bmp.Width)x$($bmp.Height) from '$($p.MainWindowTitle)'"
