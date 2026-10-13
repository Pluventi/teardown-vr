# Building Teardown VR

This page explains how to build Teardown VR from a fresh clone. It covers:

- the mod's DLL;
- the test-only tools;
- the players' installer;
- installing a build by hand.

All commands are run from the repository root. `<game>` is the Teardown folder, the one that contains
`teardown.exe`.

> The mod is provided as is, without support. These instructions describe how the project builds; nothing
> promises that a given setup will be helped or fixed.

## 1. Prerequisites

- **Windows 10 or 11, 64-bit.**
- **Visual Studio 2022**, or the free **Visual Studio 2022 Build Tools**, with the workload "Desktop development
  with C++". Keep these components:
  - MSVC v143 build tools (x64/x86);
  - a Windows 10 or Windows 11 SDK;
  - **C++ CMake tools for Windows** (CMake 3.20 or newer is required).
- **Git**, to get the third-party libraries (git submodules).
- **Teardown 2.0.1** (Steam), to test the result.
- Optional, only for the development scripts in `tools/dev/`: Python 3.10 or newer, with `numpy`, `scipy`,
  `pillow`, `capstone`, `pefile` and `psutil`. You do not need Python to build the mod.

Run the commands below in a **"Developer PowerShell for VS 2022"** or an **"x64 Native Tools Command Prompt for
VS 2022"** (both are in the Start menu). These prompts put CMake and the compiler on `PATH`. A stand-alone CMake
on `PATH` also works. If several Visual Studio versions are installed, add `-G "Visual Studio 17 2022"` to the
configure commands.

## 2. Get the source

```bash
git clone --recursive <repository URL>
cd <repository folder>
```

If you cloned without `--recursive`, fetch the submodules afterwards:

```bash
git submodule update --init --recursive
```

GitHub's "Download ZIP" and the release source archives do **not** contain the submodules. `third_party/` is then
empty and the configure step fails. Use `git clone`.

The submodules are pinned to these upstream versions. They are used unmodified.

| Folder | Upstream | Version |
|---|---|---|
| `third_party/imgui` | https://github.com/ocornut/imgui | tag `v1.91.9` (commit `97428e8ac99e339ce05eee531cf55b77b29ea709`) |
| `third_party/minhook` | https://github.com/TsudaKageyu/minhook | commit `8af6b4acae5a9388fd742b56fa79ece89d96f823` (untagged) |
| `third_party/openxr` | https://github.com/KhronosGroup/OpenXR-SDK | tag `release-1.1.49` (commit `977f6675bc0057d5a54ed290cb5c71c699b1c0ab`) |

## 3. Build the DLL

```bash
cmake -S . -B build -A x64
cmake --build build --config Release --target TeardownVR
```

The result is **`build/Release/dsound.dll`**. The CMake target is `TeardownVR`, and its output name is `dsound`.
In a public clone, the configure step prints:

```
TeardownVR: built WITHOUT the game-derived VR shaders (stereo particles and VR anti-aliasing off)
```

That message is expected (see section 4).

What the root `CMakeLists.txt` sets up:

- **C++20**, with `/utf-8 /W3`.
- The **dynamic** Visual C++ runtime: `/MD`, or `/MDd` in Debug. The player's PC needs the Microsoft Visual C++
  2015-2022 Redistributable (x64).
- The **OpenXR loader** is built from `third_party/openxr` as a **static** library, with its tests, API layers and
  conformance tests turned off. There is no `openxr_loader.dll` to ship.
- **MinHook** (`minhook`) and **Dear ImGui** (`imgui`, with the DX12 and Win32 backends) are built as static
  libraries.
- `d3d12.dll`, `dxgi.dll` and `pdh.dll` are **delay-loaded**, so the DLL's `DllMain` never pulls them in under the
  loader lock.
- The dsound exports come from `src/dsound.def`. They forward to `System32\dsound.dll`.

A Debug build (`--config Debug`, output `build/Debug/dsound.dll`) needs Visual Studio's debug runtime. Use it
only on a development PC.

## 4. The `TDVR_GAME_SHADERS` option

