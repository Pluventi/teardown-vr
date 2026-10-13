# Read-only: wait until the player stops playing - game closed, or the headset still (camera
# motion "mean step 0.0 mm" in every report) for $stillMinutes - then print what happened.
param([int]$minutes = 180, [double]$stillMinutes = 3)
$game = if ($env:TEARDOWN_DIR) { $env:TEARDOWN_DIR } else { "C:\Program Files (x86)\Steam\steamapps\common\Teardown" }
$log = "$game\TeardownVR.log"
$t0 = Get-Date
$stillSince = $null
while (((Get-Date) - $t0).TotalMinutes -lt $minutes) {
    Start-Sleep 20
    if (-not (Get-Process teardown -ErrorAction SilentlyContinue)) { Write-Output "GAME CLOSED at $((Get-Date).ToString('HH:mm:ss'))"; return }
    $last = Get-Content $log -Tail 400 | Select-String -Pattern 'camera motion over 300 pairs: mean step ([0-9.]+) mm' | Select-Object -Last 4
    if (-not $last) { continue }
    $moving = $false
    foreach ($m in $last) { if ([double]$m.Matches[0].Groups[1].Value -gt 0.5) { $moving = $true } }
    if ($moving) { $stillSince = $null }
    elseif (-not $stillSince) { $stillSince = Get-Date }
    if ($stillSince -and ((Get-Date) - $stillSince).TotalMinutes -ge $stillMinutes) {
        Write-Output "HEADSET STILL since $($stillSince.ToString('HH:mm:ss')) (now $((Get-Date).ToString('HH:mm:ss')))"
        return
    }
}
Write-Output "TIMEOUT"
