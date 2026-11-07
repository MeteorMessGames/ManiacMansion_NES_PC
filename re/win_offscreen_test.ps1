$ErrorActionPreference = "Stop"
$dir = "$env:USERPROFILE\Desktop\ManiacMansionMouse"
Set-Location $dir

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT lpPoint);
    public struct POINT { public int X; public int Y; }
}
"@

# Remember the real cursor position so we can restore it after the test.
$origPos = New-Object Win32+POINT
[Win32]::GetCursorPos([ref]$origPos) | Out-Null

Remove-Item -ErrorAction SilentlyContinue "mouse_debug.log"

$env:SDL_AUDIODRIVER = "dummy"
$proc = Start-Process -FilePath "$dir\nes_game.exe" -ArgumentList '"Maniac Mansion (Prototype).nes"' -PassThru -RedirectStandardOutput "$dir\stdout_test.log" -RedirectStandardError "$dir\stderr_test.log"

# Wait for the window handle to exist. Polled fast (25ms) and moved
# off-screen the instant it's found, to keep the on-screen flash as brief
# as possible -- Start-Process -WindowStyle Hidden suppresses the window
# before .NET ever reports a MainWindowHandle, so this is the tradeoff.
$tries = 0
while ($proc.MainWindowHandle -eq [IntPtr]::Zero -and $tries -lt 200) {
    if ($proc.HasExited) {
        Write-Output "ERROR: process exited early, exit code $($proc.ExitCode)"
        exit 1
    }
    Start-Sleep -Milliseconds 25
    $proc.Refresh()
    $tries++
}
if ($proc.MainWindowHandle -eq [IntPtr]::Zero) {
    Write-Output "ERROR: window handle never appeared, HasExited=$($proc.HasExited)"
    exit 1
}

$SWP_NOSIZE = 0x0001
$SWP_NOZORDER = 0x0004
$SWP_NOACTIVATE = 0x0010
$offX = 3000
$offY = 0
[Win32]::SetWindowPos($proc.MainWindowHandle, [IntPtr]::Zero, $offX, $offY, 0, 0, $SWP_NOSIZE -bor $SWP_NOZORDER -bor $SWP_NOACTIVATE) | Out-Null
Start-Sleep -Milliseconds 500

# Click at a point inside the (now off-screen) window: offX+100, offY+100.
$clickX = $offX + 100
$clickY = $offY + 100
[Win32]::SetCursorPos($clickX, $clickY) | Out-Null
Start-Sleep -Milliseconds 300

$MOUSEEVENTF_LEFTDOWN = 0x0002
$MOUSEEVENTF_LEFTUP = 0x0004
[Win32]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 1000
[Win32]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 2000

# Restore the real cursor position.
[Win32]::SetCursorPos($origPos.X, $origPos.Y) | Out-Null

Write-Output "done; killing process $($proc.Id)"
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
