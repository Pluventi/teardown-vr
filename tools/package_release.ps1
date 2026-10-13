# Teardown VR - builds a release from a clone of the repository:
#   1. builds the mod's DLL (build\Release\dsound.dll);
#   2. writes installer\gen\payload.h and installer\gen\installer_gen.rc (git-ignored: they hold this PC's absolute
#      paths), then builds the players' installer (installer\: one .exe carrying the DLL and the Lua mod as resources);
#   3. writes dist\: "Installer Teardown VR.exe", README.txt (English guide), LISEZMOI.txt (French guide),
#      LICENCES.txt (Teardown VR's licence and source address, the third-party notices, the GPL text) and
#      "Teardown VR.zip" (those four files).
#
# usage (from any folder):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_release.ps1 -RepoUrl <url>
#                                                     [-GameShaders] [-NoBuild] [-OutDir <folder>]
#   -RepoUrl        REQUIRED: the public source repository's URL (https://github.com/<login>/<repository>), written
#                   in LICENCES.txt. GPL-3.0 section 6: a release must say where its source code is.
#   -GameShaders    build WITH the game-derived VR shaders (private src\generated\ only, never published; not covered
#                   by the GPL: LICENCES.txt then says so). Without it (the default), the DLL is built without them
#                   (CMake option TDVR_GAME_SHADERS=OFF), like any clone of the repository: the stereo-particles fix
#                   and the VR anti-aliasing modes are then unavailable and the game's own shaders are kept.
#   -NoBuild        package the DLL already in build\Release (no CMake run for the DLL). A DLL that build\ configured
#                   with the game-derived shaders is refused unless -GameShaders is given.
#   -OutDir         where to write the release files (default: dist\ in the repository).
#
# Needs: Visual Studio 2022 or its Build Tools, with the "Desktop development with C++" workload (CMake included),
# and the git submodules in third_party\ ("git clone --recursive", or "git submodule update --init").
# CMake is taken from $env:CMAKE, else from PATH, else from the Visual Studio installation (vswhere).
#
# Provided as is, without warranty and without support.
param(
    [string]$RepoUrl = '',
    [switch]$GameShaders,
    [switch]$NoBuild,
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
if (-not $RepoUrl -or $RepoUrl -match '[<>]' -or $RepoUrl -notmatch '^https?://') {
    throw ('Pass -RepoUrl https://github.com/<login>/<repository> (GPL-3.0 section 6: the release must say where ' +
           'its source code is).')
}
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exeName = 'Installer Teardown VR.exe'
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
elseif (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path (Get-Location).Path $OutDir }

function Find-CMake {
    if ($env:CMAKE -and (Test-Path $env:CMAKE)) { return $env:CMAKE }
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $pf = ${env:ProgramFiles(x86)}
    if (-not $pf) { $pf = $env:ProgramFiles }
    $vswhere = Join-Path $pf 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $paths = & $vswhere -all -prerelease -products '*' -property installationPath
        foreach ($vs in $paths) {
            $p = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $p) { return $p }
        }
    }
    throw ('CMake not found. Install Visual Studio 2022 (or its Build Tools) with the "Desktop development with ' +
           'C++" workload, or put cmake.exe on PATH, or set $env:CMAKE to its full path.')
}

# Writes text in the system's ANSI code page (the resource compiler reads .rc files that way); refuses a path that
# this code page cannot represent.
function Write-Ansi([string]$path, [string[]]$lines) {
    $enc = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage)
    $text = ($lines -join "`r`n") + "`r`n"
    if ($enc.GetString($enc.GetBytes($text)) -ne $text) {
        throw "The repository's path has characters the resource compiler cannot read ($root). Move the repository to a folder whose path has only plain characters."
    }
    [IO.File]::WriteAllText($path, $text, $enc)
}

$cmake = Find-CMake
Write-Host "CMake: $cmake"

# --- 0. the third-party libraries (git submodules) -------------------------------------------------------------
$tp = Join-Path $root 'third_party'
$notices = @(
    @('MinHook (BSD 2-Clause)', 'minhook\LICENSE.txt'),
    @('Dear ImGui (MIT)', 'imgui\LICENSE.txt'),
    @('OpenXR SDK - loader (Apache 2.0 OR MIT; the Apache 2.0 text)', 'openxr\LICENSES\Apache-2.0.txt'),
    @('JsonCpp, used by the OpenXR loader (public domain / MIT)', 'openxr\LICENSES\LicenseRef-jsoncpp-public-domain.txt'))
