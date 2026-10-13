# Interleaved frameBMode stress runs (0,2,1,0,2,1) on the mock headset; each run waits until nobody has
# touched mouse/keyboard for 45 s (our own clicks happen only in the first ~90 s of a run).
param([string[]]$Configs = @('0/0', '2/0', '2/2', '0/2', '2/2', '2/0'))   # frameBMode/frameLatency[/key=v;key=v] (more control-file keys)
$here = $PSScriptRoot
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class UI2 { [StructLayout(LayoutKind.Sequential)] public struct LII { public uint cb; public uint t; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LII l);
public static double Idle() { LII l = new LII(); l.cb = 8; GetLastInputInfo(ref l); return ((uint)Environment.TickCount - l.t) / 1000.0; } }
'@
$runs = @()
foreach ($c in $Configs) {
    $parts = $c.Split('/'); $m = [int]$parts[0]; $lat = [int]$parts[1]
    $extra = if ($parts.Count -gt 2) { $parts[2].Split(';') } else { @() }
    $tag = if ($extra.Count) { '_' + (($extra | ForEach-Object { $_ -replace '[^A-Za-z0-9]', '' }) -join '_') } else { '' }
    $t0 = Get-Date
    while ([UI2]::Idle() -lt 45) { Start-Sleep 10; if (((Get-Date) - $t0).TotalMinutes -gt 120) { "STOP: the PC was in use for 2 h"; return } }
    "=== $(Get-Date -Format HH:mm:ss) run frameBMode=$m frameLatency=$lat $($extra -join ' ')"
    $o = & "$here\stress_run.ps1" -Label "fb${m}_lat$lat$tag" -DebugLua 0 -FrameB $m -FrameLatency $lat -Keys $extra -IUserAgreed 2>&1
    $o | Select-Object -Last 45
    $dir = Get-ChildItem "$here\runs" -Directory | Sort-Object LastWriteTime | Select-Object -Last 1
    $runs += [pscustomobject]@{ mode = $m; lat = $lat; extra = ($extra -join ';'); dir = $dir.FullName }
}
$runs | ConvertTo-Json | Set-Content "$here\matrix_runs.json" -Encoding ascii
"=== done $(Get-Date -Format HH:mm:ss)"
