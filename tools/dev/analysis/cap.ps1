# Capture the left eye on the mock: cap.ps1 <name> <renderPreset> <taaMode> <hjit>
# It EDITS the game folder's TeardownVR.ini (renderPreset / taaMode) and does not restore it: keep a copy of
# your settings. It restarts the game in a level (..\mock\golevel2.ps1). Captures: logs\cap_<name>.bmp.
$s = $PSScriptRoot
$game = if ($env:TEARDOWN_DIR) { $env:TEARDOWN_DIR } else { "C:\Program Files (x86)\Steam\steamapps\common\Teardown" }
$ini = "$game\TeardownVR.ini"
$py = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$out = if ($env:TDVR_MOCK_OUT) { $env:TDVR_MOCK_OUT } else { Join-Path (Resolve-Path "$s\..\..\..").Path "build\Release\mock_out" }
$mock = Join-Path $s "..\mock"
$logs = Join-Path $s "logs"; New-Item -ItemType Directory -Force $logs | Out-Null
$name, $preset, $mode, $jit = $args
$c = Get-Content $ini
$c = $c -replace '^renderPreset=.*', "renderPreset=$preset" -replace '^taaMode=.*', "taaMode=$mode"
$c | Set-Content -Encoding ascii $ini
& "$mock\golevel2.ps1" | Out-Null
& $py "$mock\mockctl.py" set headstill=1 hjit=$jit aimpitch=-80 hy=-0.9 | Out-Null; Start-Sleep 6
$shots = if ($env:CAP_SHOTS) { [int]$env:CAP_SHOTS } else { 1 }
for ($i = 0; $i -lt $shots; $i++) {
    $t0 = Get-Date
    & $py "$mock\mockctl.py" snap $name | Out-Null
    $bmp = Get-ChildItem $out -Filter '*_eye_left.bmp' | Where-Object { $_.LastWriteTime -ge $t0.AddSeconds(-2) } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $dst = if ($shots -gt 1) { "$logs\cap_${name}_$i.bmp" } else { "$logs\cap_$name.bmp" }
    Copy-Item $bmp.FullName $dst -Force
    Start-Sleep 1.3
}
Write-Output "$name -> $shots capture(s)"