foreach ($n in $notices) {
    if (-not (Test-Path (Join-Path $tp $n[1]))) {
        throw "third_party\$($n[1]) is missing: get the submodules first (git submodule update --init)."
    }
}
# (the GPL's own text: the repository's LICENSE, which GitHub adds when the repository is created with the
#  "GPL-3.0" licence chosen; a repository created without it needs that file before its first release tag)
$gpl = $null
foreach ($name in 'LICENSE', 'LICENSE.txt', 'COPYING') {
    $p = Join-Path $root $name
    if (Test-Path $p) { $gpl = $p; break }
}
if (-not $gpl) {
    throw ('LICENSE not found at the repository root: the release must include the GPL-3.0 text. Build from a ' +
           'clone of the published repository (its LICENSE file holds that text). If the repository has no ' +
           'LICENSE yet: on GitHub, "Add file" > "Create new file", name it LICENSE, "Choose a license template" > ' +
           '"GNU General Public License v3.0", commit it to main, then push a new version tag on a commit that has it.')
}

# --- 1. the mod's DLL --------------------------------------------------------------------------------------------
$build = Join-Path $root 'build'
$hasGenerated = Test-Path (Join-Path $root 'src\generated\particles_p0.h')
# ($withShaders, not $gameShaders: PowerShell variable names ignore case, and $GameShaders is the switch)
$withShaders = $false
if (-not $NoBuild) {
    $opt = 'OFF'
    if ($GameShaders) {
        if (-not $hasGenerated) { throw '-GameShaders needs the private src\generated\ headers, which are not here.' }
        $opt = 'ON'
        $withShaders = $true
    }
    & $cmake -S $root -B $build -A x64 "-DTDVR_GAME_SHADERS=$opt" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'the DLL project did not configure (CMake)' }
    & $cmake --build $build --config Release --target TeardownVR | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'the DLL did not build' }
} else {
    # (-NoBuild: what the existing build was configured with)
    $cache = Join-Path $build 'CMakeCache.txt'
    if (Test-Path $cache) {
        $m = Select-String -Path $cache -Pattern '^TDVR_GAME_SHADERS:BOOL=(.+)$' | Select-Object -First 1
        if ($m -and ($m.Matches[0].Groups[1].Value -match '^(ON|TRUE|1|YES)$')) { $withShaders = $true }
    } elseif ($hasGenerated) {
        throw ('-NoBuild: build\CMakeCache.txt is missing, so it is unknown whether build\Release\dsound.dll has the ' +
               'game-derived shaders. Run without -NoBuild.')
    }
    if ($withShaders -and -not $GameShaders) {
        throw ('-NoBuild: build\ is configured WITH the game-derived shaders (TDVR_GAME_SHADERS=ON). Pass -GameShaders ' +
               'to release it that way, or run without -NoBuild to rebuild without them.')
    }
    if ($GameShaders -and -not $withShaders) {
        Write-Warning '-GameShaders: build\ is configured WITHOUT the game-derived shaders; the release has none.'
    }
}
$dll = Join-Path $root 'build\Release\dsound.dll'
if (-not (Test-Path $dll)) { throw 'build\Release\dsound.dll not found' }
if ($withShaders) {
    Write-Warning ('This DLL is built WITH the game-derived VR shaders (src\generated\, -GameShaders). They are not ' +
                   'part of the public source and not covered by the GPL: LICENCES.txt says so.')
}

# --- 2. the installer: its payload (resources 1000...), version, icon, manifest ----------------------------------
$inst = Join-Path $root 'installer'
$gen = Join-Path $inst 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null
$files = @(@{ src = $dll; rel = 'dsound.dll' })
$modSrc = Join-Path $root 'mod\TeardownVR'
foreach ($f in (Get-ChildItem $modSrc -Recurse -File | Sort-Object FullName)) {
    $files += @{ src = $f.FullName; rel = 'mods\teardownvr\' + $f.FullName.Substring($modSrc.Length + 1) }
}
$hash = (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 8).ToLower()
$version = (Get-Date -Format 'yyyy-MM-dd') + " ($hash)"
function RcPath($p) { return '"' + ($p -replace '\\', '\\') + '"' }
$h = @('// Generated by tools/package_release.ps1 - do not edit (git-ignored).', '#pragma once',
       'struct PayloadFile { int id; const wchar_t* path; };', 'static const PayloadFile kPayload[] = {')
$rc = @('// Generated by tools/package_release.ps1 - do not edit (git-ignored: it holds this PC''s absolute paths).',
        '#include <winresrc.h>', '#define IDI_APP 1',
        'IDI_APP ICON ' + (RcPath (Join-Path $inst 'icon.ico')),
        'CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST ' + (RcPath (Join-Path $inst 'installer.manifest')))
$id = 1000
foreach ($f in $files) {
    $h += '    { ' + $id + ', L"' + ($f.rel -replace '\\', '\\') + '" },'
    $rc += "$id RCDATA " + (RcPath $f.src)
    $id++
}
$h += '};'
$h += 'static const wchar_t kPayloadVersion[] = L"' + $version + '";'
# (one language-neutral block with the description in both languages: Windows shows the first block whatever
#  its own language - file properties, Task Manager, the administrator prompt)
$rc += @('VS_VERSION_INFO VERSIONINFO', ' FILEVERSION 1,0,0,0', ' PRODUCTVERSION 1,0,0,0', ' FILEFLAGSMASK 0x3fL',
         ' FILEFLAGS 0x0L', ' FILEOS VOS_NT_WINDOWS32', ' FILETYPE VFT_APP', ' FILESUBTYPE VFT2_UNKNOWN', 'BEGIN',
         '  BLOCK "StringFileInfo"', '  BEGIN', '    BLOCK "000004b0"', '    BEGIN',
         '      VALUE "CompanyName", "Teardown VR"',
         '      VALUE "FileDescription", "Teardown VR Setup / Installation de Teardown VR"',
         ('      VALUE "FileVersion", "' + $version + '"'), '      VALUE "ProductName", "Teardown VR"',
         ('      VALUE "ProductVersion", "' + $version + '"'), ('      VALUE "OriginalFilename", "' + $exeName + '"'),
         '    END', '  END', '  BLOCK "VarFileInfo"', '  BEGIN', '    VALUE "Translation", 0x0, 1200', '  END', 'END')
Set-Content -Path (Join-Path $gen 'payload.h') -Value $h -Encoding ascii
Write-Ansi (Join-Path $gen 'installer_gen.rc') $rc
$ib = Join-Path $inst 'build'
if (-not (Test-Path (Join-Path $ib 'CMakeCache.txt'))) {
    & $cmake -S $inst -B $ib -A x64 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'the installer project did not configure (CMake)' }
}
& $cmake --build $ib --config Release | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'the installer did not build' }
$exe = Join-Path $ib 'Release\TeardownVR_Installer.exe'
if (-not (Test-Path $exe)) { throw 'installer\build\Release\TeardownVR_Installer.exe not found' }

