# Cycle through all tools with the right stick (mouse wheel) on the mock and snap each mod tool.
param([int]$n = 22, [switch]$snap)
$s = $PSScriptRoot
$py = if ($env:PYTHON) { $env:PYTHON } else { "python" }
$game = if ($env:TEARDOWN_DIR) { $env:TEARDOWN_DIR } else { "C:\Program Files (x86)\Steam\steamapps\common\Teardown" }
$log = "$game\TeardownVR.log"
for ($i = 0; $i -lt $n; $i++) {
    & $py "$s\mockctl.py" hold ry=-0.9 0.25 | Out-Null
    Start-Sleep -Milliseconds 1800
    $tool = (Select-String -Path $log -Pattern 'Lua: .*tool' | Select-Object -Last 1).Line
    if ($snap) {
        $o = & $py "$s\mockctl.py" snap
        Write-Output "[$i] $($o | Select-Object -Last 1)"
    }
}
Select-String -Path $log -Pattern "mod tool '|comes from a mod|mod tool .*grip" | ForEach-Object { $_.Line }
