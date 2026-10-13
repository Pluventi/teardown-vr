# Updating Teardown VR for a new game version

Teardown VR is written for **Teardown 2.0.1** (Steam, x64, Direct3D 12). It finds the game's internal functions
and data at run time from byte patterns. A Teardown update can change the game's code, so some patterns stop
matching. This page is a practical procedure for anyone who wants to bring the mod back to life after such an
update.

> **No commitment.** This procedure is documentation, not a promise. Nobody is committed to updating the mod for
> a new Teardown version, or to answering questions about it.

`<game>` is the Teardown folder, the one that contains `teardown.exe`. "RVA" means an address relative to the
start of the `teardown.exe` module. The tools named below are described in [DEV_TOOLS.md](DEV_TOOLS.md).

## 1. What happens when the game changes

The DLL does its work in three phases. Each one reacts differently to a game update:

| Phase | When | What it touches | After an update |
|---|---|---|---|
| 1. `DllMain` | When the game loads `dsound.dll` | The game's import table (clipboard, input and window functions of `USER32`) and the `dxgi.dll` exports. No game code is touched. | Rarely affected. A missing import logs `IAT hook <dll>!<function> NOT FOUND`. |
| 2. First DXGI factory | Before the renderer creates its pipelines | The particle pipeline hooks (`ParticlesVrEarly`, retried at each factory creation, up to 16 times while the code is still encrypted) | `Particles: ... not found (game version?) - stereoParticles unavailable` |
| 3. First `Present` | The game's code is decrypted in memory by then | `EngineInit` scans the game's code and installs every engine hook | `Engine: ... not recognised ...` lines; at worst `Engine: not recognised (update=... time=...) - synchronised stereo unavailable` |

Each feature checks its own patterns and the offsets it reads, and **refuses to hook** on any mismatch. It then
writes one log line saying what was not recognised and what happens instead, and the game keeps its own
behaviour for that part. Examples of fallbacks:

| If this is not recognised | The mod falls back to |
|---|---|
| `Game::update` and the frame-time bookkeeping | **No synchronised stereo**: alternating eyes (each eye shows its own game instant). The settings panel shows "Game version not recognized: alternating eyes used." |
| The render camera | The right eye uses the game's own camera |
| The 2D interface camera | The HUD is left as the game draws it |
| Per-eye temporal history | Both eyes share one history (blur, halos) |
| The tool placement | The tool keeps its normal desktop position |
| `GetCameraTransform` | Other mods aim where the head looks |
| The particle update / sort / mesh tasks | Particles are one frame late in the left eye, or sorted serially as before, or have no right-eye motion |
| The frame-B replay targets | Frame-B mode 1 or 0 (a full game update in the right-eye frame) |
| The lazy beginFrame, dead passes, shape jobs | Each performance feature is simply off |
| The game's pause, the level map, mod cameras, mod windows | The game's own behaviour for each |

The fallbacks have limits:

- A pattern can still match the right code while the **meaning** of a field it reads has changed. A refusal cannot
  catch that, so the mod may then misbehave or crash. This is why every update must be re-tested (section 5).
- Some offsets are **constants** in the source rather than read from a pattern. Many of them are checked against
  the game's code at start, but not all (section 3.3).
- Faults inside the replay code (frame B, particle motion) are caught and turn the feature off until restart.
  Elsewhere, a fault can crash the game. The DLL's exception handler then tries to write the last log lines
  first.

**Lines that are not symptoms of an update.** In a build without the game-derived shaders
(`TDVR_GAME_SHADERS=OFF`, the default for a clone of this repository), these lines appear with every game version,
2.0.1 included:

- `Particles: this build has no stereo-particle shaders (TDVR_GAME_SHADERS=OFF) - the game's are kept`;
- `TAA: this build has no VR anti-aliasing shaders (TDVR_GAME_SHADERS=OFF) - the game's is kept`.

The settings panel says "-> not in this build" under those options. In a public build, none of these means that
the game changed (section 6). In a build WITH the shaders, `Particles: ... is not one the VR shaders were made
from` and `Particles: no VR pipeline (...) - stereoParticles unavailable` do point to a game change.