Three of the mod's shaders are rebuilt from Teardown's own shaders:

- the VR anti-aliasing (TAA) compute shader;
- the particle G-buffer shader;
- the ambient / directional light shader.

They are game-derived, so they are **not** in this repository. `shaders/` (their HLSL) and `src/generated/`
(their compiled blobs) are git-ignored. The CMake option `TDVR_GAME_SHADERS` decides whether the build uses them:

| Value | When | Result |
|---|---|---|
| default (automatic) | `ON` only if `src/generated/particles_p0.h` exists, so `OFF` in a public clone | see below |
| `OFF` | `-DTDVR_GAME_SHADERS=OFF` | defines `TDVR_NO_GAME_SHADERS`; builds without any game-derived file |
| `ON` | `-DTDVR_GAME_SHADERS=ON` | needs all ten headers of `src/generated/` (private builds only: these files are not published) |

In an `OFF` build:

- The **VR anti-aliasing modes** (`[Affichage] taaMode` 1 to 4, default 4) keep the game's own TAA. The log
  says: `TAA: this build has no VR anti-aliasing shaders (TDVR_GAME_SHADERS=OFF) - the game's is kept`. The
  development mode `taaMode=9` still works: it loads a test shader file `TeardownVR_taa_test.cso` placed next to
  `teardown.exe`.
- **Stereo particles** (`[Affichage] stereoParticles`) has no effect: the particle hooks are not installed. The log
  says once `Particles: this build has no stereo-particle shaders (TDVR_GAME_SHADERS=OFF) - the game's are kept`.
- The settings panel says so too: "-> not in this build (made without the game-derived shaders)" under the
  particle option, and "-> not in this build: the game's own anti-aliasing is used" under "Distance sharpness"
  when a VR mode is selected.
- Everything else is identical.

The value is cached in `build/CMakeCache.txt`. To change it, run the configure command again with the option, or
delete `build/`.

`ON` needs exactly these ten files in `src/generated/`:

- `taa_net.h`, `taa_natural.h`, `taa_sharp.h`, `taa_far.h`, `taa_raw.h`;
- `particles_p0.h`, `particles_p1.h`, `particles_p0_parity.h`, `particles_p1_parity.h`;
- `ambient_vr.h`.

The automatic detection only checks `particles_p0.h`. A partial folder therefore selects `ON` and fails to
compile. Never commit these files.

## 5. Test-only targets: the mock runtime and the probe

Two more targets are `EXCLUDE_FROM_ALL`. They are only built when you name them, and they are **never shipped**
to players:

```bash
cmake --build build --config Release --target tdvr_mock_openxr
cmake --build build --config Release --target tdvr_xr_probe
```

| Target | Output | What it is |
|---|---|---|
| `tdvr_mock_openxr` | `build/Release/tdvr_mock_openxr.dll`, with `tdvr_mock_openxr.json` and `tdvr_mock_openxr_nohmd.json` copied next to it | A fake OpenXR runtime ("mock headset"). With it, the mod's real OpenXR path runs without a headset. |
| `tdvr_xr_probe` | `build/Release/tdvr_xr_probe.exe` | A command-line check of an OpenXR runtime. It goes through the same static loader as the mod. |

The two manifests name the DLL as `./tdvr_mock_openxr.dll`, so keep the three files together.
[DEV_TOOLS.md](DEV_TOOLS.md) explains how to use both.

## 6. The players' installer

`installer/` is a **separate CMake project** (`TeardownVRInstaller`, languages C++ and RC). The root build does
not include it. It builds one Win32 program, the target `TeardownVR_Installer`. It uses the **static** C runtime
(`/MT`), so it runs on a PC with no runtime installed.

The installer carries the mod as resources. It needs two **generated** files in `installer/gen/`. That folder is
git-ignored and must never be committed, because the generated `.rc` contains absolute paths of the machine that
wrote it.

**`installer/gen/payload.h`** holds the list of files to install and a version string:

