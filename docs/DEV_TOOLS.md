# Development tools

This page describes the tools used to develop and test Teardown VR:

- the mock OpenXR runtime;
- the runtime probe;
- the mod's logs and debug settings;
- the live control file;
- the development scripts in `tools/dev/`, including a helper for the game's own shader compiler.

None of them is shipped to players. They are provided as is, without support.

`<game>` is the Teardown folder, the one that contains `teardown.exe`. `<build>` is your CMake build folder,
`build/` in the commands of [BUILDING.md](BUILDING.md).

## 1. The mock OpenXR runtime (`tools/mock_openxr/`)

`tdvr_mock_openxr.dll` is a real OpenXR runtime: it exports `xrNegotiateLoaderRuntimeInterface`. It pretends to
be a headset, so the mod's whole OpenXR path runs without one. It creates:

- a real D3D12 session on the game's GPU;
- swapchains;
- frame pacing;
- Touch-like controllers;
- haptics.

It checks every submitted frame and can save what the headset would show as BMP files. Most of the mod was
developed and checked on it.

**Build it:**

```bash
cmake --build build --config Release --target tdvr_mock_openxr
```

This writes `<build>/Release/tdvr_mock_openxr.dll`, plus two manifests next to it:

- `tdvr_mock_openxr.json`: a headset is present;
- `tdvr_mock_openxr_nohmd.json`: the same DLL, but it reports "no headset".

The manifests name the DLL as `./tdvr_mock_openxr.dll`, and the OpenXR loader requires that `./`.

**Use it.** In `<game>\TeardownVR.ini`, add:

```ini
[Debug]
openxrRuntime=<full path to build>\Release\tdvr_mock_openxr.json
```

The mod then forces that runtime, without its automatic headset detection. Start the game as usual. To go back
to the real headset, remove the line, or leave it empty.

> Back up `TeardownVR.ini` before a mock session, and restore it afterwards. The mock's field of view rewrites
> `[Affichage] vrAspect` (about 1.02 with `offCenter=1`, 1.155 with `offCenter=0`). That value is then used at the
> next game start with the real headset.

### 1.1 What it emulates

| Item | Mock value |
|---|---|
| Runtime name | `TDVR Mock Headset`. The mod looks for "Mock" in the name: it then allows the test shape of `hiddenAreaFill=2` and sets bit 32 of the Lua debug flags, which turns on mock-only test helpers in `main.lua`. |
| System | `Mock Quest 3 (test)`, vendor id `0x2833` |
| Extensions | `XR_KHR_D3D12_enable` and `XR_FB_display_refresh_rate` only (no visibility mask) |
| View configuration | `PRIMARY_STEREO`, recommended 2064 x 2208 per eye |
| Field of view (radians, left / right / up / down) | left eye -0.94 / 0.77 / 0.84 / -0.87, right eye -0.77 / 0.94 / 0.84 / -0.87 |
| Eyes | ±0.032 m from the head |
| Spaces | `VIEW`, `LOCAL` (1.65 m) and `STAGE` |
| Swapchain formats | `R8G8B8A8_SRGB`, `B8G8R8A8_SRGB`, `R8G8B8A8`, `B8G8R8A8` |
| Refresh rate | 72 Hz by default; 72 / 80 / 90 / 120 Hz through `XR_FB_display_refresh_rate`. Frames are paced on a high-resolution waitable timer. |
| Session states | `IDLE` -> `READY` on creation, then `SYNCHRONIZED` -> `VISIBLE` -> `FOCUSED` on begin |
| Head | Moves slowly (yaw ±25°, a small walk) unless the control file scripts it |
| Hands | In front of the body; the right hand sways |

### 1.2 Driving it

**Keyboard.** The keys work only while a window of the game process is in front:

| Key | Control | Key | Control |
|---|---|---|---|
| I / K / J / L | left stick | H | right trigger |
| U / O | right stick X (turning) | N | right grip |
| T / G | right stick Y | Y | left trigger |
| M | A | B | B |
| V | X | Z | Menu (left) |
| P | left stick click | | |

