Add-Type @"
using System; using System.Runtime.InteropServices;
public class WinFocus {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { "teardown not running"; exit 1 }
$h = $p.MainWindowHandle
if ([WinFocus]::IsIconic($h)) { [WinFocus]::ShowWindow($h, 9) | Out-Null }
$fg = [WinFocus]::GetForegroundWindow()
$fgThread = [WinFocus]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$me = [WinFocus]::GetCurrentThreadId()
# Share the input state with the current foreground thread so the switch is allowed.
[WinFocus]::AttachThreadInput($me, $fgThread, $true) | Out-Null
[WinFocus]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[WinFocus]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[WinFocus]::BringWindowToTop($h) | Out-Null
[WinFocus]::SetForegroundWindow($h) | Out-Null
[WinFocus]::AttachThreadInput($me, $fgThread, $false) | Out-Null
Start-Sleep -Milliseconds 200
if ([WinFocus]::GetForegroundWindow() -eq $h) { "focused" } else { "focus failed" }