## 2. Step 1: read `TeardownVR.log`

1. **Play one session.** Start the game with the headset connected (or with the mock runtime, see section 5).
   Load a level, play for a minute, and quit. Keep `<game>\TeardownVR.log`: at the next start, it is moved to
   `<game>\TeardownVR_journaux\`.
2. **Check that the DLL loaded.** If there is **no `TeardownVR.log` at all**, the DLL was not loaded. Possible
   causes:
   - the game no longer imports `DSOUND.dll`;
   - the file is missing or renamed;
   - an antivirus removed it;
   - the Visual C++ runtime is missing.

   The `dsound.dll` proxy only works while the game imports `DSOUND.dll`.
3. **List the failures:**

   ```powershell
   Select-String -Path "<game>\TeardownVR.log" -Pattern "not recognised","not recognized","unavailable","NOT hooked","NOT FOUND","NOT available","not found","FAILED","ERROR"
   ```

4. **Compare with a log of the last working version**, if you kept one (`TeardownVR_journaux\`). Many `Engine:`
   lines print the RVAs and offsets they found, for example `Engine: update=+7a080 dt=204 accumulators=214/218` in
   2.0.1. The difference shows which functions moved and which offsets changed.
5. **Sort the result:**
   - **Nothing in the engine was recognised** (`synchronised stereo unavailable`): start with `Game::update` and
     the time bookkeeping. Nothing else matters until they work.
   - **A few features are off**: fix them one by one, **in the order of `EngineInit`**. Several `Init*` functions
     need the result of an earlier one (each call in `EngineInit` says which), so one early failure can switch
     off several later features. For example, per-eye history is needed by particle motion, the shape-job skip,
     the dead-pass skip and the same-noise feature.
   - **Everything is found but something looks or behaves wrong**: a field offset or the meaning of a value
     changed without breaking the pattern. Compare the behaviour with `taaMode=0`, `stereoParticles=0`,
     `frameBMode=0` and the other switches, through the live control file `TeardownVR_perf.txt`, to isolate the
     feature.
6. **Check the Lua side too:**
   - Run `tools/dev/lua/luacheck.py` on `mod/TeardownVR/main.lua`. It reads the game's new `data\script_defs.lua`
     and lists every API function the mod uses that no longer exists.
   - With `[Debug] debugLua=1`, look for `Lua:` lines and lost requests.
   - The game's own log (`%LOCALAPPDATA%\Teardown\log.txt`) may also show script errors.

## 3. How the hooks are found

Read this before changing a pattern. Most of the scanning code is at the top of `src/engine.cpp`, in its `Init*`
functions, in `FindFrameBTargets` and in `InitPerfTrace`. The rest is in `src/particles_vr.cpp` (`kCreatePsoPattern`,
`kCreatePsoCall`, `kScenePattern`, `kSceneCall`, `kUploadPattern`).

### 3.1 The primitives

- **`Scan(pattern)`** searches the game's `.text` section, located from the PE section table, for a byte pattern
  written in hex with `??` wildcards. It returns the first match.
- **`ScanOnce(pattern)`** requires exactly **one** match in `.text`, or the feature is left out. The performance,
  frame-B and newer features use it.
- **`ScanIn(start, size, pattern)`** searches a small window, usually inside a function already found. This is how
  several short checks confirm one function.
- **`RelTarget(...)`** decodes a `rel32` call / jump / RIP-relative operand, so a pattern leads on to the
  function it calls. **`U32(...)`** reads a 32-bit field offset embedded in an instruction, so **most struct
  offsets are read from the game's code instead of being hard-coded**.
- **`FindLuaBinding(name)`** finds the binding's name, as a stand-alone string in `.rdata`. It then finds the
  registration code that loads both the implementation and the name (`lea r8,[impl]` just before
  `lea rdx,[name]`). Lua binding names rarely change, which makes them the most stable anchors.
- **`FindUiBinding(name)`**: the `Ui*` bindings are registered from a table of {function, name} pairs.
- **`FindCoreFunctions`**: a static {function, name, next} list in `.data` (registry setters and a few others).
- **`FindBindingDispatcher`**: the function every Lua binding goes through. It is taken from three different
  bindings, and the three results must agree.
- **`FunctionStart`** finds the start of the enclosing function through the unwind table
  (`RtlLookupFunctionEntry`).
- **Vtables** are confirmed through their single `.rdata` reference and their RTTI type name, for example
  `TRendererD3D12`.

### 3.2 Cross-checks, and refusal on mismatch

An `Init*` function hooks nothing until all of its checks pass. The checks are:

- every pattern found, and unique where `ScanOnce` is used;
- the offsets read from different places are equal (for example, the UI camera offset read in two functions);
- the extracted values are in a plausible range;
- for per-eye history, every field verified at fixed positions inside `Renderer::render`.

Otherwise the function logs `... not recognised (...)`, with the addresses it found or `0000000000000000` for a
missing one, and returns. Hooks always call the game's original function through MinHook's trampoline. Apart from
MinHook's own jumps, the only game instructions changed are three RIP-relative constant operands redirected by
the grab-distance feature.

### 3.3 Constants to re-check by hand

Functions are found by pattern, and most field offsets are read from the matched instructions and cross-checked.
Some offsets, however, are written as **2.0.1 constants**, and one debug-only address too. A missing pattern will
not reveal them, because the pattern may still match. Search the sources for `constexpr uint32_t k`, for `0x`
offsets and for offsets in comments, mainly in:

- `src/engine.cpp` (about 30 constants):
  - per-eye history fields, TAA textures and renderer fields (`kHistFbo`, `kTaaTex`, `kRend*`), mostly verified in
    `InitHistoryHook`;
  - shape fields for object motion and the shape-job skip (`kShapeCurXf` = `0x13c`, `kShapeOldXf`, `kShapeFlags`,
    `kSceneShapes`...);
  - particle system fields (`kSysSorted` = `0x1ac0178`, `kSysMeshMode`, `kParticleSize`...);
  - the dead-pass fields (`kDp*`) and the tool and entity fields (`kPlayerToolBody`, `kShapeLocal`, `kShapeVox`...).

  Many of them are checked against the matched code at start, but not all: check every one anyway.
- `src/frameb.cpp`: the game, renderer, camera and sound offsets at the top of the file. Each one is inside a
  pattern that `FindFrameBTargets` matches, and the comments list the 2.0.1 values.
- `src/perf.cpp`:
  - a few job-manager and entity offsets (`kJmSem`, `kEntClient`...);
  - the lazy beginRender (`[Performance] lazyBeginFrame`): three renderer methods are left without a stub only
    when a hash of their bytes equals the 2.0.1 value. On another version the hashes differ and those methods are
    treated like the others; re-check them and update the hashes.
- `src/taa_vr.cpp`: the TAA wrapper's name, pipeline and root signature offsets (`kTaaCompute` = `0x40`,
  `kWrapPso` = `0x60`...). The only guard is a check that the wrapper's inline name is `temporalaacs`.
- `src/particles_vr.cpp`: the renderer's particle and lighting program slots (`kRendPrograms`, from `0xdc0`), the
  shader name and pipeline slots in a program object (`kDescName`, `kProgPso`), and the size of the scene
  constants.
- `src/vr.cpp`: **one hard-coded RVA**, the game object pointer `0x1c6e270` (with the camera at `+0x60` and its
  position at `+0x1d0`), read only with `[Debug] debugWatchCamera=1` in `VrHandleLuaRequest`. It has no pattern
  and no check: on another game version it reads a wrong address. Replace it with the new RVA found for
  `tdmem.py` (section 4.3, step 2), or leave `debugWatchCamera` off.
- `mod/TeardownVR/main.lua`: a few transform numbers read from the game's player prefab (hand and forearm parts
  of the free arm).
- Data files the mod reads at run time, whose format must still match:
  - `data/prefab/tool/<name>.xml` (`fp_action`, `tp_action_rh`), for the tool placement;
  - `options.xml` (the display mode and the key map);
  - the game's `log.txt` (`Active mod:` lines) and the `modlists` / `savegame.xml` files, for the keys panel;
  - `data/ui/font/*.ttf`, for the VR inventory.

### 3.4 Constraints that do not change

- The Lua ↔ DLL channel uses `SetClipboardText` / `GetClipboardText`. The game allows them only for its
  built-in mods, which is why the Lua mod lives in `<game>\mods\teardownvr`.
- **Never patch that check.** If an update takes these functions away from built-in mods, the bridge cannot work,
  and the answer is not to bypass the game's sandbox.
- Keep every Lua → DLL message at **30 characters or less**. The game's `SetClipboardText` reads its temporary
  string after freeing it. A string that short survives inside the string object; longer ones sometimes arrive
  empty. Re-verify this after an update, but do not lengthen messages. (The only exception is the diagnostic
  `TDVRL <text>` lines, which can be longer and may be lost.)

## 4. Step 2: find the new patterns

### 4.1 Why the running game, and what is allowed

`teardown.exe` is encrypted on disk by the SteamStub DRM: its `.text` section is only decrypted in memory when the
game starts. Byte patterns therefore cannot be searched in the file on disk. All analysis is done on the
**memory of the running game**, which is also where the mod searches, at the first `Present`.

The `tools/dev/re/` helpers open the running `teardown.exe` with read-only access and use `ReadProcessMemory`.
They inject nothing and write nothing into the game.

- The mod does not modify, remove or bypass the DRM.
- Read the running game's memory directly with these read-only tools. Nothing needs to be written to disk.
- **Never save, share or commit the game's decrypted code, a memory dump or a disassembly listing.** Only short
  byte patterns, offsets and plain descriptions belong in your notes, the repository and the documentation.

### 4.2 The tools

Python 3.10+ with `capstone`, `pefile` and `psutil` (see [DEV_TOOLS.md](DEV_TOOLS.md), section 5.5):

| Need | Tool |
|---|---|
| Read memory; game object and local player landmarks | `tdmem.py`. **Update its landmarks first** (4.3). |
| Disassemble at an RVA | `tddis.py`; `calls.py` lists one function's calls, strings and branches |
| Raw bytes at an RVA | `bytes.py` |
| Find a string (binding name, shader program name, profiler category) | `findstr.py` |
| Who calls / references this RVA | `callers.py` |
| Which code calls this import (D3D12, DXGI, USER32...) | `iatrefs.py` |
| Which vtable slot points here | `vtref.py` |
| Instructions using a field offset (e.g. `+ 0x2cc]`) | `findop.py` |
| References to strings, with disassembly around them | `memscan.py` |
| Where the main thread spends its time | `threadstate.py` |
| OpenXR loader messages | `dbwin.py` |

In the DLL itself:

- `WatchMemory` (`src/re_tools.cpp`) sets a hardware data breakpoint that logs the instructions that touch a
  field;
- `LogStack` logs a call stack as `teardown.exe` offsets;
- `[Debug] debugGpuTrace=1` logs one frame's GPU commands with their call sites;
- `[Debug] debugProfile=1` samples the main thread.

### 4.3 Procedure for one broken pattern

1. **Know what you are looking for.** Open the pattern in the source. The comment above it describes, in plain
   words, which function it is in, which few instructions it matches, and which offsets the code extracts from
   it (`U32(p + n)`, `RelTarget(...)`). [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) gives the 2.0.1 RVA and
   the role of each function, with a table of landmarks in section 8. [ARCHITECTURE.md](ARCHITECTURE.md) gives the
   hook table.
2. **Update the landmarks of `tdmem.py`.** The global game object pointer is the first one: 2.0.1 RVA
   `0x1c6e270`. Many bindings load it with one RIP-relative `mov`. Take any binding (step 3), disassemble its start
   with `tddis.py`, and read the new RVA from that load. Then re-check the local player offsets the same way. The
   same 2.0.1 RVA is written in two more places: `calls.py` always prints the instructions that load it (its
   `rva_1c6e270` label), and `src/vr.cpp` reads it for `[Debug] debugWatchCamera` (section 3.3). Update both.
3. **Find the function again from an anchor that survives updates.** In order of reliability:
   - a **Lua binding name**: `findstr.py <name>`, then `callers.py <string RVA>`, gives the registration code and
     the implementation (the `FindLuaBinding` method by hand). Many engine functions are one call away from a
     binding.
   - a **string** the function uses: a profiler category, a shader program name (`temporalaacs`,
     `gbufferparticles`, `ambientdirectionallight`), a registry key or a file path;
   - a **vtable** with an RTTI name (`vtref.py`), or an **import** it calls (`iatrefs.py`);
   - a **caller or callee** you already found (`callers.py`, `calls.py`);
   - a **field offset** it uses (`findop.py`), or a float constant it loads.
4. **Compare the code with the description.** Disassemble the candidate. The logic is usually the same, while
   registers, stack offsets, struct offsets or instruction lengths moved.
5. **Write the new pattern.**
   - Keep the opcode bytes.
   - Wildcard (`??`) every `rel32` call or jump target, and every displacement or struct offset that may change
     again.
   - Bytes that the code **extracts** (`U32(p + n)`) must stay at the same positions. If an instruction's encoding
     changed length, update the extraction positions too.
   - Practical method: wildcard more of the old pattern, search, then look at each candidate and extend the
     pattern until only the right one matches.
6. **Check uniqueness in the running game.** Patterns used with `ScanOnce` must match exactly once in `.text`.
   For `Scan`, the first match must be the right one. A few lines on top of `tdmem.py` that read `.text` and count
   matches are enough.
7. **Run the cross-checks.** Rebuild, start the game, and check that the `Init*` function now logs what it found
   (RVAs, offsets) instead of `not recognised`. Its cross-checks must agree with the values you expect.
8. **Re-check the constants** listed in 3.3 that belong to the same feature, by disassembly or with a hardware
   data breakpoint.
9. **Update the comments** with the new version and RVAs, and add the new values to the reverse-engineering
   notes. Use plain descriptions only: no listings.

The mod targets one game version at a time. Steam updates every player, so supporting the old version too is
optional. Where it costs nothing, a pattern whose wildcards match both versions is fine.

## 5. Step 3: re-verify with the mock runtime, then in a headset

Most checks can be done without a headset, with the mock OpenXR runtime ([DEV_TOOLS.md](DEV_TOOLS.md), section 1).

1. **Build.** Build the DLL and `tdvr_mock_openxr`. Install the DLL and the Lua mod into the game, and **back up
   `TeardownVR.ini` and the active mod list** first.
2. **Point the game at the mock.** Set `[Debug] openxrRuntime=<build>\Release\tdvr_mock_openxr.json`, start the
   game and load a sandbox level.
3. **Check the log:**
   - `Engine: synchronised stereo ready` is there;
   - there are no unexpected `not recognised` lines;
   - in a level, the `Stereo frames` lines show equal `left` and `right` counts and almost no `other`;
   - `mock_out\mock.log` has no `PROBLEM:` lines.
4. **Check synchronised stereo directly** with `[Debug] debugSyncTest=1`. The log line
   `SyncTest: ... marker ok=.. wrong=.. none=..` should show no `wrong`. It also saves 8 frames for inspection.
5. **Look at both eyes.** Save stereo pairs with `dump=<n>` in `mock_out\controls.txt`, or with
   `tools/dev/mock/mockctl.py snap`. Check them with `tools/dev/analysis/stereo_disp2.py`: the disparity must be
   consistent, the right eye must not show a different moment, and the HUD must converge.
6. **Exercise the features** through `controls.txt` (`mockctl.py`):
   - aiming and the held tool, grab and the free hand;
   - the VR inventory, menus on the virtual screen, the pause screen and the level map;
   - a vehicle;
   - a few popular mods (weapons, cameras, mod windows).
7. **Check that "no visible change" features still change nothing.** Compare captures with
   `tools/dev/analysis/imgdiff.py` between `frameBMode` 0 and 2. Use the built-in checks `skipDeadPasses=2` and
   `parallelParticleSort=2`, which log any difference.
8. **Measure without judging stutter on the mock.** `perfTrace=1` shows where the time goes. Smoothness can only
   be judged in a real headset.
9. **Restore** `TeardownVR.ini` (the mock rewrites `vrAspect`) and the mod list.
10. **Play a real headset session** at the end. Some problems (layer composition, the streaming runtime) only show
    there.

## 6. The game-derived shader parts (prose only)

**Public builds** (`TDVR_GAME_SHADERS=OFF`, the default for a clone of this repository) do not contain the VR
anti-aliasing and stereo particle shaders. A game update has nothing to break there: those features keep the
game's own shaders. Their `Particles:` and `TAA:` "this build has no ... shaders" log lines say so, with any game
version (section 1).

**Builds with the shaders** can only be made from private files that are not in this repository (the HLSL and the
compiled headers). In such a build, a game update affects them in two ways:

- **Stereo particles and lighting are self-protecting.** A VR "twin" pipeline is created only when the game's
  pixel shader is exactly the 2.0.1 blob it was made from, checked by the DXBC container hash and the size. A
  changed shader is logged as
  `Particles: the game's ... shader (permutation ..., hash ...) is not one the VR shaders were made from (Teardown 2.0.1) - the game's pipeline is kept`.
- **The VR anti-aliasing is not hash-checked.** The swap is guarded only by the TAA wrapper's name and by
  pipeline creation with the game's root signature. If the game's TAA shader changed, a VR mode could still be
  accepted and give a wrong image. After an update, compare `taaMode=0` with the VR modes. If in doubt, ship with
  `taaMode=0` as the default, or ship a build without the shaders, until the shaders are rebuilt.

Only someone who has the private shader sources can rebuild them for a new game version, and nothing promises
that it will be done. If it is done, it is done privately, in this order:

1. **Take the new game's compiled shaders** from the game's own shader cache, in the local install's `data`
   folder.
2. **Rebuild the HLSL** of the affected shaders from them.
3. **Re-apply the VR changes**, which [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) describes in prose:
   - for the TAA: a sharper (Catmull-Rom) history fetch, the variants' blend weights, and the fix for glass and
     particles;
   - for the particles and the lighting: the screen-space dither, lighting checkerboard and lighting noise are
     anchored to the left eye's pixels, using a small shift that the DLL appends to the scene constants in the
     right-eye frame;
   - the checkerboard parity variants.
4. **Compile with the game's own shader compiler** (the `dxcompiler.dll` and `dxil.dll` shipped in the game
   folder), so that the bytecode matches the game's vertex shaders and root signatures. The published helper
   `tools/dev/shader_tools/gamedxc.py` does this ([DEV_TOOLS.md](DEV_TOOLS.md), section 5.6).
5. **Prove the rebuild is exact.**
   - With the VR change switched off, each rebuilt permutation must compile to the game's blob **byte for byte**.
   - The signature and root-signature parts of the VR blobs must be identical to the game's.
6. **Regenerate the headers** in `src/generated/`, and the table of game blob hashes and sizes they replace. Then
   update the constants of `src/particles_vr.cpp` and `src/taa_vr.cpp` if the game's layout changed (section 3.3).
7. **Test**: GPU comparisons against the game's own blobs, the mock (section 5), then a headset.

None of these files may be published. The public repository must keep building with `TDVR_GAME_SHADERS=OFF`.

## 7. Before publishing an updated version

If someone publishes an update (nothing promises one), they should:

- State which Teardown version it was tested with, in `README.md` and in the players' guides.
- Update the RVAs and offsets in the comments and in [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md), as
  descriptions without code listings.
- Make sure no dump, disassembly, game shader or generated header is in the commit.
- Build the release with `tools/package_release.ps1` ([BUILDING.md](BUILDING.md), section 6.1).