# --- 3. the release files ----------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($n in $exeName, 'LISEZMOI.txt', 'README.txt', 'LICENCES.txt', 'Teardown VR.zip') {
    $p = Join-Path $OutDir $n
    if (Test-Path $p) { [IO.File]::Delete($p) }
}
Copy-Item $exe (Join-Path $OutDir $exeName)
$bom = New-Object System.Text.UTF8Encoding($true)
$guide = [IO.File]::ReadAllText((Join-Path $root 'tools\LISEZMOI.txt'), [Text.Encoding]::UTF8)
[IO.File]::WriteAllText((Join-Path $OutDir 'LISEZMOI.txt'), $guide, $bom)
$guideEn = [IO.File]::ReadAllText((Join-Path $root 'tools\README.txt'), [Text.Encoding]::UTF8)
[IO.File]::WriteAllText((Join-Path $OutDir 'README.txt'), $guideEn, $bom)

$nl = "`r`n"
$line = '=============================================================================='
$e = [char]0x00E8   # (this script stays plain ASCII: Windows PowerShell reads a .ps1 without BOM as ANSI)
$lic = "Teardown VR - Copyright (C) 2026 VR Games For All - GPL-3.0-only; source: $RepoUrl$nl" +
       "Teardown VR is free software under the GNU General Public License, version 3 only. It is provided as is,$nl" +
       "without any warranty and without support; a Teardown update can break it. It is not affiliated with or$nl" +
       "endorsed by Tuxedo Labs.$nl"
if ($withShaders) {
    $lic += "$nl" + "NOTICE: this build contains shader bytecode derived from Teardown's own shaders (Tuxedo Labs). That$nl" +
            "part is not covered by the GPL and is not in the source repository.$nl"
}
$lic += "$nl" + "Teardown VR uses these open-source libraries; their licences require these notices.$nl" +
        "(Teardown VR utilise ces biblioth$($e)ques libres. Leurs licences demandent de joindre ces textes.)$nl"
foreach ($n in $notices) {
    $lic += "$nl$line$nl" + $n[0] + "$nl$line$nl" + [IO.File]::ReadAllText((Join-Path $tp $n[1]), [Text.Encoding]::UTF8)
}
# (the GPL's own text, from the repository's LICENSE: section 0)
$lic += "$nl$line$nl" + 'Teardown VR - GNU General Public License version 3' + "$nl$line$nl" +
        [IO.File]::ReadAllText($gpl, [Text.Encoding]::UTF8)
[IO.File]::WriteAllText((Join-Path $OutDir 'LICENCES.txt'), $lic, $bom)
Compress-Archive -Path (Join-Path $OutDir $exeName), (Join-Path $OutDir 'LISEZMOI.txt'), (Join-Path $OutDir 'README.txt'),
                       (Join-Path $OutDir 'LICENCES.txt') -DestinationPath (Join-Path $OutDir 'Teardown VR.zip')
$global:LASTEXITCODE = 0
$shadersText = 'without'
if ($withShaders) { $shadersText = 'WITH' }
Write-Host "Version $version ($shadersText the game-derived VR shaders)"
Write-Host "Release files: $OutDir"
