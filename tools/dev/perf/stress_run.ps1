# CPU stress test on the MOCK headset that recreates "shooting everywhere, then it freezes".
# What it does: restarts Teardown (cycle.ps1 closes the running game and DEPLOYS build\Release\dsound.dll and
# mod\TeardownVR\* into the game folder), enters Lee Chemicals (golevel2.ps1), keeps the head still, fires the
# rocket launcher along a fixed fan of aims + mock-only Lua explosions, and records TeardownVR.log, per-thread CPU
# (thread_sampler.py), CPU/GPU counters (typeperf) and nvidia-smi. Then restores your ini / options / mod list.
# It restarts the game: run it only on purpose (-IUserAgreed) and never while someone plays.
# usage: stress_run.ps1 -Label baseline -DebugLua 1 -IUserAgreed      (3 runs per configuration, interleaved A,B,A,B,A,B)
param(
    [string]$Label = "baseline",
    [int]$DebugLua = 1,          # 1 = with the diagnostics; 0 = to measure their cost
    [int]$DebugProfile = 0,      # 1 = main-thread sampler attribution pass (slows the game: never mix with timing runs)
    [int]$Refresh = 90,          # the headset refresh rate to simulate (the mock accepts 72/80/90/120)
    [int]$Settle = 30, [int]$Calm = 20, [int]$Inflict = 40, [int]$After = 40,
    [ValidateSet('rocket', 'boom', 'both')][string]$Mode = 'both',
    [int]$FrameB = -1,           # >= 0: [Performance] frameBMode for this run (also written to the live control file)
    [int]$PerfTrace = 1,
    [int]$FrameLatency = -1,     # >= 0: [Performance] frameLatency for this run (also in the control file)
    [string]$MockSet = '',       # extra mock controls for the whole run, e.g. 'vdhold=50 vdholdevery=2' (simulated VD holds)
    [string[]]$Keys = @(),       # more [Performance] key=value pairs for this run (also in the control file), e.g. 'lazyBeginFrame=1','jobSpin=3'
    [switch]$Now,                # start now: no wait for an idle PC
    [switch]$IUserAgreed,
    [string]$BusyPattern = 'python'   # other programs whose CPU use would skew the results (regex on names)
)
$ErrorActionPreference = 'Continue'   # (native tools write to stderr: 'Stop' turned that into a failure)
$here = $PSScriptRoot
$s = Join-Path (Split-Path $here) 'mock'                  # tools/dev/mock: golevel2.ps1, cycle.ps1, mockctl.py, goto_tool.ps1
$py = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$game = if ($env:TEARDOWN_DIR) { $env:TEARDOWN_DIR } else { "C:\Program Files (x86)\Steam\steamapps\common\Teardown" }
$ini = "$game\TeardownVR.ini"; $log = "$game\TeardownVR.log"
$proj = (Resolve-Path "$here\..\..\..").Path   # (the repository)
$mockJson = "$proj\build\Release\tdvr_mock_openxr.json"
$opt = "$env:LOCALAPPDATA\Teardown\options.xml"
$ml = "$env:LOCALAPPDATA\Teardown\modlists\1.xml"

# ---- gates --------------------------------------------------------------------------------------------------
if (-not $IUserAgreed) { 'ABORT: this restarts the game - pass -IUserAgreed to confirm'; return }
if ((Get-Process teardown -ErrorAction SilentlyContinue) -and -not (Select-String -Path $ini -Pattern '^openxrRuntime=' -Quiet)) {
    'ABORT: Teardown is running a real headset session'; return
}
# (the game window comes to the front: never while someone is using the PC - 45 s without mouse/keyboard)
if (-not ('UIdle' -as [type])) { Add-Type 'using System; using System.Runtime.InteropServices; public static class UIdle { [StructLayout(LayoutKind.Sequential)] public struct LII { public uint cb; public uint t; } [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LII l); public static double Idle() { LII l = new LII(); l.cb = 8; GetLastInputInfo(ref l); return ((uint)Environment.TickCount - l.t) / 1000.0; } }' }
if (-not $Now -and [UIdle]::Idle() -lt 45 -and -not (Get-Process teardown -ErrorAction SilentlyContinue)) { "ABORT: the PC was used $([int][UIdle]::Idle()) s ago"; return }
$busy = Get-Process | Where-Object { $_.ProcessName -match $BusyPattern -and $_.CPU -gt 60 }
if ($busy) { "WARNING: other busy programs: $($busy.ProcessName -join ', ') - results may be skewed" }

