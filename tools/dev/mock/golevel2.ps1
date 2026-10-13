# Restart the game (cycle.ps1: build + mod installed, session log saved) and enter Lee Chemicals
# (sandbox) on the mock headset; waits for the first stereo frame in the level. The ini must
# already point to the mock.
# The active mod list is snapshotted first (in logs\) and put back if it changed during the run: a blind
# click can land in the mod manager (Play submenu) and change it. So a failed attempt restarts the game
# instead of clicking again.
$s = $PSScriptRoot
$py = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$game = if ($env:TEARDOWN_DIR) { $env:TEARDOWN_DIR } else { "C:\Program Files (x86)\Steam\steamapps\common\Teardown" }
$log = "$game\TeardownVR.log"
$ml = "$env:LOCALAPPDATA\Teardown\modlists\1.xml"
$env:TD_BB_W = '1920'; $env:TD_BB_H = '1080'
$snap = [IO.File]::ReadAllBytes($ml)
$logs = Join-Path $s "logs"; New-Item -ItemType Directory -Force $logs | Out-Null
[IO.File]::WriteAllBytes("$logs\modlist_before_mock.xml", $snap)
function CheckModList {
    $now = [IO.File]::ReadAllBytes($ml)
    if ([Convert]::ToBase64String($now) -ne [Convert]::ToBase64String($snap)) {
        [IO.File]::WriteAllBytes("$logs\modlist_changed_$(Get-Date -Format HHmmss).xml", $now)
        [IO.File]::WriteAllBytes($ml, $snap)
        Write-Output "WARNING: the active mod list changed during the run - put back"
    }
}
for ($try = 0; $try -lt 3; $try++) {
    & "$s\cycle.ps1" | Out-Null
    & $py "$s\mockctl.py" clear | Out-Null
    & $py "$s\mockctl.py" set laimyaw=0 laimpitch=-80 | Out-Null
    Start-Sleep 4
    # (the start-up warning "Appuyez sur une touche pour continuer" eats the first click: one
    # click at the bottom centre - empty in the main menu - dismisses it)
    & $py "$s\mockctl.py" point 960 1000 0.25 | Out-Null; Start-Sleep 3
    & $py "$s\mockctl.py" point 608 76 0.25 | Out-Null; Start-Sleep 1.5
    & $py "$s\mockctl.py" point 608 255 0.25 | Out-Null; Start-Sleep 3.5
    $n = (Get-Content $log).Count
    & $py "$s\mockctl.py" point 512 594 0.25 | Out-Null
    $t0 = Get-Date
    do { Start-Sleep 1; $ok = (Get-Content $log | Select-Object -Skip $n | Select-String -Pattern 'Stereo frames: left=[1-9]' -Quiet) } until ($ok -or ((Get-Date) - $t0).TotalSeconds -gt 35)
    if ($ok) { Start-Sleep 3; CheckModList; Write-Output "level ready"; return }
    Write-Output "level not reached, restarting the game"
    $p = Get-Process teardown -ErrorAction SilentlyContinue
    if ($p) { $p.CloseMainWindow() | Out-Null; $p.WaitForExit(25000) | Out-Null; Start-Sleep 3 }
    CheckModList
}