```cpp
// Generated - do not edit.
#pragma once
struct PayloadFile { int id; const wchar_t* path; };
static const PayloadFile kPayload[] = {
    { 1000, L"dsound.dll" },
    { 1001, L"mods\\teardownvr\\info.txt" },
    { 1002, L"mods\\teardownvr\\main.lua" },
    { 1003, L"mods\\teardownvr\\proxy.vox" },
};
static const wchar_t kPayloadVersion[] = L"2026-10-13 (1a2b3c4d)";
```

- Each entry is a resource id and the file's path relative to `<game>`.
- `dsound.dll` is 1000, then each file of `mod/TeardownVR/` follows in order.
- The version string is the release date, plus the first 8 hex digits of the DLL's SHA-256.

**`installer/gen/installer_gen.rc`** contains:

- the icon (`IDI_APP`, i.e. 1, from `installer/icon.ico`);
- the manifest (`installer/installer.manifest`, as the `CREATEPROCESS_MANIFEST_RESOURCE_ID` / `RT_MANIFEST`
  resource; the linker's own manifest is turned off);
- one `RCDATA` resource per payload file, with the same ids as `payload.h`;
- a `VERSIONINFO` block.

### 6.1 The release script: `tools/package_release.ps1`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_release.ps1 -RepoUrl <repository URL>
```

This generic script makes a complete release from a clone. It has no personal paths. Its options:

| Option | Meaning |
|---|---|
| `-RepoUrl <url>` | **Required.** The public source repository's address (`https://...`), written in `LICENCES.txt`. GPL-3.0 section 6 requires a binary release to say where its source code is, so the script refuses to run without it. |
| `-GameShaders` | Build **with** the game-derived VR shaders. Only possible with the private `src/generated/` headers, which are not published. That part is not covered by the GPL, and `LICENCES.txt` then says so. Without this switch, the release is always built without them. |
| `-NoBuild` | Package the DLL already in `build/Release/` instead of building it. A `build/` configured with the game-derived shaders is refused unless `-GameShaders` is given. |
| `-OutDir <folder>` | Where to write the release files (default: `dist/`). |

The script stops before building anything if the submodules or the repository's `LICENSE` file are missing. It:

1. finds CMake: `$env:CMAKE` first, then `PATH`, then Visual Studio's `vswhere`;
2. configures `build/` with `-DTDVR_GAME_SHADERS=OFF` (or `ON` with `-GameShaders`) and builds the DLL
   (`TeardownVR`);
3. generates `installer/gen/payload.h` and `installer/gen/installer_gen.rc` from `build/Release/dsound.dll` and
   every file of `mod/TeardownVR/`;
4. configures and builds `installer/`, which gives `installer/build/Release/TeardownVR_Installer.exe`;
5. writes `dist/` with:
   - `Installer Teardown VR.exe`;
   - `README.txt` and `LISEZMOI.txt`, the players' guides from `tools/`;
   - `LICENCES.txt`: Teardown VR's licence line with the source repository's address (given with `-RepoUrl`), the
     notices of the third-party libraries (MinHook, Dear ImGui, the OpenXR SDK loader and the JsonCpp code it
     bundles), the GPL-3.0 text from the repository's `LICENSE`, and, only for a build with the private
     game-derived shaders, a NOTICE that this part is not covered by the GPL;
   - `Teardown VR.zip`, which contains the four files above.

`dist/` is git-ignored.

### 6.2 Building the installer by hand

1. Build the DLL (section 3).
2. Write `installer/gen/payload.h` and `installer/gen/installer_gen.rc` as described above.
3. Build the installer:

   ```bash
   cmake -S installer -B installer/build -A x64
   cmake --build installer/build --config Release
   ```

The result is `installer/build/Release/TeardownVR_Installer.exe`.

### 6.3 Testing the installer safely

Without arguments, the installer shows its windows. For tests, it accepts:

| Argument | Meaning |
|---|---|
| `/install`, `/uninstall`, `/logs` | Run that action without the main window. `/logs` builds the logs zip. |
| `/out "<folder>"` | Where `/logs` writes its zip (default: the Desktop). |
| `/gamedir "<folder>"` | The game folder to use, instead of finding it through Steam. |
| `/lang en` or `/lang fr` | The language, without the language window. |
| `/silent` | No window at all. |

Exit codes: 0 done, 1 closed or cancelled, 2 game running, 3 no game folder, 5 access denied. Any other value is
a Windows error code.

> **Without `/gamedir`, the installer finds and changes the real game installation through Steam.** For tests,
> use a separate folder that contains a dummy `teardown.exe`, and always pass `/gamedir`.
>
> Nested PowerShell quoting can silently drop the `/gamedir` argument. Check the arguments the installer really
> received, or start it with .NET `ProcessStartInfo`.

The installer will not install or uninstall while any process named `teardown.exe` is running, even when it
targets a test folder. It offers Retry / Cancel, and with `/silent` it exits with code 2.

## 7. Installing by hand

1. **Close Teardown.**
2. **Open the game folder**, the one with `teardown.exe`. In Steam: right-click Teardown, then Manage, then Browse
   local files. The default location is `C:\Program Files (x86)\Steam\steamapps\common\Teardown`.
3. **Keep any other `dsound.dll`.** If a `dsound.dll` is already there and it is not Teardown VR's (another mod),
   rename it to `dsound.dll.original`. Teardown VR's DLL always contains the text `TDVR1`; the installer and
   `tools/install.ps1` use that test. Only one of the two DLLs can be loaded at a time.
4. **Copy the DLL.** Put `build/Release/dsound.dll` into the game folder, next to `teardown.exe`.
5. **Copy the Lua mod.** Create `<game>\mods\teardownvr\` and copy into it the files of `mod/TeardownVR/`:
   `info.txt`, `main.lua` and `proxy.vox`.
   - It must be the game's own `mods` folder. The Lua ↔ DLL channel uses two Lua functions that the game only
     allows for its built-in mods.
   - A copy among the local mods (`Documents\Teardown\mods`) does not work.
   - The mod never patches that check.
6. **Enable the mod (once).** Start Teardown, then go to Play > Mod manager > "Built-in" list > "Teardown VR" and
   enable it.
7. **Connect the headset before you start the game.** The VR render resolution is chosen when the game starts.
8. **Check `<game>\TeardownVR.log`.** It should contain the line `Engine: synchronised stereo ready`.

**With a script.** `tools/install.ps1` does the same, together with `tools/install.bat` and
`tools/uninstall.bat`. It finds the game through Steam and keeps another `dsound.dll` as `dsound.dll.original`.
It copies `dsound.dll` and the contents of a `mod\` folder **from its own folder**, so first put the DLL and a
`mod\` folder with the three Lua mod files next to the script. `install.ps1 -Uninstall` removes the mod. Its
messages are in French.

**Uninstalling by hand:**

1. Delete `<game>\dsound.dll`.
2. Rename `dsound.dll.original` back to `dsound.dll`, if it exists.
3. Delete `<game>\mods\teardownvr\`.
4. Optionally, also delete what the mod wrote next to the game:
   - `TeardownVR.ini`;
   - `TeardownVR.log` and the `TeardownVR_journaux\` folder;
   - `TeardownVR_xrloader.log` and `TeardownVR_perf.txt`, if present.

## 8. Common build problems

| Symptom | Cause |
|---|---|
| `The source directory ... third_party/openxr does not contain a CMakeLists.txt file` | The submodules are missing: run `git submodule update --init --recursive`. |
| `cmake` is not recognised | Use a "Developer PowerShell for VS 2022", or install CMake and put it on `PATH`. |
| A `src/generated/...h` file is not found | `src/generated/` is incomplete: delete it, or configure with `-DTDVR_GAME_SHADERS=OFF`. |
| The game does not start once `dsound.dll` is in place | The Visual C++ x64 runtime is missing, or you copied a Debug build to a PC without Visual Studio. |
| No `TeardownVR.log` appears next to the game | The DLL is not loaded: check that it is named `dsound.dll` and sits next to `teardown.exe`, and that an antivirus did not remove it. |

See also [DEV_TOOLS.md](DEV_TOOLS.md), [ARCHITECTURE.md](ARCHITECTURE.md) and the [licence](../LICENSE).
