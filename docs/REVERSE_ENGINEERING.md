# Teardown VR: reverse-engineering notes

## About these notes

These are the reverse-engineering notes written while developing Teardown VR, a
VR mod for Teardown. They explain how the mod finds the engine functions and
data it relies on, which fields it reads and writes, what was measured, and why
each fix works the way it does. They are interoperability documentation: they
help to read the source code (mostly `src/engine.cpp`) and to find the byte
patterns again after a game update.

* **Game version.** Everything here concerns Teardown 2.0.1 (Steam, x64,
  D3D12). All addresses are RVAs (offsets from the base of the `teardown.exe`
  module) in that version. The mod finds every engine function at run time
  with byte patterns (see `src/engine.cpp`), and reads most field offsets from
  the matched instructions, cross-checked. Some offsets (renderer, shapes,
  particles, the TAA wrapper) are 2.0.1 constants in the source, and one
  debug-only RVA is hard-coded (`[Debug] debugWatchCamera`): they must be
  re-checked by hand after an update (`docs/UPDATING_FOR_A_NEW_GAME_VERSION.md`,
  section 3.3). If a pattern is no longer found after a game update, the
  feature that depends on it disables itself and the rest of the mod keeps
  working. A Teardown update can still change what is described here and
  break the mod, or crash the game where no check catches the change.
* **Encrypted executable.** The code of `teardown.exe` is encrypted on disk by
  the SteamStub DRM. The analysis was therefore done on the memory of the
  running process, after the game has decrypted itself, and the mod installs
  its engine hooks the same way, at run time. The mod does not modify, remove or
  bypass the DRM, and this project neither contains nor distributes any game
  binary, decrypted or not.
* **No game code.** These notes reproduce no code from the game: no
  disassembly listing, no decompiled function, no shader source. They contain
  RVAs, structure offsets, vtable slots, a few short byte patterns and
  instruction sequences of a few instructions used as signatures, one-line
  formulas, and descriptions in plain words.
