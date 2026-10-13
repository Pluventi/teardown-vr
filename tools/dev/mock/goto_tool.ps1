# Cycle tools on the mock (right stick down) until the held tool is $name, then dump its shapes (read-only).
param([string]$name, [int]$max = 26, [string]$dir = "-0.9")
$s = $PSScriptRoot
$py = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$snapPy = Join-Path $s '..\re\snap_shapes.py'   # (reads the running game's memory, read-only)
for ($i = 0; $i -lt $max; $i++) {
    $o = & $py $snapPy 2>$null
    if (($o | Select-Object -First 1) -match "tool $name") { $o; return }
    & $py "$s\mockctl.py" hold "ry=$dir" 0.25 | Out-Null
    Start-Sleep -Milliseconds 1200
}
Write-Output "tool $name not reached"