# ---- run folder, backups, build identity -------------------------------------------------------------------
$run = Join-Path $here ("runs\" + (Get-Date -Format yyyyMMdd_HHmmss) + "_$Label")
New-Item -ItemType Directory -Force $run | Out-Null
Copy-Item $ini "$run\ini_live_backup.ini"
Copy-Item $opt "$run\options_backup.xml"
Copy-Item $ml "$run\modlist_backup.xml"
Get-FileHash "$proj\build\Release\dsound.dll", "$proj\mod\TeardownVR\main.lua" | ForEach-Object { "$($_.Hash) $($_.Path)" } | Out-File "$run\build.txt"
(Get-Item "$proj\build\Release\dsound.dll").LastWriteTime | Out-File -Append "$run\build.txt"
powercfg /getactivescheme | Out-File -Append "$run\build.txt"      # (read-only: recorded, never changed)

# ---- test ini = your ini + a few overrides (ASCII) ---------------------------------------------------
$over = @{
    'Debug'     = @{ debugLua = "$DebugLua"; debugProfile = "$DebugProfile"; openxrRuntime = $mockJson; perfTrace = "$PerfTrace" }
    'Affichage' = @{ refreshRate = "$Refresh" }
    'Controles' = @{ offGrab = '0' }   # left grip = crouch press -> main.lua mockPushTest Explosion (main.lua:2072-2078)
}
$ctl = @(); $perfOver = @{}
if ($FrameB -ge 0) { $perfOver['frameBMode'] = "$FrameB"; $ctl += "frameBMode=$FrameB" }
if ($FrameLatency -ge 0) { $perfOver['frameLatency'] = "$FrameLatency"; $ctl += "frameLatency=$FrameLatency" }
foreach ($kv in $Keys) { $k, $v = $kv -split '=', 2; $perfOver[$k] = $v; $ctl += "$k=$v" }
if ($perfOver.Count) { $over['Performance'] = $perfOver }
if ($ctl.Count) { Set-Content "$game\TeardownVR_perf.txt" $ctl -Encoding ascii }
$out = New-Object System.Collections.Generic.List[string]; $sec = ''; $done = @{}
foreach ($l in (Get-Content "$run\ini_live_backup.ini")) {
    if ($l -match '^\[(.+)\]') {
        if ($over.ContainsKey($sec)) { foreach ($k in $over[$sec].Keys) { if (-not $done["$sec.$k"]) { $out.Add("$k=$($over[$sec][$k])"); $done["$sec.$k"] = 1 } } }
        $sec = $Matches[1]; $out.Add($l); continue
    }
    if ($l -match '^([A-Za-z0-9_]+)=' -and $over.ContainsKey($sec) -and $over[$sec].ContainsKey($Matches[1])) {
        $k = $Matches[1]; $out.Add("$k=$($over[$sec][$k])"); $done["$sec.$k"] = 1; continue
    }
    $out.Add($l)
}
if ($over.ContainsKey($sec)) { foreach ($k in $over[$sec].Keys) { if (-not $done["$sec.$k"]) { $out.Add("$k=$($over[$sec][$k])"); $done["$sec.$k"] = 1 } } }
# (sections your ini does not have, e.g. [Performance]: added at the end)
foreach ($sn in $over.Keys) {
    $missing = @($over[$sn].Keys | Where-Object { -not $done["$sn.$_"] })
    if ($missing.Count) { $out.Add("[$sn]"); foreach ($k in $missing) { $out.Add("$k=$($over[$sn][$k])") } }
}
$out | Set-Content -Encoding ascii $ini
Copy-Item $ini "$run\ini_test.ini"

try {
    # ---- start the game in the level (golevel2: snapshot/restore of the mod list, waits for stereo) ---------
    $env:TD_BB_W = '1920'; $env:TD_BB_H = '1080'
    $gl = & "$s\golevel2.ps1"; $gl | Out-File "$run\golevel.txt"
    if (-not ($gl -match 'level ready')) { throw 'level not reached' }
    $p = Get-Process teardown
    $first = (Select-String -Path $log -Pattern 'Stereo frames: left=[1-9]' | Select-Object -First 1).Line
    $mainTid = if ($first -match '^\[\s*[\d.]+\]\[\s*(\d+)\]') { $Matches[1] } else { 0 }
    "pid=$($p.Id) mainTid=$mainTid start=$($p.StartTime.ToString('o'))" | Out-File "$run\process.txt"
    Select-String -Path "$env:LOCALAPPDATA\Teardown\log.txt" -Pattern 'Active mod:' | ForEach-Object Line | Out-File -Append "$run\process.txt"

    # ---- fixed pose, weapon ---------------------------------------------------------------------------------
    & $py "$s\mockctl.py" clear | Out-Null
    & $py "$s\mockctl.py" set headstill=1 headyaw=0 headpitch=0 hjit=0 hx=0 hy=0 hz=0 aimyaw=0 aimpitch=0 laimyaw=0 laimpitch=-80 | Out-Null
    if ($MockSet) { & $py "$s\mockctl.py" set @($MockSet -split ' ') | Out-Null }
    Start-Sleep 2
    if ($Mode -ne 'boom') { & "$s\goto_tool.ps1" -name rocket -max 45 | Select-Object -First 1 | Out-File "$run\tool.txt" }
    & $py "$s\mockctl.py" snap stress_view | Out-File -Append "$run\tool.txt"     # what the fixed camera sees (once per run)
    Start-Sleep $Settle                                                            # shader cache, streaming, physics at rest

    # ---- measure ---------------------------------------------------------------------------------------------
    $total = $Calm + $Inflict + $After + 8
    # (Start-Process joins the arguments with spaces: paths with spaces must be quoted)
    $ts = Start-Process $py -ArgumentList @("`"$here\thread_sampler.py`"", $p.Id, "`"$run\threads.csv`"", "`"$log`"", $total, 0.5) -PassThru -WindowStyle Hidden
    $tc = Start-Process typeperf -ArgumentList @('-cf', "`"$here\counters.txt`"", '-si', '1', '-sc', $total, '-f', 'CSV', '-o', "`"$run\counters.csv`"", '-y') -PassThru -WindowStyle Hidden
    $tg = Start-Process nvidia-smi -ArgumentList @('--query-gpu=timestamp,utilization.gpu,utilization.memory,utilization.encoder,clocks.sm,power.draw', '--format=csv,nounits', '-lms', '500', '-f', "$run\gpu.csv") -PassThru -WindowStyle Hidden
    Start-Sleep 2
    & $py "$here\stress_inputs.py" --calm $Calm --inflict $Inflict --after $After --mode $Mode --out "$run\phases.json" --log $log
    $ts.WaitForExit(20000) | Out-Null; $tc.WaitForExit(20000) | Out-Null
    if (-not $tg.HasExited) { Stop-Process -Id $tg.Id }
    Copy-Item $log "$run\TeardownVR.log"
    Copy-Item "$proj\build\Release\mock_out\mock.log" "$run\mock.log" -ErrorAction SilentlyContinue
}
finally {
    # ---- restore: close the game, your ini (byte for byte), options.xml, the mod list -----------------
    $p = Get-Process teardown -ErrorAction SilentlyContinue
    if ($p) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(25000)) { "WARNING: game still open" } }
    Copy-Item "$run\ini_live_backup.ini" $ini -Force
    # (a control file left behind would override your ini in your next headset session)
    if (Test-Path "$game\TeardownVR_perf.txt") { Copy-Item "$game\TeardownVR_perf.txt" "$run\control_file.txt"; [IO.File]::Delete("$game\TeardownVR_perf.txt") }
    if ((Get-FileHash $ini).Hash -ne (Get-FileHash "$run\ini_live_backup.ini").Hash) { "WARNING: ini not restored" }
    if ((Get-FileHash $opt).Hash -ne (Get-FileHash "$run\options_backup.xml").Hash) { Copy-Item "$run\options_backup.xml" $opt -Force; "options.xml put back" }
    if ((Get-FileHash $ml).Hash -ne (Get-FileHash "$run\modlist_backup.xml").Hash) { Copy-Item "$run\modlist_backup.xml" $ml -Force; "WARNING: mod list had changed - put back" }
}
& $py "$here\analyze_run.py" $run