* **Game-derived shaders.** Two fixes (the VR anti-aliasing modes of
  [6 ter](#6-ter-temporal-anti-aliasing-blur-in-the-distance) and the stereo
  particles of
  [6 ter bis](#6-ter-bis-particles-the-same-dither-pattern-in-both-eyes-stereoparticles))
  replace game shaders with modified versions rebuilt from the game's compiled
  shaders. Those files (`shaders/*.hlsl`, `src/generated/*.h`) are derived from
  the game and are not published; their technique is described here in prose
  only. The CMake option `TDVR_GAME_SHADERS` (by default on only when
  `src/generated/particles_p0.h` exists) builds the mod without them: the VR
  anti-aliasing modes and the stereo-particles fix are then unavailable, and
  the game's own shaders are kept.
* **Status.** These are working notes, provided as is, without warranty and
  without support (see the GPL-3.0 licence in `LICENSE`). Some points were
  checked on one setup only, and some are marked as not verified.

### Conventions

* Settings are written `[Section] key`, as in the mod's `TeardownVR.ini`. Some
  section names are French and are the real names used in the file:
  `[Affichage]` is the display section and `[Ecran]` the screen section. The
  settings panel is bilingual; its labels are quoted here in English.
* A hexadecimal number with no name in front (`0xd5a50`; also written
  `+0x496410`, or `+4a9050` as in the log lines) is an RVA in `teardown.exe`
  2.0.1. After another module's name (`virtualdesktop-openxr.dll+0x52390`) it
  is an offset in that module. After a structure or an object (`game+0x204`,
  `player+0x640`, `ui+0x2d8`) it is a field offset, and `*(x+0x60)` is the
  pointer stored at that offset.
* Section numbers keep the Latin suffixes of the original notes (bis, ter,
  quater, quinquies, sexies, septies, octies: 2nd to 8th sub-section).
  Sections were added over time, so they are not in numeric order; references
  such as "see 6 sexies" use these numbers.
* Frame A and frame B are the two engine frames of one synchronised stereo
  pair: frame A renders the left eye with a normal game update, frame B renders
  the right eye with no time passing (the "zero-time" or "frozen" frame). See
  [section 4](#4-synchronised-stereo-two-eyes-same-instant).
* "Mock headset" is the mock OpenXR runtime of [section 9](#9-tools)
  (`tools/mock_openxr`), which runs the mod's real OpenXR code without a
  headset. "Simulated headset" means a measurement made without a real headset
  (with that runtime or with the DLL's built-in simulated head,
  `debugFakeHmd`). "In the headset" means on a real headset.
* "The Lua" is the mod's Lua script (`main.lua`), "the DLL" is its native part
  (`dsound.dll`), and "a mod" is another mod running in the game. Field numbers
  ("field 81 of the reply") refer to the reply the DLL sends to the Lua through
  the channel of [section 3](#3-the-lua-dll-channel).
* VD is Virtual Desktop and VDXR its OpenXR runtime. LnL and UMF are frameworks
  used by some Workshop weapon mods (their scripts read the weapon's bones).
  NP is the Natural Phenomena mod and TO the Total Overdose mod.
* Rounds ("round 2", "r17", "round 18") and dates are development iterations,
  kept so that the history of a fix stays readable.

## Contents

* [About these notes](#about-these-notes)
  * [Conventions](#conventions)
* [1. Loading](#1-loading)
* [2. Rendering and OpenXR](#2-rendering-and-openxr)
* [2 bis. Choosing the OpenXR runtime (headsetRuntime)](#2-bis-choosing-the-openxr-runtime-headsetruntime)
* [3. The Lua-DLL channel](#3-the-lua-dll-channel)
* [4. Synchronised stereo (two eyes, same instant)](#4-synchronised-stereo-two-eyes-same-instant)
* [5. Camera](#5-camera)
* [5 bis. Frame pacing and input in VR](#5-bis-frame-pacing-and-input-in-vr)
* [5 ter. Field of view, right-eye camera, pacing, per-eye history](#5-ter-field-of-view-right-eye-camera-pacing-per-eye-history)
* [6. Aiming and the tool](#6-aiming-and-the-tool)
* [6 octies. Dual weapons (Total Overdose)](#6-octies-dual-weapons-total-overdose)
* [6 ter. Temporal anti-aliasing (blur in the distance)](#6-ter-temporal-anti-aliasing-blur-in-the-distance)
* [6 ter bis. Particles: the same dither pattern in both eyes (stereoParticles)](#6-ter-bis-particles-the-same-dither-pattern-in-both-eyes-stereoparticles)
* [6 quater. Stutters](#6-quater-stutters)
* [6 quater bis. Performance measurement, gains with no visible change](#6-quater-bis-performance-measurement-gains-with-no-visible-change)
* [6 quater ter. Lighter frame B (frameBMode, steps 6 and 7)](#6-quater-ter-lighter-frame-b-framebmode-steps-6-and-7)
* [6 quater quater. CPU and GPU in parallel (frameLatency)](#6-quater-quater-cpu-and-gpu-in-parallel-framelatency)
* [6 quater quinquies. Explosion physics: what waits for what (r17, 2026-10-12)](#6-quater-quinquies-explosion-physics-what-waits-for-what-r17-2026-10-12)
* [6 sexies bis. Simulated keys and vehicles](#6-sexies-bis-simulated-keys-and-vehicles)
* [6 sexies ter. Other mods' keys, simulated mouse](#6-sexies-ter-other-mods-keys-simulated-mouse)
* [6 quinquies. Particles and physics catch-up](#6-quinquies-particles-and-physics-catch-up)
* [6 sexies. Physical tool (Lua)](#6-sexies-physical-tool-lua)
* [6 bis. 3D level map](#6-bis-3d-level-map)
* [6 septies. Mod cameras and per-eye 2D interface](#6-septies-mod-cameras-and-per-eye-2d-interface)
* [7. Render resolution](#7-render-resolution)
* [7 bis. Free-hand arm, pointing hand](#7-bis-free-hand-arm-pointing-hand)
* [7 ter. The game's grab (right grip)](#7-ter-the-games-grab-right-grip)
* [7 quater. Active window and held buttons](#7-quater-active-window-and-held-buttons)
* [7 quinquies. The game's in-world screens (the campaign's computer)](#7-quinquies-the-games-in-world-screens-the-campaigns-computer)
* [8. Other landmarks (2.0.1)](#8-other-landmarks-201)
* [9. Tools](#9-tools)

## 1. Loading

* `teardown.exe` imports `DSOUND.dll` (ordinal 1, `DirectSoundCreate`).
  `dsound` is not a KnownDLL, so a `dsound.dll` placed next to the exe is
  loaded instead of the system one. `src/proxy.cpp` + `src/dsound.def` forward
  the 12 exports to `System32\dsound.dll`.
* The game's `.text` section is encrypted on disk by the SteamStub DRM. All of
  the analysis is therefore done on the process memory, and the engine hooks
  are installed at the first `Present` (by then the code has been decrypted).
* `DllMain` does nothing if the host exe is not `teardown.exe`.

## 2. Rendering and OpenXR

* DXGI hooks: `CreateDXGIFactory*` → the factory's vtable
  (`CreateSwapChain` 10, `CreateSwapChainForHwnd` 15) → the vtables of the
  swapchain (`Present` 8, `ResizeBuffers` 13, `Present1` 22, `ResizeBuffers1` 39)
  and of the command queue (`ExecuteCommandLists` 10). The numbers are
  vtable slot indices.
* `DXGI_USAGE_SHADER_INPUT` is added to the backbuffer so that it can be read.
* OpenXR session through `XR_KHR_D3D12_enable`, on the game's own device and
  command queue. STAGE reference space. A projection layer (the eyes) and quad
  layers (the virtual screen and the settings panel).
  `XR_FB_display_refresh_rate` when it is available.
* On every `Present`, the backbuffer is copied (blit shader: sRGB→linear,
  sharpening, brightness, vignette) into the OpenXR swapchain of the eye that
  was just rendered.
* The game renders a **symmetric** frustum whose horizontal angle is given by
  `SetCameraFov` and whose width/height ratio comes from the backbuffer. The mod
  therefore renders a symmetric field of view that encloses the headset's
  asymmetric one, and declares it as is in the projection layer.

## 2 bis. Choosing the OpenXR runtime (headsetRuntime)

* `[Affichage] headsetRuntime`: 0 = automatic (the default, since 2026-10-12). At startup (`HeadsetInUse`,
  before the game window exists; otherwise at the first `Present`),
  `vr.cpp RuntimeStartup` asks each candidate runtime whether it has a headset:
  it creates an instance of that runtime alone (`xrCreateInstance` +
  `xrGetSystem` + `xrDestroyInstance`) after putting that runtime's `.json`
  into `XR_RUNTIME_JSON`. The OpenXR loader (statically linked,
  `third_party/openxr`) re-reads the variable on every `xrCreateInstance` that
  has to load a runtime, and unloads the runtime on `xrDestroyInstance`
  (`loader_core.cpp` `LoaderXrDestroyInstance` →
  `RuntimeInterface::UnloadRuntime`; `manifest_file.cpp` `FindManifestFiles` →
  `GetEnvironmentVariableW`). Only one instance can exist at a time
  (`XR_ERROR_LIMIT_REACHED`). Caution:
  `xrEnumerateInstanceExtensionProperties` loads the runtime without unloading
  it. In a process running as administrator, the variable is ignored.
* Order: Windows' active runtime (HKLM `...\OpenXR\1` `ActiveRuntime`; when
  both are connected, this one is used), then Virtual Desktop, then Meta Quest
  Link; SteamVR only if it is Windows' active runtime, and last (querying it
  launches SteamVR). No headset found: Windows' runtime is used, and
  `RuntimeRedetect` asks again when the signal described below stays raised for
  two checks in a row (6 s; at most once every 15 s), destroying the game's
  instance first.
* Headset signal: the named event `OculusHMDConnected` (in the user's
  session). `OVRServer_x64.exe` +0x24f55c creates it (`CreateEventW`, manual
  reset; `NtQueryEvent`: notification type). Meta's OpenXR runtime
  (`LibOVRRTImpl64_1.dll` +0xebc7f, inside `xrGetSystem`) does
  `OpenEventW` + `WaitForSingleObject(0)` and answers
  `XR_ERROR_FORM_FACTOR_UNAVAILABLE` (-35) when the event is not there. So the
  mod only queries Meta while the signal is raised, without losing anything,
  and never loads Meta's runtime when no headset is connected through Link.
  Virtual Desktop's streamer uses the same event name (Oculus emulation): this
  is why Virtual Desktop is queried before Meta. The VDXR runtime hooks
  `OpenEventW` for that name only inside `RobloxPlayer`
  (`virtualdesktop-openxr.dll` +0x6d4a0); it imports neither `CreateProcess`
  nor `ShellExecute`.
* 1 = Virtual Desktop, 2 = Meta Quest Link, 3 = SteamVR: forced (`ConfigLoad`).
  4 = Windows' runtime without detection (the old `xrRuntime=0`). The
  `xrRuntime` key is removed from the ini when it is loaded.
  `[Debug] openxrRuntime` takes precedence over everything. Testing:
  `[Debug] xrAutoRuntimes` (see the README), and a mock headset that reports
  "no headset" through `tdvr_mock_openxr_nohmd.json` or `controls.txt`
  `nohmd=1`.

## 3. The Lua-DLL channel

Lua scripts have no access to files or sockets. The channel uses
`SetClipboardText` / `GetClipboardText`, which the DLL intercepts in the game's
IAT (`OpenClipboard`, `EmptyClipboard`, `SetClipboardData`,
`GetClipboardData`, `CloseClipboard`): a text that starts with `TDVR2` is a
request. The reply (eye pose, hand poses, buttons, settings) is returned
through `GetClipboardData`. Nothing ever reaches the real clipboard.

* These two Lua functions are reserved for "internal" mods (type 0/1, tested
  through `[ctx+0x420]+0x3c4` in the Lua binding). The mod is therefore
  installed in the game's `mods` folder, where it appears in the "Built-in"
  list. None of the game's checks is modified.
* **Game bug**: the `SetClipboardText` binding (≈ RVA `0x4b7120`) copies the
  Lua string into a temporary `std::string`, destroys it, then reads its buffer
  again (use after free). For a long string (allocated on the heap), the block
  is sometimes reused in the meantime and the text arrives empty, roughly every
  0.4 s. A string of at most 30 characters stays in the `std::string`'s
  internal buffer (SSO, flag byte at `+0x1f`), which the destructor does not
  touch. Requests are therefore at most 30 characters long
  (`TDVR2 <seq> <flags> <vibG> <vibD> <vignette>`, where `vibG` / `vibD` are
  the left / right controller vibration; `TDVR1` = an old copy of the mod, to
  which the DLL answers "VR inactive", so that copy does nothing). Diagnostic
  messages are sent in a separate `TDVRL …` request, which may be lost.
* The `platform->vfunc[0x20]` check in `Platform::setClipboardText` calls
  `ISteamUtils010` slot 34 = `IsSteamRunningOnSteamDeck` (constant result).

## 4. Synchronised stereo (two eyes, same instant)

The engine cannot render two views within one frame: rendering twice in a
frame overwrites its per-frame GPU data (tested: `DEVICE_HUNG`). Each stereo
pair therefore uses two engine frames that show **the same simulation state**:

| Frame | `Game::update` | Physics | Camera |
|---|---|---|---|
| A | normal | normal fixed steps | left eye |
| B | `dt = 1e-5`, accumulators restored | 0 steps | right eye |

* `Game::update` is found through its call in `Game::frame`
  (`49 8B CC E8 ?? ?? ?? ?? 49 8B CC E8 …`, RVA 0x7a080 in 2.0.1).
* Time fields (`Game::frame`, end of its timekeeping):
  `dt` at `game+0x204`, fixed-step accumulators at `game+0x214` and
  `game+0x218`.
* Frame B: the time of B is set aside, then added to the next frame A
  (accumulators clamped to 2.99/60, as the engine does), so the game runs at
  the correct speed.
* Scripts, tools and animations also run in B (with zero time), so everything
  stays consistent. The Lua script knows when it is in B (field 81 of the
  reply) and changes nothing there (no turning, no movement, no aiming).
* Mods that move an object by a fixed step per tick, without using `dt`: their
  tick in B moves it again. The AC130 mod (`AdvanceGameplayPlane`, +0.15 m and
  0.05° per tick) is one of them. The two eyes then saw two different worlds.
  - Measurement on the simulated headset (left → right shift within one pair, view from the plane): ground at 200 m:
    −567 px.

    | Fix | Plane | Relative to the ground |
    |---|---|---|
    | without (`syncScriptMotion=0`) | −545 to −548 px | +19 to +22 px, "farther than infinity": the double image people saw |
    | with | −580 to −584 px | −13 to −17 px, i.e. about 5 m, correct |

    Vertical shift < 1.2 px in both cases.
  - Fix (`FixObjectMotion`, `syncScriptMotion`): when the left eye is
    rendered, every shape of the scene (`scene+0x238`) is recorded together
    with its current render transform `+0x13c` and its previous one `+0x158`.
    When the right eye of the next frame is rendered, the left eye's `+0x13c`
    and `+0x158` are written back onto every shape whose `+0x13c` has changed.
  - Even on the ground, without the plane, a few hundred shapes change between
    the two eyes:
    - level parts rotated by a script, at a constant position;
    - an object of a mod parked at y = 10,000 in one eye and visible in the
      other.
  - Shapes created during B are not touched.
  - Who writes what (review of 10 October, read in the memory of the running
    game):
    - `+0x13c` / `+0x158` are written by ShapeInterpolationJob (0xd5a50) for
      every shape with `flags+0xc8 & 0x60`. 0x20 = visible, static shapes
      included; 0x40 = the player's tool; it does not mean "dynamic".
    - These jobs are launched by Scene::preRender (0xeb160) on worker threads.
      The engine only waits for them at `Renderer::render+0x4a7`
      (Scene::waitShapeInterpolation 0xf4150), after our hook.
      `FixObjectMotion` therefore calls that same wait first.
    - SetBodyTransform (0x31010) does not write `+0x13c`: only the world
      bounding box `+0xa4`, which is left as it is (it only affects the right
      eye's culling: at worst, one frame at an edge of the screen).
  - Safeguards:
    - a shape is only restored if its `+0x158` in B equals the left eye's
      `+0x13c` (the same shape, not a reused address);
    - with a shared history (`perEyeHistory` off), the "previous" position
      that is written back is the left eye's current one (zero motion).
  - Verified on the simulated headset:
    - plane −12 to −14 px relative to the ground;
    - 25 s of continuous firing from the plane without errors;
    - the tool in hand stays stable while walking (average 1 mm per eye).

* `GetCameraTransform()` called by other scripts (aiming other mods with the
  controller, `modAimHand`). The binding (0x475f80) returns the rendered
  camera, which follows the head in VR (and alternates between eyes A/B). The
  Air Strike mod, used on the ground, therefore aimed wherever you were
  looking.
  - The DLL hooks it and returns `GetPlayerCameraTransform` instead
    (0x478c10, `player+0x640`), whose axis passes exactly through the laser's
    target (measured error: 1 mm).
  - Conditions: on foot, no mod camera (mode 8), map closed, and the call has
    no argument.
  - The VR mod itself calls `GetCameraTransform(1)` to keep the real camera.
* Keyboard/mouse messages that arrive while a frame B is scheduled are held
  back and replayed before the next frame A, so that the game's "just pressed"
  logic sees them on a normal frame.

## 5. Camera

* `SetCameraTransform` puts the camera into "override" mode, but the game then
  hides the tool in hand. The mod keeps the player's normal camera and offsets
  it to the eye: `SetCameraOffsetTransform(local(playerCamera, eye))` +
  `SetCameraFov`. `RequestFirstPerson(false)` is called every tick.
* The offset applies to the player camera **as it will be rendered**. The
  script turns the player (aiming) and moves them (roomscale) during the tick,
  so the offset is computed relative to the predicted camera (aim rotation
  applied, movement added); otherwise the left eye turns by that amount for one
  frame. Measured with `GetCameraTransform()` in `draw()`: error < 0.03° and
  < 3 mm.

## 5 bis. Frame pacing and input in VR

* **Pacing**: while the OpenXR session is running, `xrWaitFrame` paces the
  game, once per stereo pair. The mod therefore presents the PC screen without
  vertical sync (`SyncInterval 0`). `DXGI_PRESENT_ALLOW_TEARING` is also needed
  (the game's swapchain is created with `ALLOW_TEARING`): without this flag,
  the desktop compositor limited the game to the monitor's refresh rate.
  Measured: 12 ms per frame with the CPU and the GPU only one-third busy,
  versus 4 to 6 ms with the flag.
* **Pointer on the virtual screen**: the cursor of the game's menus follows
  `WM_MOUSEMOVE` messages (the game does not import `GetCursorPos`). The mod
  sends them even when another desktop window has the focus, since the player
  is in the headset. The real Windows cursor is only moved when the game is in
  the foreground.
* The controllers only drive the game when the session has input focus (state
  `FOCUSED`: not while the headset's system menu is open).

## 5 ter. Field of view, right-eye camera, pacing, per-eye history

* **Field of view** (`SetCameraFov`, projection built at ≈ RVA 0x5ebcc0):
  `tan(vertical/2) = tan(fov/2) × 9/16`, then
  `tan(horizontal/2) = tan(vertical/2) × width/height`. The parameter is
  therefore a horizontal field of view *for 16:9*. The mod sends
  `fov = 2·atan(tan(vertical/2) × 16/9)`; otherwise, with a nearly square
  image, the rendered view was about 46 % narrower than the one declared to the
  headset (distorted image, no 3D fusion). Verified to the pixel.
* **Render camera**: `Game` (≈ 0x70b20) builds the camera with
  `Camera::setTransform(&cam, &player->camPos(+0x1d0), &player->camRot(+0x1dc))`
  (player = `game+0x60`). In the zero-time frame, the game still applies
  "per-frame" camera effects (height smoothing, landing shakes): the right
  eye's camera drifted by up to 43 cm / 18°. The mod hooks
  `Camera::setTransform` and renders the right eye from the left eye's actual
  camera, offset by the distance between the eyes (verified: 0 px of vertical
  offset in mid-jump).
* **Aiming**: turning the player with `SetPlayerTransformWithPitch` every frame
  also repositioned them (the hand always moves a little): camera and tool
  jerks of up to 39 mm. Pitch goes through `SetPlayerPitch`
  (`entity+0x700`); yaw is written by the DLL into `entity+0x704` (radians;
  local entity = `game[0xc0][game[0x168]]`, offsets read from the code of
  `GetPlayerYaw`). The script sends it in a short `TDVRY <deg>` message.
* **Pacing**: the simulation used to advance according to CPU time, which is
  irregular, while the headset displays at a fixed rate. Each left frame now
  advances the game by exactly the interval between two display times
  predicted by OpenXR (`predictedDisplayTime`); measured camera steps: a
  regular 150 mm per pair instead of 72 to 175 mm.
* **Head bob**: `options.input.headbob` is set to 0 while VR is active.
* **Per-eye temporal history** (TAA, disocclusion mask, linear depth, light /
  reflection / fog denoisers). `Renderer::render` (RVA 0x87f40, a single call
  site) reads all of its history from a few fields: 10 FBOs
  (`+0xa50…+0xb00`), 5 TAA textures (`*(r+0xa30)+0x50…+0x70`), ping-pong
  indices `+0x2210/+0x2214`, previous matrices `+0x22ec` (stable VP) and
  `+0x2258` (view), jitter counter `taa+0`, reset flag `+0x23f1`. The mod
  keeps one set of these per eye and swaps them on entry to `render`. The
  second set is created with the engine's own allocators (FBOs through
  `backend->vtbl[0x138]` with a copy of the 0x38-byte descriptor that starts
  each FBO; textures through `TemporalAa::createResources` 0xa5c90).
  Auto-exposure stays shared. Sharpness measurement (variance of the
  Laplacian): × 2.5 in the distance, × 6 to 7 up close. The full analysis was
  done read-only, then checked by an independent review.
* **Same noise in both eyes** (`[Affichage] stereoSameNoise`, 1 by default).
  All of the engine's screen-space noise is animated by a single number per
  frame, `FrameRnd` = `mubFrameParams.w` of the scene constants
  (`SceneDynamicBuffer` +0x1ac, filled at 0x9e130, called from
  `Renderer::render` +0x75e): `blueNoise()` returns
  frac(blue-noise texel + 0.618 × FrameRnd). It drives the dithered
  transparency of every particle (rain, smoke, fire, the extinguisher: a
  particle pixel is drawn opaque if this value is below its alpha, otherwise it
  is discarded; `gbufferparticles`) and of glass, and the sample offsets of AO,
  the sun, lamps, fog, reflections, water, rain on the ground and their
  denoisers. 0x9e130 takes it from the TAA counter (`taa+0`, `(n + 1) % 8` on
  every `TemporalAa::update` 0xa5950, called at +0x37a, which also derives the
  sub-pixel jitter and the TAA ping-pong from it), except while the game is
  paused (`game+0x1e8`, outside photo mode: frozen). The per-eye history keeps
  one counter per eye, and the two were created one frame apart: the right eye
  always drew the next noise frame. `SameNoiseBegin`: frame B's render starts
  with the counter that frame A's render had (mode 1; mode 0: with the counter
  as it was at the end of frame A, as before, even after switching 1 → 0).
  Result: the same noise frame, the same jitter, the same parity — only if the
  right eye has its own TAA history (`perEyeHistory`; otherwise the shared
  parity would make it read a history one pair older, reprojected with the left
  eye's matrices). The left eye is unchanged, and each eye still steps through
  the 8 noise frames once per pair (TAA unchanged). Limitation: the noise is
  then identical at the same PIXEL of both eyes, not at the same 3D point. With
  `offCenter=1`, the pixel grids of the two eyes are offset
  (W × (tanL0 − tanL1) / (tanL + tanR): 733 px on a Quest 3), and a point at distance z adds f × IPD / z px: the dither
  pattern of rain, smoke and particles therefore still differs between the eyes
  at the same point (binocular rivalry). This is the prerequisite for a dither
  pattern anchored to objects (the right eye's particle shader reading the
  noise where the left eye sees the same point), not that pattern itself.
  Checked after every render (`SameNoiseEnd`, a "stereoSameNoise=… pairs" log
  line every minute). Rain itself is not the cause: its particles are spawned
  at random (`rand`) once per physics step (0xf2af0, called from Scene::update
  0xf2640 inside the step 0x7b810), never in frame B; the per-eye sort
  (0x437df0) only removes what is behind the camera; each particle's noise
  index (`aIndex`) is its slot in the particle array, the same for both eyes.
  Still remaining: the noise angle of the depth of field (`game+0x1f0 % 64`,
  only when depth of field is drawn, `removeDof=0`).

## 6. Aiming and the tool

* Aiming: the player is turned (yaw/pitch, `SetPlayerTransformWithPitch`)
  toward the point hit by the controller's ray; velocity is preserved.
* In first person, the engine ignores `SetToolTransformOverride`. Hook on
  `ToolAnim::update` (RVA 0x452f90 in 2.0.1, found through the
  `override → final` copy: `movups xmm0,[rcx+0A0Ch] ; movups [rcx+844h],xmm0`
  preceded by `cmp byte [rcx+0A38h],0 ; jne`). After the normal computation,
  the final pose becomes `hand × inverse(hold) × inverse(rest) × animatedPose`:
  the tool follows the VR hand and keeps its animations (recoil, sledgehammer
  swing). `this+0x58` = player.
  * `animatedPose` = `ToolAnim+0x68` (the animator's pose before the
    desktop-mode offsets: `SetToolOffset`, the offset that depends on
    `options.gfx.fov`, the lowering `+0x8a8`). Found through
    `lea rdi,[rsi+68h] … movups xmm0,[rdi] ; movups [rcx+844h],xmm0`.
  * `rest` = the `fp_action` location of the tool's prefab
    (`data/prefab/tool/<name>.xml`; Teardown's Euler angles = Ry·Rz·Rx in
    degrees); the sledgehammer has none: rest position (0.2, −0.45, −0.35),
    rotation kept. `hold` = the `tp_action_rh` location (where the hand holds
    the tool).
  * Tool name: `*(ToolAnim+0x30)+0x18` (inline string); the offsets were read
    in the function called right after `mov rcx,[rcx+58h]` (name → number).
  * Mod tools ("script tools", tool number 0): `ToolAnim::update` takes another
    branch (0x453afc). In the flat-screen view, the tool's pose is the
    transform that the script gives every frame with `SetToolTransform` (eye
    frame, identity by default), smoothed (rate 60/s, animator 0x451de0); the
    engine reads no `fp_action` location (the "version 2" tools read it
    themselves with `GetToolLocationLocalTransform` in
    `data/script/toolanimation.lua`). The voxels of a `.vox` tool are placed in
    the body according to the MagicaVoxel scene (5 cm voxels). With the
    override flag set, the branch simply copies the override.
  * `SetToolTransform` (binding 0x497330) and `SetToolTransformOverride`
    (0x497480) write the SAME field `player+0xa0c`; only the override sets
    `+0xa38` (the sway of `SetToolTransform` goes to `+0xa34`). These are reset
    to zero every frame by 0xc4460 (called by `Game::update` before `ToolAnim`).
    The order in which the scripts ran therefore decided who won. The DLL hooks
    the `SetToolTransform` binding (pattern
    `movups [rax+0A0Ch]… movss [rax+0A34h],xmm6`): the mod's pose is stored
    separately and the VR hand is written back into the field.
  * Hand posed by the mod (`SetToolHandPoseLocalTransform`, 0x496fa0):
    `+0xa39` = set this frame, right hand `+0xa70` (tool frame), used if
    `+0xa8c`; left `+0xa90` / `+0xaac`. With the override flag set, the engine
    does not rewrite them.
  * Placement in VR: tool = hand x H⁻¹ x screen pose (smoothed by the DLL). H
    (computed by the Lua, once per tool) = `fp_action` x `tp_action_rh` if the
    tool has them; otherwise the point of the tool held by the mod's hand (if
    it is less than 10 cm from the tool's shapes); otherwise the point of the
    tool that sits, in the flat-screen view, at the median hand position of
    the game's tools (0.30, -0.40, -0.45), brought back inside the bounding box
    of the tool's shapes.
  * Tool name (`*(ToolAnim+0x30)+0x18`): a 32-byte engine string (copy
    constructor 0x507500), with the characters stored inline as long as byte
    0x1f is 0. Mod identifiers often contain capital letters ("GPD",
    "BD_38Revolver", "elboydo_Thermite_Cannon"): the DLL reads any printable
    name of at most 31 characters; only lower-case names are looked up in
    `data/prefab/tool/`.
  * Size of mod tools. Mods pose their tool for the flat screen, 0.3 to 3.4 m
    in front of the eye, and model it accordingly: 1 to 4.5 m long. Held in the
    hand, they were huge. The DLL scales each mod tool down (Hook_ToolAnim,
    "kind 2" case) by a factor s, computed once per tool name from its unscaled
    shapes and its screen pose:
    s = 0.45 m (eye → VR hand) / screen distance of the box centre, clamped to
    [0.25, 1]. The factor is lowered further so that the tool is at most 1.1 m
    long (the game's shotgun is 1.1 m long, the sledgehammer 1.2 m). It is
    multiplied by the `modToolScale` setting.
    - Tool body: `player+0x820`, u16 entity type at `+8` (1 = body,
      2 = shape). First child at `+0x28`, next sibling at `+0x18`.
    - Shape: local transform at `+0x44` (pos, quat). Voxel data at `+0xf0`:
      sizes (3 ints) at `+0`, voxel size (float) at `+0x20`.
    - The voxels belong to each shape (they are destructible). Changing the
      voxel size therefore only affects the tool. The positions of the shapes
      within the body are multiplied by the same s.
    - A position or a size that the mod rewrites becomes the new original.
    - `SetShapeLocalTransform`: the binding (+0x496410, found by its name)
      reads the handle (ArgInt +0x631b80) and finds the shape in the scene's
      entity tables (`game+0x50`; positive handles: array `+0xa20`, count
      `+0xa18`; negative ones: `+0x210`, `+0x208`; the tool body and its shapes
      have negative handles). It then calls the engine function (+0x496550:
      `rcx` = script context, `rdx` = shape, `r8` = transform; it writes
      `shape+0x44`; `SetProperty` also calls it, +0x493cfa). That function is
      the one that is hooked (`Hook_ShapeLocalImpl`): after the call, only that
      shape is rescaled, if its parent (`entity+0x10`) is the body of the held
      tool. Without this hook, the parts animated by the mod (the barrels of
      the DOOM Chaingun) were at their original size during the script tick.
      The Lua's physics copy then measured 2.33 m instead of 0.94 m. Earlier,
      the binding itself was hooked and the whole tool was rescaled on every
      call: too slow for the revolver, which moves about 125 of its 1028 shapes
      every tick. If the function is not recognised, that is still what
      happens, but only for tools with at most 128 shapes.
    - All the shapes of the body are walked (up to 4096). The old limit of 64
      left 964 of the 1028 shapes of THE EVERYTHING REVOLVER (LnL: 266 + 762
      shapes) at their screen size: a 1.3 m revolver right against your face.
      Voxels smaller than 1 mm (21 engravings on the revolver, down to
      0.35 mm) are scaled down too.
    - Measurement: taken when the tool's shape count, its length and its
      screen distance (within 3 %) have not changed for 0.3 s (at most 3 s; it
      is the distance that is compared and not the factor, because the factor
      stays at its bound while a distant tool comes closer). Mods build their
      tool over several frames: LnL replaces the placeholder shape given by
      the game, then adds its shapes at `Transform(1/0)` before placing them.
      Unfinished shapes, and shapes more than 20 m from the body (hidden by
      their mod), do not count.
    - In the meantime, the tool is scaled down according to its current look:
      the factor follows the shape count, but follows the look only when the
      look changes it by more than 10 % (a tool that sways keeps one size;
      each new size makes the Lua redo the hold and the physics copy). This
      factor is kept at measurement time if the measurement confirms it
      (within 3 %, or 10 % for a tool that has never become stable).
    - An already measured tool whose shape count changes a lot (by more than
      max(8, half): a different look) is measured again, keeping its factor in
      the meantime; a few shapes more or fewer (a lightning bolt) change
      nothing.
    - Measured on the simulated headset (before → after):

      | Tool | Before | After |
      |---|---|---|
      | revolver (64 shapes out of 1028) | 1.2 m | 0.43 m |
      | DOOM Chaingun | 4.45 m | 0.94 m |
      | GLARE | 2.3 m | 0.92 m |
      | GPD | 2.6 m | 0.75 m |
      | physgun | 0.93 m | 0.39 m |
      | magnetizer | 0.8 m | 0.31 m |
      | Thermite | 2.15 m | 0.73 m |
      | Vacuum | 2.05 m | 1.1 m |

    - The Lua receives s (field 101 of the reply). It applies s to the
      `fp_action` / `tp_action_rh` locations, which are not scaled. It redoes
      the hold and the physics copy when s changes. The hand posed by the mod
      (fields 98-100) is already brought onto the scaled tool by the DLL
      (m + s (hand − m)).
    - The muzzle, pivot of the scaling (review 12). The mods' shots, lightning
      bolts, beams and lights started from the unscaled tool, up to 1 m in
      front of the drawn barrel. Survey of the installed mods (starting point,
      in the frame of the tool body):

      | Mod | Computation | Point |
      |---|---|---|
      | DOOM Chaingun | UMF bone `mainbarrelend` (body × bone, in Lua) | (0.03, 0.28, −1.55) |
      | physgun | UMF bone `nozzle`, `arm*_e` | (0, 0, −0.48) |
      | revolver (BD_38Revolver) | LnL bone `main.shoot` | (−0.23, −1.24, −0.62) |
      | GLARE | LnL bones `body.barrelend`, `body.ball`; shot fired from the camera | |
      | GPD | body × (0.055, 0.385, −1.35); beam from the body origin | |
      | Omnicutter | body × (0.18, −0.18, −0.58 to −0.8) | |
      | Vacuum | body × (0.355, −0.425, −2) | |
      | Z-Glock (Robot Vehicles), Meteor | body × (0.3, −0.45, −2.4), bullet simulated in Lua | |
      | Thermite Cannon | shape 1 (world) × (0.25, 1.7, 0.4) | |
      | Portal Gun, Ultra Grav Gun, Magnetizer, AC130 | camera (VR aim) | not affected |
      | Total Overdose | `GetPlayerEyeTransform` ± 0.25 m sideways, 0.5 m forward | dual weapons ([6 octies](#6-octies-dual-weapons-total-overdose)) |

      Most bullets are simulated in Lua (position advanced every tick):
      correcting the positions in `SpawnParticle`, `PointLight`... would only
      work for the first frame.
    - Each tool is therefore scaled around its muzzle m (DLL `ToolMuzzle`):
      the centre of the solid voxels that are furthest forward in the mod's
      screen view (−Z of the eye through its pose), within a slice of
      max(2.5 voxels, 3 % of the depth), 20 cm at most, placed on their front
      face. One barrel gives the centre of its muzzle, three barrels give their
      midpoint. Shapes are drawn at m + s (p − m): any point that the mod
      computes in the frame of its unscaled tool (body × offset, bones,
      locations) lands on the drawn tool at the muzzle; elsewhere the error
      equals (1 − s) × its distance from the muzzle.
      m is measured in Hook_ToolAnim right after ScaleModTool, at the first
      measurement of the tool's size and at every new measurement
      (`ModToolPivot`), then the tool is scaled around it in the same frame.
    - Pose: the body is placed at hand × (q, s·p − (1 − s)·q·m), (q, p) being
      the smoothed screen pose, hence body × (m + s (x − m)) = s (p + q·x): the
      flat-screen image scaled by s toward the eye, animations included. A
      recoil is at the weapon's scale (before, its translations stayed at full
      size). The Lua receives this pose (fields 90-96) and derives the hold
      from it: the image seen does not depend on m. It also receives m
      ("PIV x y z", after the numbered fields): the physics copy and the
      collision spheres, measured in the body frame, are rebuilt when m
      changes; the `tp_action_rh` location is brought to m + s (x − m).
    - Consequence: the origin of the tool body is |m| behind the drawn barrel,
      often behind the head (1 to 2 m). The Lua checks that were made at the
      body origin (physics copy: offset, jump, hand speed; collisions: jump)
      are made at the hand instead, and the allowed body-to-hand offset goes
      from 1.5 to 6 m. A beam or a sound placed at the body origin (the GPD's
      beam) starts behind the player: with your back to a wall less than about
      one metre away, the GPD's beam hits that wall.
    - `GetShapeLocalTransform` (+0x47cdd0) and `GetShapeWorldTransform`
      (+0x47da60) are hooked: for mod scripts, a shape of the held tool is
      reported where the mod put it (unscaled; the shape is moved there for the
      duration of the call). A mod that reads one part back to place the
      others (Portal Gun, the Thermite's reload, the AC130's radio) no longer
      scales them twice; the Thermite's barrel (part × offset) starts from the
      muzzle. The VR mod passes a 2nd argument (`GetShapeLocalTransform(sh, 1)`)
      to measure the tool as it is drawn.
      - Handle read by `ArgInt(args, 0, 0)` (+0x631b80; `cmp edx,[rcx+8]`:
        argument count at `args+8`). Entity → handle at `+0xc` (negative for
        client entities, which include the tool's shapes). The tool's shapes
        are listed by handle once per frame (scripts read many shapes, rarely
        the tool's).
      - GetShapeWorldTransform composes the body pose with `+0x44`, either
        directly (0x4de6c0) or interpolated (0x4de540). In the interpolated
        path, a dynamic body (`+0x138`), a parent of type 0xc or a shape with
        flag 0x400 (`+0xa`) go through the shape's history (`+0x60`, 0x322c0,
        which writes into it); the body of a held tool is in none of these
        cases (re-read on the shotgun: flags 0, `+0x138` = 0; still to be
        checked on a mod tool). GetBodyTransform (0x475280), for its part,
        reads `+0x40` or an interpolation history (0x2eb90, which writes into
        the body): we do not touch it.
    - Voxels of a shape: data `+0xf0` → sizes (3 ints) `+0`, bytes `+0x10`
      (palette index, 0 = empty; index (z·sy + y)·sx + x, as in
      `GetShapeMaterialAtIndex` 0x47d130), voxel size `+0x20`.
    - Log: "mod tool 'X': muzzle ..." at measurement time; with debugLua, 1 s
      later, "Engine diag: mod tool 'X' ...": the muzzle, the front of the
      drawn tool (the same point, ~0 mm apart) and their positions in the
      world.
* Why the gun aimed upward: the Lua passed OpenXR's "grip" pose. On Virtual
  Desktop, `aim = grip × {(0, −0.067, −0.074), Rx(−60°)}`: the −Z axis of the
  grip pose is 60° above the aim ray. Weapons now take the rotation of the
  "aim" pose (the position stays the hand's).
* In the render frame (override flag), the tool is placed in
  `{interpolatedFeet + 1.7·up, q(+0x70c)·Ry(yaw +0x704)} × (+0x844)`, without
  pitch: it is the same frame as the Lua's.
* Sledgehammer strike by gesture: the Lua measures the speed of the dominant
  hand's grip pose (room frame); above 2.8 m/s downward or forward, it sends
  `TDVRS` and the DLL presses "use tool" for 0.12 s.
* Sledgehammer strike with the trigger (`Sledge::update` 0x450690; `rcx` = the
  sledgehammer, `xmm1` = dt, `r8` = the player's inputs), studied so that a
  short, light press gives a full strike with the game's reach (r18).
  - Start: the tool is usable (`player+0xa04`, recomputed every frame by
    0xc4460: nothing grabbed, alive, not in a vehicle...), action 9 "use tool"
    PRESSED in this frame (`inputs+0xdad`; 8-byte actions starting at
    `+0xd64`: +0 held down, +1 pressed, +2 released, the ones read by
    InputDown / InputPressed / InputReleased 0x4b51b0 / 0x4b54a0 / 0x4b56a0)
    and the cooldown `+0x138` has elapsed. It then sets the strike timer
    `+0x130` = 0.01, "hit" `+0x134` = 0, the cooldown `+0x138` = 0.3 s, a
    random draw `+0x140` (animation variant), the sound and the vibration
    (local player).
  - The timer advances by 1.8/s (1.1/s after a strike that hit something).
    When it passes 0.47, 0.256 s after the press, the strike looks for what
    it hits, only once: a 0.3 m sphere (0x193910, `xmm2` = 3.0 m,
    `xmm3` = 0.3) cast from the player's camera (`player+0x640`, 0xb9300;
    third-person view: `+0x2cd4`, moved forward by `+0x2cb0`) along its
    forward direction, with no filter (mask 0xff: all layers, including the
    Lua's physics copy on layer 128). Hit point P = origin +
    forward·distance − 0.3·normal; then a hole 0x138260 (P, 1.0, 0, 0), sound
    and debris according to the material (0xd9260), and a push on bodies
    within 1 m (0xd4180, 0x2d560). The timer goes back to 0 after 1.
  - So the button only counts at the moment it is pressed. In VR, a trigger
    released before 0.256 s gave its collisions back to the physics copy
    ([6 sexies](#6-sexies-physical-tool-lua)), which follows the strike in front of the camera: the sphere
    hit the copy (unbreakable) and the targeted object stayed intact (the
    swing was visible but nothing broke). Fix: `main.lua` keeps the copy, and
    the free hand when it is less than 0.4 m from the axis, as ghosts
    (no collision) for 0.4 s of game time after each press (`phys.strikeT`,
    `updateTool`); with the sledgehammer, the DLL presses from 20 % of trigger
    travel (`sledgeTrigger`) for at least 0.15 s (`SledgeUse`, setting
    `sledgeTap`).

## 6 octies. Dual weapons (Total Overdose)

Requested feature: with the "Total Overdose" weapon (two pistols), one pistol
in each hand, each fired by its own trigger, like two real guns. Setting
`dualWield` (Controls tab, "Dual wield", checked by
default). Nothing is changed in the mod's files or in the game's Lua sandbox.

* The mod (Workshop 3449723355, read without modifying anything):
  - Tools `totaloverdose` (vox/tool.xml) and `totaloverdose2`
    (tool_silenced.xml, SEU cheat). A single body: `tool.vox` (4 cm voxels,
    16 voxels) contains BOTH pistols, two separate pieces of 8 voxels (y = 0
    and y = 11 in MagicaVoxel, side by side after the node's rotation, ~44 cm
    apart), plus two markers `totalOverdosegun1/2` (guns.vox, 4 voxels of
    1 cm); the silenced variant adds two silencers (20 voxels of 3 cm). No
    bones, no SetShapeLocalTransform.
  - Screen pose: `SetToolTransform(Transform(Vec(-0.015, -0.26, -0.25),
    QuatEuler(-10, 0, 0)))`, `QuatEuler(30, 0, 0)` on the only frame where
    `InputPressed("usetool")` (the recoil). Right hand (SetToolHandPose...) at
    (0.24, 0.13, −0.1) for 3 s after the weapon is drawn and after each shot,
    otherwise (0.25, 0.27, −0.28).
  - Firing (main.lua 218-495): `if canusetool and InputPressed("usetool")`,
    tool with no grabbed object; `cannon1 = GetPlayerEyeTransform()`,
    `cannon2 = GetPlayerEyeTransform()`, then ONE shot, from one side or the
    other depending on `shoot1` (hidden alternation): muzzle = eye + rot ×
    (±0.25, 0, −0.5) + world (0, −0.05 + 0.05 × crouch, 0), direction = −Z of
    rot, `Shoot(muzzle, dir, "bullet", 1, 300)`. Light (PointLight) at the
    eye, sound at the player, `ShakeCamera(0.5)`. So the bullets never left
    from a drawn pistol.
  - Right grip ("grab"): the slow-motion dodge (ragdoll, vehicle, third-person
    camera); E: dash; mouse wheel: parachute.
* `GetPlayerEyeTransform` (+0x4791b0, `mov [rsp+8],rbx ; push rdi ; sub rsp,60h`):
  `ArgInt(args, 0, 0)` (player), player lookup (+0x4a4360; 0 = local player:
  `game+0xc0[game+0x168]`), eye at `player+0x640` (v2 scripts: `+0x678`),
  returned as a {pos, rot} table (+0x630b60, +0x630cc0) like
  `GetCameraTransform`. `GetPlayerCrouch` (+0x4790f0): same player,
  `movss xmm1,[rax+768h]`.
* Erasing voxels of the tool: `DrawShapeBox` (+0x472130) accepts positive AND
  negative handles (the tool's handles, client entities), and only refuses a
  server shape from a client script (`ctx+0x2f8 == 2` and handle ≥ 0); then
  (+0x49ed50) it writes the brush index (`ctx+0x2712`, `SetBrush`;
  0 = empty) into the shape's bytes (`+0xf0` → `+0x10`, box clamped to the
  shape), and marks the shape as modified. Verified in the game's code: nothing
  prevents doing it on the held tool. The Lua reads back every erased voxel
  (`GetShapeMaterialAtIndex`): if one is not empty, everything is restored and
  nothing changes (log "erasing the other gun had no effect").
* Split (main.lua, `tdvrDualTick`): the tool's solid voxels as they are drawn
  (`GetShapeLocalTransform(sh, 1)`), placed in the mod's screen view (the
  DLL's scaled screen pose); the largest gap from left to right (eye x), at
  least 4 cm wide, with at least a quarter of the volume on each side. The
  side that is kept is the one with the mod's hand; the voxels on the other
  side are erased and saved (shape, x, y, z, material: restored when dual
  wield stops on the same tool). Only the voxel contents change: the size and
  the muzzle measured by the DLL (shape boxes) do not move.
* Hold: the grip of the kept pistol (the rear voxels, 30 % of its length, and
  the bottom ones, 60 % of its height; pieces smaller than 10 % of the
  volume - the 1 cm markers - do not count) becomes H for the dominant hand
  AND for the copy, as long as dual wield is active (the mod's hand, posed for
  the character's arm, was ~15 cm from the pistol; its first pose, during the
  3 s after the weapon is drawn, is not the same). Muzzle: centre of the
  furthest-forward voxels, on their front face.
* Copy in the other hand: a static body tagged `tdvr_dual nomerge`, one shape
  per kept shape (`MOD/proxy.vox` at the drawn voxel size,
  `CopyShapeContent`, `CopyShapePalette`, same local transform, filter 0/0,
  no collision), placed at hand × H⁻¹ × resting screen pose (the one of a
  frame with no shot from the dominant gun for 0.25 s), on the drawn hand
  (tdvrOffArmTick: the physical hand against an obstacle), with the tool's
  offset mirrored. Its own recoil: +20° on each shot of this gun,
  ×exp(−60 dt). Re-aligned at render time like the arm (`TDVRV` body,
  `TDVRX` eye − body; `OffArmLateFix`, one more group, 8 shapes at most).
  Hidden (and its trigger has no effect) when this hand holds an object or
  its controller is no longer tracked.
* Each shot leaves from its own gun (DLL, `DualEye`):
  - Messages (≤ 30 characters): `TDVRG <gun> x y z` = the muzzle in the frame
    of the gun's body (1 = tool, 0 = copy); `TDVRK <gun> x y z` = the point
    aimed at by this hand's laser minus the muzzle (world); the DLL turns it
    into a world point by reading the body (`+0x40`, the memory that
    GetBodyTransform reads) within the same call. Neither the direction nor
    the muzzle of the left gun is expressed in the right gun's frame: its
    recoil (30° for one frame, smoothed) does not move them.
  - Clicks (input.cpp, `DualClicks`): each trigger press = ONE left click,
    one after the other (queue): press; when the window procedure sees our
    message reach the game (the WM_LBUTTONDOWN at our position or, without
    legacy messages, our fake left-button WM_INPUT) and a game update has
    started: release; when the release has arrived and an update has started:
    next click. About one shot per two stereo pairs (~22 per second at
    90 Hz); both triggers together = two shots ~45 ms apart. No left click
    from the left trigger while the right grip holds an object (it would
    throw it): a press of the left gun that is still pending is also dropped
    when the game grabs an object or when the copy is hidden. The timeouts
    (0.3 s per step, 0.5 s of waiting) count presents of at most 50 ms each (a
    hitch does not interrupt a click), and a step whose message may still be
    held back for the frozen frame (`g_deferred`) does not expire. If dual
    wield starts with the left button held down (right trigger held just
    before), the button is released immediately, even with no pending press.
  - Click in flight (no exact frame matching): the DLL knows which gun the
    click belongs to as soon as the click is posted; the first calls to
    `GetPlayerEyeTransform` from the tool's script (the one that calls
    SetToolTransform), without argument, in the first normal (not frozen)
    update after the click has arrived, are its shot: TO (Total Overdose)
    fires in `tick` on `InputPressed`, which is the update that sees the
    press (counter `g_normalUpdates`). A later call (its dash, the E key in
    dash_3, its `Spawn` of parkour.xml) keeps the real eye; the log says
    "late eye" if such a call comes in the next two normal updates, and
    "click N reached the game, but no eye ..." if the first one asked for no
    eye at all (no shot: object held, `canusetool` false). Call k
    (k = 1, 2): rot = R (−Z = d, +Y as close as possible to the gun's up
    direction), pos = M − R × (±0.25, 0, −0.5) − (0, −0.05 + 0.05 × crouch,
    0), with M the gun's muzzle (world, body read at that moment), d pointing
    to its aimed point, and crouch re-read the way GetPlayerCrouch reads it:
    whatever `shoot1` is, the bullet leaves M toward the target. The table is
    built by the original GetCameraTransform (its trampoline) from the camera
    fields, which are written for the duration of the call and then restored
    (like `ShapeAsMade`). The following calls (a dash in the same tick) keep
    the real eye. A shot that asks for fewer than 2 eyes is noted (log);
    3 in a row: the profile is turned off until restart (a single one may be
    another call of the mod mistaken for a shot).
  - During a click of the left gun, the mod's recoil pose (different from the
    pose of frames without a shot) is replaced by the latter
    (Hook_SetToolTransform): the right gun does not recoil for a shot of the
    left one.
  - Reply `" DUA <allowed> <shots> <gun> <ready>"`: the Lua vibrates the hand
    that fired and makes the copy recoil.
* Controls meanwhile (input.cpp): right trigger = right gun, left trigger =
  left gun, left grip = crouch (while it is squeezed), right grip = the mod's
  dodge (left-handed mode: the grips are swapped, each trigger still fires
  the gun in its own hand). A trigger held when dual wield starts or stops
  counts as released.
* Lua state: the global table `tdvrDual` (tick has no room left for an
  upvalue) carries `nonserializable = true`. The serializer of the scripts'
  global variables for quicksaves (+0x636cd0, one entry: number or string
  key; boolean, number, string or table value; functions skipped) first reads,
  for a table, its `nonserializable` field (+0x636d41, the only reference to
  the string "nonserializable") and skips the table if the field is true.
  Without it, a reload (F9) would write the saved state (handles, erased
  voxels, copy, hold) back into the fresh table. `init()` also resets these
  fields before `physCleanup` (orphaned copy deleted). When VR stops (headset
  removed, Quest menu, `onDeactivate`), the voxels are restored and the copy
  is deleted; on resume, a new split is made.
* Fallback (plan C): if erasing had no effect, hide the whole tool and draw
  two copies. Not implemented: the game's code shows that DrawShapeBox writes the
  tool's voxels; if the read-back fails anyway, both pistols stay in the
  dominant hand, with the usual controls.
* Limits: one shot per game update (the mod's limit); the shot's light stays
  at the "eye" that was given, ~0.5 m behind the muzzle; the dodge, the dash,
  the parachute and the TLO bullet (crouch + fire) remain the mod's own;
  without the physical tool, the tool's collision spheres still cover the
  place of the erased pistol.
* Log: "Engine: dual: GetPlayerEyeTransform hooked", "Lua: dual: ...
  kept the right gun ...", "Lua: dual: copy body ...", "Input: dual wield on",
  "Input: dual: click N for the ... hand's gun", "Engine: dual: shot #N by the
  ... hand's gun ..." (the first one; all of them with debugLua), "Engine: the
  off hand's gun (dual wield) is drawn on the camera drawn"; signs of a
  problem: "Engine: dual: click N reached the game, but no eye ...",
  "Engine: dual: late eye ..." (the game would read the press one update
  later: an unexpected case), "Input: dual: N press(es) of the off hand's gun
  dropped", "Lua: dual: stopped (VR stopped)".

## 6 ter. Temporal anti-aliasing (blur in the distance)

* In D3D12, the game's TAA is the compute shader `temporalaacs` (record #274
  of `data/shader_cache.cache`, precompiled DXBC, 0x2b38 bytes): the game
  compiles no shaders. It reads the previous frame back bilinearly at
  `uv - motion` and keeps 95 % of it (90 % if the history was clamped),
  dropping to 0.5 only at a motion of `width/64` pixels per frame. In a
  headset the head always moves by a fraction of a pixel: the history is
  re-filtered every frame, fine (distant) detail disappears, and it comes back
  when you turn your head quickly.
* The engine's sharpening pass never runs in 2.0.1 (flag `exe+0x1c6fdf1`
  always false); the history is the raw TAA output (R11G11B10).
* Replacement: `shaders/temporalaacs_vr.hlsl` (exact reconstruction, RTS0 and
  PSV0 identical byte for byte) with a Catmull-Rom fetch of the history;
  compiled with the game's own shader compiler (the `dxcompiler.dll` of the
  game folder) into `src/generated/`. Not published: derived from the game's
  shaders (see the CMake option `TDVR_GAME_SHADERS`; without it, the game's own
  TAA shader is kept). At every `Dispatch`, the
  engine binds the pipeline stored in its object `*(TemporalAa+0x40)` (name
  `temporalaacs` at +0x10, `ID3D12PipelineState*` at +0x60, root signature
  at +0x68): the DLL creates its own pipeline with the same signature and
  swaps the pointer before `Renderer::render` (`src/taa_vr.cpp`); the
  original is kept.
* Measurement (simulated headset, slight head tremor, variance of the
  Laplacian): distance 278 → 466 ("Sharp") → 571
  ("Very sharp"; blend 0.9/0.88 and the current frame
  sampled with Catmull-Rom). Comparison: `docs/netete_comparaison.png`.
* Slight remaining blur on distant forests: it is not a level of detail (the
  `gbuffervox` ray march starts at mip 2 to skip empty space but always ends
  at mip 0), nor depth of field (`options.gfx.dof` is 0, `SetCameraDof(0, 0)`
  has no measurable effect). It is the temporal averaging of detail finer
  than a pixel, made worse by the bilinear resampling of the current frame
  (`uv + jitter`).
* Over-sharpening of the old "Very sharp" (player feedback): the negative
  lobes of the Catmull-Rom (on the history re-read every frame, and on the
  current frame) create a dark fringe along silhouettes against the sky
  (pixels < 12/255 at the edge of the sky: game 0, Sharp 37, old Very
  sharp 61).
* Reference measurement (capture and measurement tools of the simulated
  headset): the
  same view rendered at double resolution (mode without resampling) and then
  downscaled serves as ground truth; each mode is aligned to sub-pixel
  precision region by region and compared: detail (high-frequency energy /
  reference) and halo (overshoot beyond the reference's 3x3 min/max on strong
  edges). Head with a slight tremor, 4 captures:
  - "Sharp": detail 0.40 / 0.39 / 0.40 (forest / buildings / middle
    distance), forest halo 1.13;
  - old "Very sharp": 0.66 / 0.67 / 0.65, halo 2.40;
  - "Natural" (the default): 0.59 / 0.66 / 0.67, halo
    0.98;
  - new "Very sharp": 0.70 / 0.86 / 0.86, halo 1.50.
* "Natural" = `SHARP_CURRENT=2` (each pixel keeps its own sample, without
  resampling) + `HIST_CLAMP=1` (Catmull-Rom of the history clamped to the
  min/max of its 4 texels: sharpness without overshoot) + `GLASS_FIX=1`
  (glass and particles that are dithered on every other pixel are blended
  50/50 with their 4 neighbours, otherwise a checkerboard remains); blend
  0.95/0.9. "Very sharp" = the same thing with 0.93/0.9; "Maximum" is without
  `HIST_CLAMP`, 0.9/0.88.
* Mode 9 (.ini file only): the shader is loaded from `TeardownVR_taa_test.cso`
  next to the game, to compare variants without recompiling the DLL.

* Object motion in the right eye: each shape keeps its previous render
  transform (+0x158, after the current one at +0x13c) for the motion vectors;
  `ShapeInterpolationJob` (0xd5a50) rewrites it every frame, so in the right
  eye's "zero time" frame it equals the current one. The DLL snapshots +0x158
  of the moving shapes (`flags +0xc8 & 0x60`) before the left eye is rendered
  and puts the values back before the right eye is rendered (shape list:
  `*(scene+0x238)`, count at +0, array at +8; scene = `*(game+0x50)`).
* Light halo: `SetPostProcessingProperty("bloom", 0)` removes the bloom that
  veiled the distance (+23 % measured distant sharpness). The haze
  (`fogParams`, `fogscale`, `sunFogScale`) is indeed removed too, but it made
  little difference.

* Off-centre projection per eye: `0x5ebcc0` (the only perspective
  constructor) resets m8/m9 to 0; everything else (the copy `Game+0x240`,
  `Renderer::render`, the stable and jittered VP, culling 0x1a2160/0x1a3f20
  in clip space, the shaders' rays through
  `mubProjectionData = (1/m0, 1/m5, m8/m0, m9/m5)`, the UI) starts from the
  full matrix, and the TAA jitter is added to m8/m9. The camera hook
  therefore writes m0 = 2/(tl+tr), m8 = (tr−tl)/(tl+tr), m5 = 2/(tu+td),
  m9 = (tu−td)/(tu+td) for each eye (the positive tangents of the OpenXR
  field of view: l/r/u/d = left/right/up/down). Verified to the pixel on the
  simulated headset (lines of the field-of-view test).

* Solid tool: every frame, the tool's spheres (derived from its shapes) are
  swept from the previously displayed pose toward the hand; at the first
  contact the tool stops 1 cm from the surface and slides along it. When it is
  already in contact, it can move back out but not sink in. Contact with a
  movable object: an impulse so that the object follows the hand (at most
  1500 N). Caution: right after a tool change, the tool's `GetBodyTransform`
  has a null rotation.

## 6 ter bis. Particles: the same dither pattern in both eyes (stereoParticles)

* The particle shader `gbufferparticles` (2 permutations: 0, mesh built by
  the CPU; 1, "GenerateMesh=1", mesh built by the GPU from
  `ParticleMeshDataBuffer`) draws each particle as opaque into the G-buffer
  with dithered transparency: a pixel is kept if
  `frac(blueNoise + 0.618 × FrameRnd) < alpha − 0.01`, otherwise it is
  discarded; the TAA blends the frames together. The noise texel is taken at
  the screen pixel (`gl_FragCoord / 512`, plus `alpha2 × index` per
  particle). The same point of a smoke cloud is ~730 px apart in the two
  images (off-centre fields of view, plus its disparity): each eye saw a
  different dither pattern (binocular rivalry, eye strain).
  `stereoSameNoise` (the same FrameRnd) is necessary, but not sufficient.
* The game compiles no shaders (DXIL blobs from `data/shader_cache.cache`).
  Programs are created once, when rendering starts: loader 0x93f00 →
  `createGraphicsProgram` (vtable +0x180 = 0x5afd50) → `createGfxPSO`
  0x5ae850, `bool (backend, desc, layout, blobs, rs, out)`: the D3D12
  description is built from `desc` (shader name +0x70, permutation +0x90),
  VS = blobs +0x20 / +0x28, PS = +0x30 / +0x38, `CreateGraphicsPipelineState`
  (backend +0xe38, vtable +0x50). Program object (0x80 bytes): pipeline +0x70
  (set at every draw if it differs from the last one, 0x5b6c40), root
  signature +0x78. Renderer: particle programs +0xdc0 / +0xdc8 (CPU),
  +0xdd0 / +0xdd8 (GPU); +0x9a7 selects the second program of each pair.
* teardown.exe's code is still encrypted (Steam `.bind` stub) when
  dsound.dll loads: the `createGfxPSO` hook is installed when the first DXGI
  factory is created (the engine creates one before its device, 0x5b5104,
  hence before its shaders), `particles_vr.cpp` `ParticlesVrEarly`.
* `shaders/gbufferparticles_vr.hlsl` (not published: derived from the game's
  shaders; see the CMake option `TDVR_GAME_SHADERS`): both permutations
  rebuilt from their DXIL (ISG1, OSG1, PSV0, RTS0 identical byte for byte; verified on the GPU
  against the game's blobs, every bit of the 4 outputs, all FrameRnd values
  and the three modes of the particle system). The only change: the game's
  noise formula is evaluated at the centre of the LEFT-eye pixel that shows
  the same point - this pixel shifted by `(cx / w + bx, by)` (UV units, w =
  clip depth in the right eye), snapped to the pixel grid. It is written as
  `mad(alpha2, index, pixel × pixelSize)`, the way the GPU evaluates the
  game's own formula (fused multiply-add): the left eye (offset 0) is the
  game's image bit for bit, and the right eye reads exactly the left eye's
  texel, even for large particle indices (where the game's noise coordinate
  is only accurate to several texels). The lighting checkerboard (normal
  flipped on every other pixel, `mod(x + y, 2)`) is taken at the same
  left-eye pixel (round 2, below).
* The offset travels in the 12 padding bytes of the scene constants:
  `SceneDynamicBuffer` is 0x1f4 bytes, its buffer and its view are 0x200
  (constant buffers rounded up to 0x100, 0x5a9086; view 0x5df000).
  `updateBuffer` (vtable +0x128 = 0x5b9e00) is hooked for the scene-constants
  call only (0x9e130, return address +0x9e4e3): 0x200 bytes uploaded; 0 in
  frame A; in frame B, the reprojection toward the left eye, fitted from the
  `mubVpMatrix` (with jitter) of frames A and B of the pair and verified over
  the whole field of view from 0.4 to 300 m.
* Twins: for a `gbufferparticles` whose PS is the 2.0.1 blob (container
  hash), `createGfxPSO` is called again with our PS: the same pipeline state
  (blend, raster, depth, formats, input layout, root signature). Before each
  `Renderer::render`, the program object receives either the VR pipeline or
  the game's (as for the TAA): the setting takes effect immediately.
* Log: "Particles: hooks ready before the renderer starts ...", "Particles:
  VR twin of the pipeline gbufferparticles p0 ...", "Particles:
  stereo-anchored dither and front/back lighting on ...", "Particles:
  stereo-anchored lighting noise on (ambientdirectionallight ...)", and every
  minute "Particles: stereoParticles=1: N stereo pairs, the right eye took the
  left eye's noise in N (...; the lighting's in N) ...".
* Round 2 (in-headset feedback: better, but a slight discomfort remained,
  sometimes in the right eye, sometimes in the left eye). What was still tied
  to each eye's own pixels:
  * the particles' lighting checkerboard: every other pixel has its normal
    flipped (`mod(x + y, 2) == 0`, lit from the back of the sphere). With the
    dither points anchored, a point's parity in the right eye = the left
    eye's parity XOR the parity of the rounded disparity: for one particle in
    two (depending on its depth), each point was lit from the front in one
    eye and from the back in the other (rivalry, which matches the discomfort
    moving between the right and the left eye).
    `gbufferparticles_vr.hlsl` takes the parity of the left-eye pixel
    (`noisePixel`, = `position.xy` in the left eye: bit for bit).
  * the noise of the `ambientdirectionallight` lighting (cosine ambient
    occlusion rays - not screen-space occlusion -, start jitter, sun
    direction): `blueNoiseInit(vTexCoord)`, taken at each eye's own pixel;
    the denoiser rejects other depths and normals, so a lot of it remains on
    the particle points. `shaders/ambientdirectionallight_vr.hlsl` (not published either): the 16
    permutations (DirectionalLight, Distance24, Samples2, UnrollRaycastLoop;
    the engine creates two of them, Renderer +0xc38 without sun and +0xc40
    with sun, 0x9a2c0, selected by 0x8bf53 depending on the sun's colour),
    written with the engine's HLSL functions (scenecommon.h, raytracing.h,
    common.h): with `STEREO_ANCHOR=0` the file compiles EXACTLY the game's 16
    blobs (byte for byte, same container hash). The VR change: this pixel's
    texture coordinate is shifted by the whole number of pixels that leads to
    the left-eye pixel showing the same point (`shift(w)`, `1 / w` taken from
    the same depth as `linearizeDepth`), then the game's formula is applied.
    Verified on the GPU against the game's blobs: left eye bit for bit (16
    permutations, 8 FrameRnd values, two random scenes), right eye at pixel
    p = left eye at pixel p + offset.
  * Tests only, control file `TeardownVR_perf.txt`: `stereoParticles=2`
    (particles only) / `3` (lighting only); `taaMode=8`: no anti-aliasing,
    each pixel shows its own sample of the current frame
    (`temporalaacs_vr.hlsl` `RAW_CURRENT=1`), to measure the frame-to-frame
    grain on the mock headset. Never shown in the panel.
  * Still tied to the pixels (not handled: less visible, or anchoring is
    limited): `diffuselight` lamps (fires, explosions; 16-bit depth only),
    volumetric fog, rain on `wet` surfaces, reflections, rotation of the
    denoiser kernel.
* Round 3 (2026-10-12; reported issue: closing one eye and then the other
  while using the fire extinguisher, the left and right eyes
  showed slightly different things). Two defects of the right eye alone:
  * **No particle motion in the right eye** (`stereoParticleMotion`,
    default 1). The mesh generators (`MeshGenerationTask::run` 0x434160,
    slot 1 of vtable 0xa291d8 → GPU 0x4341d0 / CPU 0x434830) read each
    particle's "last drawn position" (+0xC) as `lastPos` (motion vector:
    `vOldPos = mubOldStableVpMatrix × (vertex − (pos − lastPos))`), then
    overwrite it with the drawn position. Frame A reads what B(n−1) wrote
    (correct), but B reads back what A has just written: `lastPos == pos`,
    camera motion only; the right eye's TAA (its own history) fetches the
    history from the wrong place (trailing, different blending), and so does
    the denoiser. `Hook_MeshTask` (`engine.cpp`): in A, it records +0xC for
    each slot (before the real work, without writing anything into the
    game); in B (same physics step, the right eye's own history), it puts the
    recorded value back (not for a particle born within the pair: its
    recorded value is zero); the generator then rewrites +0xC as before.
    Runs under SEH; counters with `particleMotionLog=1`.
  * **Patchwork front/back checkerboard** (`stereoParity`, default −1 =
    auto). Round 2 took the parity at the left-eye pixel, layer by layer: in
    the right eye, neighbouring pixels belonging to different layers have
    unrelated parities, which the TAA's GLASS_FIX filter (0.5·c + 0.125·Σ of
    the 4 neighbours, which exactly cancels a screen-space checkerboard;
    `taaMode` 2, 3 and 4) no longer cancels. `STEREO_PARITY`
    (`gbufferparticles_vr.hlsl`): 0 = round 2, 1 = the pixel's own parity for
    particles with a projected radius of at least 8 px, 2 = for all particles
    (= round 1, byte for byte). Auto: 1 under a TAA mode with GLASS_FIX,
    otherwise 0. Left eye identical bit for bit (GPU test bench, 496 checks).

## 6 quater. Stutters

* Measurements made with `[Debug] debugProfile=1` (main-thread sampling 1000
  times per second, `src/sampler.cpp`) and the breakdown of every slow frame in
  the log.
* Main cause: Virtual Desktop's `xrEndFrame` waits for its send queue to drain
  (`SleepConditionVariableSRW`, virtualdesktop-openxr.dll+0x52390) when the
  graphics card is saturated (93-98 % during the 60-110 ms bursts).
  Remedy: GPU headroom (resolution), Steam overlay turned off.
* The rest (20-30 ms): the engine waiting on its particle jobs
  (`Renderer::finish` 0x91830), its fire jobs and its physics jobs; rasterising
  a font the first time it is used; in the menu, decoding a `UiSound` sound or
  an image. The mod's Lua and its garbage collector are not the cause.
* Test session of 2026-10-11 (test setup: Quest 3, VirtualDesktopXR runtime
  1.0.10, H.264+ codec with 2-pass encoding, PC on a wired connection,
  high-end GPU): one burst of `xrEndFrame` waiting every 18 s on average (median
  gap 13 s), even with the player standing still and in the menu; these bursts
  account for ~85 % of the frames longer than 20 ms. The first frame of a burst always waits ~90 ms
  (+ ~10 ms of `xrWaitFrame`: ~100 ms, probably a fixed timeout in VD), the
  following ones 45-75 ms. The graphics card was running at ~72 % (encoder
  ~20 %, read with `nvidia-smi`): here the cause is not GPU saturation (the
  93-98 % above) but VD's streaming (network, headset); a one-off reading
  taken during a wait saw it drop to 13 % (encoder 2 %). Remedy on the VD side:
  lower bitrate, another codec (AV1), headset close to the router.
* Equipping THE EVERYTHING REVOLVER: its script re-creates its ~1000 shapes
  every time it is taken out (`Spawn` in batches), ~77 ms across the two
  frames of a pair (frame B's tick continues the batches). On top of that, the
  physics copy (`physBuild`) used to read every voxel of its first 16 shapes
  (~52,000 reads, 0.5 mm voxels): now the reading of a 10 cm cell stops at its
  first solid voxel (114 reads), and the result is cached per tool shape (name,
  size, voxel count; checked the next time the tool is taken out by re-reading
  the 21 voxels that were kept). The copy's "visible" shapes (no mass, no
  collision, hidden) are no longer created when the copy is not shown
  (`PHYS_SHOW_COPY`).
* The "Stereo frames" log line counts the headset refreshes that received no
  new game frame ("headset refreshes missed", from the gaps between
  `predictedDisplayTime` values), and separately those where the runtime held
  `xrEndFrame` for more than one refresh (VD): this is the measure of small
  stutters, which "frames > 20 ms" does not see (a pair of 12 ms frames
  already misses one refresh at 90 Hz).

## 6 quater bis. Performance measurement, gains with no visible change

Plan of 2026-10-11 (steps 2 to 5; steps 6 and 7, skipping the mods' tick or
the whole update of frame B, are in the next section, switched off by default;
8 and 9 are not done). Settings, all in `TeardownVR.ini` only:

| Section / key | Default | Purpose |
|---|---|---|
| `[Debug] perfTrace` | 0 | measurement in the log: 0 nothing (no hook installed, nothing extra timed), 1 sections per eye + engine profiler + waits + CPU per thread group, 2 additionally each mod's tick, the census of Lua bindings and their time |
| `[Debug] logSync` | 0 | 1 = log written line by line by the calling thread (as before) |
| `[Performance] cpuPriority` | 1 | "above normal" priority during the XR session |
| `[Performance] fastToolShapes` | 1 | 5a: the part of the scaled-down mod weapon is found through the engine's handle lookup |
| `[Performance] fastToolScaleB` | 1 | 5b: frame B, the mod weapon's scale is taken from frame A |
| `[Performance] fastObjectMotion` | 1 | 5c: allocation-free shape index, memory prefetch |
| `[Performance] fastLua` | 1 | 5d: fast paths in the Lua (bit 64 of `debugFlags`, see `main.lua`, `fastLua`) |

**Measurement (`perfTrace`, `src/perf.cpp`, `src/prof.h`).** Every 600
frames, the "Stereo frames" line receives, per eye (A = left, normal frame;
B = right, frozen frame), the per-frame average of:

* `update` (Game::update, like "update+scripts"), `fixed loop` (from the exit
  of Game::update to the entry of ParticleSystem::update: the fixed-step loop
  0x7c5c0 and the start of drawing, Scene::preRender before the particles),
  `particles` (ParticleSystem::update 0x42fb50, our hook `Hook_PsUpdate`),
  `ours` (our code at the entry of Renderer::render: per-eye history, TAA,
  `FixObjectMotion` with its `g_waitShapes` wait, `OffArmLateFix`), `render`,
  `VR submit`, `present`, `headset wait` (as in the "Slow frame" line) and
  `rest` = the frame minus all of the above (preRender after the particles,
  `draw()` of the mods and of the HUD, Renderer::finish, overlays in
  Present...). The first four plus `rest` add up to the old "physics/other";
* the main-thread waits, which overlap the sections: `jobs` (Job::wait
  0x5025e0, hooked, with the 4 most expensive callers: RVA of their start,
  e.g. +4304d0 ParticleSystem::finish (called by Renderer::finish 0x91830),
  +42fb50 ParticleSystem::update, +15c280 FireSystem::update, +b5510 narrow
  phase; beyond 23 callers in one frame, or 24 per eye, the rest is grouped
  under `+other`), `GPU` (TFenceD3D12::waitValueOnCpu 0x5aa4c0, hooked; called,
  among others, by beginFrame 0x5b35a0) and `shapes` (the wait for the
  shape-interpolation jobs inside `ours`);
* the physics steps per frame (counter `game+g_gameStep`, read at the exit of
  Game::update and at the entry of the particles);
* the categories of the engine's profiler (Profiler::add 0x45bcf0, hooked:
  `rcx` profiler, `edx` category, `xmm2` seconds; it counts nothing while
  `game+0x1e8`, the `SetPaused` flag, is set; our own sums always count):
  1 destr, 2 part, 3 path, 4 phys, 5 rend, 6 screen, 7 scrS (server scripts),
  8 scr (scripts), 9 water;
* then, for the pair: scene shapes, and shapes moved by a script that were put
  back in place per pair (`FixObjectMotion`), and the CPU (% of one core,
  average since the previous line, sampling thread once per second,
  `GetThreadTimes`): main thread, jobs (the threads started in teardown.exe at
  `THREAD_PRIORITY_BELOW_NORMAL`: the JobManager's 15-16 workers), other engine
  threads, ours (dsound.dll), the rest (VD, driver, sound), the whole process,
  and the load of the whole system (`GetSystemTimes`).

A "Slow frame" line (more than 18 ms of work) also breaks down its
"physics/other" (`fixed loop`, `particles`, `ours`, rest; waits and their 3
longest callers, physics steps, profiler categories for that frame). With
`perfTrace`, the "camera motion over 300 pairs" line (mean camera step) is
written even without `debugLua`: it is the validity check of the load test
(camera still, no recoil, no death). Never open the game's profiler
(SHIFT+ALT+F3) during a measurement: `prof+1` switches Game::frame to a single
update per frame, capped at 60 frames/s (0x72780-0x727cd).

`perfTrace=2` adds, after the "Stereo frames" line:

* "Perf mods' tick": the 5 most expensive mods per eye (ms per frame). Hook on
  0x46b060 (the tick of a script entity: server script `[ent+0x48]`, then
  client script `[ent+0x40]`; its only caller is 0xf2170, the Scene::update
  loop). Name: the end of the script's path, an engine string at `ent+0xa8`
  (read in the code that prints "Single player script disabled: %s", 0x46a026
  `lea rdi,[rcx+0A8h]`; 32 bytes, byte +0x1f non-zero = pointer to the heap);
* "Perf Lua bindings": calls and ms per frame, the 6 most expensive bindings
  per eye. Hook on the dispatcher 0x4a9050 that every binding goes through
  (486 names): each wrapper does
  `lea r8,[impl] ; lea rdx,[name] ; mov ecx,domain ; call 0x4a9050`, `r9` =
  the calling Script, then 2 arguments on the stack (arguments, results):
  6 arguments in total, `call r14` at 0x4a9331. Found at run time through
  three wrappers that must agree;
* `Perf census '<mod>'` (when the set grows): the bindings called in that
  mod's tick A and in its tick B; reads (Get/Query/Is/Has/Find) are only
  counted, all the others are named (input data for step 6: none is left out;
  a line too long for the log continues on `Perf census '<mod>' (cont.):`
  lines). The "core" functions do not go through the dispatcher: a static list
  `{ function, name, next }` in .data (SetBool 0x4b7050 at entry 0xc43eb0,
  SetInt 0x4b7400, SetFloat 0x4b7330, SetString 0x4b7900, SetColor 0x4b7200,
  ClearKey 0x4b1b00, SetValue 0x4b7a00, SetValueInTable 0x4b7bb0, SetPaused
  0x4b74d0; called as `(script, args, ret)`), found through the pointer to
  their name and hooked one by one.

Every hook calls the engine function unchanged (no change to the dispatcher's
domains or to its init checks); every pattern must be unique in .text,
otherwise the corresponding measurement is left out (the log says "NOT
hooked"). Checks: between `perfTrace` 0 and 1, the median work per pair must
not move by more than 0.1 ms; and `fixed loop + particles + ours + rest` must
add up to the old "physics/other" within ±0.2 ms.

**Asynchronous log (`src/log.cpp`).** The line is formatted by the calling
thread (same bytes as before, CRLF line endings) and then appended to a
buffer; a writer thread writes it out every 50 ms (or as soon as 64 KB are
pending). Before, every line did `fprintf` + `fflush` on the main thread (a
`WriteFile` of 10-30 µs, and rare stalls of several ms with the antivirus).
Written immediately: the hang detector (`Hang:`), `LogStack` and DLL unload
(at most 0.1 s of waiting per lock, measured), and serious exceptions. Those
go through a vectored exception handler (installed last), which is called
before any `__try` that would handle them: it may not be a crash, and it may
be the main thread. So the handler never waits: it writes out what is pending
only if both locks are free at that instant (at most one write per batch of
lines, never more than the old line-by-line writing); otherwise it wakes the
writer thread, which keeps running while a real crash is being reported.
Beyond 8 MB pending (disk stalled), lines are counted and then reported
("lines dropped"). Risk: an abrupt termination of the process
(TerminateProcess) can lose the last 50 ms of lines; `logSync=1` restores
line-by-line writing.

**Priority (`cpuPriority`).** During the XR session,
`SetPriorityClass(ABOVE_NORMAL)` (only if the game is at NORMAL: a priority
chosen by the player is not touched), back to NORMAL when the session stops
(`Perf::SessionPriority`, called at every Present). The workers go from base
priority 7 to 9, the main thread from 8 to 10; VD's TIME_CRITICAL thread
inside the game (15) and VirtualDesktop.Streamer (High, 13) stay above them.
Never HIGH, REALTIME or MMCSS (they would get ahead of VD; MMCSS "Games" is in
the Medium category here and demotes the thread below the workers when it is
busy).

**Overlays.** At the first Present and then at every "Stereo frames" line: if
`gameoverlayrenderer64.dll` (Steam) or `nvspcap64.dll` (NVIDIA capture) is
loaded, a "Warning: overlay hooks loaded" line goes to the log and, at the
bottom of the settings panel, "... overlay active: may cause stutters."
is shown (the Steam
overlay waited 4-7 ms in its Present hook, counted in "physics/other").

**Our own costs (step 5).**

* 5a `ToolShapeOfHandle`: the part of a script's scaled-down weapon is found
  through `EntityOfHandle` (type 2, parent = the weapon's body, voxels),
  instead of a table of all its shapes rebuilt every frame (~1000 allocations
  with THE EVERYTHING REVOLVER).
* 5b `ScaleModTool` in frame B: same body, name and shape count as frame A
  just before (and still the scaled-down weapon) → frame A's factor is reused,
  without `ToolSettled` and without the full `ApplyToolScale` pass, provided
  that the mods' writes are scaled down one by one (`Hook_ShapeLocalImpl`, or
  the binding's hook for 128 shapes at most). Remaining case: if a mod changes
  the size of a voxel through another path during frame B, it would be shown
  unscaled in the right eye for one frame.
* 5c `FixObjectMotion`: the index of the left eye's shapes uses open
  addressing, and its table is kept from one build to the next (instead of a
  `std::unordered_map` rebuilt as soon as the order of the shapes changes);
  the transforms of the shape 8 slots further on are prefetched
  (`_mm_prefetch`).
* 5d `main.lua` (`fastLua`): the poses in the reply are stored as plain
  tables (normalised like `Transform`); the reply's PIV/INV/ARM/LAS/DUA labels
  are located in a single pass; each `toWorld` pose is computed once per tick;
  the shapes of a big tool (> 64) are re-read every 10 frames, and immediately
  if the tool, its body, its grip, its scale or its physics copy changes (a
  change to its shapes alone is seen up to 10 frames later: the invisible copy
  is rebuilt then); `offHand.find` for the DLL (object under the hand) runs
  one normal frame in three, immediately when the search becomes useful again
  or the hand reappears, and every frame as soon as that hand's grip exceeds
  0.3 (the DLL reads the value when it passes 0.6); the shapes taken by the
  hand are kept in a list for `offHand.restore` instead of calling
  `FindShapes("tdvr_lm", true)` at every check (about 4 per second): the
  search in the scene is still done at the first check after each release,
  then one check in four, and every time when only the held object or nothing
  at all is left (broken pieces of an object, which carry the tag without
  being in the list: restored at most ~1 s later than before); the periodic
  re-reads of the inventory are spread over frames (12 tools, ammunition
  8 tools per frame; right away when it is opened). `physPlayerFilter` is not
  touched: it already does only one search per frame (the calls from the
  physical tool and from the physical hand count down the same delay, hence
  one check every 0.5 s, which is kept).
* 5f (shorter reply) is not done (protocol change).

**Mock runtime (testing).** `controls.txt` `autodump=1` brings back the BMP
dumps of both eyes every 8 s (40 at most), now switched off by default: each
one blocked `xrEndFrame` for 55-76 ms (fake "VR submit" stutters, 27 of the
30 slow frames of a test session on the mock headset). On-demand dumps (`dump=<n>`) remain.
`xrWaitFrame` waits on a high-resolution timer
(`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) instead of spinning for the last
2 ms: the main thread's CPU usage is no longer inflated (it was up to ~18 % of
a core at 90 Hz).

## 6 quater ter. Lighter frame B (frameBMode, steps 6 and 7)

Frame B (the right eye of synchronised stereo) re-runs the whole game update
with no time passing: every mod's tick (ours included), the player, the tool,
Scene::update. That was ~55 % of the main thread's work per pair when things
are calm, and half of every tool-switch "freeze". Code: `src/frameb.cpp`,
`src/frameb.h` (switching, mode 2), `src/perf.cpp` (mode 1, census),
`engine.cpp` (`FindFrameBTargets`, `Hook_Update`).

| Setting | Default | Purpose |
|---|---|---|
| `[Performance] frameBMode` | 2 | 0 = as before; 1 = frame B does not re-run the tick of "clean" mods; 2 = frame B does no update at all any more (render only) |
| `[Performance] frameBKeepMods` | empty | mode 1: mods that are always re-run in frame B (fragments of their script path, comma-separated, case-insensitive; the name is the one shown in the "Perf census" / "Perf mods' tick" lines) |
| file `TeardownVR_perf.txt` next to `teardown.exe` | absent | `frameBMode=N` (and `frameLatency=N`, next section): overrides the ini value, re-read once per second (only if it has changed); once the file is deleted, the ini takes over again. Every change is written to the log ("Frame B: control file ...", "Frame B: mode ...") |

Default 2 since the load measurement (next section); the "CPU optimization
(recommended)" checkbox in
the Display tab sets `frameBMode` 2 / 0 and `frameLatency` 2 / 0 together,
live when the hooks are installed (otherwise at the next launch). Nothing
extra is hooked if `frameBMode=0` in the ini **and** the control file does not
exist at start-up (first Present): exactly the previous behaviour. Otherwise
the census hooks (perfTrace 2: mods' tick 0x46b060, dispatcher 0x4a9050,
"core" functions) and the mode 2 hooks are installed once and for all (they
call the engine function unchanged); the mode is then chosen for each pair, at
frame A (never between A and B). To switch live during a test, the control
file must exist **when the game is launched** (for example with
`frameBMode=0`).

**Mode 1 (step 6).** In frame B, the tick of a script entity (0x46b060: server
script, client script, copy of the `shared` table 0x46b230, message queue
0x17c680; the last two do nothing without a tick) is not called if: it is not
our mod (recognised by its TDVR message: `Perf::NoteOurScript`, bridge.cpp),
nor a mod listed in `frameBKeepMods`; it ticked in frame A just before (same
pair); and that tick called **only** read bindings. Read = a name starting
with Get/Query/Is/Has/Find via the dispatcher (232 names), except QueryPath
(it starts a path search) and **GetLastSound**: it returns the loudest sound of
the previous frame (moved at the start of every frame, 0xff911), so the shots
and explosions of frame A can only be read during frame B; a mod that listens
keeps its B tick. Any other dispatcher binding (`Draw*`, `Debug*`, `Set*`, `Spawn*`,
PlayLoop, `SetCamera*`, PointLight...) and any hooked "core" function makes the
tick "dirty": SetBool, SetInt, SetFloat, SetString, SetColor, ClearKey,
SetValue, SetValueInTable, SetPaused, ClientCall, ServerCall, PostEvent,
InputClear, Command, RadiolinkDrawLinesCPP (lines), FxEmitSmokeCPP, AddHeat,
SetGravity, RobotHoverGetUpCPP, SetMapOrientation, **GetEventCount /
GetEvent** (script events are double-buffered per frame, see mode 2: what is
posted during frame A can only be read during frame B; a mod that reads them
keeps its B tick), and **UiSoundLoop / UiMute**: per-frame requests consumed
at the start of the next frame (loop request +0x88 read by 0xffc30; mute
sm+0x7690 / +0x7694 reset to 0 at 0xff9e2 / 0xffa13), like PlayLoop; they are
called from `tick()` (Workshop mods: Alicanto, cranetank, ewstation...), and
without a B tick the loop would only be requested every other frame (it would
pump) and the mute would flicker. And **PauseMenuButton**: every update clears
`game.pausemenu` before the ticks (0x7ad25); a mod without a B tick would lose
its pause-menu button (the menu opens on frame B's list: Natural Phenomena,
Map Menu, Total Overdose...), so it keeps its B tick. The other core functions
(GetBool, GetInt, `Input*`, GetTime, maths, other `Ui*`...) are reads or UI and
are not hooked (32 hooks at most, 25 used). The core list is found by name
(`{function, name, next}` entries in .data; if another table contains the
same name pointer first, as with ServerCall, the next occurrence is tried).
The decision is exact for every pair: the requests consumed at the next frame
(SetToolTransform, SetTimeScale) only come from "dirty" mods, which are
therefore re-run. "Stereo frames" line:
`frame B mode 1: B ticks skipped: X mods (of Y), Z ms saved per pair`
(Z = time of the skipped mods' tick A, an estimate of what B saves).

**Mode 2 (step 7, "render only").** `Hook_Update`, frame B: the time
accounting stays (accumulators, `dt` = 1e-5, `g_rightThisFrame`), but
Game::update is not called; `FrameB::BReplay` re-issues what A's update had
requested for one frame only and that A's render / draw / frame start
consumed. This is recorded at the entry and at the exit of A's update
(`AEnter` / `AExit`), and by hooks that are active only during that update:

| What | Where (2.0.1) | Consumed by | Replayed in B |
|---|---|---|---|
| lines (DrawLine, laser, RadiolinkDrawLinesCPP) | list renderer+0x23b0 (0x3c-byte entries: a, b +0xc, colour0 +0x18, colour1 +0x28, flag +0x38) | 0x8f460 in Renderer::render (count reset to 0) | the entries added by A's update (count at entry → exit), copied at the exit, re-added through 0x86ca0 |
| sprites (DrawSprite, icons) | list renderer+0x23c0 (0x58 bytes, sort key +0x50) | 0x90a00 (emptied at 0x90c00) | same, through 0x86f50 (recomputes the key with the camera) |
| debug lines | GPU builders renderer+0x18f8 | 0x8a710 | the calls to 0x86a90 (r, a, b, colour) made by A's update are recorded (hook) and redone |
| outlines (DrawBody/ShapeOutline, engine) | table renderer+0x2410 | 0x8fa90 | calls to 0x9d2d0 (r, shape, rgba) recorded, redone if the shape still exists (handle +0xc → `EntityOfHandle` = the shape itself, type 2) |
| highlights | table renderer+0x23f8 | 0x8e2b0 | calls to 0x9d280 (r, shape, alpha xmm2), same |
| lights (PointLight, scene) | two lists renderer+0x2428 / +0x2f38 (0xb10 bytes, count at +0) | emptied only by 0x87a40 in Game::update | counts brought back to their values at the exit of A's update (A's physics steps, preRender and draw added entries); B's preRender adds its fires |
| depth of field (SetCameraDof) | [renderer+0xa28] +0/+4/+8, flag renderer+0x23f0 | Renderer::render (0x890b4) | if the flag was set: 0x9cbe0 with the same values |
| low-health blur | renderer+0x23f4 | reset to 0.6 (0x89408) | value at A's exit |
| camera: SetCameraTransform | camera (game+0x60) +0x264 pose, +0x2c4 fov, flag +0x2cc | Camera::update 0x33180 (mode 8) | copied if the flag was set |
| SaveCameraOverrideTransform | +0x261 | same | same |
| SetCameraOffsetTransform | +0x1b0 pose, flag +0x1cc | same (0x3689a) | same, **composed with the right eye's offset** (`g_eyeOffset`, separation × `g_curK`, as in `Hook_CamSet`): A's offset is the left eye's (our Lua puts the player camera on the eye of the tick), and in mode 0 our B tick sets the right eye's. The image does not depend on it (`Hook_CamSet` replaces the right eye's camera), but the camera pose kept by the engine does: `GetCameraTransform` in B's `draw()` calls, debugLua check "right-eye camera corrected, max drift" |
| SetCameraFov | +0x2c8 | Game::render (0x70ca8) | if non-zero |
| AttachCameraTo | +0x2d0 entity, +0x2d8 | same (0x36897) | if the entity still exists |
| sound loops (PlayLoop, UiSoundLoop) | each Loop of [game+0x58]+0x60 (count +0x58): request +0x88..+0x97 | 0xff420 at the start of the next frame (0xffc30) | the request at A's exit is put back if it is louder than the current one (otherwise every pair would lower the volume) |
| music: `game.music.volume`, `game.music.lowpass` (Cullington's musicfade.lua on every tick within 40 m of the trigger, caveisland_dishes, espionage) | registry | 0xff420 at the start of every frame: read, applied to the track ([sm+0x75a8]+0x38, absent = 1.0; [sm+0x7448]+8), then erased (clearKey at 0xff638 / 0xff6e9) | value at A's exit **rewritten unconditionally** (no comparison with the previous A: a fade changes it every tick; without this, the music would return to full volume every other frame, a ~90 Hz buzz) |
| last sound heard (GetLastSound) | sm+0x7670 volume, +0x7674 position (read by the binding 0x477e80); loudest sound recorded during the frame: +0x7680 / +0x7684 (0xfd2f0, registerVolume flag) | 0xff911 at the start of every frame: +0x7680 → +0x7670, then +0x7680 = 0 | B's frame start has moved A's loudest sound (update, physics, draw) to a place no script reads any more: B **puts it back** into +0x7680 (if it is louder), and the start of the next A moves it again. The ticks (hunter helicopters) and the updates (robot hearing, robot.lua `hearingUpdate`) of the next A hear it, once. Without this, GetLastSound would always return 0 in mode 2 (deaf robots). Side effect: robots also hear A's one-off sounds, which they miss in mode 0 (their update only runs in A, where A's frame start puts B's sounds) |
| mute (UiMute) | sm+0x7690 (everything), +0x7694 (music): maximum of the frame's requests (0x4c7bdb, 0x4c7c1b) | 0xff9e2 / 0xffa13 at the start of every frame (reset to 0; +0x7694 only if game+0x351 = 0) | values at the exit of A's update put back if higher (those from a `draw()` are not affected: drawing runs in both frames) |
| time scale (SetTimeScale) | game+0x1fc → +0x1f8 at the start of every frame, then 1 | frame start | B: +0x1fc := +0x1f8 (A's request carries over to the next frame A) |
| registry: per-frame flags | keys written by a script (dispatcher bindings such as DisableMotionBlur, or SetBool/SetInt/SetFloat/SetString/ClearKey) during A's update; value = the one at the exit (the engine's writes to these keys count) | HUD drawing (hud.disable, hud.hide, hud.aimdot reset to true, hints `hud.hintimage*`, level.inescapevehicle), frame start (game.disablemotionblur, disablepause, disablemap) | rewritten (Registry::setString 0x4626c0, or clearKey 0x45fdb0) **only** if the same key had the same value at the exit of the previous A update: a flag is rewritten on every tick; a key written once is an event (notification) and is never repeated; `hud.notification*` never. clearKey also deletes all sub-keys (0x45fe56: the children, recursively): a cleared key one of whose sub-keys was written **after** the clear in the same update (non-empty exit state: THE EVERYTHING REVOLVER clears `38Revolver.Stats` at the end of its tick while another script writes `38Revolver.Stats.name`; Portal Gun clears `portalgun` on every tick) is **not** cleared again (otherwise the subtree, and the sub-keys written by the engine, would disappear until the next A). Table: 2048 keys per update, plus 256 reserved for `hud.*` / `game.*` (our hud.aimdot / hud.disable are among them); beyond that, logged once and counted |

No more script ticks in frame B. Consequences, all handled:

* **Script events** (PostEvent / GetEvent / GetEventCount, manager
  [game+0x158]): two buffers, events are posted into [mgr+0] and read from
  [mgr+4]; Game::frame swaps them at the start of every frame (0x157e50,
  called at 0x72c61) and empties the new write buffer. What a frame posts can
  therefore only be read in the next frame: without a tick in B, mods would
  never see what frame A posts (damage, deaths...). The swap is therefore
  **skipped** at the start of a render-only frame B (hook on 0x157e50): what A
  and B post is read in the next frame A, exactly once, by the mods as well as
  by the HUD. Only scripts call the readers (0x1582b0, 0x158230, 0x158300:
  the bindings GetEvent, GetEventCount, SelectedMapMarker); the engine itself
  does not read these buffers. The "render-only B" decision is taken there, at
  the start of B (latch `g_latch*`), and `BReplay` follows it.
* **Real time** (game+0x208): Game::update passes it to the HUD tick
  (0x7b14d), to the game-mode tick (0x7b16b, state 6), to the players'
  position traces (0xc5ea0, 0x7b1a7), to the spectator controller
  (multiplayer) and to the gamepad's alarm LED. Since these now only run in
  frame A, frame A gives them the real time of **the whole pair** (+0x208 +
  the real time of the previous render-only frame B, restored after the
  update: the `GetTimeStep` seen by `draw()` stays that of the frame).
* The player requests (RequestFirstPerson, SetPlayerHidden, SetToolTransform /
  ...Override / ...Offset, hand pose) are only reset in Game::update (0xc4460,
  called at 0x7af86): B keeps A's. The tool is placed in Game::render
  (0xc8670) from P+0x844, left there by ToolAnim in A.
* The right eye's camera remains the one set by `Hook_CamSet`
  (Camera::setTransform is called by Game::render, 0x70e8d). A mod's camera
  mode 8 (AC130) is maintained by the replayed request.
* Our script: no more B tick, hence no more TDVR request in B
  (`CheckRequests` is not called for a render-only B; vr.cpp knows it is the
  right eye through `EngineConsumeFrameEye`). The laser, its outlines and its
  icons in the right eye are A's, replayed: an intended change, both eyes see
  the same line (before, B recomputed it after the physics step: up to ~7 cm
  of difference while walking). `main.lua` `draw()` no longer runs
  `checkCamera` (debugLua diagnostic) for a frame without a tick: its
  measurements come from the left eye's tick.
* `FixObjectMotion` should no longer find anything "moved by a script" in B.

**Fallback to the full update** for a frame B (line "Frame B: full update in
this frame B (...)", 10 at most, then counters): no record of A (`no frame-A
record`: first VR pair, frame A was not in VR, or its exit came early after a
level reset; tested first), frame start not seen (`frame start not seen`: the
event swap was not intercepted at the start of B), not in a level
(state ≠ 6) or paused, game objects changed (renderer, camera, scene,
registry, sound), `debugSyncTest` (it needs our Lua's B tick: one camera per
eye). A memory fault during a replay (structured exception handling) turns
mode 2 off until restart ("ERROR - memory fault").

**Locating the targets.** Every pattern contains the offsets it uses
(`FindFrameBTargets`, an "Engine: frame B targets" line in the log): 0x86ca0
(+0x23b0/+0x23b4), 0x86f50 (+0x23c0, 0x58-byte entries), 0x86a90
(+0x18f8/+0x1918), 0x9d280 (+0x23f8), 0x9d2d0 (+0x2410), 0x9cbe0 (+0xa28,
+0x23f0), 0x9cc20 (+0x23f4), 0x87a40 (+0x2428, 2 × 0xb10) and its call
`mov rcx,[rsi+48h]` (renderer = game+0x48), the end of Game::update at 0x7b149
(+0x38, +0x208, +0x80, +0x98, +0x850, +0xb8, +0xc0), 0x7274c
(+0x1f8/+0x1fc), 0x72c77 + 0xffb80 + 0xffc30 (sound), in
SoundManager::update 0xff420 the music keys
(`lea rdx,["game.music.volume" / ".lowpass"]` then `call` clearKey: 0xff61e,
0xff6cf), the last-sound copy 0xff911 (unique pattern, within that function)
with the GetLastSound binding 0x477e80 (+0x7670), the mute reset 0xff9e2 /
0xffa13 with UiMute's maximum 0x4c7b50 (+0x7690 / +0x7694), the bindings
SetCameraFov / SetCameraTransform / SaveCameraOverrideTransform /
AttachCameraTo and the setter of SetCameraOffsetTransform 0x32520 (camera),
0x127920 (registry game+0x100, string 0x5075a0 / 0x507710, setBool 0x462500
→ setString 0x4626c0), ClearKey 0x4b1b00 → 0x45fdb0, the event swap 0x157e50
and its call (game+0x158). If even one is missing, mode 2 is not available
(the log says which one) and the requested mode falls back to 1.

**Log.** "Stereo frames" line: `frame B mode 2: N render-only of M, frame-B
work X ms per pair (replay), frame-A record Y ms, real time of the pair for
the scripts Z ms`; then a line "Frame B: replayed per pair: lines, sprites,
debug lines, outlines, highlights (dropped: shape gone), registry keys set
(music requests) / cleared (written once or changed: not repeated, clears
kept back: sub-keys written after), camera requests (offsets moved to the
right eye), loop requests, last sound kept, mute, depth of field, light
entries cut ; full updates: `<reason> <count>`". The "Engine: frame B targets"
line also gives `music`, `last sound`, `mute` (1 = found).

**Known limitations.**

* Modes 1 and 2: a mod that counts ticks without `dt` for an animation (drawn
  in `draw()`) animates it at the pace of a ~45 frames/s screen instead of
  twice per pair; per-tick spawns (particles, SpawnParticle, impulses) too:
  as on a monitor.
* Mode 2: what is done only once (PlaySound, SpawnParticle, ShakeCamera, which
  decays over time) is not repeated. SetColor writes the registry without
  setString: a colour reset by the draw would not be replayed. A cleared key
  one of whose sub-keys is written again afterwards is not cleared again in B:
  whatever A's draw would have added there stays (as in a frame B without
  replay). `draw()` / `render()` functions that read events see them twice (A
  and B). Real key edges that fall into a frame B (real mouse, gamepad read by
  the game) are seen by no update: our simulated keys are already delayed
  until frame A (input.cpp). The first tick of a registry flag is not replayed
  (one frame B without it). The game's internal scripts that read body hits
  (GetBodyHit..., "internal usage") have not been checked.
* Mode 2: without a recent previous frame A (level start, VR resume, live
  switch to mode 2), frame B does a full update ("no previous frame A"):
  per-frame registry flags are only replayed if the frame A before also wrote
  them.
* Mode 2: the vibrations requested by our Lua in frame A go out at the next
  frame A (~11 ms later) instead of in frame B.
* Existing defect, not fixed here: in VR, frame A's time is replaced by the
  headset interval without the time scale (+0x1f8), so SetTimeScale (Better
  slow-motion) has no effect in VR, whatever `frameBMode` is (fix: multiply
  that interval by game+0x1f8, in state 6 and when not paused, as
  0x72828-0x72855 does).

**Verification (mock headset).** Before making it the default: comparisons of
right-eye images in modes 0 / 1 / 2, switched live through the control file
(on-demand dumps, `dump=<n>`), head still, the difference between
modes within the noise between dumps of the same mode; scenes: our laser +
outline, a mod that draws lines, a sprite, a lamp / PointLight, Physics Gun
(outlines and highlights), AC130 (camera mode 8, HUD hidden), Better
slow-motion, the fade of a HUD notification, smoke; by ear: Cullington within
40 m of the musicfade.lua trigger (music lowered, no buzz, modes 0 and 2), a
robot turning towards a shot (GetLastSound), a mod's UiSoundLoop loop called
from `tick()` in mode 1 (steady); THE EVERYTHING REVOLVER (shot with special
ammunition: `38Revolver.Stats.*` kept); debugLua check "right-eye camera
corrected, max drift" close to 0 in both modes; plus the load test and `debugSyncTest` (which forces the full update).

## 6 quater quater. CPU and GPU in parallel (frameLatency)

**Finding (mock headset, `perfTrace=1`, measurement hook below).** After every
Present, the engine waits on the "frame latency" object of the desktop swap
chain (`TRendererD3D12::endRender` 0x5b6e50 → 0x5bc180: `mov rcx,[rcx+60h]`;
`WaitForSingleObject` INFINITE; the object comes from
`GetFrameLatencyWaitableObject` at 0x5bba84, after
`SetMaximumFrameLatency([+38h])` at 0x5bba4b, value 1; swap chain flags
0x842). With 1, this wait lasts until the GPU has finished the frame that was
just presented. In synchronised stereo, the CPU therefore only starts frame B
once the left eye is fully drawn, and then the GPU waits for B's commands: CPU
and GPU take turns. Measured: 4.4 ms of "display" wait per frame B under
destruction (B's "rest"), GPU at 55-65 %, main thread at ~40 % of a core. This
is why removing B's update (mode 2 alone) gained little in real time: the
wait grew by the same amount.

**Fix.** `[Performance] frameLatency` (default 2): during the headset session,
`IDXGISwapChain2::SetMaximumFrameLatency(2)` on the game's swap chain
(`Perf::FrameLatency`, perf.cpp, called at every Present by vr.cpp, checked
every 30 frames: the game restores its own value when it recreates the
buffers); the game's value is restored when the session stops. Only for a
swap chain created with `FRAME_LATENCY_WAITABLE_OBJECT`. Frame B's CPU work
then happens while the GPU draws frame A. Nothing that is drawn changes (same
commands, same order, same queue); the engine's two frames in flight
(`beginFrame` fence) still protect its resources; the headset still receives
the pair at `xrEndFrame` after B (earlier, since B is submitted earlier).
Control file: `frameLatency=N` (0-3) overrides the ini live. Log:
"Perf: desktop swap chain frame latency 1 -> 2".

**Measurement** (`perfTrace` ≥ 1): hook on 0x5bc180 (pattern
`48 8B 49 60 48 85 C9 74 0C BA FF FF FF FF 48 FF 25`), time added to
`P_DisplayWait`, written as "display" in each eye's waits ("waits: ... GPU x,
display y, shapes z").

**Results (mock headset, Lee Chemicals, 20 s calm / 40 s of explosions / 40 s
after, medians of 2 interleaved runs)** — mode 2 + latency 2 versus mode 0 +
latency 1:

| Phase | pair (ms) | right eye (ms) | missed headset frames /s | stutters /600 |
|---|---|---|---|---|
| calm | 6.8 → 4.3 | 3.5 → 1.0 | 0 → 0 | 0 → 0 |
| explosions | 15.8 → 13.9 | 5.8 → 1.9 | 33.9 → 24.4 | 105 → 63 |
| after | 9.9 → 7.6 | 4.1 → 1.6 | 11.1 → 5.1 | 0 → 0 |

Latency 2 alone (mode 0) gains little: B's update (~2.5 ms) then moves onto
the critical path; the two together complement each other. Under the heaviest
load, frame A now waits for the GPU ("GPU 3.3": fence of frame N-2): the GPU
becomes the limit, no longer the main thread. The "VR submit" freezes
(xrEndFrame held by Virtual Desktop) do not come from the CPU and do not
change.

## 6 quater quinquies. Explosion physics: what waits for what (r17, 2026-10-12)

**Measurements** (mock headset, Lee Chemicals, `perfTrace=1` + `perfDetail=7`:
"Perf waits / jobs / GPU" lines after each "Stereo frames" line). At the peak
of the explosions, frame A (left eye) only computes for ~6 ms. The rest is
waiting:
- **jobs** (4-7 ms): mostly the solver (`+5fd110`). `Job::run` timed per RTTI
  class shows `TSolver::TSolverJob` at ~65 ms of CPU per frame A (8
  Gauss-Seidel iterations), and a single island (a pile of debris pieces
  touching each other) solved in one block by a single job: up to 3.5-6 ms.
  This is the engine's underlying bottleneck (islands are never split; see
  Dennis Gustafsson's 2025 talk). Splitting them would change the Gauss-Seidel
  order and therefore the physics: not done. The **number** of jobs also sets
  how the narrow-phase pairs are distributed (`min(lists, 2×workers)`): never
  change the number of workers.
- **GPU at the start of frame A** (~3 ms): `Game::frame` 0x71ce0 calls
  `TRendererD3D12::beginRender` 0x5b35a0 (call at 0x71f84) before
  `Game::update`; it waits for the fence of frame N-2 (with 2 contexts, the
  previous frame A). The next simulation could therefore never overlap the GPU
  drawing.
- The GPU is busy ~6.3 ms per eye at the peak (GPU timestamps around each
  submission on the game's queue): ~12.8 ms per pair, above 11.1 ms. At the
  very peak, the graphics card becomes the limit.
- Physics on the GPU is not feasible: voxel collision, solver and destruction
  are closed source, the results would not be identical, there would be a
  CPU↔GPU round trip inside the step, and the GPU is already the limiting
  factor.

**`[Performance] lazyBeginFrame`** (default 1; "CPU optimization
(recommended)" checkbox):
the `beginRender` of frame A of a stereo pair is held back until the **first
use of the renderer**. The `TRendererD3D12` vtable (0xa81d80, 74 entries,
verified by pattern, single entry in `.rdata`, endRender at +18, RTTI name) is
redirected: +10 → `LazyBegin`, 57 methods → small generated stubs (a `cmp` +
`jmp` when nothing is held back; otherwise `LazyTouch` runs the original
`beginRender`, with its wait, and then the method). 16 entries are left
alone: empty ones, accessors, and the 3 creation functions used by the
asynchronous uploader (hash of their body verified). Same commands, same
order, same queue: only the CPU wait moves. Unexpected case (first use from
another thread, or a `beginRender` still held back when the next one comes):
it is run immediately, logged, and the feature is turned off for the session.
Control file: `lazyBeginFrame=N` (the line must exist at launch for the stubs
to be installed if the ini says 0). Log: "Perf: lazy beginFrame ready / 1 -
..." and on "Stereo frames":
`lazy beginFrame 1: held 300/300 A (GPU busy n), first use +x ms (+30 by +467dd0), beginRender y ms`.

**`parallelParticleSort`** (default 1): in frames without a physics step (all
B frames), the depth sort of the 3 particle systems (0x437df0) runs on 3
threads at once (same function, same inputs; the main thread takes over if a
helper thread did not start). Mode 2 = every sort is re-checked against the
serial sort: 18,003 checks, 0 differences.

**Rejected after measurement** (left as options, default 0): `jobSpin` (the
engine's threads spin for a few µs before sleeping: 93 % fall asleep anyway,
0.6-1.3 ms of CPU per frame A, gain within the noise); `skipShapeJobsB` (gain
~0.02 ms, and the job also updates the interpolation history of dynamic
shapes); `hiddenAreaFill` (corners outside the lens filled with black:
0.3-2 % of the pixels of a Quest 3 with the 2° margin, no effect on the
bitrate in CBR, risk of black arcs during a long reprojection);
`gpuPriority`.

**Results** (2 + 2 interleaved runs, `fb2_lat2`, explosions): lazy 0 → 1:
frame A 13.2 → 10.6 ms, pair 15.0 → 14.2 ms, missed headset frames
29.5 → 26.7 /s, stutters 88 → 72; just after: missed 7.2 → 5.7 /s, pair p90
14.8 → 10.9 ms. At the peak, part of the GPU wait moves to frame B (the GPU
is the limit). Images compared live with lazy 0/1: difference = noise (0.088
versus 0.084). With all the defaults (`perfTrace=0`): 21.9 missed /s and 57
stutters, versus 34-40 /s and 105-154 before this work.

**Virtual Desktop** (`vdStats`, default 1: every `xrEndFrame` longer than one
refresh is counted, with its duration, on "Stereo frames"; codec / 2-pass
read from `C:\ProgramData\Virtual Desktop\StreamerSettings.json`, read-only;
network bitrate sent). The holds come from VDXR's
`waitForAsyncSubmissionIdle` (MIT-licensed source): VD waits for its own
`ovr_EndFrame`/`ovr_WaitToBeginFrame`, with the encoder almost idle during
the hold → Wi-Fi network or the headset's decoder. H.264+ at 500 Mbit/s =
5.6 Mbit per frame at 90 Hz (above H.264 level 5.2, a single NVENC engine
without sliced encoding), versus 2.2 Mbit at 200 Mbit/s: the peaks (level
start, smoke) no longer fit into 11.1 ms of airtime. An asynchronous
submission thread would not help (VDXR blocks every function during the
wait). The lever is on the VD side: AV1/HEVC 10-bit ≤ 200 Mbit/s.

**`holdSmooth`** (default 1; checkbox "No jump after a streaming hitch
(recommended)"): frame A's time step comes from the gap between two
`predictedDisplayTime` values. After a VD hold (last `xrEndFrame` longer than
one refresh), that gap covers the whole hold (20-90 ms): the game jumped
forward by the same amount (everything that moves teleports on resume) and
`limitCatchUp` still left 2 physics steps in the next frame A, which was
heavier exactly when the stream is fragile (holds in a chain). With
`holdSmooth`, that pair only advances by one refresh: the game resumes where
it stopped, and the held time is not played (game time stays a few ms behind
the clock, never ahead of it). Log: "VR: N runtime holds since the last
line - the game resumed where it stopped (X ms of game time not
played...)". Mock headset with simulated holds (`controls.txt` `vdhold=50 vdholdevery=2`, new
commands of the mock runtime): ~55 ms of jump avoided per hold; during
explosions, dropped catch-up steps 4.0 → 1.9 /s, physics loop 3.6 → 2.8 ms,
stutters 74 → 62.

**Dead passes: bloom and depth of field (`[Performance] skipDeadPasses`).**
With `removeBloom`, the post-processing bloom factor (`scene+0xa4c`, clamped
to [0, 10] by `SetPostProcessingProperty`) is 0. The `post` shader adds the
bloom result, multiplied by the constant `mubPostParams.z`, to the composite.
The `Bloom::render` chain (0x7ea80 via 0x7f050, 20 draws into
`Bloom::mMipFbo[7][2]` and `mResultFbo`, R11G11B10) is therefore useless. Its
result is only read by the post pass and by the "bloom" debug view
(renderer+0x978), and a finite texel × 0 gives ±0, which leaves the composite
unchanged bit for bit. The skip clears the result target to 0, as the chain
itself does first. With `removeDof`, `SetCameraDof(0, 0)` → 0x9cbe0 sets the
single flag renderer+0x23f0, which makes `Dof::render` (0x80ea0) run even with
`options.gfx.dof = 0`, with an amount `Dof+8` = 0. The circle of confusion is
then ±0 or NaN, no blur loop runs (max/min discard NaN), and the last pass
rewrites the composite as a linear blend with a weight of 0
(t1 + (t2 − t1) × 0 = t1): an exact copy of the composite through two RGBA16F
targets of the same size (samples at the texel centres). The skip draws
nothing and only writes the noise angle (global 0x1c6ecb0, read only by that
pass). The focus pass (0x80da0, its own history) keeps running. Neither skip
happens in photo mode (game+0x8a0) or with a debug view. Both assume an HDR
image without Inf/NaN: D3D conversions saturate to the largest finite value,
and only a shader dividing by 0 would produce them. `skipDeadPasses=2` checks
this on the GPU every ~5 s with an exact comparison (engine copies, read back
after the frame's barrier). A difference turns the skips off until restart.
Measured gain: 0.12 + 0.18 ms per eye at the peak of the explosions. The
"debug view" guard accepts the normal value `final` (constructor 0x82d90),
not only an empty string. Mock headset (2026-10-12, explosions): 77 exact
comparisons on the GPU, 0 different, 0 Inf/NaN texels; bloom and depth of
field at 0.00 ms. Default `skipDeadPasses=2` (skips + check) until a session
on a real headset, then 1.

## 6 sexies bis. Simulated keys and vehicles

* Simulated keys and clicks go through `PostInput`: during the right eye's
  "zero-time" frame, the window sets input messages aside and sends them back
  after display; a message posted in the meantime overtook the ones set aside
  (a "release" arriving before its "press" left the key held down for the
  game: the player kept walking on their own after getting out of a vehicle).
  While messages are waiting, new ones are queued behind them. On foot, the
  DLL also re-sends "release" for W/A/S/D every second.
* The game binds movement to "w a s d" (options.xml); on an AZERTY keyboard
  the letter W is not where W is: every key is sent with the US letter (VK)
  AND the US position code (scan 0x11, 0x1E, 0x1F, 0x20).
* The game's accelerator is all-or-nothing: a "pulsed" key did not move the
  vehicle forward. Triggers: pressed beyond 40 %. Steering: pulsed over
  0.25 s (progressive).
* The game reads gamepads through GameInput (not XInput): not simulated.
* In a vehicle, the VR "room" turns by the same angle as the vehicle every
  frame (its yaw); the right thumbstick turns the view relative to it.
* The player character is made of 26 animated bodies whose shapes accepted
  every collision layer: the tool's physics copy rubbed against it. The mod
  removes layer 128 from their mask (checked every second).

## 6 sexies ter. Other mods' keys, simulated mouse

* **Camera taken over by a script** (`SetCameraTransform`, binding ≈ RVA
  0x48e9d0): the function writes the requested pose into the camera object
  (`Game+0x60`, +0x264 position and rotation, +0x2c4 field of view) and sets
  the byte +0x2cc to 1. The camera update (≈ RVA 0x36d00) sees this byte,
  switches the camera to **mode 8** (the previous mode is kept), copies the
  requested pose into the player camera (+0x1d0) and resets the byte to 0;
  without a new request, the next frame returns to the previous mode. The mod
  therefore reads `*(int*)camObj == 8` (the constant is re-read at start-up
  from `80 BE … 0F 84 … 48 8D 8E … 48 8B FE BB <+0x1d0> 83 F8 <mode>`): it
  means a mod has taken the camera (AC130 plane, drone, turret...).
* Mods read the mouse through `InputValue("camerax" / "cameray")` (radians,
  multiplied by the sensitivity) or `"mousedx" / "mousedy"` (pixels). The game
  registers the mouse as raw input (page 1, usage 2, without NOLEGACY): a
  simulated `WM_INPUT` with `lLastX / lLastY` feeds them. While a mod has the
  camera, a thumbstick becomes that mouse (1500 counts/s at full deflection,
  squared curve); the thumbstick is then no longer sent to the Lua (no walking
  or turning at the same time). A `WM_MOUSEMOVE` also follows a virtual
  cursor: the mods' windows (`UiMakeInteractive`) follow that message, like
  the game's menus (not verified in game).
* **Active mods**: the game writes `Active mod: <name> (<id>)` into
  `%LOCALAPPDATA%\Teardown\log.txt` for each global mod when a level starts
  (one block of consecutive lines per level). Before the first level: the
  current mod list, `modlists\<id>.xml` (`<mod id="…"/>`), whose name is in
  `Loading mod list: <name>` and whose id is in `modlists\index.xml`.
  Folders: `steam-<n>` → `steamapps\workshop\content\1167630\<n>`,
  `builtin-<x>` → `<game>\mods\<x>`, `local-<x>` →
  `Documents\Teardown\mods\<x>`. The registry key
  `mods.available.<id>.active` also exists on the Lua side.
* **A mod's keys**: its Lua scripts are read (never executed):
  `InputPressed/Down/Released/Value("q")` (also via `pcall(InputPressed, …)`),
  the variables passed to these functions (`AC130_KEY_BINDS[action]`,
  `GLOBAL.noClipKey`, `config.keybinds.turn`) and their values written in the
  scripts (`previousShell = "q"`, `turn = "r"`, `throw = { "string", "r" }`;
  a long list of keys = a key-binding menu, ignored), plus the keys the player
  chose in the mod's options (`savegame.xml`,
  `<savegame><mod><id>…<…key…/bind…> value="Q"`). The game's actions
  (`interact`, `usetool`, `scroll_up`…) become the player's key
  (`options.xml`, `<keymap>`). Measured on 21 active mods: 126 keys in 46 ms
  (separate thread).
* The simulated keys from the panel (US letter + US position code, see above)
  also get through while the panel is open: the window recognises the
  messages posted for these keys (same message, same parameters, less than
  2 s old) and always holds back those from the real keyboard / real mouse
  and the usual controller keys (pause...). Alt is sent as `WM_KEYDOWN` (a
  `WM_SYSKEYUP` for Alt alone would open the window's system menu and freeze
  the message loop). F8 and F10 are not offered (they are the mod's own
  shortcuts).

## 6 quinquies. Particles and physics catch-up

* `ParticleSystem::update` (0x42fb50, ps = `*(*(game+0x50)+0x18)`) runs once
  per frame inside the draw (call at 0xeb2ef, from 0xeb160), after the physics
  loop. If the previous frame lasted less than 0.8 × the fixed step
  (`game+0x208` < 13.3 ms, test at 0x42fc16-0x42fc38), it takes the step
  counter, the alpha and the time saved by the previous call
  (ps+0x270/+0x26c/+0x268) instead of the current values (`game+0x1f4`,
  `game+0x21c`, ps+0x264): a "particles one frame late" mode that spreads the
  load on a screen running above 75 fps. In synchronised stereo every frame is
  short: the simulation of frame A's step fell into frame B (zero time), the
  left eye saw the particles one display interval late (~10 cm for fast
  smoke) and frame B paid for the simulation and its waits.
* Fix (`Hook_PsUpdate`, `syncParticles`): before the call, write the current
  values into the "previous" slots. Both paths are then identical: A
  simulates its step, and B (no new step) only sorts and rebuilds the particle
  mesh (vertex buffers alternate from frame to frame: never skip the call in
  B). Verified read-only: ps+0x274 now changes in frame A, and A and B receive
  the same (step, alpha).
* Fast sparks are stretched by velocity × `game+0x204` (constant
  `mubParticleSystemParams.z`, written at 0x8d725): during frame B's
  `Renderer::render`, the DLL puts frame A's frame time back into
  `game+0x204`.
* Catch-up: after a late frame, the next display interval covers two, and the
  fixed loop would do two physics steps, which makes the next frame slow in
  turn (chained stutters in explosions). `limitCatchUp` clamps the
  accumulators to `max(1.98, interval + 0.98)` steps: at most one step per
  frame when the headset runs faster than the physics; the excess time is lost
  (counter "physics catch-ups dropped" in the log).

## 6 sexies. Physical tool (Lua)

* The game's tool is a static, non-collidable body placed by the engine. The
  mod creates a copy: a dynamic body (`Spawn`, tag `tdvr_proxy`) with, for each
  shape of the tool, an identical visible shape and an invisible physics
  shape.
* Pitfalls found:
  - `CreateShape(body, t, refShape)` takes the palette (and therefore the
    materials) of the reference shape, but always uses 10 cm voxels; the tools
    use 5 cm voxels. `CopyShapeContent` into a 10 cm shape doubles the size.
  - A `<vox scale=...>` shape created by `Spawn` has the right voxel size, but
    its materials come from its own palette: after `CopyShapeContent` and
    `CopyShapePalette`, the mass stays zero and the body becomes static.
  - Hence: visible shape = `<vox file="MOD/proxy.vox" scale=...>` + copy of
    the content and of the palette, collision filter (0, 0); physics shape =
    `CreateShape` with the tool's palette, downsampled to 10 cm
    (`GetShapeMaterialAtIndex` + `DrawShapeBox`), tag `invisible`. A wooden
    `voxbox` keeps the body dynamic during creation.
  - `SetShapeDensity` multiplies the density of the materials (a factor, not
    kg/m³).
  - The `invisible` tag (present in the exe) hides a shape at run time,
    shadow included: it is also used to hide the game's tool.
* Driving, in `update()` (60 Hz): `ConstrainVelocity` on 3 axes and
  `ConstrainAngularVelocity` on 3 axes, target velocity = hand velocity
  (player movement included) + 0.8 × offset / dt, impulses clamped (2000 N,
  120 N·m). Contacts are solved by the engine at the same time: the copy goes
  through nothing. Offset measured while walking at 4 m/s: 1.5 cm; while
  rotating at 150 °/s, no visible lag behind the game's tool drawn at the
  hand.
* Slow frames (15-20 fps, 2 physics steps per frame): the hand velocity,
  computed over the frame's duration (55 ms) while the game only simulated
  33 ms, made the copy trail by about 10 cm and made the controller vibrate
  continuously. Since then: the target of each step = hand at the tick +
  actual movement of the player since the tick (`GetPlayerTransform` read in
  `update`) + the hand's own motion; the game's tool is shown at the hand +
  the copy's physical offset at the same instant of the simulation
  (`PHYS_SHOW_COPY = false`, the game's animations kept). Vibrations: only if
  the copy's velocity departs from the command (contact) near another shape.
  Measured on the simulated headset: displayed offset 3 mm while walking at
  4.5 m/s, at 72 and at 16 fps; 0 vibrations in the air; a 50 kg crate pushed
  with one vibration on impact.
* Collision layer 128; the player ignores it (the player's `CollisionMask`).
  The game's queries (shots, trigger strikes) ignore the game's tool but not the
  copy: with the trigger held or an object carried, the copy moves to layer 0;
  with the sledgehammer, also for 0.4 s of game time after each press (see [section 6](#6-aiming-and-the-tool),
  sledgehammer strike with the trigger).

## 6 bis. 3D level map

* The map has no camera of its own: it is mode 7 of the same camera controller
  (`*(game+0x60)`). It writes into the same fields (+0x1d0 position, +0x1dc
  rotation): target +0x128 (on the plane y = 0) + up × (distance +0x188 + 10),
  smoothed by a spring (+0x110 position, +0x11c velocity), fixed rotation of
  −90° around X (top view, north up).
* The draw forces a 90° field of view (16:9 convention) depending on the
  transition `+0x25c` (mode +0, previous mode +4), near plane 1 m, far plane
  500 m: `SetCameraFov` is ignored. Hence the "zoomed, distorted, flat" view in
  VR (58.7° vertical instead of ~110°, and 6.4 cm between the eyes from 70 m
  up).
* In VR (hook on `Camera::setTransform`): the projection takes the VR field of
  view (the camera's P[0], P[5], aspect ratio kept), the rotation stays the
  head's, the camera looks at the target from 50° above the horizon, and the
  separation between the eyes and the head movements are multiplied by
  `distance / 1.2 m`: the level appears as a scale model placed 1.2 m away.
  Opening and closing transition with the same curve as the game. Left
  thumbstick = move the target (+0x128), right thumbstick = zoom (+0x188); the
  game clamps both itself.
* Never call `SetCameraTransform` while the map is open: it switches the camera
  to mode 8 and freezes the map. `SetCameraOffsetTransform` also applies to
  the map (the Lua no longer calls it while the map is open).
* Patterns: `41 83 F8 07 75 ?? F3 0F 10 82 <transition> EB ?? 83 7A <previous> 07`
  (draw), `C7 87 <distance> 00 00 70 42 0F 14 C1 F2 0F 11 87 <position>`
  (`setMode`, zoom 60 on opening), `80 B9 … F2 0F 11 81 <target>` (target).
* Known limitation: `data/ui/map.lua` places its markers with
  `UiWorldToPixel` without testing whether they are behind the camera; a
  marker behind you can appear mirrored in the sky.

## 6 septies. Mod cameras and per-eye 2D interface

* `SetCameraTransform(t, fov)` (binding 0x48e9d0) writes into the camera
  controller `*(game+0x60)`: pose +0x264 / +0x270, field of view +0x2c4,
  flag +0x2cc. At the end of the controller update (0x36d00): flag set →
  mode 8, and the camera (+0x1d0 / +0x1dc) becomes the script's pose (smoothed
  by 0x322c0). The offset from `SetCameraOffsetTransform` (+0x1b0 / +0x1bc,
  flag +0x1cc, applied just before, at 0x3689a) is then lost. Without the flag,
  the previous mode comes back on the next frame. In mode 8, drawing uses the
  script's field of view: pattern
  `48 8B 56 60 44 8B 02 41 83 F8 08 75 ?? F3 0F 10 82 C4 02 00 00 0F 2F C7`
  (0x70caf).
* `GetCameraTransform` (0x475f80) reads +0x1d0 / +0x1dc: the game's camera
  (the mod's one), not the one the DLL renders.
* AC130 (Workshop 2401575709, `script/camera.lua`, `FlyCam`):
  `SetCameraTransform(Transform(gun, QuatLookAt(gun, target) × mouse), zoom × 10)`
  on every tick, 150 m up; the reticle is a 2D image at
  `UiCenter() / UiMiddle()`, the shells leave along `plane.barrel`
  (= the camera's rotation). Previously: the DLL kept the position of the mod's
  camera with the head's rotation in the room (log: `camerr ...
  ang=48..80`: the view looked at the horizon, the target 50 to 80° lower, with
  no head position).
* Now (`ScriptView`): the mod's camera, without roll, is the origin of a room:
  the head's direction at the start = the mod's view, the head looks around and
  leans, normal eye separation (`modCameraDepth` > 1: scale-model depth). If the
  previous frame's camera is less than 1.5 m from the player's camera (the Lua
  signals it, bit 16 of the request), it is a "first-person" camera mod: normal
  head view.
* Zoom (`ModZoom`, setting `modCameraZoom`): at the time of
  `Camera::setTransform` (call 0x70e8d), the projection has just been built
  (0x5ebcc0, call 0x70e72) with the mod's field of view (+0x2c4 in mode 8 if it
  is > 0, otherwise `SetCameraFov` +0x2c8; pattern 0x70caf):
  1/m5 = tan(fov/2) × 9/16. Before replacing it with the headset's projection,
  the DLL reads this 1/m5; magnification M = tan(30°) / tan(fov/2), clamped to
  [1, 16] (60° or more = actual size; AC130: 60° at the start, 5° at the
  tightest → x13.2). Applied after the off-centre projection: m0 and m5 × M,
  m8 / m9 kept (magnification around each eye's optical axis: infinity stays
  fused, disparity × M = the world seen M times closer). Smoothed (τ 0.12 s, in
  log space), computed on the left eye's frame and reused for the right eye;
  reset to 1 outside the mod's view, and a new view starts directly at the
  mod's zoom. Stabilisation: the head turns the camera by only 1/M (a "swing"
  rotation from the starting direction, roll kept in full): the magnified image
  stays fixed in the room and the reticle (`ApplyUiShift`, same projection)
  stays on the mod's axis. The AC130 zooms with the mouse wheel
  (`InputValue("mousewheel")`, ±0.5 level = ±5° per notch) and sets
  `game.input.locktool`: the wheel does not change tool. In a mod view, the
  right thumbstick sends one notch then, after 0.3 s, one notch every 0.12 s.
* Move-closer zoom (`DollyTravel`, setting `modZoomMode` 0, the default; the
  magnifier above = mode 1). Why the magnifier distorted the image (it looked
  like a fisheye lens whose curvature kept changing): a narrow rectilinear image
  spread over the headset's ~100° only stays fixed in the room at its centre.
  A point seen at a degrees from the axis follows the head by
  sin^2(a) (1 - 1/M^2) of its rotation (at 50°, x13: 58%, x2: 46%); the
  runtime's reprojection, which assumes a normal image, is wrong by that much
  on every frame (~1.3° at 150°/s); a zoom notch makes the centre grow faster
  than the edges (sin 2a / 2a = 0.56 at 50°); and depth is flattened M times
  (disparity × M = the world flattened along the axis: an object 10 m deep
  looks 0.76 m deep at x13).
  Now the camera moves forward along the mod's axis by (1 - 1/M) D towards the
  aimed point (D = distance along the axis; AC130 at x13.2, D = about 212 m: it
  moves forward 196 m, to 16 m from the point): this point is seen M times
  larger with true perspective, and the projection, the head's rotation and
  position, and the eye separation stay those of the headset (rigid world,
  exact reprojection). D comes from the Lua (`TDVRZ <m>` on every left-eye
  frame as long as a mod camera is far from the player): a ray along
  `GetCameraTransform(1)` (the mod's camera of the previous frame),
  `QueryRequire("visible")`, ignoring small objects (< 1.5 m: AC130 shells,
  debris) and fast-moving ones (> 5 m/s: debris thrown out or falling), and,
  within 4 m, the object surrounding the camera (the aircraft's fuselage), never
  the static scenery (`GetWorldBody`); the water surface is included
  (`QueryRaycastWater`). On the DLL side, D is tracked quickly (time constant
  0.1 s): the AC130's aircraft flies in circles (0.15 m per tick, and ticks run
  in both frames of a pair: ~27 m/s at 90 Hz) and the distance to a fixed
  target changes by up to ~15 m/s; at x13 the camera is only D/13 from the
  point, so a lag of a few metres made the target change by a third (with
  0.8 s: x9 to x15 over one circle). A closer surface counts immediately (the
  last 3 messages), a farther one = the farthest of the last ~8 (an object that
  merely crosses the axis is closer than the surface behind it), and the camera
  moves towards it at no more than 20% of D per second (aim leaving a roof).
  Something that blocks the axis behind the camera (aim passing over a roof):
  an instant jump, like a cut to a new shot, and the farther distances measured
  before are forgotten. The camera stays 2 m (× the relief factor) short of the
  farthest of the last 3. New view, or jump of the mod's camera (> 30 m): D is
  measured again; while it is unknown (only sky) or less than 6 to 8 m (× the
  relief factor, hysteresis), the magnifier is used.
  During the zoom only, the centre of the HUD is placed on the aimed point
  (`ApplyUiShift`): the AC130's reticle is seen on the target, at its
  distance, by both eyes (without zoom, the HUD stays at infinity or at the
  "HUD distance" setting, as before, for all mod
  cameras).
* 2D interface: `Game::frame` (0x71e94, every frame) rebuilds an orthographic
  "UI camera" at `game+0x2c8` (`Camera::setOrtho` 0x5ebf50:
  m0 = 2/width, m5 = −2/height, m12 = −1, m13 = 1); its view at +0x308 is the
  canvas → pixels scale (the canvas is 1920 wide). After
  `Camera::setTransform`, drawing copies it (0x711ae) into the 2D renderer
  (`*(game+0x30)+8`) then, per script, into `ui+0x18` before `draw()`
  (0x49eb20). `UiWidth` / `UiHeight` = the distance between the un-projected
  screen corners (0x4d4e5d, `ceilf`). `UiWorldToPixel` (0x4cf080) projects with
  the 3D camera (`game+0x240`) then un-projects with `game+0x2c8`.
* 16:9 pause of a level (`[Ecran] pauseWide`, on by default; round 19). The UI
  camera's view is W/1920 in both directions (H/1080 if W/H > 16/9): a canvas
  1920 wide and 1920 × H/W tall. In VR the window has the size of the headset
  render, almost square (3024 × 3264 → canvas 1920 × 2072). `menu.lua` draws
  inside `UiSafeMargins` (1920 × 1080 in the middle) over black: the main menu
  looks 16:9; the pause menu (`hud.lua` `pauseMenu`) takes the whole canvas
  (bottom bar at `UiHeight() − 80`) and the 3D view the whole window: a square
  virtual screen. While paused, a single-player game runs no `tick`
  (`Game::update` 0x7acec: `game+0x1e8` and `*(game+0x160)` = {1, 0} ⇒ end of
  the update): our Lua requests no eye, and the frame goes to the virtual
  screen. The DLL then draws it for a 16:9 screen, anamorphically
  (`WideDecide`, at the end of the update of an A frame: the frame's UI camera
  is already built, drawing comes after): canvas → pixels view =
  (W/1920, H/1080), hence a 1920 × 1080 canvas (`UiWidth`/`UiHeight`,
  `UiSafeMargins` and the mouse all go through this camera) stretched over the
  whole window; the game's 3D projection (its own field of view, not the
  eye's) at m0 = m5 × 9/16 (`Hook_CamSet`), stretched the same way. `vr.cpp`
  copies the whole backbuffer into the top 16:9 band of the virtual screen's
  image (`imageRect`; the rest is black): a 16:9 quad, PC proportions, square
  pixels, no render target resized, mouse unchanged (a point on the quad = the
  same fraction of the window). The game rebuilds both matrices every frame:
  the first frame after the pause is the VR view from before; the TAA history
  restarts from zero on both transitions (flag +0x23f1). How it was located:
  Game::frame 0x729cd `cmp dword [r12+8],6 ; jne ; cmp byte [r12+1E8h],0 ;
  jne ; call` and the test at 0x7acec (same pause field). Nothing changes in
  multiplayer (ticks keep running: the menu stays on the HUD, in stereo), in
  cinema mode (`forceScreen`) or in alternating-eyes mode.
* Off-centre projection: on the Quest 3 (VD), tan left 1.378, right 0.840,
  up 0.966, down 1.428 → the image centre is ~15° outwards and ~13° down, on
  opposite sides for the two eyes: a reticle at the centre was seen doubled.
  The DLL adds a per-eye offset to the UI camera's view (+0x308, translation in
  pixels): the canvas centre is seen in the wanted direction (straight ahead of
  the head, or the axis of a mod's camera), at infinity by default
  (`hudDistance`). `UiWorldToPixel` stays exact (same inverted matrix).
* Limits: the canvas keeps its size, so once shifted it goes outside the image:
  about 10% at the top in both eyes (a HUD element at the very top is no longer
  seen) and 12% on the nose side (that edge is then seen by the other eye
  only). A full-screen rectangle (fade to black, damage flash) leaves a band of
  about 12% on the outer side and 10% at the bottom of each eye.
* The eye synchronisation test (`debugSyncTest`) reads back the square that the
  Lua draws in the corner of the canvas: no shift during this test.
* Mod reticles on the aim point (`modCrosshairAim`): a reticle drawn at
  `UiCenter() / UiMiddle()` (the portal gun's logo, the chaingun's reticle)
  stayed straight ahead of the head while the weapon fires towards the laser's
  target. Each drawing function (`UiImage` 0x4c4a60, `UiImageBox`, `UiText`
  0x4cb790, and `UiRect` / `UiRoundedRect` / `UiCircle` + outlines via
  0x4d0330) computes the quad's canvas matrix (state `ui+0x2f8` × alignment),
  calls 0x4d1f90 `(ui, &M, &lo, &hi)` ("is this rectangle on screen?": view
  `ui+0x58`, screen `ui+0x10 / +0x14`, clip rectangle `ui+0x2d8`) then draws
  with the same M (`*(game+0x30)`: projection +8 × view +0x48 × M). The DLL
  shifts M inside this test for small quads (≤ 360 canvas units) whose centre
  is less than 160 units from the canvas centre: they go onto the laser's
  target, which the Lua sends on every left-eye frame (`TDVRT x y z`, relative
  to the eye), projected by each eye (seen at its distance). Exceptions: inside
  the `UiPush` scope in which the script called `UiWorldToPixel` (depth
  `ui+0xac08`, reset to 0 before each `draw()` at 0x49ec55, `UiPop` 0x4c8770),
  during `UiMakeInteractive` (0x4c73c0), mod camera, map, vehicle, head aiming.
  `UiImageAlpha` (0x4c4ef0) / `UiImageOverlayAlpha` (0x4c5a60) compute their
  draw constants before this test, and `UiTextInput` (0x4cc470) must stay under
  the mouse: their calls to the test (recognised by their return address,
  searched for within the function bounds given by `.pdata`) are never shifted.
  The `Ui*` bindings are not registered through `lea r8 ; lea rdx`: they use a
  static table `{implementation, name, next}` in `.data` (0xc44918 for
  `UiWorldToPixel`); UI context = `script+0x418`.

* Mods' interactive windows (`UiMakeInteractive` 0x4c73c0: writes the script
  into `game+0x860`, which `Game::frame` copies to `+0x868` and then resets to 0
  every frame; `game.cursor.enabled` = `+0x1e9` follows one frame late). The
  mouse of the `Ui*` functions: `MousePixel` 0x4f27a0
  (`(input, float out[2])`: position `+0x3f4/+0x3f8` + offset `+0x408/+0x40c`
  of the script's input copy `script+0x428`, global input `*(game+0x38)`),
  un-projected by the frame's UI camera (0x5ebb00). Eight calls in the block of
  `Ui*` bindings (UiGetMousePos, UiIsMouseInRect, UiGetCanvasMousePos,
  UiSlider, UiText, UiTextInput, the button helper 0x4be420, 0x4cf800). The
  engine draws no cursor in the HUD (it uses the Windows cursor, which is
  invisible in the headset).
* The problem in VR (THE EVERYTHING REVOLVER, Workshop 3183943578, "map" key
  = X):
  1. its bullet ring and its chambers are placed with
     `UiWorldToPixel(LnLGetBoneWorldTransform(weapon, 'main.con.cyl'))`, i.e.
     tool body × unscaled bone, while the DLL draws the weapon scaled by s
     around its muzzle M (M + s (p − M)). Gap (1 − s) × |p − M| =
     0.71 × 1.071 = 0.76 m, measured live (read-only): 0.761 m. With the weapon
     held in front of you, this point is behind the eyes: a mirrored ring, at a
     different place in each eye, chambers of negative size
     (size = 32 / distance);
  2. the same mouse position (window pixels) gives a different canvas point in
     each eye (per-eye shift of the UI camera, m8 = ∓0.242: about 465 canvas
     units, 25°): a dragged bullet drawn twice, a different hover in each eye;
     and nothing moved the mouse in game.
* Fixes (`ModUiPointer`, `WorldPixelView`, `Hook_MousePixel`):
  `UiWorldToPixel` reads its point (ArgVec 0x631a60), projects with the 3D
  camera `game+0x240` (projection 0x5ec3b0, then the world → eye view at +0x40,
  in columns; the UI camera follows at +0x2c8), and its "distance" is the depth
  along the axis of the camera controller `*(game+0x60)` (position `+0x1d0`,
  rotation `+0x1dc`). For a point inside the box of the unscaled tool, called
  by the tool's script (the one that calls `SetToolTransform`), the DLL moves,
  for the duration of the call, the view's translation (the point is seen where
  it is drawn, divided by s: same pixel) and the controller's position
  (M + (c − M)/s: room depth as the mod made it).
  During an interactive window: the canvas is converged at `modUiDistance`
  (1 m), the window script's markers are projected from the middle of the eyes
  onto this plane (m8 shifted like `ApplyUiShift`), the ray of the aiming
  controller (left or right: the one whose trigger was pulled last on the
  window, otherwise the dominant one) is intersected with the plane in the left
  eye's frame, and `MousePixel` (calls from the `Ui*` functions only) returns,
  in each eye, the pixel of that canvas point. The cursor (an orange square) is
  drawn by the DLL on each eye's image. The aiming hand's trigger = left button,
  its grip = right button; the left trigger no longer sends E.
* Revolver ring at the centre of the view (`ToolRing`, setting
  `modUiCenterTool`, on by default). Problem: during the bullet menu, the
  ring moved in every direction. Causes: its markers (bones
  `main.con.cyl` and `main.con.cyl.1..6`, LnL =
  `GetBodyTransform(GetToolBody())` × bone, read at `draw()` time) were
  projected where the weapon is drawn, i.e. in the hand: the ring followed the
  hand, turned, squashed or flipped over with the wrist, and the hand holding
  the weapon often aims at the window (a feedback loop). Now, during an
  interactive window of the tool's script (the one that has called
  `SetToolTransform` within the last 2 frames), a point inside the box of the
  held tool (`HeldToolBox`, scaled or not) is projected as on the PC screen:
  desktop camera E = tool body × inverse(flat), flat = the tool pose the mod
  gives for the screen (`g_scriptToolFlat`, smoothed like the engine does,
  frozen at the left eye and reused as is for the right eye). Seen point =
  flat × point in the body: independent of the hand. During the call, the DLL
  writes into the 3D camera `game+0x240` the whole view (+0x40) = inverse(E),
  and into its projection m0 = 2·V0·f/W, m8 = 1 − 2(V0·u0 + V12)/W,
  m5 = 2·V5·f/H, m9 = 2(V5·v0 + V13)/H − 1 (m1, m4, m12, m13 = 0), with V = the
  view of this eye's UI camera (`game+0x2c8`+0x40: canvas → pixels), W, H =
  2/P0, −2/P5 of this camera, f = 960 / tan(fov/2) (canvas units; fov =
  `options.xml` `<gfx><fov>`, the player's own setting, 90 by default, read
  again at each opening if the file has changed). The engine's path (static
  reading): 0x5ec3b0 = P × V × point, division by w, pixels =
  (x/w·0.5 + 0.5)·game width, (0.5 − y/w·0.5)·height; 0x74a10 (called only by
  this binding): bends the pixel around the window centre like the game's
  barrel distortion when `game+0x388` ≠ 0 (= `options.gfx.barrel`: the options
  loader 0x7b980 receives game+0x378 and writes this setting there at +0x10)
  and the map is not displayed; the DLL sets this flag to 0 during each "flat"
  call (interactive window, ring or not) and restores it afterwards (RAII):
  otherwise the same canvas point, at a different pixel in each eye, would be
  bent differently (two rings, off centre; the flag comes from a game setting,
  so it can be non-zero); 0x5ebb00 = back-projection through inverse(P × V) of
  the UI camera with the script's size (`ui+0x10 / +0x14`, assumed equal to
  the game's: the diagnostic prints both); finally the binding undoes the
  script's current UI transform (`ui+0x2f8`, inverted by 0x5ed870) and pushes
  its 3 results (0x630650 → lua_pushnumber 0x629910). Result, in canvas units:
  u = width/2 + f·x/−z − anchor, v = height/2 − f·y/−z − anchor: the same canvas
  point in both eyes. The camera controller (+0x1d0 / +0x1dc) becomes E: the
  distance (3rd result = depth along its axis, a constant vector rotated by
  0x30be0) is the desktop one, so the chambers have the same size as on the
  screen (32 or 20 / distance depending on the fov). Everything is restored
  after the call (RAII, even on a Lua error).
  Centring: the anchor = the first marker of the pair at the left eye (the
  cylinder, line 2008 of the mod), plus the smoothed (0.1 s) gap
  centroid − first marker of the previous pairs: the ring's centroid is at the
  canvas centre, which the converged canvas shows straight ahead of the head at
  `modUiDistance` in both eyes. The ring stays at the centre of the view when
  the head moves and no longer moves with the hand, and the mod's sway or its
  "choppy" 24 Hz steps (`choppy_update_rate`) move the whole ring together,
  so they change nothing. The pointer is unchanged (rays of both hands on the
  head-held canvas, grip = right click). The window's other markers (an aim
  point far away) keep the projection from the middle of the eyes. Menu key:
  the mod reads `map` (Tab, `toggle_cylinder`) and sets `game.disablemap` while
  it is in hand; X sends Tab (input.cpp, `A_Map`): X opens and closes the
  bullet menu, not the map.
  Limits: the first pair of a window is still drawn in the hand (the pair is
  decided before the first `UiMakeInteractive`, and the revolver projects its
  markers before that call). Deliberately not fixed: projecting this tool's
  markers "as on the desktop" outside the window as well would change its
  markers outside the menu (and those of any mod tool that has a window: a
  display placed on the weapon would move to the centre of the view for good),
  and no generic sign says, before `UiMakeInteractive`, that the window is
  about to open; during the 0.35 s opening, the mod's cylinder HUD is already
  sliding towards the weapon while fading out, and the ring appears there for
  one frame then moves to the centre. Another marker of the tool's script that
  falls inside its box (+5 cm) would be centred together with the ring; the
  canvas stays head-held (for the ring, this was intended in phase A: the other
  windows are panels fixed in the room, see below; since round 18: next
  paragraph).
  Ring fixed in the room (round 18, setting `modUiToolFixed`, on by default;
  `PanelToolPlace`). Goal: the menu opens straight ahead, at eye height
  (gaze heading, pitch 0), no longer moves when the head moves, and follows the
  player only when they move (it used to appear at the centre of the view even with the head lowered
  towards the weapon). The "tool" window is now a panel session like the menus
  (`PanelSession`, `PanelKindOn`), with its own placement: gaze heading at
  opening (`HeadingOf`, valid with the head lowered), pitch 0 (at eye height),
  at `modUiDistance`, upright and turned towards the eyes; anchor = canvas
  centre (where `ToolRing` centres the ring), frozen immediately;
  k = 2·D / (m0·canvas width), m0 = the left eye's projection: a canvas unit
  is seen as large as on the head-held canvas (the ring keeps its size, the
  options / skins / "?" flaps keep their angle). Markers on the held tool:
  `SetDesktop` first (same canvas point in both eyes), also outside the scope -
  the revolver computes `cylinder_pos` and the chambers (lines 2008-2011 of the
  mod) BEFORE its `UiMakeInteractive` (2226) and draws them inside the scope
  (ring 2661-3205, scope up to the `UiPop` at 3317): on the panel, around the
  anchor. The other markers follow the menus' rule (`SetPanel` inside the
  scope, each eye's own projection outside it). No converged canvas during
  this time (the ammo text and the mod's reticle, drawn before the scope, stay
  on the HUD). Following: never for a head rotation; the room carries it along
  with thumbstick movements and rotations; when the middle of the eyes is more
  than 0.25 m from where it was placed (a step, crouching), it slides by that
  amount (0.12 s) without turning, never during a trigger drag. Never
  remembered (no per-window place), never taken by a grip
  (`EngineModPanel::movable` = false: `GripTarget` and `ModPanelHighlight`
  ignore it; the aiming hand's grip remains the revolver's right click, the
  other hand's grip remains the game's); the left laser stops on it
  (`OffLaserStop`). Its 1st pair is drawn nowhere (panel rule; its markers are
  still in the hand in that pair). Unchecked (or `modUiCenterTool` unchecked,
  or panel failed): head-held canvas, as before. `PanelEnabled` is true with
  `modUiPanel` OR the fixed ring; a menu is a panel only with `modUiPanel`
  (`PanelKindOn`). Limit: what the mod draws outside the canvas (the options
  flap stowed below the bottom edge, y 1516 to 2616 on a 2072-high canvas) is
  seen in full, since the panel has no edge. Log: "mod panel opened (ui …, the
  window draws on the held tool ('…'): its ring fixed in the room, straight
  ahead at eye height - yaw +0 pitch +0 deg, … m; anchor 960 1036 of the …
  canvas; k …; centre … normal …)" (read by a mock-headset test helper, which
  looks for "mod panel … anchor … k …; centre …": hence "opened (ui", as for
  the menus; "mod panel opened again (…)" if a menu turns out to be the ring),
  "mod panel follows the player (moved … m) - the tool ring keeps its place in
  front of the eyes (…)", "mod window: the markers on the held tool '…' are
  drawn as on a desktop screen (…), centred on its panel fixed in the room",
  "mod panel closed (ui …, tool ring, … s; N eye draws on the panel, …
  hidden …)".
  Diagnostic (`debugLua`): every 300 frames, "Engine diag: mod tool ring"
  gives, for each eye of the same pair, the first marker, the centroid (canvas
  units) and the depth as the binding RETURNED them to the mod (read after the
  call at the top of the Lua stack: return object = {lua_State, number of
  results +8}, `top` at +0x10, 16-byte values of type 3; x, y passed back
  through the UI transform `ui+0x2f8`), the canvas centre, the largest gap
  between these returned values and the intended point (canvas, depth), the
  number of markers that could not be read (the intended point is then
  counted), the window and script sizes, and the barrel-distortion flag before
  the call. So it is not a model rebuilt by the DLL; only a capture image
  (disparity, ring position in each eye) additionally proves that the mod
  really draws at these points.
* Mod menus as floating windows (`ModPanel`, setting `modUiPanel`, on by
  default; round 16, phase B). Problem: the WEATHER menu of Natural Phenomena
  (Workshop 3261134951, opened by its button in the pause menu,
  `PauseMenuButton("Natural Phenomena")`; `draw()` calls `UiMakeInteractive()`
  at level 0 then draws a column of 370 × 995 canvas units, `UiImageBox` +
  `UiWindow(370, 995)` without clipping + `UiTextButton`, slid in from x = −240
  to 30 in 0.3 s, closed by a left click outside the column) was seen ~45° to
  the left, cut off by the right eye's image, and followed the head.
  Mechanism (read in the binary):
  - 2D rendering: `R = *(game+0x30)`; every frame, 0x711ae copies the UI camera
    `game+0x2c8..+0x350` into `R+8` (pixels → clip projection) and `R+0x48`
    (canvas → pixels view). Each `Ui*` draw builds its MVP on the CPU AT THE
    TIME of the call: (`R+8` × `R+0x48`) × M (0x5ef400), then copies it into
    its constant buffer: `UiImage` 0x4c4d38, button helper 0x4be420
    (0x4bef03), rectangle helper 0x4d0330 (0x4d09da), text 0x5e7160
    (0x5e73d2, 0x5e7483, 0x5e76ad), `UiImageAlpha`, `UiImageBox`,
    `UiImageOverlayAlpha`, `UiSlider`, `UiTextInput` (pattern
    `49 8D 48 08 49 83 C0 48`: 11 occurrences in the image, of which
    0x7c2822 / 0x7c2a2f are outside the UI; the text code loads `[R]`
    differently). Re-read at these sites and in the text code: the MVP is copied
    as is into the constant buffer / uniform (0x5c9b40), with no CPU
    computation on it. The UI shaders (uirect, ttext, unlit) multiply the
    vertex position (x, y, 0, 1) by this full 4×4 MVP uniform
    (`mubMvpMatrix`) and keep w, so a perspective projection gives a correct
    quad.
  - `Script::draw` 0x49eb20 (script, a2, a3, width r9d → `ui+0x10`, height
    [rsp+0x28] → `ui+0x14`, camera [rsp+0x30]): copies the camera into
    `ui+0x18` (projection) and `ui+0x58` (view), resets the `UiPush` depth
    `ui+0xac08` to 0 (49ec55, the only place in the game that resets it, pattern
    `48 8B 86 <ui> C7 80 <depth> 00 00 00 00`: this is how it was found), then
    0x4cf800 (clears the scissor: vtable+0x58, then the Lua `draw()`). Scene
    scripts: loop 0x71350-0x71392 (scene+0x280) via 0x46a0c0, camera =
    `game+0x2c8`; in-world screens: 0x467ef3, with their own camera already
    written into `R+8` (0x467e6b).
  - So replacing `R+8` for the duration of the draws of ONE script draws that
    script on a 3D panel, with no extra pass: `R+8 = Q[eye] × inverse(R+0x48)`
    ⇒ MVP = Q × M. The layout, the mouse (`MousePixel`, un-projected by the
    flat UI camera) and `UiWorldToPixel` stay flat and exact; the rest of the
    UI (the game's HUD, other mods, HUD parts of the same script) does not
    change. Rejected: moving M quad by quad in 0x4d1f90 (the button helper
    0x4be420 never calls this test: buttons would be missed), making the
    game's UI camera a perspective camera (breaks `MousePixel`,
    `UiWidth/UiHeight`, `UiWorldToPixel`, and the whole HUD would follow), an
    OpenXR quad layer fed from a render target (a lot of RE work on the D3D12
    renderer, kept for later).
  - Q (`PanelBuildQ`, on each eye frame with the VR room camera:
    `ApplyUiShift(…, room)`; not on the map, with a mod camera, or on the
    fallback frame without an anchor): panel = pose (centre c, rotation qp:
    +x right, +y up, +z towards the eyes) in the headset's recentred space,
    k = 0.8 mm per canvas unit × `modUiPanelScale`, anchor (au, av) at the
    centre. In the eye's view (room pose: left eye `g_leftEyePos/Rot`, right
    eye = left × (q, p / worldScale) of `g_eyeOffset`), a canvas point is
    A·u + B·v + C, A = conj(qe)·qp·(k, 0, 0), B = conj(qe)·qp·(0, −k, 0),
    C = conj(qe)·(c − e) − A·au − B·av; columns of Q:
    (m0·a.x + m8·a.z, m5·a.y + m9·a.z, 0, −a.z) for a = A, B, then 0, then C
    (clip z = 0 like the UI orthographic camera, w = −z; m0, m5, m8, m9 = this
    eye's projection). Checked numerically: Q = the direct projection of the panel point,
    W × V = Q, the ray finds the aimed point again, `SetPanel` returns the
    intended point.
  - Scope (`PanelBegin` / `PanelEnd`): from `UiMakeInteractive` (in a scene
    `Script::draw`, camera = `game+0x2c8`, Q built for this frame) up to the
    `UiPop` that leaves its `UiPush` level, or the end of `draw()`
    (`Hook_ScriptDraw`, RAII). The script's whole `draw()`: when its whole
    `draw()` is the window (`UiMakeInteractive` at level 0, nothing drawn
    before it in the previous pair: NP; hence also the 2 frames between its
    closing and the end of the session), for 0.4 s after such a window closes
    (NP's slide-out), and for a script that draws its window BEFORE
    `UiMakeInteractive` (empty scope: no quad tested and no mouse read by a
    button, although the script did draw before, 3 pairs in a row).
    `PanelBegin` refuses if `R+8..+0x88` ≠ the game's UI camera (another pass)
    or if `R+0x48` does not have the expected 2D form.
  - Guard: `R+8` is compared with the Q written at each quad test inside the
    scope and at its end; if it differs ⇒ "mod panel: renderer camera
    overwritten", the game's camera is restored, and the window behaves as
    before for this session (after 3 failed sessions: panels switched off until
    restart). `UiBlur` (0x4bdf40, table 0xc44978) also writes `R+8`: it saves
    `R+8..+0x88` (4be108), sets an identity camera for its full-screen pass
    (4be13b 0x5ebee0 / 4be144 0x5ec2a0), then restores the saved copy
    (4be355-4be37c): compatible (Q comes back; it does not call the quad test,
    so the guard does not see it). Magnetizer (2839238216, ui.lua:117-118)
    calls it right after `UiMakeInteractive`.
  - "On screen" test 0x4d1f90 (7 callers: `UiImage`, `UiImageAlpha`,
    0x4c5500 = `UiImageBox`, `UiImageOverlayAlpha`, `UiText`, `UiTextInput`,
    rectangle helper 0x4d0330): corners lo / hi of `ui+0x58` × M in the eye's
    pixels; refused if off screen (`ui+0x10/0x14`) or outside the clip
    rectangle `ui+0x2d8..0x2e4` (x0, y0, x1, y1 in pixels, y up; 0 = none;
    top = (int)(H − y0 − (int)(y1 − y0))). Inside the scope, a refused quad is
    drawn unless the clip rectangle refuses it (`PanelClipVisible`, same
    truncations): the off-screen test, which differs per eye, left holes in
    one eye only. The quads kept (left eye) make up the content box (except
    those covering 95% of the canvas).
  - Clipping: 0x4cf4e0 (ui, width xmm1, height xmm2, intersection r9b; called
    only by `UiClipRect` 0x4c00bf and `UiWindow(…, true)` 0x4cef8c): flat
    rectangle = `ui+0x58` × transform `ui+0x2f8`, intersected, stored at
    `ui+0x2d8`, then `R->vtable[0x50](R, x0, y0, x1, y1)` (scissor, y up);
    `UiPop` (0x4c8770) restores the saved rectangle: vtable+0x58 (none) or
    +0x50. The DLL hooks 0x4cf4e0 (found through the first call in
    `UiClipRect`) and, inside the scope, after the original and after `UiPop`,
    sends to vtable+0x50 the bounding box of the rectangle's 4 corners passed
    through canvas → Q → pixels (the whole screen if a corner is behind the
    eye). Limit: bounding box of a quadrilateral (rotated panel: the clip is a
    bit too wide).
  - Background blur (`UiBackgroundBlur` 0x4bdba0: `mov rdi,[rcx+0x418]`
    0x4bdbba, …, `minss`; `movss [rdi+0x3d0],xmm0` 0x4bdc22; the UI state is
    rebuilt at every `draw()` by 0x4d4a20, called by `Script::draw` 0x49ec66):
    it does NOT go through 0x4cf4e0. With `ui+0x3d0` > 0, the rectangle helper
    0x4d0330 (after the test 0x4d1f90, read at 0x4d04e4) blurs the FLAT
    location of the rectangle under a scissor it sets itself
    (`[ui+8]->vtable[0x50]` 0x4d074b, flat box in the eye's pixels), in two
    passes, then restores the flat clip rectangle `ui+0x2d8` (vtable+0x58
    0x4d0999 if none, otherwise +0x50 0x4d09c2); its callers already choose the
    "blur" shader from this value (`ui_circle` 0x4bfe57, `ui_circle_outline`
    0x4bffa2, 0x4c8c33, 0x4c8d80, 0x4c9df4, 0x4c9f5e) and 0x4d0b4c binds the
    blurred image. On the panel this gives a blurred ghost at the old
    (head-held) place, and the rest of a clipped window cut off at the flat
    location. The DLL sets `ui+0x3d0` to 0 at the start of the scope and after
    each `UiBackgroundBlur` inside it (`Hook_UiBackgroundBlur`, offset read
    from the binding's `movss`): plain rectangles, no blur. The `UiPush` levels
    stacked before the scope keep their blur (`UiPop` restores them). None of
    the mods tested uses it at the time of writing.
  - Markers inside the scope (`WorldPixelView::SetPanel`): 3D camera
    `game+0x240` = a camera at the middle of the eyes whose image plane is the
    panel (rotation = the panel's rotation in the world,
    `R = rendered camera × conj(room eye)`), projection
    m0 = 2·V0·(D/k)/W, m8 = 1 − 2(V0(au − ox/k) + V12)/W, m5 = 2·V5·(D/k)/H,
    m9 = 2(V5(av + oy/k) + V13)/H − 1 (o = the anchor seen from this camera,
    D = distance to the plane); camera controller = this camera (distance from
    the eyes); barrel distortion off. Same canvas point in both eyes, on the
    panel. Outside the scope, the window script's markers keep each eye's own
    projection (HUD parts).
  - Kind of window: a marker on the held mod tool, placed by its script
    (`HeldToolMarker`: `ToolRing`'s box test) in the same frame as
    `UiMakeInteractive` (the revolver projects its ring BEFORE that call) ⇒
    "tool": ring centred, on its own panel fixed in the room (round 18,
    `modUiToolFixed`, `PanelToolPlace`, see above), or on the head-held canvas
    (phase A, setting unchecked).
    The window of the held tool's script waits 0.3 s for its markers;
    otherwise it is a "menu".
  - Placement: opened straight ahead of the gaze (heading and pitch of the left
    eye, pitch clamped to −50..+25°) at `modUiDistance`, fixed in the room.
    Anchor = centre of the content box of the current pair (axis by axis if
    the window is smaller than 0.6 × the width / 0.75 × the height of the
    canvas; otherwise the canvas centre) for 0.4 s, then frozen (box stable
    for 3 pairs, or 1 s). The 1st pair of a session is drawn nowhere (`R+8` =
    a matrix that sends every vertex to clip (2, 2, 0, 1): out of view, zero
    area; the MVP only goes to the shader constants, see above): the anchor is
    only known once the left eye has measured the box, and NP's column would
    otherwise have flashed 0.8 m to the left (canvas centre). At most 3 pairs
    (a window made only of buttons: no box).
  - A window already seen at the same UI context address during the current
    game: its anchor, key and remembered place are applied immediately but are
    PROVISIONAL (`PanelCacheCheck`) — after a level change another mod can
    reuse the address, and a window can change its layout. From the 1st
    complete pair (before it nothing is visible) until freezing: same box size
    (±30; NP slides in x for 0.3 s); at freezing: same key. Otherwise the entry
    (and its "right button" mark) is forgotten, the anchor follows the box
    again, and the window goes back to where it would have opened.
  - It follows the player (sliding in 0.12 s) when the gaze HEADING is more
    than 10° away both from the content and from the heading its place is
    relative to (the gaze at opening, or at the last return) for 0.5 s
    (looking up or down does not count), or if the panel is closer than 0.3 m
    / too far; never while a ray is on it, a trigger is dragging on it or a
    hand is moving it. It then goes to the gaze heading + the remembered offset
    (straight ahead if it was never moved). Measured on the gaze alone, a
    window placed 45° to the side was "far" as soon as you looked ahead and
    followed every head movement (the original problem); with the offset
    added to the gaze, it ran away when you turned towards it.
  - Grip (vr.cpp, `GripTarget`; our own panels, settings and inventory, win
    under their ray: they are quad layers composited OVER the game image in
    which the mod's panel is drawn, so they are seen in front of it whatever
    the distances, as for the pointer and the trigger; the left laser,
    `OffLaserStop`, likewise): on the frame band (30 to 200 units around the
    content), by the hand that is not pointing at the window, or anywhere if
    the window has never read the right button; refused if that hand's trigger
    is held, or for the free hand when an object is under it ("TDVRH r n":
    "under the hand" bit sent by main.lua). The grip taken belongs to the panel
    until it is released (no right click, no game grab, no physical hand);
    dragging with the ray like our panels (0.35 to 3 m); the place is given to
    the engine for the next pair (the same for both eyes). Released: place =
    (heading − gaze heading at opening, pitch, distance), remembered per window
    (signature x0 y0 w h of the box, rounded to 10, tolerance 30; 16 at most,
    `modUiPanelPlaces`, written by `ConfigSaveKeys` off the game thread). If
    the window closes while it is held: released.
  - Right button read by the window: `InputPressed` 0x4b54a0, `InputDown`
    0x4b51b0, `InputReleased` 0x4b56a0 (`Ui*` table) are hooked; their 1st
    argument is read from the Lua 5.1 stack (args = {lua_State, number of
    arguments +8}; `top` at +0x10, `base` at +0x18; checked that
    top − base = 16 × count; a value of type 4 = string; TString: type +8,
    length +16, characters +24; under SEH). "rmb" or "grab" requested by the
    window's script ⇒ grip on its content = right click (THE EVERYTHING
    REVOLVER reads `grab`: emptying a chamber 2734, turning the cylinder
    2856-2857, loading everything 3056; NP reads only `lmb`, `menu_cancel`,
    `pause`). "Never read" requires proof: at least one key name of THIS script
    read during the session (`argSeen`; a single log line "key names read from
    the window's input queries"); otherwise (a reader that decodes nothing, a
    window that asks for no key) the safe rule still applies: frame band or the
    other hand.
  - Pointer: the rays are intersected with the panel's plane (front face),
    captured within the box + 200 units (a click next to NP = close, as on the
    desktop); cursor = Q[eye] × (u, v); no ray on it (and no drag in progress):
    the mouse is parked 300 units outside the box, with no cursor (a trigger =
    a click next to the window, never on the last hovered button; a press
    started next to it stays there until release, `pressParked`, even if the
    ray comes back onto the panel in the meantime); a blue square when the
    aiming hand's grip would take it, the corners of the zone marked in blue
    while a ray is on it. The left laser stops on the panel (OffLaserStop); the
    dominant hand's laser (Lua) goes through it.
  - HUD: during a floating menu, the canvas is no longer converged at
    `modUiDistance` (the HUD stays at infinity / `hudDistance`).
  - Limits: no depth (the panel is drawn over the scene: a closer hand appears
    behind it); a marker computed BEFORE `UiMakeInteractive` and drawn in the
    window keeps each eye's own projection (doubled); only one window as a
    panel at a time: a 2nd interactive window shown at the same time for 0.5 s
    (not a menu fading out while the other opens) puts BOTH back as before
    (canvas converged in front of the head, one mouse at the same canvas point
    for both; otherwise the 2nd one lost its convergence and received the
    panel's cursor without showing it); back on the panel 1 s after the other
    one disappears; no detection of the level change itself (the scene object
    `game+0x50` is not proven to be reallocated): it is the box check that
    protects the memory keyed by UI context address; place found again through
    the box signature (a window made only of buttons: unknown box, the whole
    canvas); on the 1st opening of a window in a game, it moves to its
    remembered place 0.4 s later (key known at the anchor); the text size may
    be chosen by the engine from the MVP (not verified). Optional layout in a
    virtual 1920 × 1080 (the VR canvas is 1920 × 2072): not done.
  - Log: "Engine: mod panel ready (script draw +49eb20, renderer +30/+8, 11 MVP
    sites; clip hooked +4cf4e0, rect ui+2d8, scissor slot +50; background blur
    left out at ui+3d0; right-button reads watched)", "mod panel opened /
    anchored / follows the player / placed / closed", "not the window shown
    before at this UI context", "another mod window is shown at the same time"
    (with the panel's centre and normal), "a menu … shown as a floating
    panel", "its window reads the right mouse button", "Panels: mod window
    taken by the … grip", "Panels: mod window let go", "Config: saved
    modUiPanelPlaces=…".

## 7. Render resolution

Teardown sizes its window and its render targets from the screen, at startup.
For a custom resolution, the DLL makes it believe there is a screen of that
size: `GetMonitorInfoA`, `QueryDisplayConfig`,
`GetSystemMetrics(SM_CX/CYSCREEN)`, the size of popup windows
(`CreateWindowExW`, `SetWindowPos`), the swapchain. `ChangeDisplaySettingsExA`
is blocked so that the real display mode is never changed. Resizing the window
during a game is not enough: the game does not recreate its targets.

* **Field of view covered** (`fovCoverage`
  = c, `fovCrop`, r18). Before: the game rendered the eye's field × c
  (tangents, `OffCenterFovFor` → m0/m5/m8/m9 in `Hook_CamSet`) into an image
  of the same size, and this narrower field was given to the runtime
  (`pv.fov`). VDXR passes it on (`layer.EyeFov.Fov`, frame.cpp), but with
  Virtual Desktop the player saw the image spread over the eye's whole field:
  a magnifier and "fisheye" effect (like the mod cameras' magnifier,
  [section 6 septies](#6-septies-mod-cameras-and-per-eye-2d-interface)), no black border, and no gain (same number of pixels). This was
  observed in the headset, not in VD's code (the mock headset does not
  composite the images). Now (`fovCrop=1`) the image sent always covers the
  eye's whole field (the 100% `pv.fov`, the one that works): the game draws
  the covered part, the DLL puts it in its place (`CropRect`: same z = -1
  plane, linear) and fills the surround with black — whatever the runtime.
  The window (render size, at startup) equals the chosen quality × c in each
  direction: same sharpness, c² of the pixels; the eye images keep the whole
  field (render / c). At the c used at startup, the rectangle (to within 1 or
  2 px from rounding) is set to the exact render size: a pixel-for-pixel copy,
  without the blur of resampling at 0.9996. Changing c in game moves the black
  border immediately (the game image is resampled into the covered part); the
  render size follows at the next launch. c is clamped to [0.5, 1] (beyond
  100% nothing more would be seen). `fovCrop=0`: as before.

## 7 bis. Free-hand arm, pointing hand

* The player's character is not drawn in first person: the tool floats in the
  VR hand. The free hand (left; right in left-handed mode) is drawn by the Lua
  mod (`tdvrOffArmTick`): two static bodies (`Spawn(xml, t, true)`,
  `collide="false"`, collision filter 0/0, `unbreakable`, ignored by the
  mod's rays) with the hand (`hand_l`) and the forearm (`arm_lower_l`) from
  `player/player_<character>.vox`, placed as in the character's prefab
  (`data/prefab/player/*.xml`, bone frame). Hand: the OpenXR grip pose
  ("grip": −Z into the fist, +X to the right, for both hands) becomes the
  character's `ik_hand_l` frame (+X forward, +Y along the fist, +Z to the
  right: the frame the game places on the tools' `tp_action_lh` handles);
  hand = ik × `ik_hand_l`⁻¹. Forearm: towards an elbow computed by two-segment
  IK (shoulder guessed below the head, 30 cm arm), wrist bent by 45° at most.
* Static bodies: the shape interpolation job (ShapeInterpolationJob 0xd5a50,
  every frame, for each shape with flag `+0xc8 & 0x60`) draws them at their
  transform (0x4de540: `body+0x40` position, `+0x4c` rotation, composed with
  `shape+0x44`; `+0x138` = 0 for a static body); only the shapes of dynamic
  bodies are interpolated (re-read in the disassembler). The tool in hand is
  also a static body moved every frame.
* Render-time correction: the Lua places the arm for the camera it requests
  (on every normal frame `TDVRE x y z`: that camera minus the hand's
  position; from time to time `TDVRW <hand> <forearm>`: the bodies' handles).
  The rendered camera is that camera shifted by how far the player moved
  after the tick (no physics, height smoothing). In `Renderer::render`, after
  the job (`Scene::waitShapeInterpolation`) and `FixObjectMotion`, the DLL
  shifts `+0x13c` of the arm's shapes by that difference (left eye; not if
  `+0x13c` still holds what the DLL wrote there: the job did not rewrite it);
  the right eye gets the same values (`+0x158`: those of the previous pair,
  with the per-eye history).
* Handle → entity: the engine's lookup (in the `SetShapeLocalTransform`
  binding, 0x496410): handle > 0 → `scene+0xa20[h]` (count `+0xa18`), < 0 →
  `scene+0x210[−h]` (count `+0x208`); the entity keeps its handle at `+0x0c`
  and its type at `+0x08` (1 body, 2 shape). Checked read-only on a running
  game (3901 entries out of 3901).
* Physical hand (`tdvrOffHandTick`, `tdvrOffHandStep` called by `update`,
  settings `offHandPhysics` / `offGrab`): like the tool's copy, an invisible
  dynamic body created once (a 10 cm `voxbox` centred on the grip pose, `dirt`
  brought down to 6 kg by `SetShapeDensity`, `unbreakable`; a material that
  does not burn) is pulled at each step towards the controller's grip pose (the
  player's actual movement included) by `ConstrainVelocity` /
  `ConstrainAngularVelocity`, capped like the sledgehammer's physical copy
  (2000 N, 120 N.m, 25 m/s, 40 rad/s; before 2026-10-11: 1.5 kg, 600 N,
  20 N.m, judged too weak; catch-up of the gap capped at 6 m/s). The drawn
  hand and forearm are placed at the controller + the body's offset at the end
  of the last step (same simulation instant); they remain two static bodies
  corrected by `OffArmLateFix`.
  Filter: layer 128, mask 127 (the player, their character and the tool's copy
  do not touch it); 0/0 ("ghost") when it is on the axis of the player's camera
  (12 cm: selection, shots fired from the eye) or on the axis of the other
  hand's aim (25 cm: shots fired from the tool) during a shot, that hand's
  trigger or the other hand's grip; within 25 cm of the object carried by the
  game; when the game's selection ray picks it (`GetPlayerPickBody`); then
  until nothing is inside its cube to within 2 cm (`QueryClosestPoint` at
  9 cm, `QueryCollisionMask(127)`, closest point compared with the cube in its
  own frame). Vibrations: the gap between the velocity obtained and the one
  the force allowed (like `physResidual`), near a shape whose mask contains
  layer 128.
* Grabbing: the free hand's grip always grabs (`input.cpp`, press = 2, except
  when the physical hand or grabbing is unchecked: it then makes you crouch);
  its trigger (L2) makes you crouch and Y interacts (E). (Before: the grip
  grabbed if the Lua had said within the last 1.5 s that an object was in
  reach, `TDVRH 1`, otherwise it made you crouch; `TDVRH` is still used for
  the log.)
  The current press comes back in ` ARM <arm> <physics> <grab> <grip>`
  (0 released, 1 crouch, 2 grab). The object (dynamic, 300 kg and 32 shapes
  at most, not a vehicle, not under the feet) is pulled to the held point
  (400 N, 50 N.m; position only if it is jointed to the scenery: a door
  swings); beyond 45 cm for 0.4 s it slips out of the hand. While held, its
  shapes move to layer 128, mask unchanged: the player and the hand go through
  it, the tool's copy and the other dropped objects touch it (they can be
  stacked). The original filters are kept in the `tdvr_lm` tag (value
  `<layer>_<mask>`) of each shape and restored as soon as the object no longer
  touches the player or the hand (`init` also restores them after a reloaded
  game).
* Free-hand laser (`tdvrOffLaserTick`, setting `offLaser`): drawn with
  `DrawLine` from the controller's aim pose (like the tool's laser, with a
  cross at the hit point), computed on frame A and redrawn as is on frame B
  (lines kept, same `DrawBodyOutline` outline). The DLL sends
  ` LAS <setting> <distance>`: where this ray meets one of our panels
  (settings; the inventory if the player is looking at it; a mod's window when
  this ray touches it, `EngineModUiOnWindow`, plane at `modUiDistance` in
  front of the eyes), in room metres from the aim pose, −1 = none; the Lua
  stops the laser there (× world scale). Since the panels are OpenXR layers
  composited over the image, the tip of the laser is hidden there: their own
  dot shows the selection.
  Grabbing from afar, like the game's grab (see "The game's grab" below; the
  old "pulled to the fist" behaviour, 25 kg at most, is replaced): a dynamic
  object with a positive handle, 10 t at most (doors and vehicles included),
  512 shapes at most, hit within 3 m of the aim pose (thin ray, otherwise a
  6 cm ray that stops before the first obstacle, point brought back onto the
  object by `GetBodyClosestPoint`): white outline, lit laser, the game's hand
  icon (`LoadSprite("ui/hud/crosshair-hand.png")`, 50 × 64, white: a black
  shadow offset underneath; `DrawSprite` without depth test, turned towards
  the head by `QuatLookAt`, seen at ~3°; kept for frame B). The grip takes it
  at the hit point (`grabFar`): that point is held at the end of a "stick"
  attached to the grip pose (`farOff`, grip frame), which slides along the
  laser's axis towards max(1 m, distance − 0.2 m) (half the gap per frame,
  0.5 m at most); with `grabKeepDistance` (the default) towards
  max(0.15 m, distance): it stays where it was taken. The turning thumbstick
  (up / down) moves this target between 0.15 m and 3 m, at the speed the DLL
  sends in ` ARM … <keep> <joystick>` (m/s, 0 when that thumbstick is on the
  other hand); while this hand holds something from afar, the Lua raises flag
  32 of its request and the thumbstick no longer changes tool (`input.cpp`,
  `GrabStick`: if both hands hold, the one that took last).
  Each step: target velocity = velocity of the stick's tip (player + hand +
  hand rotation × lever arm) + 0.1 × gap / dt (50 m/s at most), impulse
  clamp(mass, 10, 200) × (1 + gap/0.8, 2 at most) per 1/60 s step, rotation
  followed (impulse 0.5 × the same mass; position only for an object jointed
  to the scenery). Dropped if more than 3 m from its place, or when the grip
  is released (it keeps its velocity: throwing). The physical hand stays at the
  controller (ghost); the laser goes to the held point, with the icon on it.
  Snap turn / recentring: the object jumps with the hand (`carry`) if nothing
  is between them. A grip press only takes once (`pressT`).
* Pointing hand (`Input::PointerHand`): settings, VR inventory, virtual screen
  (menus, pause) and, with `offHandPointer`, the mods' interactive windows
  (`ModUiPointer`, `Input::TakePointer`) follow the same hand (another
  interface can report itself with `Input::ReportPointerHit`). The dominant
  hand by default; the other hand takes over ONLY when it pulls its trigger
  while aiming at an interface (that press is already its click, at its own
  ray). Otherwise the pointer stays with the hand that clicked last, even when
  only the other hand aims at the interface (2026-10-12, at the player's
  request: before, the only hand aiming took the pointer and the hands kept
  swapping it on their own); never during a press. Virtual screen: only the
  pointing hand's trigger clicks, and not a press started with the ray off the
  screen (it used to go to the real Windows cursor); a small grey square shows
  where the other hand is aiming. On the inventory, the pointing hand's trigger
  is no longer used by the game: `Inventory::CapturesTrigger` (dominant hand:
  no shot) and `CapturesOffTrigger` (other hand: no E).
* Floating panels (vr.cpp, "Floating panels": `UpdatePanels`,
  `UpdatePanelGrips`; 2026-10-11): the VR inventory and the settings / keys
  panel remain OpenXR quad layers in room space (composited by the runtime
  with the most recent head pose, the same for both eyes: synchronised stereo
  is not affected, no Lua cost). They no longer follow the head (before: the
  inventory followed its heading in 0.4 s, which proved uncomfortable).
  A single kind of placement for both (`FloatPanel`, `PanelPose` /
  `PlaceFromCentre`, exact round trip within the bounds): direction around an
  anchor, horizontal distance in front of the eye point, height below it; the
  panel stays upright, turned and tilted towards the eyes.
  - Inventory: anchor = the body estimated from the head (`g_body`), neck pivot
    11 cm below and 8 cm behind the eyes, followed with hysteresis (starts at
    20 cm horizontally / 8 cm vertically, then catches up to within 2 cm /
    1 cm of the neck: a step in the room or crouching carries it, leaning in to
    read does not). Fixed heading when the head turns; with `inventoryFollow`,
    a real about-turn (head more than `inventoryFollowAngle` = 55° from the
    body's heading and more than 50° from the panel for
    `inventoryFollowDelay` = 1.2 s, adjustable in the ini) turns the anchor
    towards the head (to within 5°). Placement: `inventoryYaw` (degrees, + =
    right), `inventoryForward` (0.20 to 1.00 m), `inventoryDown` (−0.40 to
    0.90 m). The anchor is reset to the neck and to the head's heading at each
    appearance (R3), on recentring and on recall: pressing R3 while the centre
    is more than 65° from the gaze, when putting it back in its place around
    the head's heading (same side: `inventoryYaw` is kept) brings it at least
    10° closer, brings it back (`VrInventoryRecall`) instead of hiding it.
    While hidden, its focus is cleared (`g_invFocus`): the first grip after it
    reappears does not test the spot where it used to be (nor during a pending
    recentre, `g_panelSnap`).
  - Settings / keys: opened straight ahead of the gaze (`PlaceUiPanel`) at
    `uiPanelDistance` (0.40 to 1.50 m) and `uiPanelDown` (−0.50 to 0.80 m), then
    fixed in the room; tilt atan2(height − 0.15, distance): vertical at the
    original place (0.90 m / 0.15 m, as before). The layers are drawn without
    depth test: distances are clamped so that a panel does not appear in front
    of a closer wall.
  - Grip: decided in `BeginXrFrame` right after `LocateHands`, under the same
    lock as `g_handsRaw` (the Lua thread never sees this press). Rising edge
    (0.6 / 0.4, like the game's grab) while this hand's ray is on the panel
    (inventory: pinned by R3 and looked at, 35° cone, like its trigger). A
    press started next to the panel stays with the game. The free hand only
    with `offHandPointer` and `offLaser` (an invisible ray takes nothing), and
    never the inventory when it touches an object (`Input::OffHandTouching`:
    `TDVRH 1` less than 1.5 s old - the Lua repeats it only every 4 stereo
    pairs, more than 0.3 s below 26 fps, and sends `TDVRH 0` as soon as it
    changes; ray on a panel ⇒ `o.lt` = 0 on the Lua side, hence "an object
    under the hand"). The test is shared (`GripTarget`). The press stays with
    the panel until its release is seen (`g_gripOwned`: grip at 0 in
    `Input::Update` and in the reply to the Lua - no game grab, no free-hand
    grab, no "ghost" hand); the Lua does not see this grip either while it
    rises towards 0.6 with the ray on a panel it can take, nor while it opens
    after the take, down to 0.25 (`g_gripQuiet`, unless it tightens again).
    The trigger of the holding hand does nothing until it is released after the
    take (`g_trigMasked`: whole fist clenched, no shot, crouch or click; ImGui
    pointer off the panel, button up; a key of the keys panel whose press
    started just before the grip is abandoned, `Ui::SetHeld` / `g_dropPress`:
    ImGui releases it elsewhere, otherwise `IsItemDeactivated` would have typed
    or locked it). A second hand clenched on the held panel is also kept away
    from the game, without taking the panel over.
    Movement: the grabbed point (distance along the ray, offset from the centre
    in the plane) stays under the ray; 2 passes (the tilt depends on the
    place); while held, the inventory's anchor is the neck itself (no
    hysteresis lag: the bounds seen during the move are those of the saved
    place, no jump on release). Without an XR frame, with the session not
    focused (also when `xrSyncActions` returns `XR_SESSION_NOT_FOCUSED` before
    the event: `g_actionsLive`) or with this controller's action state inactive
    (`g_handInput`: standby, disconnection, first sync after the headset menu;
    an inactive action reads 0, which is not a release): everything waits;
    after 0.5 s and more than 4 Presents without input (a single long game
    frame does not count) the take ends but the press stays with the panel
    (never a false release). Released: the inventory's anchor goes back to the
    neck (the place is measured again, the panel does not move); only these
    keys are written, on a separate thread, at most once per second
    (`ConfigSaveKeys`: `ConfigSave` rewrites the whole ini, 55 to 270 ms
    measured on the Present thread; a full `ConfigSave` cancels the pending
    batches, through a generation counter, otherwise the thread would write
    older values back). Vibration 0.35 / 40 ms on take, 0.15 / 20 ms on
    release; light yellow frame (inventory) or orange frame (settings) while
    held. Log: "Panels: ..." lines (%.3f m).

## 7 ter. The game's grab (right grip)

Static analysis (two independent passes + a verification), addresses = RVAs.
The original notes label this analysis "teardown.exe 2.1", while every other
section uses 2.0.1; that version label has not been re-checked.

* Aiming: `Player::updatePick` 0xc6590, every frame on foot: a thin ray
  (0xe9920, the same as `QueryRaycast`) of 3.0 m (0x982b60) from the player's
  camera (player+0x640 / +0x64c, `GetPlayerCameraTransform`), "physical"
  filter, without "player" or "visible". Point +0x94c, distance +0x948, shape
  +0x940 (only if its handle > 0).
* Can be taken (+0x964, `game.player.cangrab`): dynamic body (+0x138),
  mass = 1/[+0xfc] > 0 and ≤ 10 000 kg (0x989b74). No other condition: no
  tag, vehicle, joint-to-scenery or size check.
* `game.player.grabbing` (+0x968, held shape) is true only DURING a grab:
  `hud.lua`'s hand icon only appears then; `picking` = the ray hits anything
  physical.
* Take (0xba130, on the button press, not while it is held): held point = hit
  point (body frame, +0x790); distance +0x7ac = d0, target +0x7b0 =
  max(1.5, d0 − 0.2); every frame distance += clamp(0.5 × gap, ±0.5); mouse
  wheel ±0.25 within [1.5, 3].
* Anchor (0xc5080, every step): kinematic body +0x7b8, position smoothed
  0.8/0.2 towards camera + forward × distance, rotation = the camera's.
* Force (0xbcd50, every step, same constraints as `ConstrainVelocity`): gap
  e = anchor − held point; target relative velocity = clamp(0.1 × e / dt, ±50)
  per axis; impulse ±clamp(mass, 10, 200) × (1 + clamp(|e|/0.8, 0, 1));
  rotation: relative angular velocity 0, impulse ±0.5 × the same mass.
* Release: button released, death, vehicle, the object became static, held
  point more than 3 m from the anchor, `ReleasePlayerGrab`. Throw (the tool
  button during the grab, < 1000 kg): velocity = forward × (15 → 5 m/s
  depending on the mass).
* The target is not clamped only at the take: `Player::update` (0xc2360,
  +c2c28), on every frame in which the player holds an object (+0x788), adds
  the mouse wheel (0.25 m per notch) to +0x7b0 and clamps it to [1.5, 3]
  (`maxss` 1.5 at +c2c59, `minss` with 3.0 loaded at +c2c28); then the grab
  update (0xc56b0, called next, +c2fb7) moves +0x7ac towards +0x7b0 (half the
  gap, 0.5 m at most, +c5d3e); the anchor of each physics step (0xc5080,
  +c51a5) = camera + forward × +0x7ac. An object taken less than 1.7 m from
  the eyes was therefore pushed back to 1.5 m. Tool selection with the mouse
  wheel (0x44fb30, +450115) is ignored by the game during a grab.
* `grabKeepDistance` (`engine.cpp`, `InitGrabDistance` / `GrabControls`): the
  three instructions that read 0.2 (+ba1ec) and 1.5 (+ba1f4, +c2c59) read two
  floats of the DLL instead (rip-relative operands redirected to a page
  allocated within 2 GB of the image): in VR with the setting on, 0 and
  `grabMinDistance` (0.5 m from the eyes), otherwise 0.2 and 1.5 (the game's
  rule, same values). Before each `Game::update` of frame A, the turning
  thumbstick (up / down, `Input::GrabStick`, m/s) moves +0x7b0; the game clamps
  it to [minimum, 3] and follows it as it does with its mouse wheel. Take,
  force, throw and release remain the game's own. Log: "Engine: game grab -
  object taken, held at X m from the eyes".

## 7 quater. Active window and held buttons

* The game's window procedure (+4f7f60) keeps an "active" flag (window+0x18)
  driven by WM_ACTIVATE only: 1 if LOWORD(wParam) != WA_INACTIVE and
  HIWORD(wParam) == 0. `IsFocused` (+4f9450) reads it. As long as it is 0,
  every frame (+a6866 / +a74f3) shows the cursor, calls `ReleaseCapture`,
  calls +779b0 once, then clears the "down / pressed / released" arrays of the
  keys and buttons (+4f1fd0, 3 × 0x121 bytes).
* Mouse buttons come only from WM_xBUTTONxxx messages (+4f84c6); WM_INPUT only
  provides movement. The mod posts them once per press: with the window
  inactive (another window in front: Virtual Desktop, a notification, another
  program), a held button lasted only one frame (grab with the right grip,
  held fire, extinguisher). Measured on the simulated headset, with Notepad in
  front of the game.
* Fix (`input.cpp`, `g_vrFocus`): while VR is running, every WM_ACTIVATE
  reaches the game as "active", WM_ACTIVATEAPP(FALSE) and WM_KILLFOCUS do not
  reach it, and it receives an activation if it believed it was inactive; Alt,
  Tab and Windows are released when a deactivation is hidden; once VR stops,
  the game is told the truth again. When active, the game recentres the cursor
  on every raw mouse movement: `SetCursorPos` is refused as long as its window
  is not really in front. The real keyboard and mouse still only go to the
  window that is really in front (raw mouse without RIDEV_INPUTSINK, classic
  keyboard).
  `GetForegroundWindow` is used by the game only to bring itself to the front
  and to open a link.

## 7 quinquies. The game's in-world screens (the campaign's computer)

Static analysis of 2.0.1, checked by independent review passes; addresses = RVAs. Used by `InitScreens`,
`ScreenPointerUpdate`, `Hook_ScreenCursor` and `Hook_ScreenRender` (`engine.cpp`); every offset below is read from
the patterns at run time and cross-checked.

* A screen entity (`<screen>` in a level: size, bulge, resolution, script, tags `interactive`, `nocam`,
  `nocursor`, `noidlecursor`) runs a UI script into its own texture. The hub's mission computer: 0.90 x 0.50 m,
  bulge 0.03, 1920 x 1080, script `ui/terminal.lua` (version 1, all logic in `draw()`); hub_carib's: 0.70 x 0.40,
  no bulge.
* Fields: enabled `+0xb8`, interactive `+0xb9`, size (floats) `+0x64/+0x68`, resolution (ints) `+0x70/+0x74`,
  local transform `+0x48` (pos, quat), input `+0xd0` (an `Input`), player using it `+0x2258`, script `+0x2250`.
  Mesh: vertices `+0x24f0`, stride `+0x24e4`, position attribute offset (word) `+0x2286`, UV offset (word)
  `+0x22d6`, vertex count `+0x2508`, int32 indices `+0x2518`, index count `+0x2520`. The mesh is a 32 x 16 grid
  built at `0x468950`: x = (u - 0.5) w, y = (v - 0.5) h, z = bulge (1 - 2x²/w² - 2y²/h²), in the screen's own
  space (+z towards the viewer).
* World transform: `0x4682c0(screen, out)` (the one the renderer draws the mesh with, called at `0x8e046`) and
  `0x4683b0` (the touch hit test's; they differ only for a moving screen). Both: the parent shape's transform x
  the screen's local one.
* Sitting down: `Player::setScreen` `0xc1a50` (Lua `SetPlayerScreen`, from `data/script/terminal.lua` on Interact):
  player `+0x9a0` = the screen, screen `+0x2258` = the player. While it is set, the player's own controls get the
  global empty input (`0xc2612`-`0xc2628`).
* Camera glide: every frame `Game::frame` (`0x72eb7`-`0x72f59`) raises `game+0x358` by 1.2/s while the local
  player uses a screen without `nocam`, and lowers it by 2.4/s when none is used. With `nocam` it does neither
  (`0x72f06`): the tag freezes the glide. The camera update (`0x7b45c`) moves the camera toward `game+0x35c` (a
  point in front of the screen) by e = 0.5 - 0.5 cos(pi x glide), and the renderer draws the used screen again,
  sharp, with alpha = glide (`0x89aed`).
* `Screen::update` `0x4695d0` (from `Scene::update`, in `Game::update`): reads the cursor (`0x468330` /
  `0x468370`), copies the player's input (`player+0xae0`) into the screen's (`0x46966b`), resets the input offset,
  then cursor = clamp(cursor + raw mouse motion (`+0x3fc/+0x400`), 0, resolution) given as the event
  `{4, 0, x, y}` to `Input::handleEvent` `0x4f3e30` at `0x469a48` (return address `0x469a4d`; the touch path ends
  at the same call). The event type 4 writes the position `+0x3f4/+0x3f8`; the cursor read is position + offset
  (`+0x408/+0x40c`, set to 0 here). Last input device `+0x121c` (3 = touch).
* `Screen::render` `0x467dd0` (from the screens loop `0xeb160` in `Game::render`, for every screen not tagged
  `disabled`, not while paused): binds the screen's frame buffer, runs the script's `draw()`, then draws the cursor
  sprite at the input position. It runs in both frames of a stereo pair; it writes nothing into the screen object.
* Engine touch hit test `0x467900`: projects the mesh, finds the triangle under the touch, interpolates its UV:
  pixel = (u x width, (1 - v) x height).

## 8. Other landmarks (2.0.1)

| Item | RVA / offset |
|---|---|
| `g_game` | 0x1c6e270 |
| `Game::frame` | 0x71ce0 |
| `Renderer::render` | 0x87f40 |
| `Camera::setTransform` | 0x5ec040 |
| Player | `game+0x60` (camera override pos +0x264, rot +0x270, fov +0x2c4, flag +0x2cc) |
| Local player | `game+0xc0[game+0x168]`, `RequestFirstPerson` +0x2c92 |
| Lua binding `SetClipboardText` / `GetClipboardText` | 0x4b7120 / 0x4b3ed0 |
| `Platform` vtable: get/set clipboard | slots 18 / 19 |

## 9. Tools

* Python scripts (capstone, pefile, psutil) that read the game's memory:
  disassembly, search for calls/references, for instructions by operand, for
  strings, for vtable pointers.
* Hardware data breakpoints (`src/re_tools.cpp`, DR0 on all threads) to find
  out who writes a field.
* A trace of the GPU commands of one frame (`src/gpu_trace.cpp`) and a GPU hang
  detector (device-removed reason + the main thread's stack).
* Simulated headset (`debugFakeHmd=1` in `TeardownVR.ini`): a head that turns
  and moves, controllers on the keyboard (I/J/K/L, U/O, T/G, H, N, Y, M, B, V,
  Z). It bypasses OpenXR.
* **Mock OpenXR runtime** (`tools/mock_openxr`, CMake target
  `tdvr_mock_openxr`, not shipped with the mod): it runs the mod's real OpenXR code
  without a headset. It creates a real D3D12 session on the game's GPU,
  swapchains, Touch controllers and vibrations, paces frames at
  72/80/90/120 Hz and checks every `xrEndFrame` (layers, fields of view,
  poses, images). It also saves what the headset would display as BMP files
  (on demand, `dump=<n>`; every 8 s with `autodump=1`, see [6 quater bis](#6-quater-bis-performance-measurement-gains-with-no-visible-change)). To
  use it: `[Debug] openxrRuntime=<build>\Release\tdvr_mock_openxr.json`. The
  controllers are driven from the keyboard or through `mock_out\controls.txt`
  (`lx ly rx ry lt rt lg rg a b x y menu lclick rclick aimyaw aimpitch
  headstill dump autodump`). `tdvr_xr_probe` tests a runtime from the command
  line. The OpenXR loader's messages go through `OutputDebugString`.
* Diagnostics (`debugLua=1`): lost requests, gap between the rendered and the
  requested camera for each eye, roomscale movements.
