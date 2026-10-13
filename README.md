# Teardown VR

Teardown VR is an unofficial virtual reality mod for **Teardown 2.0.1** (Tuxedo Labs, Steam, PC). It turns the
game into a PC VR game through OpenXR. You see the world in true 3D, aim and use your tools with motion
controllers, move with the stick or by walking around the room, and use menus on a virtual screen. Both eyes show
the same moment of the game ("synchronised stereo"): each headset frame is rendered as two engine frames, the
second one at zero game time. The mod has two parts. A native DLL, which the game loads as `dsound.dll`, handles
OpenXR, Direct3D 12 and the engine hooks. A Lua mod, installed among the game's built-in mods, handles the
camera, aiming, tools and physics. It was made and tested with a Meta Quest 3 through Virtual Desktop and Quest
Link. SteamVR can also be selected.

> **Warning: a Teardown update can break the mod.**
> The mod is written for Teardown 2.0.1. It finds the game's internal functions at run time by searching for byte
> patterns. After a game update, some patterns may no longer match. The affected features then switch themselves
> off and write a line in `TeardownVR.log`. Synchronised stereo can fall back to alternating eyes, and the mod
> can stop working altogether. No fix or update is promised. See
> [docs/UPDATING_FOR_A_NEW_GAME_VERSION.md](docs/UPDATING_FOR_A_NEW_GAME_VERSION.md).

## Features

- **Synchronised stereo**: both eyes show the same game instant. Each eye renders the headset's own asymmetric
  (off-centre) field of view, at a render resolution chosen for the headset.
- **Controller aiming** with a laser. The held tool follows the VR hand and keeps its own animations. Tools added
  by other mods are scaled down to hand size.
- **Physical interaction**: a physical copy of the tool that pushes objects, a visible free arm and a physical
  free hand that can grab, plus far grab with the game's own rules. The sledgehammer can be swung physically.
- **Locomotion and comfort**: stick movement, snap or smooth turning, roomscale, an optional comfort vignette,
  and recentering.
- **A VR inventory** worn at the belt, and vehicles driven with the controllers.
- **Other mods in VR**: their weapons aim with the controller and their crosshairs follow the laser. Their mouse
  windows and menus become floating panels you can use with the controllers. Their cameras (for example an
  aircraft view) work in VR. A keys panel presses their keyboard keys for you. One two-pistol mod can be used
  with both hands (dual wield).
- **Menus, loading screens and pause** on a virtual screen (16:9 during a paused level).
- **A settings panel** in English or French, shown in the game and in the headset (F10, or a long press on the
  left controller's Menu button).
- **Automatic headset detection** at start: Virtual Desktop, Meta Quest Link or SteamVR.
- **Performance work** that keeps the image identical: a cheaper right-eye frame, CPU/GPU overlap and
  per-eye temporal history.
- **Graceful degradation**: a feature whose game code is not recognised is designed to turn itself off and log
  why, instead of crashing. A game change that no check catches can still crash the game.

## Requirements

- Teardown **2.0.1**, Steam version, on Windows 10 or 11 (64-bit). Other versions: see the warning above.
- A PC VR headset with an OpenXR runtime. The mod was tested with a Meta Quest 3 through Virtual Desktop (VDXR
  runtime) and Meta Quest Link / Air Link. SteamVR can be selected in the settings.
- A PC that can run Teardown at about **twice** the headset's refresh rate (for example about 144 fps for a
  72 Hz headset), because every headset frame takes two game frames.
- The Microsoft Visual C++ 2015-2022 Redistributable (x64). The DLL links the Visual C++ runtime dynamically.
- To build it yourself: Visual Studio 2022 (Build Tools are enough) with C++ and CMake, plus Git. See
  [docs/BUILDING.md](docs/BUILDING.md).

## Download

The **ready-to-use installer** is published on the author's Patreon:
**https://patreon.com/VRGamesForAll**. It is a single `.exe` that finds the game through Steam and installs,
updates or uninstalls the mod. The installer may include shader bytecode derived from Teardown's own shaders (for
the two features listed below). That part is not covered by the GPL and is not in this repository.

The source code in this repository builds a version **without the game-derived shaders**. Three of the mod's
shaders are rebuilt from Teardown's own shaders, so they are not published here. Without them:

- the stereo particles fix (rain, smoke and fire drawn the same way in both eyes) is unavailable;
- the VR anti-aliasing modes are unavailable;
- in both cases, the game's own shaders are used.

All the other features are built from this source.
[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) describes in prose what those shaders change.

### GitHub Releases: built by GitHub, verifiable

The [Releases](../../releases) page has ready-to-use builds of **this source** (without the game-derived shaders).
They are not built on anyone's PC: the [release workflow](.github/workflows/release.yml) builds them on GitHub's
own machines from the tagged commit, and anyone can check that:

- **SHA-256**: `SHA256SUMS.txt` and the release notes list the checksum of every file. Compare with
  `Get-FileHash Teardown-VR.zip -Algorithm SHA256` in PowerShell.
- **Build provenance**: GitHub signs an attestation that the files were built by this workflow from this
  repository. Check it with the GitHub CLI: `gh attestation verify Teardown-VR.zip --repo <owner>/<repository>`.
- **VirusTotal**: when the release notes have VirusTotal links, the files were uploaded there for scanning by
  dozens of antivirus engines. You can also upload them yourself on https://www.virustotal.com.

The program is not code-signed, so Windows may show "Windows protected your PC": "More info", then "Run anyway".
A mod that loads into a game as a `dsound.dll` can occasionally be flagged by an antivirus heuristic; the source is
here to read and to build yourself.

## Building from source

