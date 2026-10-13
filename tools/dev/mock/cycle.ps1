# Dev cycle: save the session log, close game, install DLL + mod, relaunch, wait until our UI is ready.
# (-focus: also bring the game window to the front - never while someone is using the PC)
param([switch]$focus)
$game = if ($env:TEARDOWN_DIR) { $env:TEARDOWN_DIR } else { "C:\Program Files (x86)\Steam\steamapps\common\Teardown" }
$proj = (Resolve-Path "$PSScriptRoot\..\..\..").Path   # (the repository)
$p = Get-Process teardown -ErrorAction SilentlyContinue
if ($p) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(20000)) { $p | Stop-Process -Force } }
# (the next start overwrites TeardownVR.log: always keep the previous session's log, in logs\, whether the
# game is still running or not)
if (Test-Path "$game\TeardownVR.log") {
    $kind = if (Select-String -Path "$game\TeardownVR.log" -Pattern 'VirtualDesktopXR' -Quiet) { 'headset' } else { 'mock' }
    $logs = Join-Path $PSScriptRoot "logs"; New-Item -ItemType Directory -Force $logs | Out-Null
    Copy-Item "$game\TeardownVR.log" "$logs\session_${kind}_$(Get-Date -Format MMdd_HHmmss).log" -Force
}
# The DLL stays locked until the process is completely gone.
$copied = $false
for ($i = 0; $i -lt 60 -and -not $copied; $i++) {
    try {
        Copy-Item "$proj\build\Release\dsound.dll" "$game\dsound.dll" -Force -ErrorAction Stop
        $copied = $true
    } catch { Start-Sleep -Milliseconds 500 }
}
if (-not $copied) { Write-Output "COPY FAILED"; exit 1 }
Copy-Item "$proj\mod\TeardownVR\*" "$game\mods\teardownvr" -Force
# The VR resolution is only chosen when the game starts (started while the headset is off, the game renders
# at the monitor's size). With a real-headset ini (no openxrRuntime line), launch only if the headset is
# connected now (probe run with the game closed: xrGetSystem -> 0); otherwise deploy only.
if (-not (Select-String -Path "$game\TeardownVR.ini" -Pattern '^openxrRuntime=' -Quiet)) {
    $probeExe = "$proj\build\Release\tdvr_xr_probe.exe"   # (EXCLUDE_FROM_ALL: build its target if missing)
    if (-not (Test-Path $probeExe)) { Write-Output "NOT LAUNCHED: probe missing ($probeExe) - deployed only"; exit 3 }
    $probe = & $probeExe 2>&1 | Out-String
    if ($probe -notmatch 'xrGetSystem -> (-?\d+)') { Write-Output "NOT LAUNCHED: OpenXR runtime not answering (deployed only): $($probe.Trim())"; exit 3 }
    if ($Matches[1] -ne '0') { Write-Output "NOT LAUNCHED: headset not connected, xrGetSystem -> $($Matches[1]) (deployed only)"; exit 2 }
}
Start-Process "steam://rungameid/1167630"
$log = "$game\TeardownVR.log"
$t0 = Get-Date
do {
    Start-Sleep -Milliseconds 500
    $ok = (Test-Path $log) -and ((Get-Item $log).LastWriteTime -gt $t0) -and (Select-String -Path $log -Pattern "Ui: ready" -Quiet)
} until ($ok -or ((Get-Date) - $t0).TotalSeconds -gt 90)
Start-Sleep 6
if ($focus) { & "$PSScriptRoot\focus.ps1" } else { "started" }
