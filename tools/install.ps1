# Teardown VR - installation / désinstallation
#   install.ps1              installe (ou met à jour) le mod
#   install.ps1 -Uninstall   retire tous les fichiers du mod
param([switch]$Uninstall)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-Teardown {
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    $libs = @()
    if ($steam) {
        $libs += $steam
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"') {
                $libs += $m.Matches[0].Groups[1].Value -replace '\\\\', '\'
            }
        }
    }
    foreach ($lib in $libs | Select-Object -Unique) {
        $dir = Join-Path $lib "steamapps\common\Teardown"
        if (Test-Path (Join-Path $dir "teardown.exe")) { return $dir }
    }
    return $null
}

Write-Host ""
Write-Host "=== Teardown VR ===" -ForegroundColor Cyan

if (Get-Process teardown -ErrorAction SilentlyContinue) {
    Write-Host "Ferme Teardown avant de continuer." -ForegroundColor Red
    exit 1
}

$game = Find-Teardown
if (-not $game) {
    $game = Read-Host "Dossier de Teardown introuvable. Colle le chemin du dossier qui contient teardown.exe"
    if (-not (Test-Path (Join-Path $game "teardown.exe"))) { Write-Host "teardown.exe introuvable." -ForegroundColor Red; exit 1 }
}
Write-Host "Jeu trouvé : $game"

$dll = Join-Path $game "dsound.dll"
$modDir = Join-Path $game "mods\teardownvr"

if ($Uninstall) {
    if (Test-Path $dll) { Remove-Item $dll -Force }
    if (Test-Path "$dll.original") { Rename-Item "$dll.original" "dsound.dll" }
    if (Test-Path $modDir) { Remove-Item $modDir -Recurse -Force }
    foreach ($f in "TeardownVR.log") { $p = Join-Path $game $f; if (Test-Path $p) { Remove-Item $p -Force } }
    Write-Host "Teardown VR a été désinstallé (tes réglages TeardownVR.ini sont conservés)." -ForegroundColor Green
    exit 0
}

# Une autre dsound.dll (autre mod) ? On la garde de côté.
if ((Test-Path $dll) -and -not (Select-String -Path $dll -Pattern "TDVR1" -SimpleMatch -Quiet)) {
    Copy-Item $dll "$dll.original" -Force
    Write-Host "Une autre dsound.dll existait : sauvegardée en dsound.dll.original" -ForegroundColor Yellow
}

Copy-Item (Join-Path $here "dsound.dll") $dll -Force
New-Item -ItemType Directory -Force $modDir | Out-Null
Copy-Item (Join-Path $here "mod\*") $modDir -Recurse -Force

Write-Host ""
Write-Host "Installation terminée !" -ForegroundColor Green
Write-Host "Dernière étape (une seule fois) :"
Write-Host "  1. Lance Teardown"
Write-Host "  2. Jouer > Gestionnaire de mods > liste 'Intégré' > 'Teardown VR' > clique sur 'Désactivé' pour l'activer"
Write-Host "  3. Lance Virtual Desktop (ou Meta Link) sur ton Quest 3, puis un niveau"
Write-Host ""
Write-Host "F10 ou appui long sur le bouton Menu de la manette gauche : réglages VR"