**Control file.** The file `mock_out\controls.txt` sits in the `mock_out` folder next to the mock DLL. Each line
is `name=value`, and names are lowercase letters only. The mock re-reads the file whenever its write time
changes. Values stay in force until the file changes, and deleting the file releases everything. With the file,
a test can drive the controllers without keyboard focus.

| Keys | Meaning |
|---|---|
| `lx ly rx ry` | Sticks, -1..1 (added to the keyboard) |
| `lt rt lg rg` | Left / right trigger and grip, 0..1 |
| `a b x y menu` | Buttons (`x` and `y` are the left controller's) |
| `lclick rclick` | Stick clicks |
| `aimyaw aimpitch` | Scripted right-hand aim, in degrees |
| `aimrate` | Speed (degrees per second) at which the scripted aim moves toward `aimyaw` / `aimpitch` |
| `aimsway=1` | Small tremor on the scripted aim |
| `hx hy hz` | Right-hand offset, in metres |
| `laimyaw laimpitch` | Scripted left-hand aim, in degrees |
| `headstill=1` | Head fixed at (0, 1.65, 0) |
| `headyaw headpitch` | Direction of the still head, in degrees |
| `hjit` | Tremor of the still head, in degrees, like a real headset |
| `dump=<n>` | Each **new** value of n saves the next frame (see 1.3) |
| `autodump=1` | Also saves a frame every 8 s (the first 40) |
| `vdhold=<ms>`, `vdholdevery=<s>`, `vdholdburst=<n>` | Simulated streaming holds: every `<s>` seconds, `xrEndFrame` blocks for `<ms>` ms, for `<n>` frames in a row (like Virtual Desktop waiting for its stream) |
| `nohmd=1` | No headset: `xrGetSystem` fails with `XR_ERROR_FORM_FACTOR_UNAVAILABLE`. `nohmd=0` "connects" it again. |

### 1.3 Snapshots

`dump=<n>` saves the next frame into `mock_out\`:

- for a stereo frame, both eyes: `<100+n>_eye_left.bmp` and `<100+n>_eye_right.bmp`;
- every quad layer as `<100+n>_quad<i>.bmp`.

Quad layers are the virtual screen, the settings panel and the VR inventory. The mock log gives each quad's size
and position. For example, `dump=5` writes `105_eye_left.bmp`, `105_eye_right.bmp`, and so on.

`autodump=1` saves numbered frames every 8 s. Each save blocks `xrEndFrame` for 55-76 ms, which shows up as fake
hitches in the mod's log. Leave it off for any timing measurement.

### 1.4 Validation and statistics: `mock_out\mock.log`

- **Validation.** Every `xrEndFrame` is checked, and the first problems found (about 40) are logged as
  `PROBLEM: ...`. The checks cover:
  - call order and swapchain acquire / release;
  - the number of views and the image rectangles;
  - field-of-view sanity and normalised quaternions;
  - both eyes using the same image, the blend mode and unsupported bindings.
- **Statistics.** Every 5 s, a statistics line gives:
  - frames per second, at the refresh rate in use;
  - the count of projection, quad and empty frames;
  - the eye image size, the distance between the eyes, both eyes' fields of view, and the haptic pulses per hand.
- **Restarts.** The log restarts at the first load in a game process. When the mod's headset detection loads and
  unloads the DLL again in the same process, lines are appended (environment variable `TDVR_MOCK_LOADED`).

### 1.5 Testing the automatic headset detection

Never test `[Affichage] headsetRuntime=0` (automatic) with the real runtimes on a development PC. It would ask
Virtual Desktop's and Meta's runtimes, and asking SteamVR's starts SteamVR. Give the mod a test list instead:

```ini
[Affichage]
headsetRuntime=0
[Debug]
openxrRuntime=
xrAutoRuntimes=<build>\Release\tdvr_mock_openxr_nohmd.json;<build>\Release\tdvr_mock_openxr.json
```

The first entry stands for Windows' active runtime. Here, the same mock is first "without a headset", then with
one. While the game runs, `nohmd=1` / `nohmd=0` in `controls.txt` removes or connects the second one's headset,
to test re-detection.

### 1.6 Limits of the mock

- **Never judge stutter on the mock.** It paces frames itself and has its own hitches.
- It does not composite layers the way Virtual Desktop does. Some image problems were only visible in a real
  headset.
- Its field of view gives a disparity at infinity (about 517 px) close to the 512-px period of the game's blue
  noise. Before stereo-correlation tests on screen-locked noise, use the real headset's field of view and eye
  offsets, or the results can show false positives.

## 2. The probe: `tdvr_xr_probe`

```bash
cmake --build build --config Release --target tdvr_xr_probe
build\Release\tdvr_xr_probe.exe [runtime.json]
```

The probe goes through the same static OpenXR loader as the mod. It:

1. sets `XR_RUNTIME_JSON` to the given manifest (no argument: Windows' active runtime);
2. sets `XR_LOADER_DEBUG=all`;
3. lists the runtime's instance extensions;
4. creates an instance with `XR_KHR_D3D12_enable`;
5. prints the runtime name, then `xrGetSystem -> <code>`. `0` means a headset is available.

The development scripts use it **before launching the game**. The VR render resolution is chosen at game start,
so a game started while the headset is off runs at low resolution for the whole session. Launch only when the
probe prints `xrGetSystem -> 0`, and only with the game closed. Probing SteamVR's runtime starts SteamVR.

## 3. The mod's logs

### 3.1 `<game>\TeardownVR.log`

The current session's log. Lines start with the time since start, in seconds, and the thread id. They are
formatted by the calling thread and written by a background thread every 50 ms, or sooner past 64 KB. Severe
exceptions and hang reports are written at once. `[Debug] logSync=1` writes every line immediately on the
calling thread, which helps with a crash that kills the process.

What to look for:

| Line prefix | Content |
|---|---|
| `IAT hook ... NOT FOUND` | An import-table hook (clipboard, input, window) could not be installed |
| `XR: ...` | Runtime detection (`XR: headset detection ...`), instance, system, session, swapchains, refresh rate, recentering |
| `Engine: ...` | What the engine scan found, with offsets. `... not recognised ...` names a feature that turned itself off and what happens instead. `Engine: synchronised stereo ready` means the core works. |
| `Frame B: ...`, `Perf: ...` | The cheaper right-eye frame and the performance features: what is active, fallbacks, control-file changes |
| `Particles: ...`, `TAA: ...` | Stereo particles and VR anti-aliasing. In a build without the game-derived shaders (`TDVR_GAME_SHADERS=OFF`, every public build) the log says `TAA: this build has no VR anti-aliasing shaders ...` and `Particles: this build has no stereo-particle shaders ...`: that is the build, not the game. |
| `Stereo frames: ...` | Every 600 presented frames (see below) |
| `Slow frame ...` | A frame with more than 18 ms of work, at most 6 per 600 frames, broken down by section |
| `Lua: ...` | Diagnostic lines sent by the Lua mod |
| `Panels: ...`, `Inventory: ...`, `Input ...` | Floating panels, the VR inventory and controller-to-input diagnostics |

The `Stereo frames` line reports:

- how many left, right and other (non-stereo) frames were presented;
- the game and headset frame rates, and the average milliseconds per frame kind;
- the average `xrWaitFrame`, the worst frame, and the frames over 20 ms;
- slow game frames, long waits and physics catch-ups dropped;
- headset refreshes missed, including those while the runtime held the frame;
- frame-B, performance and stream statistics when those features are on.

In a level with synchronised stereo, `left` and `right` should be equal and `other` close to 0.

### 3.2 Other log files

- `<game>\TeardownVR_journaux\` ("journaux" means "logs"). At each start, the previous `TeardownVR.log` is moved
  there as `TeardownVR_<date>_<time>.log`. The 10 most recent are kept, and empty logs are skipped.
- `<game>\TeardownVR_xrloader.log`. With `[Debug] debugLua=1`, the OpenXR loader's own messages
  (`XR_LOADER_DEBUG=info`) are redirected there. The loader also writes to `OutputDebugString`, which a debug
  viewer can capture.
- The installer's **"Prepare my logs"** button (or `/logs`) writes a zip `TeardownVR_journaux_<date>.zip` on the
  Desktop. It contains:
  - `TeardownVR.log` and the archived logs;
  - `TeardownVR.ini`;
  - `infos_PC.txt`: Windows version, CPU, RAM, GPUs and driver, active OpenXR runtime, and the sizes and dates of
    the game and the DLL.

  Nothing is sent anywhere. The zip describes the PC, and file paths in the logs can contain the Windows user
  name, so check it before sharing it.

### 3.3 Debug settings (`[Debug]` in `TeardownVR.ini`)

These keys are not in the settings panel. Most of them cost CPU, so leave them off for normal play.

| Key | Effect |
|---|---|
| `debugFakeHmd=1` | Simulated headset **without** OpenXR. The head turns and walks; controllers on the keyboard (same keys as the mock). Useful for Lua work without any runtime. |
| `debugLua=1` | Lua diagnostics: lost requests, the camera gap per eye, roomscale moves, the clipboard bridge trace when a request is lost, and the OpenXR loader log |
| `debugSyncTest=1` | Checks synchronised stereo. The Lua mod draws a red marker for the left eye and a blue one for the right, and the DLL reads the pixels back. Logs `SyncTest: <n> frames, marker ok=.. wrong=.. none=..`. It also saves 8 presented frames as `<game>\TDVR_frame<i>_eye<e>.bmp`. |
| `debugFovTest=1` | Measures the field of view actually rendered (test lines), and also saves 8 frames |
| `debugNoAim=1` | The Lua mod no longer turns the player toward the controller's aim (diagnostic) |
| `debugProfile=1` | A sampler thread records the main thread's call stacks, and each `Slow frame` line lists the costliest call chains |
| `debugGpuTrace=1` | Logs every GPU command of one frame with its calling site in the game (used to map the renderer's passes) |
| `debugWatchCamera=1` | Example use of a hardware data breakpoint: logs every instruction that accesses the camera position, during a short window. **Teardown 2.0.1 only**: it reads the game object at the hard-coded 2.0.1 RVA `0x1c6e270` (`src/vr.cpp`, `VrHandleLuaRequest`), with no pattern and no check. On another game version it reads a wrong address; update that RVA first ([UPDATING_FOR_A_NEW_GAME_VERSION.md](UPDATING_FOR_A_NEW_GAME_VERSION.md), section 3.3). |
| `particleMotionLog=1` | Counters of the right-eye particle motion feature, once a minute |
| `perfTrace=0/1/2` | Performance measurement (3.4) |
| `logSync=1` | Synchronous logging (3.1) |
| `openxrRuntime=<json>` | Forces an OpenXR runtime (the mock, for example) |
| `xrAutoRuntimes=<a.json>;<b.json>` | Test list for the automatic headset detection (1.5) |

The DLL also always runs a hang detector: if the game freezes, it logs the GPU status and the main thread's stack.

### 3.4 Performance measurement: `perfTrace` and `perfDetail`

`[Debug] perfTrace` only measures: each hook calls the game's function unchanged. If one of its patterns is not
unique, that measurement is left out and the log says `NOT hooked`.

- `0` (default): nothing is searched or hooked for measurement.
- `1`: per-eye sections (update, fixed-step physics, particles, the mod's own work, render, VR submit, present,
  headset wait), plus main-thread waits (jobs, GPU fence, display, shapes), physics steps per frame, the engine's
  own profiler categories and CPU per thread group. The details are appended to `Stereo frames` and
  `Slow frame` lines.
- `2`: everything in 1, plus each mod's tick time and a census of the Lua bindings each mod calls.

`[Performance] perfDetail` (bits, only with `perfTrace` 1 or more; default 7):

| Bit | Measures |
|---|---|
| 1 | Blocking job waits and GPU fence waits by caller; async upload sizes |
| 2 | Every job, timed by class |
| 4 | GPU busy / idle per frame (timestamps around each submission) |
| 8 | GPU time per render pass |

Check that measuring does not change the result: between `perfTrace` 0 and 1, the median work per stereo pair
should not move by more than about 0.1 ms. **Never open the game's own profiler (Shift+Alt+F3) during a
measurement.** It switches the game to one update per frame, limited to 60 fps.

## 4. The live control file: `<game>\TeardownVR_perf.txt`

An optional text file for tests and A/B comparisons. Its `key=N` lines override the matching settings **while the
game runs**. The file is re-read when it changes, at most once per second. Deleting it gives the `TeardownVR.ini`
values back. Each change is logged (`... control file TeardownVR_perf.txt sets ...`).

| Key | Values |
|---|---|
| `frameBMode` | 0 full update in frame B, 1 skip clean mods, 2 render-only frame B |
| `frameLatency` | Desktop swap chain frame latency during the session |
| `lazyBeginFrame` | 0 / 1 / 2 |
| `hiddenAreaFill` | 0 off, 1 runtime lens mask, 2 test shape (mock only), 3 lens mask in pink (test) |
| `gpuPriority` | The game's GPU scheduling class (experimental) |
| `parallelParticleSort` | 0 / 1 / 2 (2 = also checked against the serial sort) |
| `skipShapeJobsB` | 0 / 1 / 2 |
| `skipDeadPasses` | 0 / 1 / 2 (2 = with a GPU check) |
| `stereoSameNoise` | 0 / 1 |
| `stereoParticles` | 0 none, 1 particles and lighting, 2 particles only, 3 lighting only |
| `taaMode` | 0-4, or 8 = no anti-aliasing (test only) |
| `stereoParticleMotion` | 0 / 1 |
| `particleMotionLog` | 0 / 1 |
| `stereoParity` | Checkerboard parity variant of the stereo particle shader |
| `jobSpin`, `jobSpinUs`, `mainSpinUs` | Short spin before idle threads sleep (timing experiment) |
| `perfDetail` | 0-15, as in 3.4 |

The keys of the frame-B list (all but the last two rows) take a **single digit**.

Some hooks are only installed when they are wanted **at game start**. This is the case for frame B, job spin and
the lazy beginFrame stubs, and for `hiddenAreaFill`, whose lens-mask extension is requested when the OpenXR
instance is created. If the ini has them off, the file must already exist, with the key, when the game starts.

Interleave the runs (A B B A) and compare medians. Settings such as `sharpen` are not control-file keys: set them
in the ini before starting.

## 5. Development scripts (`tools/dev/`)

These are the scripts used during development. They work on the player's own install and on the mock, read the
game's memory read-only, and analyse captures and logs. They contain no game data. Each script's header gives its
exact arguments.

**Conventions:**

- The game folder comes from the environment variable `TEARDOWN_DIR`. It defaults to the default Steam location,
  `C:\Program Files (x86)\Steam\steamapps\common\Teardown`.
- Python is the one on `PATH`, or `PYTHON`.
- The repository root is found relative to the script.
- The mock's output folder is `TDVR_MOCK_OUT`, which defaults to `<repo>\build\Release\mock_out`.
- `TD_BB_W` / `TD_BB_H` give the game's back-buffer size, which `mockctl.py point` needs to place a 1920 x 1080
  menu pixel on the letterboxed virtual screen. They default to 1544 x 1600. `golevel2.ps1` and `stress_run.ps1`
  set 1920 x 1080 for the commands they run. When you call `mockctl.py point` yourself, set them to the size the
  game renders at.
- Python scripts need Python 3.10+ with `numpy`, `scipy`, `pillow`, `capstone`, `pefile` and `psutil`, as
  each one requires.
- Logs and outputs go to git-ignored folders.

### 5.1 `tools/dev/mock/`: driving the game on the mock

| Script | What it does |
|---|---|
| `mockctl.py` | Writes `controls.txt` for the mock; values persist between calls. Commands: `point <x> <y> [click_s]` aims the right controller at a pixel of the virtual screen (1920 x 1080 coordinates) and can click; `pointui` aims at a pixel of the settings panel; `pointinv` / `draginv` aim at, click or scroll the VR inventory; `pointpanel` aims at a mod menu shown as a floating panel (position read from `TeardownVR.log`); `set name=value ...`; `hold name=value ... <s>` sets values for some seconds; `snap [name]` captures the next headset frame; `clear` releases everything. |
| `aimfrac.py` | Aims at a fraction (u, v) of the virtual screen, then optionally clicks |
| `findlastbutton.py` | Finds a button of the pause menu on a capture (an example of image-guided menu navigation) |
| `cycle.ps1` | The development cycle: saves the previous session log, closes the game, copies the built DLL and the Lua mod into the game folder, relaunches the game (only when the probe says a headset is available), and waits until the mod is ready. `-focus` brings the game to the front. |
| `golevel2.ps1` | Restarts through `cycle.ps1` and enters a sandbox level on the mock through menu clicks. It snapshots the active mod list and restores it if it changed. A failed attempt restarts the game instead of clicking blindly. The ini must already point to the mock. |
| `goto_tool.ps1`, `cycle_tools.ps1` | Cycle through the tools with the right stick until a named tool is held, or snap every tool |
| `wait_idle.ps1` | Read-only: waits until the game is closed, or until the headset has been still for a while, before tests run |
| `focus.ps1` | Brings the game window to the front (never while someone is using the PC) |

### 5.2 `tools/dev/analysis/`: image metrics on captures

| Script | What it does |
|---|---|
| `stereo_disp2.py` | Disparity of image regions between the left and right captures of one stereo pair (normalised cross-correlation, sub-pixel). It checks that both eyes see the same thing at the right depth. |
| `ncc_tiles.py` | Tile-by-tile correlation between the eyes: the "grain" metric used for particle rivalry |
| `imgdiff.py` | Image equivalence between modes, for example `frameBMode`. It compares differences within one mode (noise) with differences between modes. |
| `sharpness.py` | Sharpness (variance of the Laplacian) of scene regions |
| `taa_metrics2.py` | Anti-aliasing captures compared with a supersampled reference: detail kept, and halo overshoot on edges |
| `cap.ps1` | Captures the left eye on the mock for a given render preset, `taaMode` and head tremor. **It edits the live `TeardownVR.ini`**: back it up first. |

### 5.3 `tools/dev/perf/`: performance runs and log analysis

| Script | What it does |
|---|---|
| `parse_logs.py` | Summarises the `Stereo frames` statistics of one or more session logs (read-only) |
| `analyze_run.py` | Per-phase analysis of a stress run, or of any `TeardownVR.log`: work per eye, budget, hitches, missed refreshes, physics catch-ups and slow-frame breakdowns. It can compare runs. |
| `thread_sampler.py` | Per-thread CPU of `teardown.exe`, sampled from outside: read-only, nothing injected, no thread suspended |
| `run_matrix.ps1` | Interleaved runs of several control-file configurations on the mock. Each run waits until the PC has been idle for 45 s. |
| `stress_inputs.py` | A fixed, timestamped input schedule for the mock: calm, then heavy destruction, then calm again |
| `stress_run.ps1` | A full CPU stress test on the mock. It restarts the game, enters a level and fires along a fixed fan of aims, while recording the log, per-thread CPU and system counters. It then restores the ini, the game options and the mod list. It asks for confirmation and must not run while someone plays. |

### 5.4 `tools/dev/lua/`

| Script | What it does |
|---|---|
| `luacheck.py` | Static checks of `main.lua` without a Lua runtime. It checks tokens and block balance, and the Lua 5.1 limits: 200 locals in the main chunk, 60 upvalues per function. It also reports identifiers that are neither locals nor Lua built-ins nor game API functions. The API list is read from the game's own `data\script_defs.lua` in your install. Run it after every change to `main.lua`, and after a game update. |

### 5.5 `tools/dev/re/`: read-only helpers for the running game

Teardown's code is encrypted on disk and only readable in memory once the game runs (see
[UPDATING_FOR_A_NEW_GAME_VERSION.md](UPDATING_FOR_A_NEW_GAME_VERSION.md)). These helpers attach to nothing and
write nothing into the game. They read the memory of a running `teardown.exe` with `ReadProcessMemory`, and
disassemble with capstone; nothing needs to be written to disk. **Their output (disassembly listings) is the
game's code: never commit, upload or share it.** `*.txt` in that folder is git-ignored. Keep your notes to
addresses, offsets and descriptions.

| Script | What it does |
|---|---|
| `tdmem.py` | The shared module. It finds the process and reads memory, and holds a few 2.0.1 landmarks (the game object's RVA, local player offsets) that must be updated for a new version. |
| `tddis.py` | Disassembles code at given RVAs. Calls, jumps and RIP-relative operands are shown as RVAs, and referenced strings are printed. |
| `bytes.py` | Prints raw bytes at given RVAs |
| `findstr.py` | Finds text in the image (binding names, shader program names...), or prints the text stored at an RVA |
| `findop.py` | Finds instructions whose operands contain a substring, for example a field offset such as `+ 0x2cc]` |
| `callers.py` | Direct calls and jumps, and RIP-relative references, to given RVAs |
| `calls.py` | Lists the calls, strings and branches of one function |
| `iatrefs.py` | Code that calls given imported functions |
| `vtref.py` | Vtable slots that point to a given RVA, and the surrounding entries |
| `memscan.py` | Finds references to given strings and disassembles around them |
| `threadstate.py` | Samples the instruction pointer and a few return addresses of every thread |
| `dbwin.py` | Captures `OutputDebugString` messages for a number of seconds (the OpenXR loader writes there) |
| `snap_shapes.py`, `tool_box.py` | Read-only dumps of the held tool's shapes, and of its box in the hand |

Aids built into the DLL (see `src/re_tools.cpp`, `src/log.cpp`, `src/gpu_trace.cpp` and `src/sampler.cpp`):

- hardware data breakpoints (`WatchMemory`) to find which instruction writes a field;
- a call stack logger that gives `teardown.exe` offsets;
- the GPU command trace of `debugGpuTrace`;
- the sampler of `debugProfile`.

### 5.6 `tools/dev/shader_tools/`: compiling with the game's shader compiler

| Script | What it does |
|---|---|
| `gamedxc.py` | Compiles an HLSL file with the shader compiler that ships with the game (`dxcompiler.dll` and `dxil.dll` in the folder given by `TEARDOWN_DIR`), so that the bytecode matches what the game's own compiler produces. It can also write a disassembly and a C header of the result. Usage: `gamedxc.py <file.hlsl> <entry> <profile> <out.cso> [-DNAME=VALUE ...] [--dis out.txt] [--header out.h NAME]`. |

It was used to build the game-derived VR shaders (VR anti-aliasing and stereo particles). Those shaders and the
headers made from them are **not published** (see the CMake option `TDVR_GAME_SHADERS` in
[BUILDING.md](BUILDING.md), section 4), so a public build never needs this script. Never commit a shader derived
from the game's, nor its compiled form.

## 6. Rules for test scripts

These rules come from real incidents during development:

- **Snapshot before, restore after.** Before any scripted restart, copy the live `<game>\TeardownVR.ini`, the
  active mod list `%LOCALAPPDATA%\Teardown\modlists\1.xml`, the game's `options.xml` and the previous
  `TeardownVR.log`. Check them after the run. Blind menu clicks once emptied the mod list, and a mock run changes
  `vrAspect`.
- **Never stop a runner that already started the game.** Let it finish, or clean up by hand: an interrupted runner
  skips its restore step.
- **Never relaunch the game blindly.** Probe the runtime first (section 2).
- **Do not disturb someone using the PC.** Do not steal focus and do not use desktop automation while the PC is in
  use. Drive the game through the mock's `controls.txt`.
- **Measure what the player sees**, with the same settings as the configuration being judged (`taaMode`,
  `sharpen`, refresh rate). Do not trust a proxy value.
- **Never commit** logs, captures, ini copies, mod lists, savegames or disassembly. They may contain game code,
  hardware details, paths or other people's mod lists.