```bash
git clone --recursive <repository URL>
cd <repository folder>
cmake -S . -B build -A x64
cmake --build build --config Release --target TeardownVR
```

This produces `build/Release/dsound.dll`. To install it by hand:

1. Copy `dsound.dll` next to `teardown.exe`.
2. Copy the files of `mod/TeardownVR/` into `<game>/mods/teardownvr/`.
3. Start the game and enable "Teardown VR" once, in the mod manager's "Built-in" list.

[docs/BUILDING.md](docs/BUILDING.md) has the full instructions: prerequisites, the `TDVR_GAME_SHADERS` option,
the test-only targets, the installer, the release script and installing by hand.

## Documentation

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the DLL and the Lua mod are organised, module by module, and the per-frame data flow |
| [docs/MODDER_GUIDE.md](docs/MODDER_GUIDE.md) | For modders: working on Teardown VR, and how it handles other mods (tools, cameras, windows, keys) |
| [docs/PROBLEMS_AND_SOLUTIONS.md](docs/PROBLEMS_AND_SOLUTIONS.md) | The problems met while making the mod, their causes and how each one was solved |
| [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) | Reverse-engineering notes on Teardown 2.0.1: engine functions, offsets and how the hooks work |
| [docs/BUILDING.md](docs/BUILDING.md) | Building the DLL, the test tools and the installer, and installing by hand |
| [docs/DEV_TOOLS.md](docs/DEV_TOOLS.md) | The mock OpenXR runtime, the probe, the logs, the live control file and the development scripts |
| [docs/UPDATING_FOR_A_NEW_GAME_VERSION.md](docs/UPDATING_FOR_A_NEW_GAME_VERSION.md) | What to do when a Teardown update breaks the mod |

The players' guides are [tools/README.txt](tools/README.txt) (English) and [tools/LISEZMOI.txt](tools/LISEZMOI.txt)
(French). A French summary of this page is in [README_FR.md](README_FR.md).

## Repository layout

| Folder | Contents |
|---|---|
| `src/` | The DLL loaded by the game as `dsound.dll`: dsound proxy, DXGI / input / clipboard hooks, OpenXR, engine hooks, settings panel |
| `mod/TeardownVR/` | The Lua mod: camera, aiming, tools, physics and locomotion. It is installed as `<game>/mods/teardownvr/` |
| `installer/` | The players' installer: a Win32 `.exe` whose payload is generated at release time |
| `tools/` | The release script (`package_release.ps1`), the manual install / uninstall scripts and the players' guides |
| `tools/mock_openxr/` | A test-only mock OpenXR runtime and the `tdvr_xr_probe` command-line probe |
| `tools/dev/` | Development scripts: mock driving, image metrics, performance analysis, Lua checks, read-only reverse-engineering helpers, and a helper that compiles HLSL with the game's own shader compiler |
| `docs/` | The documentation listed above |
| `third_party/` | Dear ImGui, MinHook and the OpenXR SDK, as git submodules |

Not in this repository: `shaders/` and `src/generated/`, which are game-derived. They are git-ignored.

## What the mod changes on your PC

- It puts `dsound.dll` next to `teardown.exe`. The installer keeps any other mod's `dsound.dll` as
  `dsound.dll.original` and puts it back on uninstall.
- It installs the Lua mod in `<game>/mods/teardownvr/`.
- It writes `TeardownVR.ini` (your settings) and `TeardownVR.log` next to the game. The logs of the last 10
  sessions are kept in `<game>/TeardownVR_journaux/` ("journaux" means "logs").
- At each start, it switches the game's display mode from "Fullscreen" to "Borderless window" in
  `%LOCALAPPDATA%\Teardown\options.xml`. In fullscreen, the game would render at the monitor's resolution and the
  headset image would be blurry.
- While VR runs, the Lua mod turns the game's head bob option (`options.input.headbob`) off, and puts your value
  back when VR stops. If the game closes while VR is still active, the option can stay off: turn it on again in
  the game's options if you want it.
- The mod has no network code: it sends nothing anywhere.

## Issues and pull requests

Issues and pull requests can be opened. Nothing promises that they will be read, answered, fixed or merged.

Before attaching logs or the "Prepare my logs" zip to a public issue, check them: file paths can contain your
Windows user name, and the zip describes your PC. Never post game files, memory dumps, disassembly listings or
shaders derived from the game's, in issues or in pull requests.

## Licence

Copyright (C) 2026 VR Games For All.

Teardown VR is free software, released under the **GNU General Public License, version 3 only** (SPDX:
`GPL-3.0-only`). See [LICENSE](LICENSE).

Third-party code, used unmodified as git submodules in `third_party/`:

- [Dear ImGui](https://github.com/ocornut/imgui): MIT licence.
- [MinHook](https://github.com/TsudaKageyu/minhook): BSD 2-Clause licence (including its Hacker Disassembler
  Engine, also BSD 2-Clause).
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) (the loader, linked statically): Apache-2.0 licence.
  The JsonCpp code it bundles is public domain / MIT.

## Disclaimer

- This is an unofficial fan project. It is not affiliated with, endorsed by or supported by Tuxedo Labs, the
  makers of Teardown.
- No game file, game code or game shader is included in this repository. The mod reads the game's files on the
  player's own PC at run time.
- The mod does not modify, remove or bypass the game's copy protection, and it never stores or distributes a
  decrypted copy of the game.
- The mod is provided **as is**, without warranty of any kind and **without support** (see sections 15 and 16 of
  the GPL-3.0). You use it at your own risk. A Teardown update can break it at any time. No answer, fix, update or
  new feature is promised.

## Credits

Made by VR Games For All with the help of Claude, Anthropic's AI.
