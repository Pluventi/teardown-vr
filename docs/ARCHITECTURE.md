# Teardown VR architecture

This document is for developers who read or change the Teardown VR source. It explains how the code is organised,
how the mod starts inside the game, and how one headset frame is produced. Most sections link to the source files
and to [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) ("RE §n"), which holds the addresses, measurements and
reasoning behind each engine hook.

The document describes the code as it is. It is not a promise of future behaviour. Teardown VR is provided as is,
without warranty and without support (see the README and the [LICENSE](../LICENSE), GPL-3.0). It targets Teardown
2.0.1, and a Teardown update can break the mod: engine functions are found by byte patterns at run time, and a
pattern that no longer matches switches its feature off (see [section 11](#11-when-a-teardown-update-breaks-a-feature)).

## Contents

1. [Scope and conventions](#1-scope-and-conventions)
2. [Overview](#2-overview)
   - [2.1 The big picture](#21-the-big-picture)
   - [2.2 Who does what: Lua or DLL](#22-who-does-what-lua-or-dll)
   - [2.3 Repository layout](#23-repository-layout)
   - [2.4 Threads and locks](#24-threads-and-locks)
3. [Life cycle](#3-life-cycle)
   - [3.1 Loading as dsound.dll](#31-loading-as-dsounddll)
   - [3.2 DllMain](#32-dllmain)
   - [3.3 First DXGI factory and the game's window](#33-first-dxgi-factory-and-the-games-window)
   - [3.4 First Present](#34-first-present)
   - [3.5 VR start](#35-vr-start)
   - [3.6 Session stop and shutdown](#36-session-stop-and-shutdown)
4. [Per-pair frame flow](#4-per-pair-frame-flow)
   - [4.1 Why two engine frames per headset frame](#41-why-two-engine-frames-per-headset-frame)
   - [4.2 Timeline of one stereo pair](#42-timeline-of-one-stereo-pair)
   - [4.3 Frame A: update and left eye](#43-frame-a-update-and-left-eye)
   - [4.4 Frame B: right eye at zero game time](#44-frame-b-right-eye-at-zero-game-time)
   - [4.5 Present: blit and submission](#45-present-blit-and-submission)
   - [4.6 Timing and pacing](#46-timing-and-pacing)
   - [4.7 Frames that are not eye images](#47-frames-that-are-not-eye-images)
   - [4.8 Fallback: alternating eyes](#48-fallback-alternating-eyes)
5. [Modules](#5-modules)
   - [5.1 Entry point and proxy](#51-entry-point-and-proxy)
   - [5.2 Hooking primitives](#52-hooking-primitives)
   - [5.3 DXGI and window hooks](#53-dxgi-and-window-hooks)
   - [5.4 OpenXR session and frame orchestrator](#54-openxr-session-and-frame-orchestrator)
   - [5.5 D3D12 toolkit](#55-d3d12-toolkit)
   - [5.6 Lua bridge](#56-lua-bridge)
   - [5.7 Engine hooks](#57-engine-hooks)
   - [5.8 Cheaper right-eye frame](#58-cheaper-right-eye-frame)
   - [5.9 Measurement and performance](#59-measurement-and-performance)
   - [5.10 Controller input](#510-controller-input)
   - [5.11 Settings panel](#511-settings-panel)
   - [5.12 VR inventory](#512-vr-inventory)
   - [5.13 Stereo particles and lighting](#513-stereo-particles-and-lighting)
   - [5.14 VR anti-aliasing](#514-vr-anti-aliasing)
   - [5.15 GPU competitor warning](#515-gpu-competitor-warning)
   - [5.16 Diagnostic tools](#516-diagnostic-tools)
   - [5.17 The Lua mod](#517-the-lua-mod)
   - [5.18 The installer](#518-the-installer)
   - [5.19 The mock OpenXR runtime](#519-the-mock-openxr-runtime)
6. [The Lua and DLL protocol](#6-the-lua-and-dll-protocol)
7. [Settings system](#7-settings-system)
   - [7.1 TeardownVR.ini](#71-teardownvrini)
   - [7.2 Sections](#72-sections)
   - [7.3 When a change takes effect](#73-when-a-change-takes-effect)
   - [7.4 The live control file](#74-the-live-control-file)
8. [Localisation](#8-localisation)
9. [Logging](#9-logging)
10. [Build options](#10-build-options)
11. [When a Teardown update breaks a feature](#11-when-a-teardown-update-breaks-a-feature)

---

## 1. Scope and conventions

- **Target.** Teardown 2.0.1 (Steam, Windows x64, Direct3D 12) with an OpenXR runtime: Virtual Desktop, Meta Quest
  Link / Air Link or SteamVR. The mod was made and tested with a Meta Quest 3, which is the reference headset of
  several defaults (render preset 6, `vrAspect`).
- **Addresses.** `+87f40` or `0x87f40` is an RVA inside `teardown.exe` 2.0.1. `game+0x60` is a field offset inside
  an engine object. Most of them are given for orientation only: the code finds every engine function at run time
  by byte pattern, and reads most field offsets out of the matched instructions, cross-checked
  ([5.7](#57-engine-hooks)). Some offsets (renderer, shapes, particles, the TAA wrapper) are 2.0.1 constants in the
  source, and one debug-only RVA is hard-coded (`[Debug] debugWatchCamera`). They must be re-checked by hand after
  a game update ([UPDATING §3.3](UPDATING_FOR_A_NEW_GAME_VERSION.md#33-constants-to-re-check-by-hand)).
- **Frame A / frame B.** In synchronised stereo, one headset frame is a *stereo pair* of two engine frames.
  Frame A is a normal game frame that advances the simulation and renders the left eye. Frame B follows it, gets
  zero game time and renders the right eye.
- **Settings** are written `[Section] key` as in `TeardownVR.ini`. The section names are French and stay so for
  compatibility with existing ini files: `Affichage` = Display, `Confort` = Comfort, `Controles` = Controls,
  `Ecran` = (virtual) Screen, plus `Interface`, `Mods`, `Debug`, `Performance`.
- **Paths.** `<game>` is the folder that holds `teardown.exe`. Links to source files are relative to this `docs/`
  folder.
- **Hands.** The dominant hand is the right hand, or the left one with `[Controles] leftHanded=1`. The off hand is
  the other one.

## 2. Overview

### 2.1 The big picture

```
                                 teardown.exe (one process)
 +------------------------------------------------------------------------------------------------+
 |                                                                                                |
 |  Teardown engine (game code, unchanged on disk)        Lua mod: <game>/mods/teardownvr/main.lua |
 |    Game::frame -> Game::update -> physics -> draw        tick(): SetClipboardText("TDVR2 ...")  |
 |         |   ^                         |                          GetClipboardText() -> pose,    |
 |         |   |  MinHook detours        |                          eye, buttons, settings         |
 |         |   +-- engine.cpp            |                               |        ^                |
 |         |       frameb.cpp            v                               v        |                |
 |         |       perf.cpp      IDXGISwapChain::Present          USER32 clipboard (IAT hooks)     |
 |         |       particles_vr.cpp      |                               |        |                |
 |         |       taa_vr.cpp            |                               |        |                |
 | ========|=============================|====== dsound.dll (this project) ===|========|=========== |
 |         |                             v                               v        |                |
 |         |                       d3d_hooks.cpp                     bridge.cpp ---+                |
 |         |                             |                               |                         |
 |         |                             v                               v                         |
 |         +<---- Engine* API ------  vr.cpp  (frame orchestrator) <-- VrHandleLuaRequest          |
 |                                    |   |   |                                                    |
 |             +----------------------+   |   +----------------------+                             |
 |             v                          v                          v                             |
 |      render.cpp (blit on        ui.cpp + inventory.cpp     input.cpp (+ pckeys, modkeys)        |
 |      the game's queue)          (Dear ImGui, offscreen)    fake keyboard/mouse -> game window   |
 |             |                          |                                                        |
 |             v                          v                                                        |
 |      OpenXR swapchains: left eye, right eye, virtual screen, UI quad                            |
 |      OpenXR session (static loader) bound to the GAME's ID3D12Device and command queue          |
 +-----------------------------|------------------------------------------------------------------+
                               v
            OpenXR runtime: Virtual Desktop / Meta Quest Link / SteamVR / mock (tests only)

 Outside the game:  installer/ (players' installer exe)    tools/mock_openxr/ (test runtime + probe)
```

In one sentence: the game loads our `dsound.dll`, which hooks the engine so that every headset frame is drawn as
two engine frames showing the same game instant, copies each back buffer into an OpenXR eye image on the game's own
D3D12 device, and talks to a Lua mod (through the clipboard) that places the camera, the tool and the player in
the world.

### 2.2 Who does what: Lua or DLL

The work is split by what each side can reach:

| Side | Can reach | Does |
|---|---|---|
| Lua mod ([main.lua](../mod/TeardownVR/main.lua)) | the game's Lua API only (camera offset and FOV, player velocity and aim, tool transforms, bodies, physics, registry) | places the camera at each eye, turns and moves the player, aims with the controller, holds the tool in the VR hand, physical tool copy and off hand, fog / bloom / depth-of-field removal, inventory list |
| DLL ([src/](../src/)) | the process: engine memory and functions, DXGI / D3D12, Windows input, OpenXR | decides which eye each frame renders and how much time it gets, rewrites the projection, keeps per-eye temporal history, blits images to the headset, owns the OpenXR session, translates controllers into keyboard / mouse, draws the settings panel |

The Lua side cannot do I/O beyond the clipboard functions, and the DLL cannot call the game's Lua API. The bridge
([5.6](#56-lua-bridge), [section 6](#6-the-lua-and-dll-protocol)) joins them once per Lua tick.

### 2.3 Repository layout

| Path | Contents |
|---|---|
| [src/main.cpp](../src/main.cpp), [src/proxy.cpp](../src/proxy.cpp), [src/dsound.def](../src/dsound.def) | entry point; DirectSound export forwarding |
| [src/hooks.cpp](../src/hooks.cpp) | import-table and MinHook primitives |
| [src/log.cpp](../src/log.cpp), [src/config.cpp](../src/config.cpp), [src/i18n.h](../src/i18n.h), [src/vrmath.h](../src/vrmath.h), [src/prof.h](../src/prof.h) | log, settings, languages, math, frame section timers |
| [src/bridge.cpp](../src/bridge.cpp) | Lua <-> DLL channel |
| [src/d3d_hooks.cpp](../src/d3d_hooks.cpp) | DXGI factory / swap chain / queue hooks, window and monitor hooks |
| [src/vr.cpp](../src/vr.cpp) | OpenXR session and the per-Present frame orchestrator (~3,500 lines) |
| [src/render.cpp](../src/render.cpp) | small D3D12 toolkit: command lists, descriptor heaps, blit shader |
| [src/engine.cpp](../src/engine.cpp) | hooks inside the Teardown engine (~8,750 lines) |
| [src/frameb.cpp](../src/frameb.cpp) | the render-only right-eye frame B |
| [src/perf.cpp](../src/perf.cpp) | measurement and performance features |
| [src/input.cpp](../src/input.cpp), [src/pckeys.cpp](../src/pckeys.cpp), [src/modkeys.cpp](../src/modkeys.cpp) | controllers to keyboard / mouse; key table; other mods' keys |
| [src/ui.cpp](../src/ui.cpp), [src/inventory.cpp](../src/inventory.cpp) | settings / keys panel and VR tool bar (Dear ImGui) |
| [src/particles_vr.cpp](../src/particles_vr.cpp), [src/taa_vr.cpp](../src/taa_vr.cpp) | pipeline swaps for stereo particles and VR anti-aliasing (need the unpublished game-derived shaders, see [10](#10-build-options)) |
| [src/gpu_watch.cpp](../src/gpu_watch.cpp), [src/gpu_trace.cpp](../src/gpu_trace.cpp), [src/sampler.cpp](../src/sampler.cpp), [src/re_tools.cpp](../src/re_tools.cpp) | diagnostics |
| [mod/TeardownVR/](../mod/TeardownVR/) | the Lua mod: `info.txt`, `main.lua` (~4,600 lines), `proxy.vox` |
| [installer/](../installer/) | the players' installer (separate CMake project) |
| [tools/package_release.ps1](../tools/package_release.ps1) | the release script: builds the DLL and the installer, writes `dist/` ([BUILDING.md](BUILDING.md), section 6.1) |
| [tools/mock_openxr/](../tools/mock_openxr/) | test-only OpenXR runtime and a runtime probe |
| [tools/install.ps1](../tools/install.ps1), `install.bat`, `uninstall.bat` | script install from a package folder |
| [tools/README.txt](../tools/README.txt), [tools/LISEZMOI.txt](../tools/LISEZMOI.txt) | the players' guides (English, French) |
| [tools/dev/](../tools/dev/) | development scripts: mock driving, image metrics, performance runs, Lua checks, read-only reverse-engineering helpers, and `shader_tools/gamedxc.py` (the game's own shader compiler) ([DEV_TOOLS.md](DEV_TOOLS.md), section 5) |
| `third_party/` | OpenXR SDK (loader), MinHook, Dear ImGui |

Not in the repository: `shaders/` and `src/generated/` (derived from the game's shaders, see
[10](#10-build-options)), `installer/gen/` (generated by `tools/package_release.ps1` at release time), build
folders.

### 2.4 Threads and locks

| Thread | What runs there |
|---|---|
| Game main thread | Lua ticks (so the bridge handler), every engine hook, the render, Present and all our per-Present work (blit, `xrEndFrame`, `xrWaitFrame` / `xrBeginFrame`), input posting, ImGui drawing, the game's window procedure (our subclass) |
| Game job threads | the engine's jobs; a few of our hooks run there (job timing with `perfTrace`, the particle mesh tasks for `stereoParticleMotion`) |
| Particle sort helpers | two threads at the main thread's priority, started on first use ([5.7](#57-engine-hooks), `parallelParticleSort`) |
| Log writer | writes `TeardownVR.log` every 50 ms ([9](#9-logging)) |
| ini key writer | `ConfigSaveKeys` worker ([7](#7-settings-system)) |
| Hang watchdog | checks every second that a Present happened in the last 3 s ([5.16](#516-diagnostic-tools)) |
| GPU watch | lowest priority, every 2 s ([5.15](#515-gpu-competitor-warning)) |
| Mod keys scan | on demand ([5.10](#510-controller-input)) |
| Optional | per-thread CPU sampling (`perfTrace`), network send rate (`vdStats`), the stack sampler (`debugProfile`) |

`vr.cpp`'s `g_mutex` guards what the Lua request handler and the Present path share: frame state, views, hand
states, panel poses. The inventory has its own mutex. Engine fields are written only from the thread, and at the moment, where the
engine itself would write them.

## 3. Life cycle

### 3.1 Loading as dsound.dll

`teardown.exe` imports `DSOUND.dll`. `dsound` is not a "KnownDLL", so Windows loads the copy in `<game>` before the
system one. [proxy.cpp](../src/proxy.cpp) and [dsound.def](../src/dsound.def) export the 12 DirectSound functions and
forward each to `%SystemRoot%\System32\dsound.dll`, loaded on the first call. The game's sound therefore works
unchanged. See RE §1 ["Loading"](REVERSE_ENGINEERING.md#1-loading).

### 3.2 DllMain

[main.cpp](../src/main.cpp), `DLL_PROCESS_ATTACH`:

1. `IsTeardown()`: any executable other than `teardown.exe` gets a plain proxy and nothing else.
2. `LogInit()`: archives the previous session's log, opens `TeardownVR.log`.
3. `ConfigLoad()`: reads or creates `TeardownVR.ini`, applies the OpenXR runtime choice through environment
   variables when it is fixed in the ini.
4. `LogConfigure()`: starts the log writer thread (unless `[Debug] logSync=1`); `g_perfTrace` is set.
5. `HooksInit()`: MinHook initialisation.
6. `BridgeInit(VrHandleLuaRequest)`: clipboard import-table hooks.
7. `Input::InstallHooks()`: raw input, cursor and `DefWindowProc` import-table hooks.
8. `FixFullscreenOption()`: in `%LOCALAPPDATA%\Teardown\options.xml`, the "full screen" display mode becomes
   "borderless window" (in full screen the game draws at a monitor mode resolution whatever window size is
   reported).
9. `D3DHooksInit()`: window / monitor import-table hooks, and MinHook on the three `CreateDXGIFactory*` exports.

Constraints that shape this sequence:

- **Loader lock.** Only file, registry and import-table work happens here. `d3d12.dll`, `dxgi.dll` and `pdh.dll` are
  delay-loaded so that nothing pulls them in early ([10](#10-build-options)).
- **Encrypted code.** The game's `.text` section is encrypted on disk by the Steam DRM wrapper. Byte patterns can
  only be searched once the code has been decrypted in memory, so all engine hooks wait for the first Present.

`DLL_PROCESS_DETACH` only calls `LogShutdown()` to write the last pending log lines.

### 3.3 First DXGI factory and the game's window

- Each `CreateDXGIFactory*` call goes through `HookFactory()` in [d3d_hooks.cpp](../src/d3d_hooks.cpp). It calls
  `ParticlesVrEarly()` until that succeeds: the particle and lighting pipeline hooks must be in place before the
  renderer creates its pipelines, but after the code is decrypted ([5.13](#513-stereo-particles-and-lighting)).
  The first time, it hooks the factory's `CreateSwapChain` (vtable 10) and `CreateSwapChainForHwnd` (15).
- While the game creates its window, its monitor queries reach our import-table hooks, which ask
  `GetSwapchainSizeOverride()` ([vr.cpp](../src/vr.cpp)). That calls `HeadsetInUse()`, which runs the OpenXR runtime
  choice once (`RuntimeStartup`, [5.4](#54-openxr-session-and-frame-orchestrator)). If a headset is found, the game
  is told that the monitor has the VR render size, so it renders natively at that size. The size is only chosen
  here, at game start.
- At swap chain creation, `DXGI_USAGE_SHADER_INPUT` is added to the back buffers (so they can be read as a texture),
  the swap chain's queue is remembered (for a D3D12 swap chain the "device" argument is the queue), and the swap
  chain's `Present` (8), `ResizeBuffers` (13), `Present1` (22), `ResizeBuffers1` (39) and the queue's
  `ExecuteCommandLists` (10) are hooked.

### 3.4 First Present

`OnGamePresent()` in [vr.cpp](../src/vr.cpp) runs its one-time block:

1. the D3D12 device from the game's queue;
2. `EngineInit()` ([engine.cpp](../src/engine.cpp)): all pattern scans and engine hooks, in a fixed order
   ([5.7](#57-engine-hooks)); it also installs the frame-B and performance hooks when they are wanted;
3. `Render::Init()`: our command lists, descriptor heaps and blit shader on the game's device;
4. `Input::Init(hwnd)`: subclasses the game's window procedure;
5. `Ui::Init(hwnd)`: Dear ImGui;
6. `GpuWatch::Start()` with the game's adapter LUID; `Perf::CheckOverlays()`.

If `EngineInit()` does not recognise `Game::update` or the time bookkeeping, it logs "not recognised" and returns
false. The mod then still works with alternating eyes ([4.8](#48-fallback-alternating-eyes)).

### 3.5 VR start

`PumpXr()` runs at every Present:

1. `RuntimeStartup()` (a no-op after the first call);
2. while there is no session, every 3 s: `CreateInstance()` -> `GetSystem()` -> `CreateSession()`. A runtime without a
   headset answers `XR_ERROR_FORM_FACTOR_UNAVAILABLE` and is asked again later. In automatic mode,
   `RuntimeRedetect()` may switch to another runtime that gained the headset;
3. `HandleEvents()`: `XR_SESSION_STATE_READY` -> `xrBeginSession` -> the session runs (`g_running`);
4. the session-only performance settings follow the session state (`Perf::SessionPriority`, `GpuPriority`,
   `StreamSession`); while the session runs, the refresh rate is applied.

After the next real Present, `OnGamePostPresent()` calls `BeginXrFrame()` (`xrWaitFrame`, `xrBeginFrame`,
`xrLocateViews`, controllers). From then on, the Lua mod's request is answered "active" (status 1). The Lua mod
takes the camera (`onActivate`) and asks for the left eye: the first stereo pair starts.

If the headset was connected after the game started, the session runs but the game kept the monitor's resolution;
the settings panel shows a "restart the game with the headset connected" banner (`VrRenderSizeLate`).

### 3.6 Session stop and shutdown

- `XR_SESSION_STATE_STOPPING` (headset taken off, runtime paused): swapchain images released, `xrEndSession`. The Lua
  mod is answered "inactive" (status 0) and gives the camera back (`onDeactivate`).
- `EXITING` / `LOSS_PENDING`: the session is destroyed and re-created after 3 s. `INSTANCE_LOSS_PENDING`: the
  instance is destroyed and re-created after 5 s.
- When the session stops, the performance settings that only apply during a session (frame latency, CPU and GPU
  priority) return to the game's own values.
- Process exit: `LogShutdown()`.

## 4. Per-pair frame flow

### 4.1 Why two engine frames per headset frame

Teardown's renderer cannot draw two views in one engine frame: its per-frame GPU data would be overwritten (tested:
the GPU hangs). Rendering one eye per frame with the game running normally ("alternating eyes") shows each eye a
different game instant, so moving objects are doubled. Teardown VR therefore uses **two engine frames per headset
frame, both showing the same simulation state**:

- frame A advances the game by exactly one headset display interval and renders the left eye;
- frame B gets zero game time (no physics step, same interpolation) and renders the right eye from the left eye's
  camera moved by the eye offset.

Consequence: the engine runs at twice the headset's refresh rate (180 engine frames per second for a 90 Hz
headset). Most performance work therefore makes frame B cheaper ([5.8](#58-cheaper-right-eye-frame)). Background:
RE §4 ["Synchronised stereo"](REVERSE_ENGINEERING.md#4-synchronised-stereo-two-eyes-same-instant).

### 4.2 Timeline of one stereo pair

```
 ... Present of frame B (pair n-1) -> xrEndFrame(pair n-1) -> real Present
     -> OnGamePostPresent: xrWaitFrame / xrBeginFrame / xrLocateViews / controllers   (poses predicted for pair n)
 |
 |  FRAME A (pair n, left eye)
 |    Game::frame: beginRender (held back with lazyBeginFrame), time added to the fixed-step accumulators
 |    Hook_Update: dt = headset display interval; accumulators rebuilt
 |      Game::update: scripts tick
 |        Lua tick: "TDVR2 ..." -> VrHandleLuaRequest: eye 0, EngineRequestStereo(), eye offset, FOV, projections
 |                  response -> Lua sets camera offset (left eye), FOV, aim, tool, velocity; one-way messages
 |    fixed-step physics loop (real steps)
 |    draw: Hook_ToolAnim, Hook_CamSet (left camera recorded, off-centre projection), particles, Hook_Render
 |    Present -> OnGamePresent: back buffer -> LEFT eye swapchain image. No xrEndFrame (pair not complete).
 |    real Present (sync interval 0, tearing allowed)
 |
 |  FRAME B (pair n, right eye)
 |    Game::frame
 |    Hook_Update: real dt taken back out of the accumulators, dt = 1e-5
 |      frameBMode 2: FrameB::BReplay (no Game::update)  |  modes 0/1 or fallback: Game::update, Lua answered eye 1
 |    physics loop: no step
 |    draw: Hook_CamSet (camera = left camera x eye offset), Hook_Render (right-eye history, motion fixes)
 |    Present -> OnGamePresent: back buffer -> RIGHT eye image -> xrEndFrame(projection layer, both views)
 |    real Present
 |    OnGamePostPresent: xrWaitFrame ... (pair n+1)
 v
```

### 4.3 Frame A: update and left eye

1. **Time.** [Hook_Update](../src/engine.cpp) sees that the previous frame was a frame B and that `vr.cpp` provided a
   display interval (`EngineSetVrFrameDelta`). It sets the frame time to that interval (at most 0.1 s) and rebuilds
   the two fixed-step accumulators so the game advances by exactly that much. `[Affichage] limitCatchUp` caps the
   accumulator so at most one physics step runs per frame ([4.6](#46-timing-and-pacing)).
2. **Before the update.** `MapControls`, `GrabControls`, `FrameB::AEnter` (starts recording one-frame requests for
   frame B), `Perf::SpinFrameA`.
3. **Scripts.** Our Lua mod's tick sends `TDVR2 ...`. [bridge.cpp](../src/bridge.cpp) hands it to
   `VrHandleLuaRequest` ([vr.cpp](../src/vr.cpp)). Not in a frozen frame -> eye 0: `EngineRequestStereo()` marks this
   frame as left and the next one as frame B; `EngineSetEyeOffset` (right eye seen from the left eye, scaled by
   `worldScale`), `EngineSetLeftEyeRotation` / `Position`, `EngineSetVrFov`, `EngineSetEyeProjection` for each eye.
   The response carries the eye pose, head, hands, buttons and settings. Lua sets the camera offset to the left
   eye, the FOV, the player's aim and velocity, the tool override, and sends one-way messages (room yaw, laser
   target...). Other mods' ticks go through `Hook_SetToolTransform`, `Hook_GetCameraTransform`, and the census hooks.
4. **After the update.** `FrameB::AExit` (snapshot), `WideDecide` (16:9 pause screen), diagnostics.
5. **Physics.** The engine's fixed-step loop runs its real steps.
6. **Draw.** `Hook_ToolAnim` puts the tool in the VR hand. `Hook_CamSet` records the camera as `g_leftCam`, rebuilds
   its rotation from the room yaw and the headset rotation, and writes the left eye's off-centre projection;
   `ApplyUiShift` moves the 2D interface for this eye. Particles are simulated here (`Hook_PsUpdate`).
   `Hook_Render` swaps in the left eye's temporal history, records the frame's noise and shape transforms, then
   calls the real `Renderer::render`.
7. **Present.** [4.5](#45-present-blit-and-submission).

### 4.4 Frame B: right eye at zero game time

1. **Time.** `Hook_Update` takes the real frame time back out of both accumulators (clamped at 0), remembers it,
   and sets the frame time to `kFrozenDt` = 1e-5 s ("zero" without dividing by zero). The physics loop then runs no
   step and the render interpolation factor stays as in frame A.
2. **Update.** With `[Performance] frameBMode=2` (default), `FrameB::BReplay` replaces `Game::update`: what frame A
   asked for one frame only is asked for again ([5.8](#58-cheaper-right-eye-frame)). Our Lua mod is not ticked;
   its laser, outlines and sprites are frame A's, replayed. With modes 0 / 1 or on a fallback, `Game::update` runs
   with no time; our Lua mod is answered "frozen, eye 1" and only re-places camera, tool and laser.
3. **Draw.** `Hook_CamSet` ignores the game's camera and uses `g_leftCam` x eye offset, with the right eye's
   projection. Reason: during the zero-time frame the game still applies per-frame camera effects (height smoothing,
   landing shake), measured up to 43 cm / 18 degrees of drift. `Hook_PsUpdate` lets the particle mesh be rebuilt from
   the same state; `Hook_Render` swaps in the right eye's history, gives frame B frame A's noise frame and jitter,
   puts frame A's shape transforms back for motion vectors, and gives the particle stretch frame A's frame time.
4. **Present.** The right eye image is filled and the pair is submitted ([4.5](#45-present-blit-and-submission)).

Input messages are held back while the next frame is a frame B (`EngineNextFrameFrozen`), so the game's "just
pressed" logic always sees them in a normal frame ([5.10](#510-controller-input)).

### 4.5 Present: blit and submission

`Hook_Present` in [d3d_hooks.cpp](../src/d3d_hooks.cpp) calls `OnGamePresent()`, then the real `Present`, then
`OnGamePostPresent()`. `OnGamePresent()` in [vr.cpp](../src/vr.cpp), in order:

1. `PumpXr()`, `UpdateRenderResolution()` (learns the headset's field-of-view aspect for the next start);
2. `EngineConsumeFrameEye()`: 0 = left (frame A), 1 = right (frame B), -1 = not an eye frame; frame statistics;
3. decision: `stereo` = the session shows images and this frame rendered an eye;
4. `Render::Begin()`, back buffer to `PIXEL_SHADER_RESOURCE`;
5. stereo: acquire that eye's swapchain image; `ClearOutside` + `Render::Blit` into `CropRect` (sharpening,
   brightness, comfort vignette); mod-window cursor and panel corner marks (`ClearRenderTargetView` squares, no
   shader); `FillHidden` (hidden-area fill, off by default); fill that eye's `XrCompositionLayerProjectionView`
   (render pose, FOV);
   not stereo: blit into the virtual screen image ([4.7](#47-frames-that-are-not-eye-images));
6. `UpdatePanels()`, `Ui::Draw()`; the UI texture is blitted into the UI swapchain and, with
   `[Interface] showDesktopUi=1`, into the top-left of the game's back buffer (the desktop mirror);
7. back buffer back to `PRESENT`, `Render::Submit()` on the game's queue, swapchain images released;
8. layers and `xrEndFrame`:
   - frame A: no `xrEndFrame` (`g_leftDone = true`);
   - frame B with the left eye done: one projection layer with both views;
   - other frames: the virtual screen quad;
   - plus the UI quad (settings panel or inventory) and the warning banner when shown;
9. the game's `Present` is changed to sync interval 0 with `DXGI_PRESENT_ALLOW_TEARING` when the swap chain allows it:
   the headset paces the game, not the monitor;
10. pointers and input: each hand's ray against our panels and the virtual screen; `Input::Update()`; haptics.

Our command list runs on the game's own queue after the game's frame and before the real Present, so no extra
synchronisation is needed.

### 4.6 Timing and pacing

- **Prediction.** `BeginXrFrame()` runs right after the real Present of frame B (or of any frame that ended an
  OpenXR frame). `xrWaitFrame` blocks there, never in the middle of the game's frame. Frame A then starts with poses
  predicted for that pair's display time, and both eyes use the same `xrLocateViews` result.
- **Game time.** Frame A advances the game by the gap between two predicted display times, not by measured CPU time,
  so motion is even on the headset. RE §5 bis
  ["Frame pacing and input in VR"](REVERSE_ENGINEERING.md#5-bis-frame-pacing-and-input-in-vr).
- **Catch-up.** After a late frame the interval covers two refreshes and the engine would run two physics steps,
  making the next frame late too. `[Affichage] limitCatchUp=1` clamps the accumulator to
  `min(2.99/60, max(1.98/60, d + 0.98/60))`: a very short slow-down instead of chained hitches (counted as
  "physics catch-ups dropped").
- **Runtime holds.** `[Performance] holdSmooth=1`: when the previous `xrEndFrame` itself was held longer than a
  refresh (a streaming runtime waiting for its encoder or network), the next interval is clamped to one period. The
  game resumes where it stopped instead of jumping ahead.
- **CPU / GPU overlap.** `[Performance] frameLatency=2` lets frame B's CPU work overlap the GPU drawing frame A;
  `lazyBeginFrame=1` lets frame A start its scripts and physics before the engine's wait for an older GPU frame
  ([5.9](#59-measurement-and-performance)).
- **Missed frames.** Gaps between predicted display times count missed headset refreshes, reported on the
  "Stereo frames" log line every 600 Presents ([9](#9-logging)).

### 4.7 Frames that are not eye images

Menus, loading screens and paused levels do not tick the Lua mod (or it asks for no eye). Those frames are blitted
into the virtual screen swapchain and shown as a quad in the room (`[Ecran] screenDistance`, `screenWidth`), each
Present ending its own OpenXR frame. A paused single-player level is drawn 16:9 (`[Ecran] pauseWide`,
[5.7](#57-engine-hooks)). The level map is either a 3D scale model in stereo or, when unavailable, a screen frame
(the Lua request then gets status 2).

### 4.8 Fallback: alternating eyes

With `[Affichage] stereoMode=1`, or when `EngineInit()` failed, `VrHandleLuaRequest` alternates the eye at each
request and every frame is a normal game frame. Each eye then shows its own game instant. Every other engine feature
that was recognised still works.

## 5. Modules

Each module section lists its role, key functions, hooks, data, settings, and the design decisions with their
reasons. Function names are as in the source.

### 5.1 Entry point and proxy

Files: [main.cpp](../src/main.cpp), [proxy.cpp](../src/proxy.cpp), [dsound.def](../src/dsound.def).

- **Role.** Make the DLL a valid `dsound.dll` and run the [DllMain sequence](#32-dllmain) in `teardown.exe` only.
- **Exports.** `DirectSoundCreate` (ordinal 1), `DirectSoundEnumerateA/W`, `DllCanUnloadNow`, `DllGetClassObject`,
  `DirectSoundCaptureCreate`, `DirectSoundCaptureEnumerateA/W`, `GetDeviceID`, `DirectSoundFullDuplexCreate`,
  `DirectSoundCreate8`, `DirectSoundCaptureCreate8`: each forwards to the system DLL, loaded lazily with
  `GetSystemDirectoryW` + `LoadLibraryW`.
- **Settings.** `[Debug] logSync`, `perfTrace` (clamped 0..2 into `g_perfTrace`).
- **Decision.** Everything that needs the game's code or the D3D12 device is deferred to the first Present.

### 5.2 Hooking primitives

File: [hooks.cpp](../src/hooks.cpp).

- `HookIAT(dll, func, replacement)`: patches `teardown.exe`'s import table, matching by name or, without a name
  table, by the export's resolved address. Returns the previous pointer, or `nullptr` (logged "NOT FOUND").
  Used for USER32 clipboard, input and window functions.
- `HookFunction(target, detour, &original)`: MinHook create + enable ("already created / enabled" count as success).
  Used for every engine function and every DXGI / D3D12 method.
- `HooksInit()`: `MH_Initialize`, safe in DllMain.
- **Decision.** Every detour calls the original through its trampoline and acts around it. The only game
  instructions changed besides MinHook's jumps are three operands redirected by the grab feature ([5.7](#57-engine-hooks)).

### 5.3 DXGI and window hooks

File: [d3d_hooks.cpp](../src/d3d_hooks.cpp).

- **Role.** See the game's queue and every Present without knowing the engine, and make the game render at the VR
  size.
- **Hooks.** `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2` (MinHook) -> factory
  `CreateSwapChain` / `CreateSwapChainForHwnd` -> swap chain `Present`, `Present1`, `ResizeBuffers`,
  `ResizeBuffers1`, queue `ExecuteCommandLists` ([3.3](#33-first-dxgi-factory-and-the-games-window)).
  USER32 import-table hooks: `GetMonitorInfoA`, `QueryDisplayConfig`, `GetSystemMetrics`, `SetWindowPos`,
  `CreateWindowExW`, `ChangeDisplaySettingsExA` (blocked: reports success without changing the monitor mode).
- **Present path.** `Hook_Present` / `Hook_Present1` (re-entrancy guard, `DXGI_PRESENT_TEST` ignored) ->
  `OnGamePresent(sc, queue, sync&, flags&)` -> real Present -> `OnGamePostPresent()`. Sync interval and flags are
  passed by reference so `vr.cpp` can change them. Each part is timed (`P_OurPresent`, `P_RealPresent`,
  `P_PostPresent`).
- `Hook_ExecuteCommandLists`: GPU timestamps (`perfDetail` 4) and the debug GPU trace.
- `ResizeBuffers`: logged, our GPU work drained (`OnGameBeforeResize`). The size is never forced there.
- `FixFullscreenOption()`: [3.2](#32-dllmain).
- **Decisions.** The render size is set by reporting a "monitor" of the VR size rather than by forcing
  `ResizeBuffers` (the game would render for one size and present another). It is chosen only at game start
  (re-applying it in game broke the image), and only if a headset was found then. RE §7
  ["Render resolution"](REVERSE_ENGINEERING.md#7-render-resolution).

### 5.4 OpenXR session and frame orchestrator

Files: [vr.cpp](../src/vr.cpp), [vr.h](../src/vr.h).

- **Role.** Owns the OpenXR instance, session, swapchains, actions and layers; runs the per-Present work
  ([4.5](#45-present-blit-and-submission)); answers the Lua mod ([6](#6-the-lua-and-dll-protocol)); places the
  floating panels.
- **Key functions.** `CreateInstance`, `GetSystem`, `CreateActions`, `CreateSession`, `EnsureSwapchains`,
  `HandleEvents`, `PumpXr`, `BeginXrFrame`, `EndXrFrame`, `OnGamePresent`, `OnGamePostPresent`,
  `VrHandleLuaRequest`, `BuildResponse`, `UpdatePanels`, `UpdatePanelGrips`, `GetSwapchainSizeOverride`,
  `HeadsetInUse`, `UpdateRenderResolution`, `RuntimeStartup`, `RuntimeRedetect`.
- **OpenXR objects.**
  - Instance: application "Teardown VR", API 1.0; `XR_KHR_D3D12_enable` required; `XR_FB_display_refresh_rate` if
    offered; `XR_KHR_visibility_mask` only if offered and the hidden-area fill is wanted at start.
  - Session: graphics binding = the game's own `ID3D12Device` and command queue. Reference space `STAGE` if
    available, else `LOCAL`; plus a `VIEW` space (head pose, banner).
  - Actions: suggested bindings for the Oculus Touch and the KHR simple controller profiles; localised action names.
  - Swapchains: `g_eyeSc[2]` (back buffer size x `eyeResScale`, enlarged to the whole field with `fovCrop`),
    `g_screenSc` (virtual screen), `g_uiSc` (1100 x 860). Format preference: `R8G8B8A8_UNORM_SRGB`, `B8G8R8A8_UNORM_SRGB`, `R8G8B8A8_UNORM`. Eye swapchains
    are never re-created in the middle of a pair.
  - Every runtime call that blocks more than 15 ms is logged (`XrCallTimer`).
- **Runtime choice** (`[Affichage] headsetRuntime`): 0 automatic (default), 1 Virtual Desktop, 2 Meta Quest Link /
  Air Link, 3 SteamVR, 4 Windows' active runtime without detection. Choices 1-3 set `XR_RUNTIME_JSON` from the
  runtimes' registry list. Automatic mode asks each candidate with a throwaway instance (`xrCreateInstance` +
  `xrGetSystem` + `xrDestroyInstance`; the static loader re-reads `XR_RUNTIME_JSON` each time): Windows' active
  runtime first, then Virtual Desktop, then Meta Quest Link (only while the `OculusHMDConnected` event is set),
  SteamVR last and only when it is Windows' runtime (asking it starts SteamVR). Total budget 3 s.
  `[Debug] openxrRuntime` forces a runtime; `[Debug] xrAutoRuntimes` replaces the candidates for tests. RE §2 bis
  ["Choosing the OpenXR runtime"](REVERSE_ENGINEERING.md#2-bis-choosing-the-openxr-runtime-headsetruntime).
- **Render size and field of view.**
  - `GetSwapchainSizeOverride`: `RenderPresetSize(renderPreset)` when a headset is in use at start. Presets
    (lines): 0 PC screen (no override), 1 Low 1280, 2 Medium 1600, 3 High 1920, 4 Very high 2272, 5 Ultra 2720,
    6 Headset native (Quest 3) 3264. Width = height x `vrAspect`, both rounded up to multiples of 8.
  - `UpdateRenderResolution`: measures the headset's field-of-view aspect and saves it as `[Affichage] vrAspect` for
    the next start.
  - `SetCameraFov` takes the horizontal FOV of a 16:9 image, so the value sent to Lua is
    `2 atan(tan(up) x 16/9)`.
  - `[Affichage] offCenter=1`: each eye renders exactly its own asymmetric field (`OffCenterFovFor`, the projection
    terms written by `Hook_CamSet`); 0: one symmetric frustum (`RenderFovFor`).
  - `[Affichage] fovCrop` with `fovCoverage` < 1: the game renders the central part of the field; each eye image
    still covers the whole field (`CropRect` places the game image, `ClearOutside` blackens the rest).
- **Floating panels.** The settings panel, the VR inventory and the warning banner are OpenXR quad layers in room
  space, composited by the runtime with the newest head pose. The settings panel opens in front of the gaze
  (`[Interface] uiPanelDistance`, `uiPanelDown`). The inventory is worn by the body (a neck pivot that follows steps
  and crouching, and turns with a real body turn when `[Controles] inventoryFollow=1`). A grip pressed while that
  hand's ray is on a panel belongs to the panel until released (`g_gripOwned`); that grip is hidden from Lua. Places
  are saved with `ConfigSaveKeys` (only those keys, off the main thread). Distances are limited because quad layers
  have no depth test.
- **Hidden-area fill** (`[Performance] hiddenAreaFill`, default 0): clears what the lenses never show (runtime lens
  mask), so a streaming encoder spends almost no bits there. Off by default because the measured gain was small.
- **Debug features.** `[Debug] debugFakeHmd` (simulated headset and controllers on the keyboard, no OpenXR),
  `debugSyncTest` (red / blue markers checked per eye), `debugFovTest`, BMP dumps of presented frames,
  `debugGpuTrace`, `debugWatchCamera`.
- **Design decisions.**
  - Synchronised stereo from one `xrWaitFrame` / `xrLocateViews` per pair, rather than alternate-eye rendering.
  - `xrWaitFrame` after Present, so the game's frame is never blocked mid-way and frame A starts with fresh poses.
  - Panels, screen and banner as runtime layers rather than drawn into the game image: no game-frame latency and the
    same image in both eyes.
  - The Lua side gets poses relative to a yaw-only recentred origin; the room's yaw in the world belongs to Lua and is
    sent back (`TDVRA`).

### 5.5 D3D12 toolkit

Files: [render.cpp](../src/render.cpp), [render.h](../src/render.h).

- **Role.** Everything the mod draws itself, on the game's device and queue, without touching the game's command
  lists.
- **Command lists.** A ring of 4 command allocators, one direct command list and one fence. `Begin()` waits (up to
  1 s) if the GPU has not finished that slot; `Submit()` closes, executes on the game's queue and signals;
  `WaitIdle()` waits up to 2 s.
- **Descriptors.** A shader-visible CBV/SRV/UAV heap (persistent slots + per-frame transient slots) and an RTV heap.
  `TransientSrv` / `TransientRtv` create views valid for the current frame; `AllocSrv` / `AllocRtv` hand out
  persistent ones (ImGui's font texture, the UI target).
- **Blit.** One full-screen triangle; a small HLSL source of our own embedded in `render.cpp`, compiled at run time
  with `d3dcompiler_47.dll`. Root signature: 12 root constants (`BlitParams`) + one SRV table + a linear clamp
  sampler. Pipelines cached per (render-target format, `Blend::None` / `Blend::Premultiplied`). `BlitParams`:
  source rectangle, sRGB linearisation, 4-neighbour sharpening, brightness, opaque alpha, texel size, vignette.
- **Decisions.** One command list per Present on the game's own queue keeps ordering trivial. The back buffer is read
  as UNORM, linearised in the shader and written to an sRGB swapchain view, so scaling (`eyeResScale`, crop) filters
  in linear space.

### 5.6 Lua bridge

Files: [bridge.cpp](../src/bridge.cpp), [bridge.h](../src/bridge.h).

- **Role.** The only channel between the Lua mod and the DLL. Teardown's Lua sandbox has no file or socket access;
  the mod uses `SetClipboardText` / `GetClipboardText`.
- **Hooks.** Import-table hooks on USER32 `OpenClipboard`, `EmptyClipboard`, `SetClipboardData`,
  `GetClipboardData`, `CloseClipboard`. Opening is deferred. A text payload (`CF_TEXT`, `CF_OEMTEXT`,
  `CF_UNICODETEXT`) that starts with `TDVR` is consumed: the handle is freed, success is returned, the real
  clipboard is never touched. Anything else opens the real clipboard and goes through, so copy and paste keep
  working.
- **Request / response.** A `TDVR2 ...` request is parsed into a `LuaRequest` and passed to the handler
  (`VrHandleLuaRequest`); the response is armed as ANSI + UTF-16 memory and returned by the next
  `GetClipboardData`. One-way messages (`TDVRY`, `TDVRT`, `TDVRA`...) call engine, input or inventory functions
  directly. Full list in [section 6](#6-the-lua-and-dll-protocol).
- **Data.** `kLuaFlag*` request flags; a ring of the last 32 clipboard calls (`BridgeDumpTrace`, logged when the
  Lua sequence number jumps).
- Every `TDVR` message except the old `TDVR1` calls `Perf::NoteOurScript()` (frame-B mode 1 must always tick our
  script). An old copy of the mod (protocol `TDVR1`) is answered "inactive" and logged.
- **Decisions and constraints.**
  - The game's `SetClipboardText` binding reads its temporary string after freeing it. A string of at most 30
    characters stays in the string object's inline buffer and survives; longer ones sometimes arrive empty. So every
    Lua -> DLL message is at most 30 characters and long data is sent in pieces. The only exception is the
    diagnostic `TDVRL <text>` lines, which can be longer and may be lost. Responses can be long.
  - These two Lua functions are only available to "internal" mods, so the Lua mod is installed in the game's own
    `mods` folder. The game's sandbox check is not modified.
  - RE §3 ["The Lua and DLL channel"](REVERSE_ENGINEERING.md#3-the-lua-dll-channel).

### 5.7 Engine hooks

Files: [engine.cpp](../src/engine.cpp), [engine.h](../src/engine.h) (the `Engine*` API used by `vr.cpp`,
`bridge.cpp` and `input.cpp`).

**Role.** Everything that must happen inside the engine: frame timing, the per-eye camera and projection, per-eye
temporal history, the held tool, mod tools and cameras, the 2D interface per eye, mods' windows as VR panels,
particles and object motion in stereo, the off arm, dual wield, the game's grab, the 16:9 pause screen, and the
engine side of the performance work.

**How engine code is found.**

- `.text` is located from the PE section table. `Scan(pattern)` / `ScanIn()` search byte patterns with `??`
  wildcards; `RelTarget` decodes rel32 call / jump / rip operands; `U32` reads an embedded offset.
- `ScanOnce(pattern)`: the pattern must match exactly once, else the feature is left out (used by newer features).
- `FindLuaBinding(name)`: finds the binding's name in `.rdata`, then the registration code that pairs it with its
  implementation. `FindUiBinding` reads the `{function, name}` table of `Ui*` bindings. `FindBindingDispatcher`
  locates the function every Lua binding goes through, from three wrappers that must agree. `FindCoreFunctions`
  reads a static list of registry setters and other core functions. `FunctionStart` uses the unwind table.
- Most patterns also contain the field offsets they read, and the `Init*` functions cross-check them (for example,
  the UI camera offset read in two different functions must be equal) before hooking anything. Any mismatch logs
  "not recognised" and the game's own behaviour stays.
- Example, the core pattern: `Game::update` is called from `Game::frame` at
  `49 8B CC E8 ?? ?? ?? ?? 49 8B CC E8 ?? ?? ?? ?? 41 80 BC 24 ?? ?? 00 00 00`; a second pattern at the end of the
  frame's time bookkeeping yields the frame-time field and the two fixed-step accumulators.

**`EngineInit()` order.** `.text` located -> `InitToolHook` -> `Game::update` and the time fields (if missing:
return false, no synchronised stereo) -> `Hook_Update` installed -> `InitCameraHook`, `InitScriptCamera`,
`InitMapView`, `InitScriptView`, `InitUiCamera`, `InitCrosshairAim`, `InitModUiPointer`, `InitPlayerYaw`,
`InitAimCamera`, `InitModPanel`, `InitHistoryHook`, `InitParticleHook`, `InitParticleSort`, `InitParticleMotion`,
`InitShapeJobSkip`, `InitDeadPasses`, `InitSameNoise`, `InitOffArm`, `InitDualWield`, `InitGrabDistance`,
`InitScreens`, `InitPerfTrace` (performance hooks and frame-B module), `InitLazyBegin`, `InitPauseWide` -> "Engine: synchronised
stereo ready". The order matters: each comment in the code says which earlier result an `Init*` needs.

**Main hooks.**

| Detour | Engine function (2.0.1) | Used for |
|---|---|---|
| `Hook_Update` | `Game::update` (call site in `Game::frame`) | frame A / B timing |
| `Hook_CamSet` | `Camera::setTransform` from `Game::lateUpdate` (`0x5ec040`) | per-eye camera and projection, UI shift |
| `Hook_ToolAnim` | `ToolAnim::update` | tool held in the VR hand |
| `Hook_SetToolTransform` | Lua binding `SetToolTransform` | keeps a mod's tool pose apart from the VR hand |
| `Hook_ShapeLocalImpl` | engine function behind `SetShapeLocalTransform` | mod tool scaling |
| `Hook_GetShapeLocal` / `World` | Lua bindings `GetShapeLocalTransform` / `GetShapeWorldTransform` | mods see unscaled parts |
| `Hook_GetCameraTransform` | Lua binding `GetCameraTransform` | mods aim with the controller |
| `Hook_GetPlayerEye` | Lua binding `GetPlayerEyeTransform` | dual wield |
| `Hook_UiRectVisible`, `Hook_UiWorldToPixel`, `Hook_UiPop`, `Hook_UiMakeInteractive` | `Ui*` bindings and the rectangle visibility test | crosshairs on the laser, mods' windows |
| `Hook_MousePixel` | the mouse pixel the `Ui*` functions read | VR pointer in mods' windows |
| `Hook_ScreenRender` | `Screen::render` (`0x467dd0`) | the game's computer screen drawn once per stereo pair |
| `Hook_ScreenCursor` | `Input::handleEvent` (`0x4f3e30`), only from `Screen::update`'s cursor event | the controllers move a game computer's cursor |
| `Hook_ScriptDraw`, `Hook_UiClip`, `Hook_UiBackgroundBlur`, `Hook_InputPressed/Down/Released` | script `draw()`, clip, blur, input bindings | mods' menus as room panels |
| `Hook_Render` | `Renderer::render` (`0x87f40`) | per-eye history, noise, object motion, TAA / particle swaps |
| `Hook_PsUpdate` | `ParticleSystem::update` | particles simulated in frame A |
| `Hook_PsSort` | particle depth sort | parallel sort in frames without a physics step |
| `Hook_MeshTask` | `MeshGenerationTask::run` | right-eye particle motion vectors |
| `Hook_ShapeJobs` | shape interpolation job group start | `skipShapeJobsB` |
| `Hook_BloomRender`, `Hook_DofRender`, `Hook_PostPass` | `Bloom::render`, `Dof::render`, a post pass | `skipDeadPasses` |

Plus the performance hooks ([5.9](#59-measurement-and-performance)) and the frame-B hooks
([5.8](#58-cheaper-right-eye-frame)).

**Subsystems.** Each one degrades to the game's behaviour when its patterns are missing.

| Subsystem | Code | Settings | What and why |
|---|---|---|---|
| Frame timing | `Hook_Update` | `[Affichage] stereoMode`, `limitCatchUp` | [4.3](#43-frame-a-update-and-left-eye), [4.4](#44-frame-b-right-eye-at-zero-game-time), [4.6](#46-timing-and-pacing) |
| Per-eye camera | `Hook_CamSet` | `[Affichage] worldScale` | frame A records the camera; its rotation is rebuilt from the room yaw and the headset rotation (the game's rotation follows the aim and shook the view). Frame B uses the left camera x eye offset. RE §5 ter ["Field of view, right-eye camera..."](REVERSE_ENGINEERING.md#5-ter-field-of-view-right-eye-camera-pacing-per-eye-history) |
| Off-centre projection | `Hook_CamSet` | `[Affichage] offCenter` | `m0`, `m5`, `m8`, `m9` of the camera's projection rewritten from the eye's four half-angle tangents; the engine derives culling, view rays and TAA jitter from it |
| Held tool | `Hook_ToolAnim`, `Hook_SetToolTransform` | - (tool offsets are applied by the Lua mod) | in first person the engine ignores the tool override; the DLL applies it. Built-in tools keep their own animation relative to the VR hand (rest and grip poses read from the game's prefab XML at run time); mod tools use their desktop pose. RE §6 ["Aiming and the tool"](REVERSE_ENGINEERING.md#6-aiming-and-the-tool) |
| Mod tool scaling | `Hook_ShapeLocalImpl`, `Hook_GetShape*` | `[Controles] modToolScale`; `[Performance] fastToolScaleB`, `fastToolShapes` | mod tools are drawn for a desktop screen and looked huge in a hand: shapes and voxel size scaled about the measured muzzle, so shots still leave the drawn barrel |
| Per-eye temporal history | `Hook_Render` | `[Affichage] perEyeHistory` | a second complete set of history targets, TAA textures, indices and previous matrices, created with the engine's own allocators and swapped in for the right eye; otherwise each eye reprojected the other eye's image. Auto-exposure stays shared on purpose |
| Same noise per pair | `SameNoiseBegin` / `End` | `[Affichage] stereoSameNoise` (ini only) | frame B starts with frame A's TAA counter: same noise frame and jitter in both eyes |
| 2D interface per eye | `ApplyUiShift`, `Hook_UiRectVisible` | `[Affichage] hudFix`, `hudDistance`, `modCrosshairAim` | with off-centre projections the screen centre differs between eyes, so a centred crosshair was seen double; the canvas is shifted per eye and small centred quads move onto the laser target. RE §6 septies ["Mod cameras and per-eye 2D interface"](REVERSE_ENGINEERING.md#6-septies-mod-cameras-and-per-eye-2d-interface) |
| Mods' windows | `Hook_MousePixel`, `ModPanel` | `[Affichage] modUiPointer`, `modUiDistance`, `modUiPanel`, `modUiPanelScale`, `modUiPanelPlaces`, `modUiCenterTool`, `modUiToolFixed` | `UiMakeInteractive` windows used with either controller; menus become panels fixed in the room, moved with a grip, places saved per window |
| Mods aim with the controller | `Hook_GetCameraTransform` | `[Controles] modAimHand` | on foot, other scripts' `GetCameraTransform()` returns the player camera the Lua mod turns toward the laser; nothing is written to the game |
| Object motion in the right eye | `FixObjectMotion` | `[Affichage] syncScriptMotion`; `[Performance] fastObjectMotion` | frame B's previous transforms equal the current ones, so moving objects smeared in the right eye's TAA: frame A's previous transforms are put back; shapes a script moved during frame B get frame A's current transform too |
| Particles in stereo | `Hook_PsUpdate`, `Hook_PsSort`, `Hook_MeshTask` | `[Affichage] syncParticles`, `stereoParticleMotion`; `[Performance] parallelParticleSort` | particles simulated in frame A only, sorts run in parallel in frame B, the right eye gets its own particle motion vectors. RE §6 quinquies ["Particles and physics catch-up"](REVERSE_ENGINEERING.md#6-quinquies-particles-and-physics-catch-up) |
| Dead passes | `Hook_BloomRender`, `Hook_DofRender` | `[Performance] skipDeadPasses` | with bloom and depth of field removed (their amounts exactly 0), those passes compute nothing visible and are skipped; mode 2 checks on the GPU every few seconds and turns the skip off on any difference |
| Shape jobs in frame B | `Hook_ShapeJobs` | `[Performance] skipShapeJobsB` (default 0) | optional; off by default (tiny gain, and the job also updates interpolation history) |
| Mod cameras, zoom, 3D map | `InitScriptView`, `InitMapView`, `DollyTravel` | `[Affichage] modCameraVr`, `modCameraDepth`, `modCameraZoom`, `modZoomMode` | a mod's camera becomes the origin of a room the head looks around; zoom moves the camera toward the aimed point by default (a magnified rectilinear image looks like a changing fish-eye); the level map is a 3D scale model. RE §6 bis ["3D level map"](REVERSE_ENGINEERING.md#6-bis-3d-level-map) |
| Off arm | `OffArmLateFix` | `[Controles] offArm` | the off hand's bodies are placed by Lua for frame A's camera; at render they are moved onto the camera actually drawn |
| Dual wield | `Hook_GetPlayerEye`, `DualEye` | `[Controles] dualWield` | a two-gun mod tool (profiles `totaloverdose`, `totaloverdose2`) is split between both hands; each shot leaves the gun of the hand that pressed. Turns itself off for the session if the tool behaves unlike its profile. RE §6 octies ["Dual weapons"](REVERSE_ENGINEERING.md#6-octies-dual-weapons-total-overdose) |
| The game's grab | `InitGrabDistance`, `GrabControls` | `[Controles] grabKeepDistance`, `grabMinDistance`, `grabStickSpeed` | three rip-relative operands (the 0.2 m pull and two 1.5 m minimums) are redirected to two floats allocated near the image; in VR the object stays at the distance it was taken and the stick moves it. RE §7 ter ["The game's grab"](REVERSE_ENGINEERING.md#7-ter-the-games-grab-right-grip) |
| The game's computer screens | `ScreenPointerUpdate`, `Hook_ScreenCursor`, `Hook_ScreenRender` | `[Controles] screenPointer`, `screenOncePerPair` | seated at an in-world screen (the campaign's mission computer), the pointing hand's ray is intersected with the screen's mesh and its pixel becomes the cursor; the screen is drawn in frame A only (its script read frame A's click twice); the game's camera glide to the screen is kept at 0 in VR. RE §7 quinquies ["The game's in-world screens"](REVERSE_ENGINEERING.md#7-quinquies-the-games-in-world-screens-the-campaigns-computer) |
| 16:9 pause screen | `WideDecide`, `Hook_CamSet` | `[Ecran] pauseWide` | a paused level (no script ticks) is drawn on a 1920 x 1080 canvas with a 16:9 3D view for the virtual screen |
| Player yaw | `EngineSetPlayerYaw` | - | writes the local player's yaw field directly (the Lua API sets yaw only with position, which shook camera and tool) |

Note on the grab feature: its source comments label their offsets with a different game version number (2.1) than
the rest of the code (2.0.1). Its functions and offsets are all read from its two patterns at run time, so this only
affects the comments.

**Design decisions.**

1. Two engine frames per pair, not two views per frame (GPU hang).
2. Frame B gets zero time by editing the frame time and the accumulators, not by pausing: scripts, tools and
   animations stay consistent and the render interpolation is identical.
3. The right-eye camera comes from the left eye's drawn camera plus the eye offset, never from the game's camera.
4. Off-centre projections go into the projection matrix the engine already uses for everything.
5. Every feature is found by pattern and cross-checks and falls back to the game's behaviour with a log line.
6. Hooks call the original unchanged; game fields are read and written, game code is not rewritten (exception: the
   grab's three operands).
7. Mod compatibility is generic where possible (scaling about the measured muzzle, aiming through
   `GetCameraTransform`, windows through `Ui*` hooks); per-mod knowledge is limited to small tables with automatic
   shut-off.

### 5.8 Cheaper right-eye frame

Files: [frameb.cpp](../src/frameb.cpp), [frameb.h](../src/frameb.h). Setting: `[Performance] frameBMode`
(default 2). RE §6 quater ter ["Lighter frame B"](REVERSE_ENGINEERING.md#6-quater-ter-lighter-frame-b-framebmode-steps-6-and-7).

- **Modes.**
  - 0: `Game::update` runs again in frame B with no time (the original scheme).
  - 1: the same, but a mod whose frame-A tick of the same pair only *read* (bindings named `Get*`, `Query*`, `Is*`,
    `Has*`, `Find*`, with a few exceptions; no registry write or other acting core function) is not ticked again.
    Our own Lua mod is always ticked; `[Performance] frameBKeepMods` lists mods that always are. The census lives in
    [perf.cpp](../src/perf.cpp).
  - 2: "render only". `Game::update` does not run in frame B. What frame A's update asked for one frame only, and
    frame A's render or the next frame start consumed, is asked for again (`ReplayAll`): renderer lines and
    sprites, debug lines, outlines, highlights; the light lists cut back to their length at frame A's update exit;
    depth of field and the low-health blur; camera requests (the camera offset composed with the eye offset); sound
    loop requests, `UiMute`, music volume / low-pass keys, the last sound heard; registry flags that scripts set at
    every tick (a key written once is an event and is never repeated); the time scale. Script events are kept for
    the next frame A (`Hook_EventSwap` skips the buffer swap). Scripts that use real time get the whole pair's real
    time in frame A.
- **Flow per pair.** `FrameB::AEnter` (frame A, before `Game::update`: latches the pair's mode, starts recording) ->
  `FrameB::AExit` (snapshot) -> `Hook_EventSwap` at frame B's start (decides "render only") -> `FrameB::BReplay` in
  `Hook_Update` (replay, or false = full update).
- **Hooks of its own** (each calls the engine unchanged and only records during frame A's update, main thread):
  `Hook_DebugLine`, `Hook_Outline`, `Hook_Highlight`, registry `Hook_SetString` / `Hook_ClearKey`, `Hook_EventSwap`.
- **Install.** `FrameB::HooksWanted()`: `frameBMode` >= 1 in the ini, or the control file exists at start.
  `FrameB::Init(Targets)` (from `InitPerfTrace`) needs every target for mode 2; missing ones are logged and the mode
  falls back to 1 or 0.
- **Fallbacks to a full update** (counted, first ones logged): no frame-A record, frame start not seen, not playing
  a level, game objects changed, sync test running. A memory fault during a replay (caught with SEH) turns mode 2
  off until restart.
- **Decision.** Frame B is made cheaper by replaying one-frame requests rather than skipping their effects: the image
  must stay identical. Image-equivalence tests switch modes live through the control file ([7.4](#74-the-live-control-file)).

### 5.9 Measurement and performance

Files: [perf.cpp](../src/perf.cpp), [perf.h](../src/perf.h), [prof.h](../src/prof.h). RE §6 quater bis
["Performance measurement"](REVERSE_ENGINEERING.md#6-quater-bis-performance-measurement-gains-with-no-visible-change).

- **Frame sections** (`prof.h`): `g_prof[]` accumulates per engine frame, on the main thread. Always timed:
  `P_Update`, `P_Render`, `P_OurPresent`, `P_RealPresent`, `P_PostPresent`. With `perfTrace`: `P_Ours`,
  `P_Particles`, `P_FixedLoop` and the waits `P_JobWait`, `P_GpuWait`, `P_ShapeWait`, `P_DisplayWait`. `ProfScope`
  is the RAII timer. `vr.cpp` logs a "Slow frame" breakdown for frames with more than 18 ms of work.
- **`[Debug] perfTrace`** (0..2, read once at start; nothing is searched or hooked at 0 unless a performance
  feature needs it):
  - 1: job waits, GPU fence waits, the engine's own profiler categories per eye, the frame-latency wait, physics
    steps per frame, CPU per thread group. `[Performance] perfDetail` bits (default 7): 1 blocking waits by caller
    and uploader bytes, 2 every job timed by class, 4 GPU busy / idle per frame (timestamps around each submission),
    8 GPU time per pass (timestamps inserted at pass changes).
  - 2: also the census: each script's tick (`Hook_ModTick`), every Lua binding (`Hook_Dispatch`), registry setters
    and other acting core functions (`Hook_Core<N>`). The census drives frame-B mode 1 and tells `frameb.cpp` which
    registry writes are a script's.
- **Performance features** (all keep the same GPU commands in the same order, or skip only what provably cannot
  change the image):
  - `[Performance] cpuPriority=1`: "above normal" while the session runs, only from "normal"; never high / realtime
    (a streaming app must stay above the game).
  - `frameLatency=2`: `SetMaximumFrameLatency(2)` on the desktop swap chain during the session (the engine sets 1 and
    waits after every Present, so CPU and GPU took turns). The game's value is restored afterwards. RE §6 quater
    quater ["CPU and GPU in parallel"](REVERSE_ENGINEERING.md#6-quater-quater-cpu-and-gpu-in-parallel-framelatency).
  - `lazyBeginFrame=1`: the renderer's `beginRender` (which waits for the GPU frame two engine frames back) is held
    back in frame A until the first call of another renderer method, so scripts and physics start while the GPU
    still draws. Same calls, same order, same thread. 2 = frames B too (test).
  - `jobSpin`, `jobSpinUs`, `mainSpinUs` (default off): idle job threads and the main thread's job wait spin briefly
    in frame A before sleeping.
  - `gpuPriority` (experimental, default 0), `vdStats` (streaming statistics), `hiddenAreaFill` ([5.4](#54-openxr-session-and-frame-orchestrator)).
- **Overlays.** `CheckOverlays()` logs whether an overlay is hooked into Present (shown in the settings panel).

### 5.10 Controller input

Files: [input.cpp](../src/input.cpp), [pckeys.cpp](../src/pckeys.cpp), [modkeys.cpp](../src/modkeys.cpp). RE §6
sexies bis ["Simulated keys and vehicles"](REVERSE_ENGINEERING.md#6-sexies-bis-simulated-keys-and-vehicles), §6 sexies
ter ["Other mods' keys, simulated mouse"](REVERSE_ENGINEERING.md#6-sexies-ter-other-mods-keys-simulated-mouse), §7
quater ["Active window and held buttons"](REVERSE_ENGINEERING.md#7-quater-active-window-and-held-buttons).

- **Role.** Teardown only reads keyboard and mouse. Controllers are translated into Windows input posted to the game
  window. Desktop hotkeys: F10 settings panel, F8 recenter.
- **Hooks.** Import table: `GetRawInputData` (serves our fake raw input), `RegisterRawInputDevices` (learns how the
  game reads input), `SetCursorPos`, `DefWindowProcA/W`. Window subclass at the first Present.
- **Injection.** Fake `WM_INPUT` messages carry handles in a reserved range that `Hook_GetRawInputData` resolves to
  our own `RAWINPUT` data, so nothing reaches other programs. Legacy key and mouse messages are posted too, unless
  the game registered "no legacy". Movement keys are sent with US virtual key *and* US scan code (W A S D by
  position), so non-QWERTY layouts work.
- **Ordering.** While the next frame is a frame B, input messages are held in the window procedure and re-posted
  after Present (`FlushDeferred`). If the frame B never comes (3 Presents), `EngineCancelStereo()`.
- **Focus fix.** The game clears its held keys every frame while it believes it is inactive. While VR runs,
  deactivation messages are rewritten or swallowed so held buttons keep working when another window is in front.
- **Mapping.** `Input::Update(mode, left, right, uiOpen, dt)` with modes Off, Gameplay, Map, Screen. Gameplay on foot:
  dominant trigger = use tool, dominant grip = grab, A jump, B flashlight, X map, Y interact, off trigger crouch,
  dominant stick up / down = tool change, left menu = Esc (long press: settings), stick clicks for recenter, quick
  key, inventory and keys panel. Vehicles, map and screen modes have their own mappings. Hysteresis on triggers and
  grips; pulses last 0.12 s so the game sees them over several frames.
- **Special behaviours.** Light-pull sledge strikes (`sledgeTap`); one click per trigger press for dual wield; stick
  as mouse while a mod owns the camera (`[Mods] modMouse`); grabbed objects moved with the stick
  (`grabKeepDistance`); pointer hand selection (`offHandPointer`); at a game computer screen
  (`EngineScreenShown`), the pointing hand's trigger is the left mouse button and the left trigger does not crouch.
- **PcKeys.** The keys the mod can press, named as Teardown's input API, each with a US virtual key and scan code and
  bilingual labels.
- **ModKeys.** Finds the keys used by the *active* mods without their help, on a background thread: active mods from
  the game's own log and mod lists, Lua files tokenised for `InputPressed("q")`-style calls, saved key options, and
  the player's key map. Shown in the keys panel.

### 5.11 Settings panel

Files: [ui.cpp](../src/ui.cpp), [ui.h](../src/ui.h).

- **Role.** One Dear ImGui context drawn offscreen into a 1100 x 860 texture, shown in the headset (quad layer) and on
  the desktop (blitted into the game's back buffer). Same picture in both places.
- **Key functions.** `Ui::Init`, `Ui::Draw(cl, &srv)` (once per Present; returns `nullptr` when nothing is shown),
  `Toggle`, `ToggleKeys`, `SetVrPointer`, `SetDesktopMouse`, `BannerShown`, `InventoryShown`.
- **Content.** Tabs Display, Comfort, Controls, Screen, Info, and a keys panel (mod keys, keyboard, mouse). Warning
  banners when the panel is closed: another program competing for the GPU, or "low resolution". The same texture also
  carries the VR inventory (top-left) and the banner.
- **Input.** The VR pointer (pointer hand's ray; trigger = click) when active, else the desktop mouse.
- **Saving.** A change marks the settings dirty; `ConfigSave()` runs 0.75 s after the last change (each save
  rewrites the whole ini).
- **Decision.** Offscreen ImGui keeps desktop and headset identical and lets the runtime composite the panel.

### 5.12 VR inventory

Files: [inventory.cpp](../src/inventory.cpp), [inventory.h](../src/inventory.h).

- **Role.** The game's tools and weapons (mods included) drawn like the game's own PC tool bar, re-implemented in C++
  with ImGui. The game's fonts are read at run time from `<game>\data\ui\font` (nothing redistributed).
- **Data flow.** Only Lua can read the game's tool registry, so the Lua mod sends the list in short pieces (`TDVRI*`
  messages, [6](#6-the-lua-and-dll-protocol)); the DLL acknowledges and reports a pick in the response's ` INV`
  group; Lua applies the pick on a normal frame after the game's own checks.
- **Interaction.** Shown on demand (stick click), or briefly on tool change with `[Controles] inventoryOnSwitch=1`;
  hidden in menus, vehicles, the map and
  mod camera views. The pointer hand's trigger picks a tool; the panel scrolls like a phone list.
  `CapturesTrigger` / `CapturesOffTrigger` tell input to suppress the game action meanwhile.
- **Threads.** Lua messages arrive on the main thread under the inventory mutex; drawing happens in the Present path.

### 5.13 Stereo particles and lighting

Files: [particles_vr.cpp](../src/particles_vr.cpp), [particles_vr.h](../src/particles_vr.h). Setting:
`[Affichage] stereoParticles`. RE §6 ter bis ["Particles: the same dither pattern in both
eyes"](REVERSE_ENGINEERING.md#6-ter-bis-particles-the-same-dither-pattern-in-both-eyes-stereoparticles).

- **Problem.** The game draws particles (rain, smoke, fire) with screen-space stochastic transparency: whether a
  pixel is kept depends on a noise value at the *screen* pixel. The same 3D point lies hundreds of pixels apart in
  the two eye images, so each eye saw an unrelated dot pattern (binocular rivalry). The same happened with the
  front / back lighting checkerboard of particles and the noise of the ambient / directional lighting pass.
- **Technique (prose only).** In the right eye, the VR variants of those pixel shaders read the noise at the pixel
  where the *left* eye saw the same point, so both eyes keep the same dots. The reprojection is a small shift fitted
  each frame from frame A's and frame B's view-projection matrices and passed in spare bytes of the scene constant
  buffer (zero in frame A, where the VR shader behaves exactly like the game's).
- **Mechanism (all our code).** `ParticlesVrEarly()` finds by pattern the renderer's graphics-pipeline creation
  function, its buffer update function and the scene-constants upload, and hooks them. `Hook_CreatePso` recognises
  the game's particle and lighting pixel shaders by container hash and size and, when a VR variant exists, creates a
  "twin" pipeline with the game's own state and our pixel shader. `ParticlesVrBeginRender` (from `Hook_Render`)
  swaps the pipeline held by the particle and lighting program objects between the game's and the twin. `ParticlesVrStatus()`: 0 unavailable, 1 game
  pipelines bound, 2 VR bound.
- **Without the game-derived shaders** (`TDVR_GAME_SHADERS=OFF`, the public default): `FindVrBlob()` returns
  nothing, no twin is created, the game's pipelines stay, and `ParticlesVrStatus()` is 0. The hooks stay installed
  and upload a zero shift, which changes nothing.

### 5.14 VR anti-aliasing

Files: [taa_vr.cpp](../src/taa_vr.cpp), [taa_vr.h](../src/taa_vr.h). Setting: `[Affichage] taaMode`. RE §6 ter
["Temporal anti-aliasing"](REVERSE_ENGINEERING.md#6-ter-temporal-anti-aliasing-blur-in-the-distance).

- **Problem.** The game's TAA re-reads its history with a bilinear fetch and keeps most of it. In a headset the head
  is never still, so the history is re-filtered by a sub-pixel offset every frame and distant detail blurs.
- **Technique (prose only).** VR variants of the TAA compute shader read the history with a sharper (Catmull-Rom)
  filter, with variants that trade sharpness against stability.
- **Mechanism.** `TaaVrUpdate()` (from `Hook_Render`, while VR is active) swaps the pipeline pointer stored in the
  engine's TAA compute wrapper; the engine binds that pointer at every dispatch. The original is kept and put back
  for mode 0; reference counts stay balanced. `TaaVrBoundMode()` reports what is bound (read by the particle code).
- **Modes.** 0 the game's own; 1-4 the VR variants (Sharp (smooth), Natural, Very sharp, Maximum). Default 4.
- **Without the game-derived shaders:** modes 1-4 keep the game's TAA, with one log line. A development mode (9)
  loads a compiled shader file from the game folder and works in any build.

### 5.15 GPU competitor warning

Files: [gpu_watch.cpp](../src/gpu_watch.cpp), [gpu_watch.h](../src/gpu_watch.h). Setting: `[Interface] gpuWatch`.

A lowest-priority thread reads Windows performance counters every 2 s (per-process local video memory and engine
utilisation, filtered to the game's adapter). A process above 3 GB or 20 % busy for two samples is reported as
competing, and the panel shows a banner. Compositor and streaming processes are ignored. `Get()` never blocks the
render thread. Reason: another program sharing the GPU can drop the headset frame rate with no visible cause.

### 5.16 Diagnostic tools

- [gpu_trace.cpp](../src/gpu_trace.cpp) (`[Debug] debugGpuTrace`): hooks the game's command-list vtable and logs
  every command of one frame with its calling site. Used to map the renderer's passes.
- [sampler.cpp](../src/sampler.cpp) (`[Debug] debugProfile`): suspends the main thread about 1000 times per second,
  walks its stack with the unwind tables, and reports the top call chains for a slow frame.
- [re_tools.cpp](../src/re_tools.cpp): hardware data breakpoints (`WatchMemory`, `[Debug] debugWatchCamera`), and the
  hang watchdog (`HangDetectorTick` at every Present): no Present for 3 s logs the device-removed reason and the main
  thread's stack, then flushes the log.
- `LogStack(tag)` ([log.cpp](../src/log.cpp)): a call stack as offsets inside `teardown.exe`.

### 5.17 The Lua mod

Files: [mod/TeardownVR/main.lua](../mod/TeardownVR/main.lua), `info.txt`, `proxy.vox`. Installed as
`<game>\mods\teardownvr\` (a built-in mod the player enables once in the game's mod manager). Comments are in French.

- **Entry points.** `init()`, `tick(dt)` (every engine frame, A and B), `update(dt)` (each physics step), `draw()`
  (debug overlays only).
- **`tick(dt)`.** Builds the request flags and calls `request()` ([6](#6-the-lua-and-dll-protocol)); inactive ->
  `onDeactivate`. Then: recenter handling, turning (snap or smooth), the VR room anchor, locomotion
  (`SetPlayerVelocity` from the stick; the game's own walk is neutralised), aim (player yaw / pitch toward the
  controller's laser), the tool (`updateTool`), the camera (`SetCameraOffsetTransform` from the player camera to this
  eye, `SetCameraFov`), the off arm and physical off hand, the off-hand laser, dual wield, inventory, sledge, tool
  collision and the physical tool copy, fog / bloom / depth-of-field removal, HUD flags.
- **Room anchor.** On foot the room is attached to the player camera (not the feet): the game smooths camera height
  at its own rate, and anchoring elsewhere put the two eyes of a pair up to ~10 cm apart vertically during a jump.
  In a vehicle the room is attached to the seat.
- **Frame B rule.** On a frozen frame Lua only re-places camera, tool and laser: no turning, locomotion, physics,
  inventory picks or fog changes, so both eyes agree.
- **`update(dt)`.** The physical tool copy (a dynamic body driven toward the hand with bounded force, torque and
  speed) and the physical off hand. RE §6 sexies ["Physical tool"](REVERSE_ENGINEERING.md#6-sexies-physical-tool-lua),
  §7 bis ["Free-hand arm, pointing hand"](REVERSE_ENGINEERING.md#7-bis-free-hand-arm-pointing-hand).
- **`[Performance] fastLua`.** Same results with fewer engine calls (cached tags, plain tables for poses).
- **Limits.** Lua 5.1 limits (locals per function, upvalues per closure) matter in a script this size; new state goes
  into global tables.

### 5.18 The installer

Files: [installer/installer.cpp](../installer/installer.cpp), [installer/logs.cpp](../installer/logs.cpp),
[installer/CMakeLists.txt](../installer/CMakeLists.txt).

- A separate CMake project (`TeardownVR_Installer.exe`, static C runtime, Unicode, Windows 10+ manifest). The DLL and
  the Lua mod files are embedded as resources. The resource script and payload header in `installer/gen/` are
  generated at release time from the built DLL and the mod folder by
  [tools/package_release.ps1](../tools/package_release.ps1) ([BUILDING.md](BUILDING.md), section 6); they are not
  in the repository.
- **Behaviour.** Language choice; finds the game through Steam's registry keys and library list, or a folder picker;
  refuses while the game runs; install / update writes each file atomically and keeps a foreign `dsound.dll` as
  `dsound.dll.original` (our DLL is recognised by a marker string it always contains); only `[Affichage] language`
  is written to the ini; uninstall restores `dsound.dll.original` and removes our files and logs but keeps
  `TeardownVR.ini`; offers to relaunch elevated on "access denied".
- **"Prepare my logs".** `CollectLogs` writes an uncompressed zip on the Desktop: the current and archived logs, the
  ini, and a short system summary (Windows version, CPU, RAM, GPUs and driver, active OpenXR runtime, game and DLL
  file sizes and dates). Nothing is sent anywhere.
- **Command line (tests).** `/install`, `/uninstall`, `/logs [/out "<dir>"]`, `/gamedir "<dir>"`, `/lang en|fr`,
  `/silent`. Without `/gamedir` it finds and modifies the real game install.
- [tools/install.ps1](../tools/install.ps1) does the same install from a package folder (with `dsound.dll` and a
  `mod\` folder next to it), with French console messages.

### 5.19 The mock OpenXR runtime

Files: [tools/mock_openxr/](../tools/mock_openxr/). CMake targets `tdvr_mock_openxr` and `tdvr_xr_probe`
(not built by default, never shipped).

- **Role.** A real OpenXR runtime DLL (`tdvr_mock_openxr.dll` with its manifest `tdvr_mock_openxr.json`) that lets
  the mod's whole OpenXR path run without a headset. Select it with
  `[Debug] openxrRuntime=<build>\Release\tdvr_mock_openxr.json`. The `_nohmd` manifest answers "no headset" and is
  used with `[Debug] xrAutoRuntimes` to test automatic runtime detection.
- **What it emulates.** A runtime named "TDVR Mock Headset" (the mod detects "Mock" in the name and enables a few
  test-only paths), an asymmetric per-eye field of view, `STAGE` / `LOCAL` / `VIEW` spaces, sRGB and UNORM swapchain formats,
  session states, 72 Hz by default (settable through the refresh-rate extension), frame pacing on a waitable timer, a moving
  head, Touch-like controllers driven from the keyboard or from `mock_out\controls.txt`.
- **Checks.** Every `xrEndFrame` is validated (call order, acquire / release, view count, rectangles, FOV,
  quaternions, blend mode); statistics go to `mock_out\mock.log`, and BMP dumps show what the headset would display.
- **Caveats.** The mock paces frames itself and has its own hitches, so it is not a stutter reference. Its field of
  view rewrites `[Affichage] vrAspect` in the ini through `UpdateRenderResolution`: back up the ini around mock runs.
- `tdvr_xr_probe.exe [runtime.json]` creates an instance with `XR_KHR_D3D12_enable` and prints whether a headset is
  available.

## 6. The Lua and DLL protocol

Transport: [5.6](#56-lua-bridge). Lua side: `exchange()` and `request()` in
[main.lua](../mod/TeardownVR/main.lua). DLL side: [bridge.cpp](../src/bridge.cpp), `VrHandleLuaRequest` and
`BuildResponse` in [vr.cpp](../src/vr.cpp), `Inventory::HandleLuaMessage`. **Every message from Lua is at most 30
characters**, except the diagnostic `TDVRL <text>` lines, which can be longer and may be lost
([5.6](#56-lua-bridge)).

**Request** (once per tick): `TDVR2 <seq> <flags> <hapticL> <hapticR> <vignette>`.

| Field | Meaning |
|---|---|
| `seq` | 1..99999, wrapping (a jump is logged with `debugLua`) |
| `flags` | 1 vehicle, 2 grabbing, 4 map open, 8 reserved, 16 the game's camera was at the player's eyes, 32 the off hand holds something at a distance |
| `hapticL`, `hapticR` | vibration amplitude 0..1 (applied for 30 ms) |
| `vignette` | how much the player moves (0..1), for the comfort vignette |

**Response** (space-separated): `TDVR2 <seq> <status> ...`. Status 0 = VR inactive (Lua gives the camera back),
1 = active, 2 = paused (map shown on the virtual screen). Only status 1 carries the rest:

| Tokens | Content |
|---|---|
| eye, FOVs | eye to render (0 left, 1 right), the FOV for `SetCameraFov` (16:9 horizontal value), the vertical FOV |
| recenter | head height at recenter, recenter counter |
| poses | eye, head, left / right aim, left / right grip (position + quaternion, recentred room space, metres) |
| controls | sticks, triggers, squeezes, a button bit field |
| settings | 20 values the Lua mod needs (turning, movement, roomscale, height, world scale, handedness, aim mode, HUD, laser, tool offsets, vignette, panel open) |
| debug flags | Lua diagnostics, sync / FOV tests, player yaw writable by the DLL, mock runtime, `fastLua` |
| frame and options | `frozen` (1 = frame B), fog / bloom / depth-of-field removal, swing gesture, tool collision / block / physics |
| tool | tool kind (0 none, 1 game tool, 2 mod tool), the mod tool's desktop pose, its right-hand point, the scale applied |
| tagged groups | ` PIV` (mod tool muzzle), ` ARM` (off arm and grab state), ` LAS` (off-hand laser), ` DUA` (dual wield), ` INV` (inventory acknowledgement and pick) |

Tagged groups are found by tag, so an older Lua ignores groups it does not know.

**One-way messages** (no response):

| Message | Effect |
|---|---|
| `TDVRL <text>` | a "Lua: ..." line in the log (diagnostics; may be longer than 30 characters, so it can be lost) |
| `TDVRY <deg>` | player yaw (`EngineSetPlayerYaw`) |
| `TDVRA <deg>` | yaw of the VR room in the world this frame (`EngineSetAnchorYaw`) |
| `TDVRT <x y z>` | laser target relative to this frame's eye camera (mods' crosshairs) |
| `TDVRZ <m>` | distance along a mod camera's axis to its aimed point (zoom) |
| `TDVRW <hand> <arm>`, `TDVRE <x y z>` | off hand / forearm bodies; camera minus hand position (late render fix) |
| `TDVRG`, `TDVRK`, `TDVRV`, `TDVRX` | dual wield: muzzle, target, off-hand copy, copy camera offset |
| `TDVRH <0/1> [<0/1>]` | the physical off hand can take an object |
| `TDVRU` | release the movement keys the game sees |
| `TDVRS` | legacy: accepted by `bridge.cpp` (presses "use tool" briefly) but no longer sent by `main.lua`, which now does the sledge swing itself ([PROBLEMS_AND_SOLUTIONS.md, I11](PROBLEMS_AND_SOLUTIONS.md#i11-sledgehammer-swing-gesture-trembling-and-swings-that-broke-nothing)) |
| `TDVRIB`, `TDVRIT`, `TDVRIN`, `TDVRIA`, `TDVRIC` | VR inventory list, in pieces (version, tool, name chunk, ammo, tool in hand); text escaped as `~XX` |
| `TDVR1 ...` | old protocol: answered "inactive" and logged as an old copy of the mod |

## 7. Settings system

Files: [config.h](../src/config.h), [config.cpp](../src/config.cpp). Some comments in `config.h` are still in
French.

### 7.1 TeardownVR.ini

- `struct Settings` (config.h) holds every setting with its default; `Cfg()` returns the single instance. Code reads
  `Cfg().key` where it uses a value, so most changes apply at once.
- The ini lives next to `teardown.exe` (`ModDirectory()`). `Fields()` maps each `{section, key, type, pointer}`; it
  is read and written with `GetPrivateProfileStringA` / `WritePrivateProfileStringA`.
- `ConfigLoad()` (in DllMain): no ini -> defaults written. An ini without `[Affichage] language` predates the English
  version and is read as French. The obsolete `[Affichage] xrRuntime` key is removed. Runtime selection:
  `headsetRuntime` 1-3 sets `XR_RUNTIME_JSON` here; 0 is decided later in `vr.cpp`, never in DllMain;
  `[Debug] openxrRuntime` forces a runtime; `debugLua` turns on the OpenXR loader's log.
- `ConfigSave()` rewrites every key (55-270 ms measured on the calling thread): the settings panel calls it 0.75 s
  after the last change. `ConfigSaveKeys({keys})` writes only those keys on a worker thread, at most once per
  second, merged (used for panel places moved with a grip). One ini writer at a time (`g_iniMutex`), because each
  write rewrites the whole file.
- `ConfigReset()` restores defaults and keeps the language.

### 7.2 Sections

| Section | Panel tab | Main consumers | Examples |
|---|---|---|---|
| `[Affichage]` (Display) | Display | vr.cpp, engine.cpp, taa_vr.cpp, particles_vr.cpp, render blit, main.lua | `language`, `renderPreset`, `fovCoverage`, `eyeResScale`, `refreshRate`, `stereoMode`, `perEyeHistory`, `taaMode`, `offCenter`, `stereoParticles`, `worldScale`, `hudFix`, `modUi*`, `modCamera*`, `headsetRuntime` |
| `[Confort]` (Comfort) | Comfort | main.lua (vignette applied by vr.cpp) | `turnMode`, `snapAngle`, `smoothTurnSpeed`, `moveDirection`, `moveSpeed`, `roomscale`, `heightOffset`, `vignette` |
| `[Controles]` (Controls) | Controls | input.cpp, engine.cpp, vr.cpp, inventory.cpp, main.lua | `leftHanded`, `aimMode`, `modAimHand`, `dualWield`, `offArm`, `offHandPointer`, `grabKeepDistance`, `inventory*`, `sledgeTap`, `toolCollision`, `modToolScale` |
| `[Mods]` | keys panel | input.cpp, ui.cpp | `modMouse`, `modMouseStick`, `modMouseSpeed`, `quickKey` |
| `[Ecran]` (Screen) | Screen | vr.cpp, engine.cpp | `screenDistance`, `screenWidth`, `forceScreen`, `pauseWide` |
| `[Interface]` | Screen | vr.cpp, ui.cpp, gpu_watch.cpp | `uiScale`, `uiPanelDistance`, `uiPanelDown`, `showDesktopUi`, `gpuWatch` |
| `[Debug]` | ini only | everywhere | `debugFakeHmd`, `debugLua`, `debugSyncTest`, `debugFovTest`, `debugProfile`, `debugGpuTrace`, `debugWatchCamera`, `perfTrace`, `logSync`, `openxrRuntime`, `xrAutoRuntimes` |
| `[Performance]` | ini only (one "CPU optimization" checkbox and `holdSmooth` in Display) | perf.cpp, frameb.cpp, engine.cpp, vr.cpp, main.lua | `frameBMode`, `frameLatency`, `lazyBeginFrame`, `holdSmooth`, `cpuPriority`, `skipDeadPasses`, `parallelParticleSort`, `hiddenAreaFill`, `perfDetail`, `fastLua` |

The "CPU optimization" checkbox sets `frameBMode=2`, `frameLatency=2` and `lazyBeginFrame=1` together (or all to 0).
A few keys of the other sections are ini only too (for example `fovCrop`, `stereoSameNoise`, `stereoParity`,
`grabMinDistance`, `grabStickSpeed`, `sledgeTrigger`, `inventoryFollowAngle`).

### 7.3 When a change takes effect

- **At once:** almost everything (read through `Cfg()` at each use).
- **At the next game start:** `renderPreset` and the render-size part of `fovCoverage` (the window size is chosen at
  start), `headsetRuntime`, the learned `vrAspect`.
- **Read once in DllMain:** `[Debug] logSync`, `perfTrace`.
- **Hooks installed only if wanted at start:** the frame-B hooks (`frameBMode` >= 1 or the control file present),
  `lazyBeginFrame`, the `perfTrace` hooks, the lens-mask extension for `hiddenAreaFill`. Turning one of these on later
  takes effect from the next start. Once its hooks are installed, `frameBMode` is re-read every frame A and can
  change live.

### 7.4 The live control file

`<game>\TeardownVR_perf.txt` is an optional file for tests (A/B and image-equivalence runs). `frameb.cpp`
(`PollControl`) re-reads it when it changes, at most once per second. Each `key=N` line overrides the ini value live; removing the file restores the ini values. Keys: `frameBMode`, `frameLatency`, `lazyBeginFrame`,
`hiddenAreaFill`, `gpuPriority`, `parallelParticleSort`, `skipShapeJobsB`, `skipDeadPasses`, `stereoSameNoise`,
`stereoParticles`, `taaMode`, `stereoParticleMotion`, `particleMotionLog`, `stereoParity`; `perf.cpp` also reads
`perfDetail`, `jobSpin`, `jobSpinUs`, `mainSpinUs`. Its presence at game start also makes the frame-B and job-spin
hooks install.

## 8. Localisation

- **Languages.** English (default for a fresh install) and French, chosen by `[Affichage] language` (`en` or `fr`).
  The installer asks; the settings panel switches at once.
- **[i18n.h](../src/i18n.h).** `LangFr()` and `Tr("English", "Français")`, which returns one of two string literals at
  each call. Nothing is allocated and a switch shows on the next frame. Printf-style pairs must take the same
  arguments in the same order. ImGui labels that change with the language end with a fixed `###id` so tabs and
  windows keep their state.
- **Runtime-built text.** `VrText {en, fr}` ([vr.h](../src/vr.h)) keeps both languages and picks one when read (status
  texts, runtime detection text).
- **Other places.** PcKeys labels and the keyboard layout shown in the keys panel (QWERTY in English, AZERTY in
  French; keys are sent by name, so this is only display). OpenXR action names use the language active when the
  session is created. The installer has its own two-language strings. The mod's `info.txt` is bilingual.
- **Not localised.** The log (English), the Lua mod's comments (French), the ini section names (French, kept for
  compatibility).

## 9. Logging

Files: [log.cpp](../src/log.cpp), [log.h](../src/log.h).

- **File.** `<game>\TeardownVR.log`. At start the previous session's log is moved to
  `<game>\TeardownVR_journaux\TeardownVR_<date>_<time>.log`; the 10 most recent are kept. The installer's "Prepare my
  logs" zips them.
- **Never blocks a game thread.** `Log()` formats the line on the calling thread (time since start, thread id, text)
  and appends it to a pending buffer. A below-normal-priority writer thread writes it with one `WriteFile` every
  50 ms, sooner past 64 KB pending. Past 8 MB pending (disk stalled), lines are dropped and counted.
- `[Debug] logSync=1`: every line is written by the calling thread (for a crash that kills the process).
- `LogFlush()`: writes now, on the calling thread, with bounded waits for the locks (a crashed or suspended thread may
  hold them). `LogShutdown()`: last lines at DLL unload.
- **Crash flush.** A vectored exception handler, added last, reacts to fatal-kind codes (access violation, illegal
  instruction, divide by zero, in-page error, heap corruption, stack buffer overrun). It never waits: it writes only
  if the log is free at that instant, else it wakes the writer thread.
- **Useful lines.**
  - `Engine: synchronised stereo ready`, or `Engine: not recognised ...`; one line per feature that was not
    recognised.
  - `XR: ...` (runtime, session states, FOVs, slow runtime calls); `Render resolution: ...`.
  - `Stereo frames: ...` every 600 Presents: frame counts and times per kind, headset and game FPS, `xrWaitFrame`
    time, slow frames, physics catch-ups dropped, missed headset refreshes, plus frame-B, lazy-begin and stream
    statistics.
  - `Slow frame ...`: the time breakdown of frames with more than 18 ms of work (a few per stats line).
  - `Hang: ...`: no Present for 3 s, with the main thread's stack.
  - `Frame B: ...`, `Perf: ...`, `TAA: ...`, `Lua: ...` (diagnostics sent with `TDVRL`).
- **Debug keys** (`[Debug]`, ini only): `debugLua`, `debugProfile`, `debugSyncTest`, `debugFovTest`, `debugGpuTrace`,
  `perfTrace` and `[Performance] perfDetail` add detail ([5.9](#59-measurement-and-performance),
  [5.16](#516-diagnostic-tools)).

## 10. Build options

Requirements: Windows x64, Visual Studio 2022 with the C++ desktop tools, CMake 3.20 or later, and the three
third-party libraries in `third_party/` (OpenXR SDK `release-1.1.49`, MinHook, Dear ImGui `v1.91.9`).

```bash
cmake -S . -B build -A x64
cmake --build build --config Release --target TeardownVR
```

Output: `build/Release/dsound.dll`. For a manual install, copy it into `<game>` and copy the contents of
`mod/TeardownVR/` into `<game>\mods\teardownvr\`, then enable "Teardown VR" once in the game's mod manager.

[CMakeLists.txt](../CMakeLists.txt):

| Item | Value | Why |
|---|---|---|
| Language | C++20 | |
| C runtime | `/MD` (dynamic), `/MDd` in Debug | |
| OpenXR loader | built as a static library (`DYNAMIC_LOADER OFF`; tests, API layers, conformance off; bundled JsonCpp) | linked into `dsound.dll`; the runtime detection relies on this loader re-reading `XR_RUNTIME_JSON` at each instance creation |
| MinHook, Dear ImGui | static libraries built from their sources (ImGui with the DX12 and Win32 backends) | |
| Target `TeardownVR` | shared library, output name `dsound` | |
| Definitions | `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `_CRT_SECURE_NO_WARNINGS`, `XR_USE_PLATFORM_WIN32`, `XR_USE_GRAPHICS_API_D3D12` | |
| Compiler options | `/utf-8 /W3` | the sources contain UTF-8 text (French strings) |
| Linked | `openxr_loader`, `minhook`, `imgui`, `dxguid`, `d3d12`, `dxgi`, `user32`, `advapi32`, `shell32`, `ole32`, `pdh`, `delayimp` | |
| Delay-loaded | `d3d12.dll`, `dxgi.dll`, `pdh.dll` | DllMain must not pull D3D12 / DXGI in under the loader lock; `pdh.dll` is only needed when the GPU watch starts |
| `TDVR_GAME_SHADERS` | option; default ON only if `src/generated/particles_p0.h` exists | see below |
| `tdvr_mock_openxr`, `tdvr_xr_probe` | `EXCLUDE_FROM_ALL` (build them by name); the mock's two JSON manifests are copied next to the DLL | test only, never shipped |

**`TDVR_GAME_SHADERS`.** The VR particle, lighting and anti-aliasing shaders are rebuilt from the game's own shaders,
so neither their sources (`shaders/*.hlsl`) nor their compiled headers (`src/generated/*.h`) are published. Without
them (the default for a clean clone) CMake defines `TDVR_NO_GAME_SHADERS` and prints "built WITHOUT the game-derived
VR shaders". The DLL then builds and runs normally, but the stereo-particles fix ([5.13](#513-stereo-particles-and-lighting))
and the VR anti-aliasing modes ([5.14](#514-vr-anti-aliasing)) are unavailable: the game's own shaders are kept, and
the settings for them have no effect. Only those two source files depend on the option; it is the only compile-time
switch in the sources. The auto-detection only tests one header, so a partial `src/generated/` would select ON and
fail to compile.

**Other build products.**

- The installer is a separate project ([installer/CMakeLists.txt](../installer/CMakeLists.txt): static C runtime,
  `/W4 /permissive-`, Unicode). It needs the generated `installer/gen/` files ([5.18](#518-the-installer)).
- `cmake --build build --config Release --target tdvr_mock_openxr` (and `tdvr_xr_probe`) builds the test runtime
  ([5.19](#519-the-mock-openxr-runtime)).

## 11. When a Teardown update breaks a feature

What happens technically:

- Engine functions are found by byte pattern, and most field offsets are read from the matched instructions,
  usually checked against another pattern. A pattern that no longer matches, or offsets that disagree, turn off
  only that feature, with a "not recognised" (or "unavailable") line in `TeardownVR.log`. The game's own behaviour
  stays for that feature.
- Some offsets (renderer, shapes, particles, the TAA wrapper) are 2.0.1 constants, and `[Debug] debugWatchCamera`
  reads a hard-coded 2.0.1 RVA. A pattern that still matches does not reveal a changed constant: re-check them by
  hand ([UPDATING §3.3](UPDATING_FOR_A_NEW_GAME_VERSION.md#33-constants-to-re-check-by-hand)).
- A change that no check catches (a field whose meaning changed, a stale constant) can make a feature misbehave or
  crash the game.
- If `Game::update` or the frame-time fields are not found, synchronised stereo is unavailable and the mod falls back
  to alternating eyes ([4.8](#48-fallback-alternating-eyes)).
- Memory faults in replay code (frame B, particle motion) are caught and turn the feature off until restart.
- The DXGI, OpenXR, input and clipboard parts do not depend on engine patterns, but the clipboard workaround depends on
  the game's binding behaviour ([5.6](#56-lua-bridge)) and the Lua mod depends on the game's Lua API.

Where to look: run a level with the headset, then read the log for those lines. Each `Init*` function in
[engine.cpp](../src/engine.cpp) documents its patterns and the offsets they carry, and
[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) explains how each one was found (RE §8
["Other landmarks"](REVERSE_ENGINEERING.md#8-other-landmarks-201) and §9 ["Tools"](REVERSE_ENGINEERING.md#9-tools)).
