# How Teardown VR works, explained for modders

Teardown VR is a VR mod for Teardown 2.0.1 (Tuxedo Labs, Windows, Direct3D 12). It shows the game in a PC VR
headset with both eyes rendered at the same game instant, head and hand tracking, tools and weapons held in the
hand, the controllers mapped onto the game's keyboard and mouse, and a settings panel inside the headset. It was
made and tested with a Meta Quest 3 through Virtual Desktop and Meta Quest Link; SteamVR can be selected too.

This guide explains how it works, one technique at a time. It is written for a modder who wants to understand
the mod, change it, or build the same kind of VR mod for Teardown or for another game. You do not need to have
read the code first. Some C++ and a general idea of how a 3D game draws a frame will help.

**How each technique is explained**

| Part | What it tells you |
|---|---|
| Problem | what goes wrong without the technique |
| Idea | the trick, in plain words |
| In Teardown VR | the files, functions and settings that do it |
| Sketch | a few lines of code written for this guide to show the shape of the solution. They are simplified, they are not copied from the game, and they are not the exact code of `src/` |
| Pitfalls | what went wrong on the way |
| Check it | how to see that it works |

**Two companion documents** go deeper:

- [PROBLEMS_AND_SOLUTIONS.md](PROBLEMS_AND_SOLUTIONS.md): every problem met during development, with its symptom,
  cause, fix, measurements and status. Cited here as "problem B2".
- [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md): what was learnt about Teardown 2.0.1's engine (addresses,
  structure offsets, behaviour). Cited here as "RE § 4".

> **Before anything else**
>
> - Teardown VR is provided **as is, without warranty and without support**. Nothing promises answers, fixes,
>   updates or new features. Issues and pull requests may be opened on the repository, but nothing promises
>   that they will be handled.
> - **A Teardown update can break the mod.** The mod is built to switch features off rather than crash when it
>   no longer recognises the game (section 3), but only Teardown 2.0.1 is known to work.
> - Teardown is a trademark of Tuxedo Labs. This project is not affiliated with or endorsed by Tuxedo Labs. The
>   repository contains no game code, data or shader: the mod finds what it needs in the game's memory, and reads
>   the game's files on the player's PC at run time.
> - The project is licensed under the GPL, version 3 only (GPL-3.0-only, see `LICENSE`). If you copy its code into
>   your own project, that licence applies.

---

## Contents

- [0. Before you start](#0-before-you-start)
- [1. The big picture](#1-the-big-picture)
- [2. Getting code into the game: the DLL proxy](#2-getting-code-into-the-game-the-dll-proxy)
- [3. Finding functions without symbols](#3-finding-functions-without-symbols)
- [4. Hooking: import tables and MinHook](#4-hooking-import-tables-and-minhook)
- [5. Seeing the game's frames: DXGI, D3D12 and OpenXR](#5-seeing-the-games-frames-dxgi-d3d12-and-openxr)
- [6. Rendering two eyes](#6-rendering-two-eyes)
- [7. Off-centre projections and the field of view](#7-off-centre-projections-and-the-field-of-view)
- [8. Render resolution and the monitor lie](#8-render-resolution-and-the-monitor-lie)
- [9. Talking to the game's Lua: the bridge](#9-talking-to-the-games-lua-the-bridge)
- [10. Camera, tool and hands](#10-camera-tool-and-hands)
- [11. Input mapping and simulated keys](#11-input-mapping-and-simulated-keys)
- [12. A VR settings panel with Dear ImGui](#12-a-vr-settings-panel-with-dear-imgui)
- [13. Working with other mods](#13-working-with-other-mods)
- [14. Stereo-consistent stochastic transparency: particles](#14-stereo-consistent-stochastic-transparency-particles)
- [15. Performance: measure first](#15-performance-measure-first)
- [16. Testing without a headset: the mock runtime](#16-testing-without-a-headset-the-mock-runtime)
- [17. Shipping: installer, logs and builds](#17-shipping-installer-logs-and-builds)
- [18. Porting these ideas to another game](#18-porting-these-ideas-to-another-game)
- [19. Where to read more](#19-where-to-read-more)

---

## 0. Before you start

**Words used in this guide**

- **Engine frame**: one turn of the game's main loop (scripts, physics, render, `Present`).
- **Stereo pair**: the two engine frames that make one headset frame (section 6).
- **Frame A / frame B**: the first engine frame of a pair draws the left eye and advances the game; the second
  draws the right eye at zero game time.
- **The DLL**: `dsound.dll`, the native part of the mod (C++, `src/`).
- **The Lua mod**: the part that runs among the game's Lua scripts (`mod/TeardownVR/main.lua` in the repository,
  installed as `<game>\mods\teardownvr\main.lua`).
- **Runtime**: the OpenXR runtime that drives the headset (Virtual Desktop's, Meta's, SteamVR's).
- **The mock**: a fake OpenXR runtime used to test without a headset (section 16).
- **RVA**: an address relative to the start of `teardown.exe` in memory, such as `0x87f40`. RVAs in this guide are
  those of Teardown 2.0.1 and are only landmarks: the mod finds everything again at run time.
- **`<game>`**: the Teardown folder, the one that holds `teardown.exe`.

**Settings** are written `[Section] key`, as in `<game>\TeardownVR.ini`. Some section names are French and must
stay so for compatibility: `[Affichage]` = display, `[Confort]` = comfort, `[Controles]` = controls, `[Ecran]` =
virtual screen. `[Interface]`, `[Mods]`, `[Debug]` and `[Performance]` mean what they say. Most testing switches
exist only in the `.ini`, not in the settings panel.

**A live control file**, `<game>\TeardownVR_perf.txt`, with `key=N` lines, overrides some settings while the game
runs. It is re-read about once per second, and deleting it gives the `.ini` values back. It exists for A/B tests.
A few keys only take effect if the file already exists when the game starts.

---

## 1. The big picture

### 1.1 The pieces

```text
teardown.exe
 |
 |-- imports DSOUND.dll ----> <game>\dsound.dll  = Teardown VR's DLL
 |                              - forwards real DirectSound calls to System32\dsound.dll
 |                              - hooks the game's import table (clipboard, input, window size)
 |                              - hooks DXGI to see every Present
 |                              - hooks engine functions found by byte patterns
 |                              - runs an OpenXR session on the game's own D3D12 device
 |                              - draws its own settings panel (Dear ImGui)
 |
 |-- runs Lua scripts every frame
 |     <game>\mods\teardownvr\main.lua = the Lua mod
 |        <------------- clipboard "bridge" ------------->  DLL
 |        sends: flags, haptics, short messages        gets: which eye, poses, buttons, settings
 |
 |-- physics, render  (engine hooks: per-eye camera and projection, per-eye history, tool in hand...)
 |
 '-- Present ------> the DLL copies the image into the left or right eye of the headset
```

Why two parts? Some things can only be done from native code (the GPU, the headset, the engine's internals).
Others are much easier with the game's own Lua API (moving the player, offsetting the camera, spawning bodies,
reading the tool list). Teardown VR uses each side for what it does best, and joins them with a small message
channel (section 9).

### 1.2 Start-up in two phases

1. **When Windows loads the DLL** (`DllMain` in `src/main.cpp`), and only if the process is `teardown.exe`: open
   the log, read the settings, initialise MinHook, patch the game's import table (clipboard, raw input, cursor,
   window and monitor functions), fix one display option, and hook the three `CreateDXGIFactory*` exports.
   Nothing inside the game's own code is touched yet: it is still encrypted (section 3.1).
2. **At the first `Present`** (`OnGamePresent` in `src/vr.cpp`): take the game's D3D12 device and queue, find and
   hook the engine functions (`EngineInit` in `src/engine.cpp`), then start our renderer, input, user interface
   and diagnostics. The OpenXR session is created from there.

### 1.3 The life of one stereo pair

1. **Frame A starts.** The engine runs its scripts. The Lua mod asks the DLL "which eye now?" and gets "left",
   with the head and hand poses predicted for the moment this pair will be displayed. It moves the camera to the
   left eye, turns the player toward the controller's aim and places the tool in the hand.
2. The engine runs its physics steps and renders. Hooks make the camera exactly the left eye's, with the left
   eye's lens-shaped projection and the left eye's own temporal history.
3. **`Present`.** The DLL copies the back buffer into the left eye's OpenXR image. Nothing is sent to the headset
   yet.
4. **Frame B.** A hook gives this frame zero game time, so nothing moves. By default the game's update is not
   even run again: what frame A asked for once is replayed (section 15.4). The right eye's camera is the left
   eye's camera moved by the distance between the eyes.
5. **`Present`.** The DLL copies the image into the right eye and submits both eyes together (`xrEndFrame`).
6. **Right after `Present`**, the DLL waits for the headset (`xrWaitFrame`), begins the next headset frame and
   reads the poses predicted for it. The next frame A starts with fresh poses.

Menus, loading screens and the paused game are not stereo: they go to a flat "virtual screen" that floats in
front of the player.

### 1.4 Where things live

| Files | Role |
|---|---|
| `src/main.cpp`, `src/proxy.cpp`, `src/dsound.def` | entry point; the `dsound.dll` proxy |
| `src/hooks.cpp` | the two hooking primitives: import table and MinHook |
| `src/d3d_hooks.cpp` | DXGI hooks, `Present`, the "monitor lie" |
| `src/vr.cpp` | OpenXR, the frame loop, the floating panels, the Lua request handler |
| `src/render.cpp` | our small D3D12 toolkit on the game's device (copies, clears) |
| `src/engine.cpp` | hooks inside the engine: stereo timing, camera, projection, history, tools, 2D interface, other mods |
| `src/frameb.cpp` | the cheap "render only" frame B |
| `src/perf.cpp`, `src/prof.h`, `src/sampler.cpp`, `src/gpu_watch.cpp`, `src/gpu_trace.cpp` | measurement and diagnostics |
| `src/bridge.cpp` | the Lua to DLL channel |
| `src/input.cpp`, `src/pckeys.cpp`, `src/modkeys.cpp` | controllers to keyboard and mouse; other mods' keys |
| `src/ui.cpp`, `src/inventory.cpp`, `src/i18n.h` | the settings panel, the VR inventory, English and French |
| `src/taa_vr.cpp`, `src/particles_vr.cpp` | shader replacements (they need headers that are not published, section 14) |
| `src/config.cpp`, `src/config.h` | every setting and its default; `TeardownVR.ini` |
| `src/log.cpp` | `TeardownVR.log` |
| `src/re_tools.cpp` | reverse-engineering helpers (hardware data breakpoints) |
| `mod/TeardownVR/main.lua` | the Lua mod |
| `tools/mock_openxr/` | the mock OpenXR runtime and a runtime probe |
| `installer/` | the players' installer |

---

## 2. Getting code into the game: the DLL proxy

**Problem.** Teardown has no API for native plugins. A VR mod must talk to the GPU, the headset and the engine's
internals, so its code must run inside `teardown.exe`.

**Idea.** When a program imports a DLL, Windows looks in the program's own folder first, unless the DLL is one of
the system's "KnownDLLs". So pick a DLL that the game imports, that is not a KnownDLL and that has few functions,
and put a DLL with the same name and the same exports next to the game. Each export simply calls the real system
DLL. Your `DllMain` now runs inside the game.

**In Teardown VR.** `teardown.exe` imports `DSOUND.dll` (only `DirectSoundCreate`, ordinal 1), and `dsound` is
not a KnownDLL. `src/dsound.def` declares the 12 exports of the real `dsound.dll`; `src/proxy.cpp` forwards each
one to `%SystemRoot%\System32\dsound.dll`, which it loads on the first call. `DllMain` (`src/main.cpp`) compares
the process name with `teardown.exe`: any other program that loads the DLL gets a plain proxy and nothing else.
CMake builds the target `TeardownVR` with the output name `dsound`.

**Sketch.**

```text
; dsound.def - the real DLL's exports, mapped to our own function names
EXPORTS
    DirectSoundCreate = Proxy_DirectSoundCreate @1
    DirectSoundEnumerateA = Proxy_DirectSoundEnumerateA
    ; ... and the other exports of the real dsound.dll
```

```cpp
// proxy.cpp - load the real DLL on first use, never inside DllMain
static HMODULE RealDsound()
{
    static HMODULE h = [] {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dsound.dll");
        return LoadLibraryW(path);
    }();
    return h;
}

extern "C" HRESULT WINAPI Proxy_DirectSoundCreate(LPCGUID dev, void** out, IUnknown* outer)
{
    using Fn = HRESULT(WINAPI*)(LPCGUID, void**, IUnknown*);
    static Fn real = (Fn)GetProcAddress(RealDsound(), "DirectSoundCreate");
    return real ? real(dev, out, outer) : E_FAIL;
}

// main.cpp - do as little as possible here
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH && ProcessNameIs(L"teardown.exe")) {
        LogInit();                // a log file next to the exe
        ConfigLoad();             // TeardownVR.ini
        MH_Initialize();          // MinHook can be initialised here
        InstallImportHooks();     // patch the game's import table only
        HookDxgiFactoryExports(); // everything else waits for the first Present
    }
    return TRUE;
}
```

**Pitfalls.**

- **The loader lock.** `DllMain` runs while Windows holds a lock over all DLL loading. Do not load D3D12 or DXGI
  there, and do not wait for other threads. Teardown VR links `d3d12.dll`, `dxgi.dll` and `pdh.dll` as
  *delay-loaded* (`/DELAYLOAD`), so they are only loaded on first use, long after `DllMain`.
- **Load the real DLL lazily**, inside the forwarded call, for the same reason.
- **Another mod may ship the same DLL name.** Two `dsound.dll` cannot both sit next to the exe. The installer keeps
  a foreign `dsound.dll` as `dsound.dll.original` and puts it back on uninstall (section 17.2). It recognises its
  own DLL by a marker string that every build contains.
- **Choosing the DLL.** List the game's imports (for example `dumpbin /imports teardown.exe`) and check the
  KnownDLLs list in the registry (`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs`). A DLL with
  few exports is the easiest to forward. The names other mods often use (`dxgi.dll`, `d3d11.dll`, `version.dll`,
  `winmm.dll`, `dinput8.dll`) tend to clash with overlays and post-processing injectors that use the same trick.

**Check it.** `TeardownVR.log` appears next to `teardown.exe` when the game starts, and the game still has sound.
If the log is missing, the DLL was not loaded (wrong folder, or a KnownDLL name).

---

## 3. Finding functions without symbols

A shipped game has no symbol names. To hook "the function that updates the game", you must find where it is, and
find it again after every game update. Everything else in this guide rests on this section.

### 3.1 The code is encrypted on disk: work on the running game

**Problem.** Byte sequences read from `teardown.exe` on disk matched nothing. The game's `.text` section (its code)
is encrypted by Steam's DRM wrapper (SteamStub, a `.bind` section) and only decrypted in memory when the game
starts. When `dsound.dll` is loaded, the code is still encrypted.

**Idea.** Look at the code where it is readable: in the memory of the running game, after the game has decrypted
itself.

- **For the mod at run time**, this means waiting. Teardown VR searches the engine at the first `Present`, long
  after the game has finished decrypting itself. Hooks that must exist before the renderer builds its shaders
  (section 14.5) are installed a little earlier, at the first DXGI factory creation, with retries until the code
  is readable (`ParticlesVrEarly`, `src/particles_vr.cpp`).
- **For you, the human doing the analysis**, it means reading the running game's memory directly with read-only
  tools (`ReadProcessMemory`, then `capstone` and `pefile` to disassemble and search, as the helpers in
  `tools/dev/re` do; RE § 9). Nothing needs to be written to disk.

The mod does not modify, remove or bypass the DRM: it simply runs after the game has decrypted itself, like any
code inside the process. **Never save, share or commit the game's decrypted code or a disassembly listing.** Keep
your notes to addresses, offsets and descriptions.

**Pitfall.** The mod itself only searches its own process.

### 3.2 Byte patterns

**Problem.** There are no names, and addresses move with every game update.

**Idea.** Describe a place by the bytes of a few instructions around it, with wildcards (`??`) for the bytes that
change between builds: call targets, data addresses, sometimes structure offsets. Search the code section for that
pattern at run time, then read what you need out of the match:

- a `call` or `jmp` target: the 4-byte relative offset after the opcode, added to the address of the next
  instruction;
- a structure offset: the 4-byte displacement inside an instruction such as `movss [r12+0x204], xmm6`.

**In Teardown VR.** `src/engine.cpp` finds `.text` from the PE section table (`EngineInit`), then uses `Scan` /
`ScanIn` (first match), `ScanOnce` (must match exactly once; used by the newer features), `RelTarget` (decode a
rel32) and `U32` (read an embedded offset).

Example: `Game::update` is found through its call in `Game::frame`:

```text
49 8B CC                    mov rcx, r12        ; r12 = the Game object
E8 ?? ?? ?? ??              call Game::update   ; <- the target we want
49 8B CC                    mov rcx, r12
E8 ?? ?? ?? ??              call ...
41 80 BC 24 ?? ?? 00 00 00  cmp byte [r12+disp32], 0
```

A second pattern, at the end of the frame-time bookkeeping, gives three offsets at once: the frame time `dt` and
the two fixed-step accumulators (`game+0x204`, `+0x214` and `+0x218` in 2.0.1; RE § 4).

**Sketch.**

```cpp
// "48 8B ?? 05" -> {0x48, 0x8B, -1, 0x05}; -1 = any byte
static std::vector<int> Parse(const char* pattern);

static std::vector<BYTE*> FindAll(BYTE* start, size_t size, const char* pattern)
{
    std::vector<int> p = Parse(pattern);
    std::vector<BYTE*> hits;
    for (size_t i = 0; i + p.size() <= size; ++i) {
        size_t j = 0;
        while (j < p.size() && (p[j] < 0 || start[i + j] == p[j])) ++j;
        if (j == p.size()) hits.push_back(start + i);
    }
    return hits;
}

// Refuse anything ambiguous: zero or several matches means "not recognised".
static BYTE* FindUnique(const char* name, const char* pattern)
{
    auto hits = FindAll(g_text, g_textSize, pattern);
    if (hits.size() != 1) {
        Log("%s: %zu matches - not recognised, feature off", name, hits.size());
        return nullptr;
    }
    return hits[0];
}

static BYTE* RelTarget(BYTE* nextInstr, BYTE* rel32)   // call / jmp / rip-relative target
{
    int32_t d;
    memcpy(&d, rel32, 4);
    return nextInstr + d;
}
```

With the pattern above: `void* update = RelTarget(hit + 8, hit + 4);` (the `call` opcode is byte 3, its rel32
starts at byte 4, the next instruction at byte 8).

**Pitfalls.**

- Too short, and the pattern matches several places; too long, and it breaks at the next update. Keep the bytes
  that carry meaning (opcodes, registers, small constants) and wildcard every absolute or relative address and,
  when unsure, the offsets.
- Search the **decrypted** code only (3.1).
- Anchor on something stable when you can: a unique constant, a string reference, a call to an already-found
  function.
- Keep RVAs in comments as hints only. Teardown VR finds its functions by pattern and reads most field offsets
  from the matched instructions; the offsets it keeps as 2.0.1 constants (renderer, shapes, particles, the TAA
  wrapper) and its one debug-only RVA must be re-checked by hand after an update
  ([UPDATING §3.3](UPDATING_FOR_A_NEW_GAME_VERSION.md#33-constants-to-re-check-by-hand)). Avoid adding more.

### 3.3 Cross-checks: refuse when anything disagrees

**Problem.** After a game update, a pattern can still match, but in the wrong place, or the structure around it can
have changed. Writing to a wrong offset corrupts the game silently.

**Idea.** Read each important fact in two independent ways and compare. Check the constants an instruction uses.
Check a structure's fields at fixed offsets inside the function that uses them. If anything disagrees, do not hook:
log one line and leave the game's own behaviour.

**In Teardown VR.**

- The 2D interface camera offset is read in two different functions, which must give the same value
  (`InitUiCamera`).
- The Lua binding dispatcher is taken from three different binding wrappers (`GetCameraTransform`, `DrawLine`,
  `QueryRaycast`), which must agree.
- `InitHistoryHook` verifies every history field with patterns at fixed offsets inside `Renderer::render` before
  hooking it.
- The grab feature checks the constants it redirects before touching anything (section 4.3).
- The lazy `beginRender` feature checks the renderer's vtable by pattern and by its RTTI class name, and
  hash-checks the bodies of the methods it leaves alone (section 15.3).
- Code that replays or rewrites engine state at run time (the frame B replay, particle motion) runs under
  structured exception handling: a memory fault switches that feature off until the game restarts.

Each feature logs one line when it is not recognised, and says what is kept instead. Examples from
`TeardownVR.log`:

```text
Engine: render camera not recognised - the right eye uses the game's own camera
Engine: tool placement not recognised - the tool keeps its normal position
Engine: not recognised (update=... time=...) - synchronised stereo unavailable
```

The last one is the big fallback: without `Game::update` and its time fields, synchronised stereo is impossible,
so the mod uses alternating eyes (section 6.2) instead of crashing. The settings panel then says "Game version not
recognized: alternating eyes used."

**Sketch** (the patterns are made up for the example):

```cpp
static bool InitSomething()
{
    BYTE* a = FindUnique("something A", "48 8B 81 ?? ?? 00 00 48 85 C0 74 ??");
    BYTE* b = FindUnique("something B", "F3 0F 10 81 ?? ?? 00 00 0F 2F C1");
    if (!a || !b) return NotRecognised("something", "pattern missing");
    uint32_t offA = U32(a + 3), offB = U32(b + 4);
    if (offA != offB) return NotRecognised("something", "the two readings disagree");
    g_offSomething = offA;
    return HookFunction(FunctionStart(a), (void*)Hook_Something, (void**)&Real_Something);
}
```

**Check it.** Search the log for "not recognised", "unavailable" and "NOT hooked". On a working game version,
`Engine: synchronised stereo ready` appears once, after a line that prints the RVA of `Game::update` and the
offsets that were read.

### 3.4 Finding named things: Lua bindings and tables

Game engines leave useful strings behind: the names of Lua functions, shaders, classes. They make excellent
anchors.

- **Lua bindings.** Teardown registers each binding with code that loads the implementation's address and the
  name's address one after the other (`lea r8,[impl]` then `lea rdx,[name]`). So: find the name string in
  `.rdata`, find the instruction that loads its address, and read the other `lea` right before it
  (`FindLuaBinding`). Some interface functions are registered from a table of `{function, name}` pairs
  (`FindUiBinding`). The "core" functions (registry, input, time) sit in a static linked list of
  `{function, name, next}` entries (`FindCoreFunctions`).
- **Function starts.** x64 Windows programs carry an unwind table that lists every function's start and end.
  `RtlLookupFunctionEntry` turns any address inside a function into the function's start (`FunctionStart`).
- **Classes.** A vtable can be confirmed by the RTTI type name stored next to it (used for the D3D12 renderer).

**Sketch.**

```cpp
// The implementation of a Lua binding, from its name.
BYTE* FindLuaBinding(const char* name)
{
    BYTE* str = FindCString(g_rdata, g_rdataSize, name);              // exact, NUL-terminated
    if (!str) return nullptr;
    for (BYTE* p : FindAll(g_text, g_textSize, "48 8D 15 ?? ?? ?? ??")) {   // lea rdx,[rip+rel32]
        if (RelTarget(p + 7, p + 3) != str) continue;
        BYTE* q = p - 7;                                               // the instruction just before
        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05)             // lea r8,[rip+rel32]
            return RelTarget(q + 7, q + 3);
    }
    return nullptr;
}
```

### 3.5 Tools that help the analysis

- **Hardware data breakpoints** (`src/re_tools.cpp`, `[Debug] debugWatchCamera`): put a debug register on a field
  and log every instruction that writes it. This is the fastest answer to "who changes this value?".
- **Call stacks as game offsets** (`LogStack` in `src/log.cpp`).
- **A trace of one frame's GPU commands** (`src/gpu_trace.cpp`, `[Debug] debugGpuTrace=1`): every draw, dispatch,
  copy and pipeline change of one frame, with the game function that issued it. This maps the renderer's passes.
- **A main-thread sampler** (`src/sampler.cpp`, `[Debug] debugProfile=1`): about 1000 stack samples per second,
  reported for slow frames.
- **The engine's own profiler categories**, collected by hooking the function that records them (section 15.1).
- **Independent review.** The important reverse-engineering conclusions of this project were re-checked by a
  second, independent reading before any code relied on them. Several wrong conclusions were caught that way.

---

## 4. Hooking: import tables and MinHook

Once you know where a function is, a *hook* makes the game call your code instead, and your code calls the
original. Teardown VR uses two kinds (`src/hooks.cpp`).

### 4.1 Import table hooks

**Problem.** You want to intercept Windows functions the game calls (clipboard, raw input, cursor, monitor size),
but only the game's own calls, not those of every DLL in the process.

**Idea.** A program calls imported functions through a table of pointers, the Import Address Table (IAT). Replace
one pointer in the game's own table, and only the game's calls through it are redirected.

**In Teardown VR.** `HookIAT(dll, func, replacement)` patches `teardown.exe`'s table and returns the old pointer
(or logs "NOT FOUND"). It is used for the five clipboard functions (section 9), for `GetRawInputData`,
`RegisterRawInputDevices`, `SetCursorPos` and `DefWindowProcA/W` (section 11), and for the window and monitor
functions of the "monitor lie" (section 8). In Teardown's case this works from `DllMain`: the table is already
filled when the DLL is initialised.

**Sketch.**

```cpp
void* HookIAT(const char* dll, const char* func, void* replacement)
{
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);                      // the game's exe
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char*)(base + d->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        auto* slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)byName->Name, func) != 0) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            void* previous = (void*)slots->u1.Function;
            slots->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return previous;
        }
    }
    return nullptr;
}
```

**Pitfalls.**

- Only calls made through *that* table are caught. A function obtained with `GetProcAddress`, or called from
  another DLL, is not.
- Delay-loaded imports have their own table.
- Some programs have no name table (only ordinals). The real `HookIAT` then matches by the resolved address of the
  export.
- If a game's protection fills its import table itself, later than usual, a patch made in `DllMain` can be
  overwritten. Check that your hooks still hold at the first `Present`.

### 4.2 Inline hooks with MinHook

**Problem.** Engine functions are not imported: they are inside the game's own code. DXGI methods are reached
through COM vtables.

**Idea.** Overwrite the first bytes of the target function with a jump to your function, and keep a "trampoline"
that runs the overwritten bytes and jumps back, so you can still call the original. MinHook (BSD-2-Clause,
`third_party/minhook`) does this correctly on x64, including relocating rip-relative instructions.

**In Teardown VR.** `HookFunction(target, detour, &original)` wraps `MH_CreateHook` + `MH_EnableHook` ("already
created" and "already enabled" count as success). Every engine function and every DXGI method is hooked this way.
Every detour calls the original through its trampoline: the mod acts *around* the game's code.

**Sketch.**

```cpp
using UpdateFn = void(__fastcall*)(void* game);
static UpdateFn Real_Update;

static void __fastcall Hook_Update(void* game)
{
    BeforeUpdate(game);   // e.g. change this frame's time (section 6.4)
    Real_Update(game);    // the game's own Game::update, through the trampoline
    AfterUpdate(game);
}

bool InstallUpdateHook(void* target)
{
    return MH_CreateHook(target, (void*)&Hook_Update, (void**)&Real_Update) == MH_OK
        && MH_EnableHook(target) == MH_OK;
}
```

**Pitfalls.**

- **Get the signature right.** x64 Windows has one calling convention, but the number and kind of the arguments
  (integer or floating-point registers) must match what the game passes. Read them from the disassembly of a
  caller.
- **Hooking a COM method hooks it for every object of that class.** That is usually what you want (one `Present`
  hook catches every swap chain), but check `this` when it matters.
- **Re-entrancy.** Your detour may cause the same function to run again (an overlay's own `Present` hook, for
  example). `Hook_Present` keeps a thread-local "already inside" flag.
- **Other threads.** Some hooked engine functions run on the engine's worker threads (particle mesh generation).
  Keep those detours short, avoid locks where possible, and protect them against faults.
- **Keep detours cheap.** They run every frame, sometimes thousands of times per frame (Lua bindings).

### 4.3 Changing a constant without rewriting code

**Problem.** The game's grab keeps a held object at least 1.5 m away, and moves every grabbed object 20 cm closer
than the distance at which it was taken. In VR, objects should stay where the hand takes them. The rule lives in three constants, read by SSE
instructions with rip-relative operands.

**Idea.** Change neither the instructions nor the game's constants. Allocate two floats of your own within 2 GB of
the game's image, and change only the 4-byte displacement of those three instructions so they read your floats.
Your floats then decide: in VR they hold 0 and a short minimum; outside VR they hold the game's own values again.

**In Teardown VR.** `InitGrabDistance` (`src/engine.cpp`): two unique patterns; the constants are checked first
(0.2, 1.5, 1.5, and a maximum between 2 and 10); the floats are allocated with `AllocNearGame`; the displacements
are rewritten with `RedirectOperand`. Apart from MinHook's own jumps, this is the **only** place where the mod
modifies the game's instructions.

**Pitfall.** A rip-relative displacement is a signed 32-bit number: the new target must be within ±2 GB of the
instruction. Allocate near the image (try addresses close to it with `VirtualAlloc`), and compute the new
displacement from the end of the instruction.

### 4.4 Two rules that kept the mod stable

1. **Act around the original.** Call the game's function unchanged, and read or write data before or after it.
2. **Write engine fields only from the thread, and at the moment, where the engine itself would write them.** For
   example, the right eye's camera is replaced inside the camera function's own call, on the main thread, during
   the render (section 6.6), not from another thread "whenever".

---

## 5. Seeing the game's frames: DXGI, D3D12 and OpenXR

### 5.1 Finding the device, the queue and Present

**Problem.** To send the game's image to the headset you need the game's D3D12 device, its command queue, and a
call every time a frame is finished.

**Idea.** Every D3D12 game creates a DXGI factory, creates a swap chain through it, then calls `Present` once per
frame. Hook the factory creation, then the factory's swap-chain creation, then the swap chain's `Present`. For a
D3D12 swap chain, the "device" argument of the creation call **is the command queue**; the device itself comes from
`queue->GetDevice`.

**In Teardown VR** (`src/d3d_hooks.cpp`):

- MinHook on the `dxgi.dll` exports `CreateDXGIFactory`, `CreateDXGIFactory1` and `CreateDXGIFactory2`;
- on the factory's vtable: `CreateSwapChain` (slot 10) and `CreateSwapChainForHwnd` (slot 15);
- on the swap chain: `Present` (8), `ResizeBuffers` (13), `Present1` (22), `ResizeBuffers1` (39); on the queue:
  `ExecuteCommandLists` (10), for measurement only;
- at swap-chain creation, `DXGI_USAGE_SHADER_INPUT` is added to the back buffers so they can be read as a texture.

**Sketch.**

```cpp
static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* device, HWND wnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out,
    IDXGISwapChain1** sc)
{
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    d.BufferUsage |= DXGI_USAGE_SHADER_INPUT;               // we will read the back buffer
    HRESULT hr = Real_CreateSwapChainForHwnd(f, device, wnd, &d, fs, out, sc);
    if (SUCCEEDED(hr)) {
        RememberQueue(*sc, (ID3D12CommandQueue*)device);    // for D3D12, "device" is the queue
        HookSwapChainMethodsOnce(*sc);                      // Present, Present1, ResizeBuffers...
    }
    return hr;
}

static thread_local bool t_inPresent = false;

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (t_inPresent || (flags & DXGI_PRESENT_TEST)) return Real_Present(sc, sync, flags);
    t_inPresent = true;
    OnGamePresent(sc, sync, flags);        // copy the image to the headset; may change sync and flags
    HRESULT hr = Real_Present(sc, sync, flags);
    OnGamePostPresent();                   // wait for the headset, begin the next headset frame
    t_inPresent = false;
    return hr;
}
```

**Pitfalls.**

- **Do not force a size in `ResizeBuffers`.** The game would render for one size and present another. Change what
  the game *believes* about the screen instead (section 8).
- **Before the game resizes**, wait for your own GPU work that reads its back buffers (`OnGameBeforeResize` calls
  `Render::WaitIdle`).
- `Present` is also called with `DXGI_PRESENT_TEST`, only to test occlusion: let those calls through untouched.

### 5.2 An OpenXR session on the game's own device

**Problem.** The headset runtime needs images on a GPU device and queue that it knows.

**Idea.** OpenXR's `XR_KHR_D3D12_enable` extension creates a session on an **existing** D3D12 device and queue.
Give it the game's. Copying the game's image into the headset's images is then a plain GPU copy on one device,
with no resource sharing between devices.

**In Teardown VR** (`src/vr.cpp`). The OpenXR loader is linked statically (`third_party/openxr`). Extensions:
`XR_KHR_D3D12_enable` (required), `XR_FB_display_refresh_rate` (when offered), `XR_KHR_visibility_mask` (only when
a lens-mask test asks for it). Reference space STAGE when available, else LOCAL, plus a VIEW space for the head.
Swapchains: one per eye, one for the virtual screen, one for the settings panel.

**Sketch.**

```cpp
ID3D12Device* device = nullptr;
gameQueue->GetDevice(IID_PPV_ARGS(&device));

// Required by the specification before xrCreateSession, and a good sanity check.
XrGraphicsRequirementsD3D12KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
pfnGetD3D12GraphicsRequirementsKHR(instance, systemId, &req);   // from xrGetInstanceProcAddr
LUID luid = device->GetAdapterLuid();
if (memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0)
    Log("XR: WARNING the headset runtime wants another GPU than the game uses");

XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
binding.device = device;
binding.queue = gameQueue;                                      // the game's own queue
XrSessionCreateInfo info{ XR_TYPE_SESSION_CREATE_INFO };
info.next = &binding;
info.systemId = systemId;
xrCreateSession(instance, &info, &session);
```

**Pitfalls.**

- **Never under the loader lock.** All of this happens from the first `Present`, never in `DllMain`.
- **Handle the session's whole life**: `READY` leads to `xrBeginSession`; `STOPPING` to releasing the images and
  `xrEndSession`; `EXITING` or `LOSS_PENDING` to destroying the session and retrying a few seconds later. Players
  take the headset off, restart the streaming app, or start the game before the headset.
- **Time the runtime's calls.** A small wrapper (`XrCallTimer`) logs any OpenXR call that blocks for more than
  15 ms. Streaming runtimes do block (section 15.7).

### 5.3 Getting the image to the headset

**Problem.** The game draws into its back buffer. The headset wants one image per eye, plus any flat panels.

**Idea.** At each `Present`, just before the real one, record a tiny command list on the game's own queue that
copies the back buffer (with a full-screen triangle) into the current eye's swapchain image. Everything that is not
the 3D view (settings, inventory, warning banners, the menus' virtual screen) is sent as separate **quad layers**:
the runtime composites them with the newest head pose, so they are identical in both eyes and lag no game frame
behind.

**In Teardown VR.**

- `src/render.cpp`: a ring of 4 command allocators, one command list, one fence, descriptor heaps, and a blit
  shader written for the mod (compiled at run time with `d3dcompiler_47.dll`) that can crop, sharpen, change the
  brightness and draw a comfort vignette.
- `OnGamePresent` (`src/vr.cpp`): a stereo frame goes to its eye's image. Frame A submits nothing; frame B submits
  the projection layer with both views. A non-stereo frame (menu, loading screen, pause) goes to the virtual screen
  quad. The settings panel and the inventory are added as quads when shown.

**Sketch.**

```cpp
void OnGamePresent(IDXGISwapChain3* sc, UINT& sync, UINT& flags)
{
    int eye = EngineConsumeFrameEye();        // 0 = frame A (left), 1 = frame B (right), -1 = not stereo
    Render::Begin();
    if (eye >= 0 && g_xrFrameBegun) {
        BlitInto(g_eyeSwapchain[eye], backBuffer);                 // sRGB-correct copy
        if (eye == 0) g_leftDone = true;                           // wait for the right eye
        else if (g_leftDone) EndXrFrame(ProjectionLayer(g_views)); // both eyes, same predicted time
    } else {
        BlitInto(g_screenSwapchain, backBuffer);
        EndXrFrame(QuadLayer(ScreenPose()));                       // menus on a floating screen
    }
    Render::Submit();                                              // on the game's own queue
    sync = 0;                                                      // the headset paces the game (5.4)
    if (g_tearingAllowed) flags |= DXGI_PRESENT_ALLOW_TEARING;
}
```

**Pitfalls.**

- **sRGB.** Read the back buffer as UNORM, convert to linear in the shader, and write to an sRGB render target view
  that converts back. The pixels come out identical, and filtering is correct when the image is scaled.
- **Never re-create the eye swapchains in the middle of a pair** (between frame A and frame B).
- **Quad layers have no depth test.** A panel placed far away would be drawn over nearer walls, so Teardown VR
  limits how far panels can be placed.

### 5.4 Pacing: who waits for whom

**Problem.** Measured: 12 ms per frame with the CPU and the GPU only one-third busy (problem C7).

**Idea.** While the session runs, the headset must set the rhythm, not the PC monitor. Call `xrWaitFrame` once per
stereo pair and present the PC window with no vertical sync. With a flip-model swap chain created with the tearing
flag, also pass `DXGI_PRESENT_ALLOW_TEARING`: without it, the desktop compositor still held the game to the
monitor's refresh rate.

**In Teardown VR.** `OnGamePostPresent` calls `BeginXrFrame` right after the real `Present`: `xrWaitFrame`,
`xrBeginFrame`, `xrLocateViews` at the predicted display time, then the controllers. So the wait happens between
two game frames, never in the middle of one, and frame A of the next pair starts with poses predicted for its own
display time. `Present` gets sync interval 0, plus `ALLOW_TEARING` when the swap chain allows it and is not in
exclusive full screen.

**Check it.** 4 to 6 ms per frame instead of 12 ms in the same scene. The log's "Stereo frames" line, written every
600 frames, gives the game and headset frame rates.

### 5.5 Choosing the OpenXR runtime

**Problem.** With nothing chosen, the OpenXR loader uses Windows' active runtime. Virtual Desktop's streamer makes
itself the active runtime whenever it runs, so a player using Quest Link found "no headset" (problem L1).

**Idea.** At start, ask each installed runtime in turn whether it has a headset, and use the first that says yes.

**In Teardown VR.** `RuntimeStartup` in `src/vr.cpp`, with `[Affichage] headsetRuntime=0` (automatic, the
default). For each candidate it puts the runtime's manifest in the `XR_RUNTIME_JSON` environment variable, then
calls `xrCreateInstance` + `xrGetSystem` + `xrDestroyInstance`: the statically linked loader re-reads the variable
at each instance creation and unloads the runtime when the instance is destroyed. Order: Windows' active runtime,
then Virtual Desktop, then Meta Quest Link (only while the named event `OculusHMDConnected` is set: without it,
Meta's runtime can only answer "no headset"), then SteamVR last, and only if it is Windows' runtime (asking it
starts SteamVR). The whole detection has a 3 s budget. Values 1 to 3 force one runtime; 4 = Windows' runtime
without detection.

**Sketch.**

```cpp
bool RuntimeHasHeadset(const wchar_t* manifestJson)
{
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifestJson);   // re-read at each xrCreateInstance
    XrInstance inst;
    if (XR_FAILED(CreateMinimalInstance(&inst))) return false;   // with XR_KHR_D3D12_enable
    XrSystemGetInfo gi{ XR_TYPE_SYSTEM_GET_INFO };
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    XrResult r = xrGetSystem(inst, &gi, &sys);
    xrDestroyInstance(inst);                                     // unloads that runtime
    return r == XR_SUCCESS;                                      // FORM_FACTOR_UNAVAILABLE = no headset
}
```

**Pitfalls.**

- Only one OpenXR instance can exist at a time.
- `xrEnumerateInstanceExtensionProperties` loads a runtime **without** unloading it: never call it between probes.
- A game process running as administrator ignores `XR_RUNTIME_JSON`.
- **Never test automatic detection on a development PC against the real runtimes**: it would start them. Use the
  test list `[Debug] xrAutoRuntimes=<a.json>;<b.json>` with the mock (section 16).

**Check it.** The log line `XR: headset detection: ... has the headset - the game uses ...`.

---

## 6. Rendering two eyes

This is the heart of a VR mod for a flat game. There are two ways to do it.

### 6.1 Why not two views in one frame?

The obvious idea is to render the scene twice inside one engine frame, once per eye. In Teardown this hung the GPU
(`DXGI_ERROR_DEVICE_HUNG`): the renderer keeps per-frame GPU data (constant buffers, ring allocations, temporal
histories), and a second render in the same frame overwrites data the first one is still using (problem B1).
Unless you control the engine, assume its renderer can draw only one view per frame.

### 6.2 Alternating eyes: simple, but each eye sees a different moment

**Idea.** Render each engine frame for the other eye in turn: left, right, left, right. Every frame is a normal
game frame.

**Flaw.** Between two engine frames the game moves on: physics, scripts, the player. Anything that moves is at a
different place in each eye, which reads as false depth and is uncomfortable. Testers rejected it: native VR games
show both eyes at the same instant.

**In Teardown VR.** Kept as a fallback: `[Affichage] stereoMode=1`, and automatically when the engine is not
recognised (section 3.3). It is also the easiest first milestone for a new VR mod: it needs the camera, not the
engine's timing.

### 6.3 Synchronised stereo: frame A and frame B

**Idea.** Use two engine frames per headset frame, but make them show **the same simulation state**:

| | `Game::update` | physics | camera |
|---|---|---|---|
| frame A | normal | normal fixed steps | left eye |
| frame B | zero time, or not run at all | no step | right eye |

**In Teardown VR.**

1. During frame A, the Lua mod's request reaches `VrHandleLuaRequest` (`src/vr.cpp`). The engine is not in a
   frozen frame, so this is eye 0: `EngineRequestStereo()` records "left now, frozen right eye next", and the eye
   poses, the eye offset and both projections are handed to the engine (`EngineSetEyeOffset`,
   `EngineSetEyeProjection`...).
2. At the start of the next frame, `Hook_Update` sees the pending right eye and freezes time (6.4).
3. In that frame the Lua mod, if it is ticked at all, gets "eye 1, frozen" (field 81 of the reply) and must not
   change the world (section 9.5).
4. Each `Present` reports which eye it showed (`EngineConsumeFrameEye`), and `vr.cpp` copies the image accordingly
   (5.3).

### 6.4 Freezing game time for frame B

**Problem.** Frame B must draw exactly the instant frame A drew. Pausing the game does not work: scripts and
animations stop, menus change.

**Idea.** Teardown runs a fixed-step physics loop. Every frame, `Game::frame` adds the frame time to an accumulator
*before* `Game::update`, and right *after* it runs `while (acc >= step) step();`. The render then interpolates
between the last two physics states with `alpha = acc / step`. So, in frame B: take the frame's time back out of
the accumulators and set the frame time to almost zero. The physics loop runs no step, and `alpha` stays exactly
as in frame A. Then give the removed time to the next frame A, so the game still runs at real speed.

**In Teardown VR.** `Hook_Update` in `src/engine.cpp` edits three fields of the game object, found by pattern
(3.2): the frame time `dt` and the two accumulators.

**Sketch.**

```cpp
static void __fastcall Hook_Update(BYTE* game)
{
    float& dt   = *(float*)(game + g_offDt);
    float& acc1 = *(float*)(game + g_offAcc1);
    float& acc2 = *(float*)(game + g_offAcc2);
    const float frameTime = dt;

    if (g_rightPending) {                      // frame B
        g_rightPending = false;
        g_savedTime += frameTime;              // played in the next frame A
        acc1 = std::max(0.0f, acc1 - frameTime);
        acc2 = std::max(0.0f, acc2 - frameTime);
        dt = 1e-5f;                            // "zero", but never a division by zero
        g_inFrozenFrame = true;
        Real_Update(game);                     // or replay frame A's requests instead (section 15.4)
        g_inFrozenFrame = false;
        g_rightThisFrame = true;
        return;
    }
    // Frame A. With a headset, the time step is the headset's display interval instead (6.5).
    if (g_savedTime > 0) {
        dt += g_savedTime;
        acc1 = std::min(acc1 + g_savedTime, kMaxAccumulated);   // the engine's own clamp, 2.99/60 s
        acc2 = std::min(acc2 + g_savedTime, kMaxAccumulated);
        g_savedTime = 0;
    }
    Real_Update(game);
}
```

**Pitfalls.**

- Use a tiny time, not exactly zero: some code divides by `dt`.
- Scripts that count ticks instead of using `dt` still move things in frame B (6.9).
- Read and write these fields only inside the hook, on the main thread.

### 6.5 Making game time follow the headset

**Problem.** Walking looked uneven: the camera moved 72 to 175 mm per pair instead of a steady amount
(problem C5). The game advanced by measured CPU time, which is irregular, while the headset displays at a fixed
rate.

**Idea.** Advance each frame A by exactly the gap between two consecutive predicted display times from
`xrWaitFrame`. Motion is then even on the headset's display.

**In Teardown VR.** `vr.cpp` passes the gap to `EngineSetVrFrameDelta`; `Hook_Update` uses it as frame A's `dt` (at
most 0.1 s) and rebuilds the accumulators from it. Two refinements:

- **`[Affichage] limitCatchUp=1`.** After a late frame, the next interval covers two physics steps, the loop runs
  both, and that frame is slow too: hitches chain up during explosions. The accumulator is capped so that at most
  one physics step runs per frame when the headset is faster than the physics. The extra time is dropped (a short
  slow-down instead of a chain of hitches) and counted in the log (problem G3).
- **`[Performance] holdSmooth=1`.** When the runtime held the previous `xrEndFrame` for longer than a refresh
  (streaming back-pressure, section 15.7), the gap covers the whole hold (20 to 90 ms) and the world jumped forward.
  After a hold, the pair advances by one refresh only: the game resumes where it stopped (problem G2).

**Pitfall.** The headset interval replaces the game's frame time without the game's time scale, so slow-motion
mods (`SetTimeScale`) have no effect in VR. This is a known, unfixed limit (problem C5).

**Check it.** With `[Debug] debugLua=1`, the log compares game time with real time ("game time X s for Y s of real
time") and reports the mean camera step per pair. Measured afterwards: a steady 150 mm per pair while walking.

### 6.6 The right eye's camera comes from the left eye's

**Problem.** The right eye's view drifted away from the left one by up to 43 cm and 18 degrees, worst during jumps
and landings (problem B3).

**Root cause.** Even in a zero-time frame, the game applies per-frame camera effects (height smoothing after a
jump, landing shake) when it builds the render camera.

**Idea.** Ignore the game's camera in frame B. Record the left eye's **actual** render camera in frame A, and build
the right eye's from it, moved by the eye offset (the right eye as seen from the left, times the world scale).

**In Teardown VR.** `Hook_CamSet` hooks `Camera::setTransform`, which `Game::lateUpdate` calls with the camera
object's position and rotation. In frame A, the camera is recorded (`g_leftCam`), and its rotation is rebuilt from
the headset's left-eye rotation and the VR room's yaw sent by the Lua mod (`TDVRA`): the game's own rotation
follows the player's aim and shook when the arm moved. In frame B, camera = `g_leftCam` × eye offset. The
projection is written in the same hook (section 7).

**Sketch.**

```cpp
static void __fastcall Hook_CamSet(void* cam, const Vec3* pos, const Quat* rot)
{
    Vec3 p = *pos;
    Quat q = *rot;
    if (g_leftThisFrame) {
        q = YawQuat(g_roomYaw) * g_headsetLeftEyeRot;   // not the game's aim-driven rotation
        g_leftCam = { p, q };
    } else if (g_rightThisFrame) {
        Pose r = g_leftCam * g_eyeOffset;                // the game's own camera is ignored
        p = r.pos;
        q = r.rot;
    }
    Real_CamSet(cam, &p, &q);
    WriteEyeProjection(ProjectionOf(cam), g_rightThisFrame ? 1 : 0);   // section 7
}
```

**Check it.** `[Debug] debugLua=1` logs the gap between the rendered and the requested camera for each eye.
Measured afterwards: 0 px of vertical offset between the eyes in the middle of a jump.

### 6.7 Per-eye temporal history

**Problem.** The image was soft, worst up close, with alternating halos along edges (problem E1).

**Root cause.** Modern renderers are temporal: TAA, the disocclusion mask, linear depth, and the light, reflection
and fog denoisers all blend the current frame with "the previous frame". With one eye per engine frame, the
previous frame was the **other** eye's, seen from 6 cm away.

**Idea.** Keep two complete sets of history, one per eye, and swap the right set in at render entry.

**In Teardown VR.** `Hook_Render` (`Renderer::render`, which has a single call site). All of its history sits in a
few fields: 10 history framebuffers, 5 TAA textures, the ping-pong indices, the previous stable view-projection and
view matrices, the TAA counter and a reset flag (offsets in RE § 5 ter). The second set is created with the
engine's **own** allocators (its framebuffer creation function, `TemporalAa::createResources`) and re-created when
sizes or formats change. Auto-exposure stays shared on purpose, so both eyes have the same brightness. Setting:
`[Affichage] perEyeHistory=1`.

**Sketch.**

```cpp
struct EyeHistory {
    void* fbo[10];
    void* taaTexture[5];
    int   pingPong[2];
    float prevStableViewProj[16], prevView[16];
    int   taaCounter;
};
static EyeHistory g_hist[2];
static int g_bound = 0;               // whose set is in the renderer right now

static void SwapHistory(BYTE* renderer, int eye)
{
    if (eye == g_bound) return;
    SaveFrom(renderer, g_hist[g_bound]);   // copy the fields out
    LoadInto(renderer, g_hist[eye]);       // copy the other eye's set in
    g_bound = eye;
}
```

**Pitfalls.**

- Create the second set the way the engine does, or the engine may free or resize it in ways you do not expect.
- Reset an eye's history when it is stale (a pair was skipped, a size changed).
- Verify every field before hooking (section 3.3): one wrong offset here corrupts the renderer.

**Check it.** Sharpness measured as the variance of the Laplacian on mock captures: × 2.5 in the distance, × 6 to
7 up close.

### 6.8 Same noise, same jitter in both eyes

**Problem.** Thin smoke and rain "fought" between the eyes (problem F1).

**Root cause.** All of the engine's screen-space noise (particles, glass, ambient occlusion, sun, fog, reflections)
is animated by one number per frame, derived from the TAA counter, which steps once per frame and also sets the
sub-pixel jitter. With two history sets, the right eye's counter was one step ahead: it always drew the *next*
noise frame. For thin smoke, the two eyes' dot patterns did not overlap at all.

**Idea.** At the start of frame B's render, set the counter back to the value frame A started with. Both eyes then
use the same noise frame and the same jitter, and each eye still steps through every noise frame over time.

**In Teardown VR.** `SameNoiseBegin` / `SameNoiseEnd` in `src/engine.cpp`, `[Affichage] stereoSameNoise=1`. The
end function checks the result after every render and logs a summary every minute.

**Check it.** The log line `Engine: stereoSameNoise=1 ...` says in how many pairs both eyes had the same noise
frame. Setting `stereoSameNoise=0` in the control file shows "in 0 of N": a live measurement of the bug.

This is necessary, but not sufficient: see section 14.

### 6.9 Things that still change in frame B

Freezing time is not enough: some work runs per frame, not per unit of time.

- **Mods that move objects by a fixed step per tick.** The AC130 mod's plane moved 0.15 m between the eyes and
  looked doubled. `FixObjectMotion` (`[Affichage] syncScriptMotion=1`) records every shape's render transforms
  when the left eye renders, and puts them back for the right eye where a script moved them (problem B4).
- **Motion vectors.** Each shape keeps its previous transform for TAA motion vectors. In frame B "previous" equals
  "current" (no time passed), so moving objects smeared in the right eye. The same `FixObjectMotion` restores the
  left eye's previous transforms (problem B5).
- **Particles simulated in the wrong frame.** Above 75 fps, Teardown's particle update takes a "late" path that
  simulates the previous frame's step. In stereo every frame is that short, so particles were simulated in frame B.
  `Hook_PsUpdate` (`[Affichage] syncParticles=1`) makes frame A simulate and frame B only rebuild the particle
  mesh. Frame B also gets frame A's frame time back during its render, so fast sparks are stretched the same in
  both eyes (problem B6).
- **Input.** Keyboard and mouse messages that arrive while a frame B is due are held back and re-posted after
  `Present`, so the game's "just pressed" logic sees them in a normal frame (section 11.4).

**Sketch** of the object fix:

```cpp
// Frame A, at render entry, after the engine's own wait for its shape jobs:
for (Shape* s : SceneShapes()) g_left.put(s, { s->renderCurrent, s->renderPrevious });

// Frame B, same place:
for (Shape* s : SceneShapes()) {
    const Record* r = g_left.find(s);
    if (!r || s->renderPrevious != r->current) continue;   // not the same shape (a reused address)
    s->renderPrevious = r->previous;                       // motion vectors as the left eye had them
    s->renderCurrent  = r->current;                        // undo a move a script made in frame B
}
```

**Pitfall.** Those transforms are written by worker threads, and the engine only waits for them deep inside
`Renderer::render`. `FixObjectMotion` first calls the engine's own wait (found by pattern), then reads.

### 6.10 Checking stereo

- **`[Debug] debugSyncTest=1`**: the Lua mod draws a red marker for the left eye and a blue one for the right; the
  DLL reads back pixels of each presented frame and counts right and wrong eyes.
- **The "Stereo frames" log line**, every 600 frames: how many frames went to the left eye, the right eye and
  elsewhere, the game and headset frame rates, per-eye work, and missed headset refreshes.
- **Stereo disparity on mock captures**: measure the left-to-right offset of the same region (normalised
  cross-correlation) and compare an object with the ground at a known distance. A moving object that appears
  "beyond infinity" is the signature of two different game instants (problem B4).

---

## 7. Off-centre projections and the field of view

### 7.1 Know your engine's field-of-view convention

**Problem.** The image looked squeezed, and the eyes could not fuse it into 3D (problem C1).

**Root cause.** Teardown's projection builder treats `SetCameraFov(fov)` as the **horizontal** field of a **16:9**
image: `tan(vertical/2) = tan(fov/2) × 9/16`, and the horizontal field then follows the image's real aspect
ratio. With a nearly square VR image, passing the headset's field naively rendered a view about 46 % narrower
than the one declared to the headset.

**Idea.** Convert: `fov = 2·atan(tan(vertical/2) × 16/9)`. The DLL computes this value and sends it to the Lua mod
(reply field 5).

**Check it.** `[Debug] debugFovTest=1` draws test lines at known angles; on the mock they land on the expected
pixels.

### 7.2 Off-centre projection per eye

**Problem.** A headset's field of view is not symmetric. On a Quest 3 through Virtual Desktop the tangents are
about 1.38 outward, 0.84 toward the nose, 0.97 up and 1.43 down, mirrored for the other eye. A symmetric frustum
that contains this wastes pixels, and the image centre, where the game draws its crosshair, is not straight ahead:
it is about 15 degrees outward and 13 degrees down, on opposite sides for each eye.

**Idea.** A renderer builds its view rays, culling and jitter from one 4×4 projection matrix. Write the off-centre
terms into that matrix, and everything downstream follows.

**In Teardown VR.** `[Affichage] offCenter=1`. The engine's only perspective builder sets the off-centre terms to
zero, but culling, the shaders' view rays, the TAA jitter and the 2D interface all start from the full matrix
(RE § 5 ter). `Hook_CamSet` writes them for each eye, from the OpenXR tangents.

**Sketch.**

```cpp
// tl, tr, tu, td = tangents of the eye's left, right, up and down half-angles (all positive)
void WriteOffCenter(float* m, float tl, float tr, float tu, float td)
{
    m[0] = 2.0f / (tl + tr);
    m[8] = (tr - tl) / (tl + tr);   // horizontal shift
    m[5] = 2.0f / (tu + td);
    m[9] = (tu - td) / (tu + td);   // vertical shift
}
```

**Pitfalls.**

- The signs and indices depend on your engine's matrix layout (row or column major, handedness). Prove them with
  a pixel test, not by reasoning.
- **The two eyes' pixel grids no longer line up**: the same distant point is about 730 px apart in the two
  images. Anything drawn "at the same pixel" in both eyes (screen-space noise, the 2D HUD) now disagrees (sections
  7.3 and 14).
- The pixel saving is small (about 4 % on a Quest 3). The main reason is correctness.

### 7.3 The 2D interface per eye

**Problem.** The crosshair and the HUD text at the canvas centre were seen double (problem J4).

**Idea.** The HUD is drawn with an orthographic camera. Add a per-eye shift to it, so the canvas centre is seen in
the same direction by both eyes: straight ahead of the head (or along a mod camera's axis), at infinity or at a
chosen distance.

**In Teardown VR.** `ApplyUiShift`, called from `Hook_CamSet`, changes the "canvas to pixels" part of the UI
camera (`[Affichage] hudFix=1`, `hudDistance`, 0 = infinity). `UiWorldToPixel` stays exact because it uses the same
matrix.

**Limit.** The canvas keeps its size, so a shifted canvas leaves part of the image: a full-screen fade leaves a band
of about 10-12 % on one side.

---

## 8. Render resolution and the monitor lie

### 8.1 Making the game render at the headset's size

**Problem.** Teardown sizes its window and all its render targets from the monitor, once, at start. A 1920 × 1080
monitor gives a blurry, wrongly shaped image in the headset. Resizing during play gave a broken image: the game
does not re-create its targets (problem D1).

**Idea.** Lie about the monitor. When the game asks Windows how big the screen is, answer with the VR render size.
Block real display-mode changes, so the player's monitor is never touched.

**In Teardown VR.** Import-table hooks in `src/d3d_hooks.cpp`, all driven by `GetSwapchainSizeOverride()` in
`src/vr.cpp`: `GetMonitorInfoA` (monitor and work rectangles), `QueryDisplayConfig` (source mode size),
`GetSystemMetrics(SM_CXSCREEN / SM_CYSCREEN)`, `CreateWindowExW` and `SetWindowPos` (pop-up windows resized), and
`ChangeDisplaySettingsExA` (answers "success" without changing anything). The game then renders natively at, for
example, 3024 × 3264.

**Sketch.**

```cpp
static BOOL WINAPI Hook_GetMonitorInfoA(HMONITOR mon, LPMONITORINFO mi)
{
    BOOL ok = Real_GetMonitorInfoA(mon, mi);
    SIZE vr = GetSwapchainSizeOverride();          // 0 x 0 = no headset at start: tell the truth
    if (ok && vr.cx > 0) {
        mi->rcMonitor.right  = mi->rcMonitor.left + vr.cx;
        mi->rcMonitor.bottom = mi->rcMonitor.top  + vr.cy;
        mi->rcWork = mi->rcMonitor;
    }
    return ok;
}

static LONG WINAPI Hook_ChangeDisplaySettingsExA(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID)
{
    return DISP_CHANGE_SUCCESSFUL;                 // never change the real monitor mode
}
```

### 8.2 Decide once, at start, and say so

**Problem.** Players saw a blurry 1920 × 1080 image when the game started before the headset was connected
(problem D2).

**Idea.** The size is chosen once, so ask the headset runtime at start whether a headset is there
(`HeadsetInUse`, with the probe of section 5.5). If not, use the real monitor. If a headset appears later, show a
clear message instead of a silent blur.

**In Teardown VR.** The log says `Render resolution: headset connected when the game started` (or "not
connected"). `VrRenderSizeLate()` drives an in-headset banner: "Low resolution: restart the game with the headset
connected."

### 8.3 Presets and the headset's aspect ratio

- `[Affichage] renderPreset` chooses the height in lines: 1 Low 1280, 2 Medium 1600, 3 High 1920, 4 Very high
  2272, 5 Ultra 2720, 6 Headset native 3264 (the default); 0 = the PC screen.
- The width is the height × `vrAspect`, the headset's field-of-view aspect, rounded up to a multiple of 8.
  `UpdateRenderResolution()` measures it from the views and saves it when it changes by more than 0.01; it is used
  from the **next** start. The default (0.925, a Quest 3) gives the right shape at the very first launch.
- `eyeResScale` scales the images sent to the headset separately from the render size.

### 8.4 The full-screen trap

**Problem.** A sudden blur, in a session where nothing in the mod had changed (problem D3).

**Root cause.** The game's own display option had become "full screen". In that mode the game picks a resolution
from the monitor's list of modes (1920 × 1080), whatever size is reported.

**Fix.** `FixFullscreenOption` (`src/d3d_hooks.cpp`, called from `DllMain` with plain file calls) rewrites the
display mode in `%LOCALAPPDATA%\Teardown\options.xml` from full screen to borderless window at every start. This is
the only value the DLL writes to `options.xml`. (The Lua mod also changes one game option while VR runs: it sets
the head bob, `options.input.headbob`, to 0 and puts the previous value back when VR stops; see 10.2.)

**Check it.** Whenever blur is reported, look first at the log line `XR: swapchain WxH`.

### 8.5 Rendering less than the full field: crop, do not zoom

**Problem.** Lowering "Field of view covered" made the image zoomed and fish-eyed instead of cropped, and nothing
got faster (problem D6).

**Root cause.** The DLL rendered a narrower field and declared it to the runtime, but Virtual Desktop's stream
showed it stretched over the eye's whole field anyway. And the render size did not shrink.

**Idea.** Always submit an image that covers the eye's **whole** field (the case known to work). Render only the
covered part, place it where it belongs inside that image, and paint the rest black.

**In Teardown VR.** `[Affichage] fovCrop=1` (default) with `fovCoverage` c in [0.5, 1]: the render size is the
preset × c in each direction (same sharpness, c² of the pixels), `CropRect()` places the game image, and
`ClearOutside()` blackens the border.

**Lesson.** A runtime may accept a projection-layer field of view that it does not honour. When in doubt, submit
what is known to work and do the clever part yourself.

---

## 9. Talking to the game's Lua: the bridge

### 9.1 Why a bridge

The Lua mod moves the camera, turns the player, places the tool and spawns physics bodies through the game's own
API. It needs the head and hand poses and the buttons from the DLL every frame. But Teardown's Lua sandbox has no
files and no sockets (problem A3).

### 9.2 The clipboard trick

**Idea.** The sandbox does offer `SetClipboardText` and `GetClipboardText`. The game implements them with the
Windows clipboard functions, which it imports. Hook those imports. A text that starts with a magic prefix is a
message for the DLL: consume it, never touch the real clipboard, and make the next "read clipboard" return the
DLL's answer. Any other text goes to the real clipboard, so copy and paste keep working.

**In Teardown VR.** `src/bridge.cpp` hooks the game's imports of `OpenClipboard`, `EmptyClipboard`,
`SetClipboardData`, `GetClipboardData` and `CloseClipboard`. Opening is deferred (a "virtual" open). A
`TDVR2 ...` request calls `VrHandleLuaRequest` in `src/vr.cpp` and arms the answer (in ANSI and UTF-16) for the
next `GetClipboardData`; `CloseClipboard` disarms it once it has been read.

**Sketch.** The Lua side:

```lua
-- Every message from Lua must be 30 characters or fewer (9.3).
local function exchange(msg)
  for attempt = 1, 2 do
    local ok = pcall(SetClipboardText, msg)
    local ok2, reply = false, nil
    if ok then ok2, reply = pcall(GetClipboardText) end
    if ok2 and type(reply) == "string" and string.sub(reply, 1, 5) == "TDVR2" then
      return reply
    end
  end
  return nil     -- no DLL: the mod stays inactive
end

-- e.g. "TDVR2 99999 63 1.00 1.00 1.00" = 29 characters
local reply = exchange(string.format("TDVR2 %d %d %.2f %.2f %.2f", seq, flags, hapticL, hapticR, vignette))
```

The DLL side:

```cpp
static HANDLE WINAPI Hook_SetClipboardData(UINT format, HANDLE data)
{
    std::string text;
    if (IsText(format) && ReadGlobalText(format, data, text) && text.rfind("TDVR", 0) == 0) {
        GlobalFree(data);                         // it never reaches the real clipboard
        if (text.rfind("TDVR2 ", 0) == 0)
            ArmReply(VrHandleLuaRequest(text));   // returned by the next GetClipboardData
        else
            HandleOneWayMessage(text);            // TDVRY, TDVRT, TDVRL...
        return data;                              // non-null = "success" for the caller
    }
    OpenRealClipboardIfDeferred();
    return Real_SetClipboardData(format, data);
}
```

### 9.3 The 30-character limit

**Problem.** About every 0.4 s, a request arrived empty, and that frame's poses were lost (problem A4).

**Root cause.** A bug in the game's `SetClipboardText` binding: it copies the Lua string into a temporary
`std::string`, destroys it, then reads its buffer again (a use after free). A long string lives on the heap, and the
freed block is sometimes reused in between. A string of **at most 30 characters** stays inside the `std::string`'s
own inline buffer (the small-string optimisation), which the destructor does not touch.

**Fix.** Every message from Lua is 30 characters or fewer. Longer data is cut into chunks: the VR inventory's tool
list travels as several short messages (`TDVRIB`, `TDVRIT`, `TDVRIN` with 14-character pieces of each name...),
with a version number, an acknowledgement in the reply, and a re-send after 1 s if the list is incomplete. Replies
(from the DLL to Lua) can be long: they do not go through that binding. The one exception on the Lua side is the
diagnostic `TDVRL <text>` lines: they can be longer, and losing one costs nothing but a log line.

**Pitfall.** This is a hard rule for anyone who extends the protocol. A message one character too long works most
of the time, which makes the bug look random.

### 9.4 The protocol

- **Request, once per Lua tick**: `TDVR2 <seq> <flags> <hapticL> <hapticR> <vignette>`. The flags tell the DLL the
  game's state: in a vehicle, grabbing, map open, game camera near the player, off hand holding something at a
  distance.
- **Reply**: space-separated numbers. A status (0 = VR inactive, 1 = active, 2 = paused on the virtual screen),
  then the eye, the field of view, the eye, head and hand poses, the sticks, triggers and buttons, the settings,
  the frozen flag and tool information. Optional tagged groups follow (` PIV ...`, ` ARM ...`, ` INV ...`); they
  are found by their tag, so an older Lua mod simply ignores them.
- **One-way messages from Lua**, each 30 characters or fewer: `TDVRY <deg>` (player yaw), `TDVRA <deg>` (VR room
  yaw), `TDVRT x y z` (laser target), and the others listed in ARCHITECTURE.md (section 6) and `src/bridge.cpp`.
  The exception is `TDVRL <text>` (a diagnostic line for the log), which can be longer and may be lost.
- **Versioning.** The prefix carries the protocol version. An old first-version copy of the Lua mod, left enabled
  among a player's local mods, sends `TDVR1`: the DLL answers "VR inactive" and logs "OLD copy", so that copy does
  nothing (problem A5).

### 9.5 Frozen frames on the Lua side

When the reply says "frozen" (frame B), the Lua mod only re-places the camera, the tool and the laser for the right
eye. It does not turn the player, walk, push physics, pick inventory items or change the fog. Values measured in
frame A are reused so both eyes agree. A world-space value reused in frame B can still be wrong if the player moved
between the two ticks: the held tool trembled in the right eye until its pose was kept relative to the player's
body instead (problem I7).

### 9.6 Pitfalls of the Lua side

- **The sandbox check.** `SetClipboardText` and `GetClipboardText` are reserved for "internal" mods. Teardown VR
  **never patches that check**. The Lua mod is installed in the game's own `mods` folder, where it is listed as a
  built-in mod and is allowed by the game's own rules. The player enables it once in the mod manager.
- **Lua 5.1 limits** matter in a 4600-line script: 200 local variables per function, 60 upvalues per closure.
  New state goes into global tables.
- **Quicksave.** A global table that holds entity handles must carry `nonserializable = true`, or loading a
  quicksave restores stale handles (problem I15).
- **When the DLL is missing**, the Lua mod retries every 2 s and prints one message to the game's debug output.

**Check it.** `[Debug] debugLua=1` logs lost requests and the number of Lua requests per game update (exactly one
is expected). When a request was lost, `BridgeDumpTrace` prints the last 32 clipboard calls.

---

## 10. Camera, tool and hands

### 10.1 Moving the camera to the eye

**Problem.** `SetCameraTransform` takes the camera over, but in that mode the game hides the tool in the player's
hand (problem C3).

**Idea.** Keep the player's normal first-person camera, and add an **offset** that moves it to the eye.

**In Teardown VR.** Every tick, the Lua mod calls `SetCameraOffsetTransform(local(playerCamera, eye))`,
`SetCameraFov(fov)` with the converted value of section 7.1, and `RequestFirstPerson(false)` (the request only
lasts one frame). The DLL then fixes the exact camera of each eye in `Hook_CamSet` (section 6.6).

**Pitfall.** The offset applies to the player camera *as it will be rendered*, after the tick. The script turns
and moves the player during the same tick, so the offset must be computed against the **predicted** camera (aim
rotation applied, movement added); otherwise the left eye turns by that much for one frame.

**Check it.** Read `GetCameraTransform()` in `draw()` and compare it with the requested eye: below 0.03° and 3 mm.

### 10.2 Turning the player without jerks

**Problem.** Re-orienting the player every frame with the call that sets position, yaw and pitch together also
re-positioned them: the camera and the tool jerked by up to 39 mm (problem C4).

**Fix.** Pitch goes through `SetPlayerPitch`. Yaw is written by the DLL straight into the local player's yaw field
(`EngineSetPlayerYaw`; the field was found by reading the code of `GetPlayerYaw`), from the Lua message
`TDVRY <deg>`. Also, the game's walking head bob (`options.input.headbob`) is set to 0 while VR runs (problem C6).

### 10.3 The tool in the VR hand

**Problem.** In first person, the engine ignores `SetToolTransformOverride`: the tool stayed where the desktop game
puts it (problem I1).

**Idea.** Let the engine's tool animation run, then replace the final tool transform so the tool's handle lands in
the VR hand, while keeping the game's own animation (recoil, swing, sway) relative to that handle.

**In Teardown VR.** `Hook_ToolAnim` (`ToolAnim::update`). For the game's own tools:

```text
final = hand × inverse(grip) × inverse(rest) × animated
```

- `animated`: the animator's pose, before the desktop offsets;
- `rest`: the tool's first-person rest pose (`fp_action` in `data/prefab/tool/<name>.xml`, read at run time);
- `grip`: where a hand holds the tool (`tp_action_rh` in the same file).

Tools added by mods take another path: their own desktop pose is kept aside, smoothed the way the engine smooths
it, then carried by the hand and scaled down (section 13.7).

**Pitfalls.**

- `SetToolTransform` and `SetToolTransformOverride` write the **same** field. Scripts run in an unknown order, so
  whichever ran last won. `Hook_SetToolTransform` keeps the mod's value aside and puts the VR hand back
  (problem I2).
- Read prefab files by the tool's **lower-case** name only: a mod tool named "Gun" must not pick up `gun.xml`
  through the case-insensitive file system.
- The sledgehammer has no `fp_action`; a fixed rest pose is coded for it.
- Euler angles in Teardown's prefab files are in degrees and applied as Ry·Rz·Rx.

### 10.4 Aim pose versus grip pose

**Problem.** Guns pointed 60 degrees above the laser (problem H8).

**Root cause.** OpenXR gives two poses per controller. The **grip** pose follows the fist; the **aim** pose is the
pointing ray. On Virtual Desktop the grip's forward axis is 60 degrees above the aim ray.

**Fix.** Guns take the aim pose's rotation and the hand's position. The sledgehammer keeps the grip pose.

### 10.5 A tool that collides: the physical copy

**Problem.** The game's tool is a static, non-colliding body moved every frame: it went through walls and pushed
nothing (problem I6).

**Idea** (the approach of "physics hands" in VR games). Spawn an invisible **dynamic** copy of the tool and drive
it toward the hand with velocity constraints at every physics step, so the engine resolves contacts while moving
it. Draw the game's tool at the hand plus the copy's physical error.

**In Teardown VR.** In `main.lua` (`[Controles] toolPhysics=1`): target velocity = hand velocity + 0.8 × error /
dt, with a capped force (2000 N) and torque (120 N·m), applied with `ConstrainVelocity` /
`ConstrainAngularVelocity` in `update()` (60 Hz). The copy lives on collision layer 128, which the player ignores.
The same scheme drives the physical free hand.

**Pitfalls** (Teardown engine facts, details in problem I6):

- `CreateShape` always makes 10 cm voxels; copying content into it doubles the tool's size.
- A shape spawned from `<vox scale=...>` keeps the right voxel size, but after its content is copied its mass
  stays zero and the body turns static. The working recipe is in problem I6.
- The game's shots and grab pick do not skip your copy: move it to a layer they skip while the trigger is pulled,
  and release the grab if it took the copy anyway (problem I10).
- At low frame rates (two physics steps per frame), aim each step at where the hand will be, or the copy lags and
  buzzes (problem I8).

### 10.6 The free hand and its arm

The player's body is not drawn in first person, so the free hand was invisible. `tdvrOffArmTick` (Lua) spawns the
character's own hand and forearm voxels as two fixed bodies, maps the OpenXR grip pose onto the character's hand
frame, and bends the forearm with two-segment IK toward a guessed elbow (problem I13).

**Pitfall: the camera moves after the script.** The Lua mod places the arm for the camera it asked for, but the
camera actually drawn has moved a little since the tick (a physics step, height smoothing: up to about 7 cm at
running speed). `OffArmLateFix` (in `Hook_Render`, after the shape jobs) shifts the arm's render transforms by the
difference. The Lua mod sends `TDVRE x y z` (camera minus hand) every normal frame.

---

## 11. Input mapping and simulated keys

### 11.1 Controllers become keyboard and mouse

**Problem.** For its actions, Teardown only understands keyboard and mouse (it reads gamepads through an API the
mod does not simulate).

**Idea.** Translate controller actions into the keys and mouse buttons the game already understands, and inject
them into the game's window only.

**In Teardown VR** (`src/input.cpp`), on foot, for a right-handed player (`[Controles] leftHanded=1` swaps the
hands):

| Controller | Game action | Sent as |
|---|---|---|
| right trigger | use tool | left mouse button |
| right grip | grab | right mouse button |
| A | jump | Space |
| B | flashlight | F |
| X | map | Tab |
| Y | interact | E |
| left trigger | crouch while held | Ctrl |
| right stick up / down | change tool | mouse wheel |
| left menu button | pause (short press) / settings panel (long press) | Esc / our panel |

Walking and turning are not keys: the Lua mod moves the player with `SetPlayerVelocity` and turns the VR room
itself.

### 11.2 Injecting input the game believes

**Idea.** Post window messages to the game's window. Teardown reads key presses from classic window messages and
mouse motion from raw input (`WM_INPUT`).

- **Classic messages**: `WM_KEYDOWN` / `WM_KEYUP` with a correct `lParam` (scan code, extended bit, transition
  bits); `WM_LBUTTONDOWN` and the like at the pointer position; `WM_MOUSEWHEEL`.
- **Fake raw input**: post `WM_INPUT` with a handle value from a private range that cannot be a real handle, and
  hook `GetRawInputData` to return your own `RAWINPUT` for those handles. `DefWindowProc` is hooked too, so the
  fake handles never reach Windows. Nothing reaches other programs.
- The DLL watches `RegisterRawInputDevices` to learn whether the game uses the raw keyboard or the raw mouse, and
  whether it disabled classic messages (`RIDEV_NOLEGACY`).

**Sketch.**

```cpp
constexpr UINT_PTR kFakeBase = 0x7EAD0000;      // a private range of "handles"
constexpr int kSlots = 256;
static RAWINPUT g_fake[kSlots];

void PostMouseDelta(HWND game, LONG dx, LONG dy)
{
    int i = NextSlot();
    g_fake[i] = MakeRawMouseMove(dx, dy);
    PostMessageW(game, WM_INPUT, RIM_INPUT, (LPARAM)(kFakeBase + i));
}

static UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT_PTR i = (UINT_PTR)h - kFakeBase;
    if (i < kSlots) return CopyRawInput(g_fake[i], cmd, data, size);    // one of ours
    return Real_GetRawInputData(h, cmd, data, size, header);
}

void PostKey(HWND game, UINT vk, UINT scan, bool down)
{
    LPARAM lp = 1 | ((LPARAM)scan << 16);                  // repeat count 1, scan code
    if (!down) lp |= (LPARAM)3u << 30;                     // previous state + transition bits
    PostMessageW(game, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}
```

### 11.3 Keys that work on every keyboard layout

**Problem.** On an AZERTY keyboard, the simulated W/A/S/D did nothing in vehicles (problem H4).

**Fix.** Send each key with the **US** virtual key **and** the US scan code (W = 0x11, A = 0x1E, S = 0x1F,
D = 0x20). The game binds letters, and depending on the path it reads one value or the other.

Also, send Alt as `WM_KEYDOWN`, not `WM_SYSKEYDOWN`: a lone injected `WM_SYSKEYUP` for Alt opened the window's
system menu and froze the message loop.

### 11.4 Timing: pulses, hysteresis, frame B and order

- **Pulses last 0.12 s**, several game frames, so the game's per-frame "just pressed" logic always sees them.
- **Hysteresis** on analogue inputs: triggers press at 0.55 and release at 0.35; grips at 0.6 and 0.4. Without it,
  a finger resting near the threshold "clicks" again and again.
- **Never deliver input to frame B.** While the next frame is a frozen frame B (`EngineNextFrameFrozen`), keyboard
  and mouse messages are held back in the window procedure and re-posted after `Present` (`FlushDeferred`). If the
  frozen frame never comes, the pending stereo request is cancelled.
- **Keep the order.** While messages are held back, new ones queue behind them. A key-up that overtakes its
  key-down leaves the key stuck for the game (problem H2).
- **Safety nets.** On foot, the DLL regularly releases W/A/S/D. If the Lua mod sees the game's own movement input
  held for 0.5 s while the player walks with the stick, it sends `TDVRU` and the DLL releases the movement keys
  (problem H3: the player walking by themselves).

### 11.5 The game must believe it has the focus

**Problem.** Holding a button (grab, automatic fire, the extinguisher) stopped after less than a second whenever
another window was in front: the streaming app, a notification (problem H1).

**Root cause** (from reading the game's window procedure): the game keeps an "active" flag driven by `WM_ACTIVATE`.
While it is 0, it clears all key and button states every frame. Mouse buttons come only from classic messages,
posted once per press, so a held button lasted a single frame.

**Fix** (`g_vrFocus`, while VR runs): every `WM_ACTIVATE` reaches the game as "active"; `WM_ACTIVATEAPP(FALSE)`
and `WM_KILLFOCUS` are swallowed; an activation is posted if the game believes it is inactive; Alt, Tab and the
Windows keys are released when a deactivation is hidden; and the game is told the truth when VR stops.
`SetCursorPos` is refused while the window is not really in front. The real keyboard and mouse still go only to the
window that is really in front.

**Check it.** On the mock, with Notepad in front of the game: trigger held for 3 s, and a 750 kg crate grabbed for
3 s.

### 11.6 Special mappings found by reading the game

- **Vehicles.** The accelerator is all-or-nothing and the steering is on/off. The triggers become W / S, and the
  stick's X axis becomes A / D pressed for a share of each 0.25 s cycle (a simple pulse-width modulation), for
  progressive steering (problem H5).
- **Sledgehammer.** The game's sledge code starts the swing on the press and looks for its target only 0.26 s
  later. So a light trigger pull presses "use tool" for at least 0.15 s, and the physical tool copy is kept out of
  the way of the game's hit test for 0.4 s (problem I11).
- **Menus on the virtual screen.** The pointer is mapped to the game's own drawing size (`EngineGameSize`), not to
  the window's, and every mouse message carries the pointer's position while it is on the screen. Otherwise the
  real cursor and the VR pointer alternated, and menu items flickered (problems D4, D5).

### 11.7 One pointer hand

A single "pointer hand" owns every 2D interface. The other hand takes it **only** by pulling its trigger with its
ray on an interface, and that press is its first click. The first rule (switch whenever only the other ray touched
a panel) made the pointer jump between crossed hands (problem H10).

---

## 12. A VR settings panel with Dear ImGui

### 12.1 Draw offscreen, show it as a layer

**Problem.** Settings must be changeable inside the headset, without taking it off, and also from the desktop.

**Idea.** Draw the panel with Dear ImGui into an **offscreen texture**, then show that texture twice: as an OpenXR
quad layer in the headset, and copied into a corner of the game's back buffer for the desktop window.

**In Teardown VR.** `src/ui.cpp`: one ImGui context; a 1100 × 860 RGBA texture created on the game's device;
ImGui's DX12 backend with 4 frames in flight and descriptors from our own heap. `Ui::Draw` runs once per `Present`.
Tabs: Display, Comfort, Controls, Screen, Info, plus a Keys panel for other mods' keys (section 13.1). The same
texture also carries the VR inventory and the warning banners.

**Sketch.**

```cpp
ImGuiIO& io = ImGui::GetIO();
io.DisplaySize = ImVec2(1100, 860);
if (g_pointer.valid) {                                // a controller ray hits the panel (12.2)
    io.AddMousePosEvent(g_pointer.px, g_pointer.py);
    io.AddMouseButtonEvent(0, g_pointer.trigger > 0.6f);
}
ImGui_ImplDX12_NewFrame();
ImGui::NewFrame();
DrawSettingsWindow();
ImGui::Render();

cmd->OMSetRenderTargets(1, &g_uiRtv, FALSE, nullptr);
const float clear[4] = { 0, 0, 0, 0 };
cmd->ClearRenderTargetView(g_uiRtv, clear, 0, nullptr);
ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);
// Then copy the texture into the panel's OpenXR swapchain image, and optionally into the desktop back buffer.
```

### 12.2 Pointing with a controller

Intersect the pointer hand's aim ray with the panel's plane, turn the hit point into pixels, and give ImGui a mouse
position. The trigger is the left button.

```cpp
bool RayToPanelPixels(const Pose& panel, float widthM, float heightM, Vec3 o, Vec3 dir, float& px, float& py)
{
    Vec3 n = Rotate(panel.rot, { 0, 0, 1 });            // the panel's normal
    float den = Dot(dir, n);
    if (std::fabs(den) < 1e-6f) return false;           // ray parallel to the panel
    float t = Dot(panel.pos - o, n) / den;
    if (t <= 0) return false;                           // the panel is behind the hand
    Vec3 local = InverseRotate(panel.rot, o + dir * t - panel.pos);
    px = (local.x / widthM + 0.5f) * 1100.0f;
    py = (0.5f - local.y / heightM) * 860.0f;
    return px >= 0 && px < 1100 && py >= 0 && py < 860;
}
```

A drag of more than 24 pixels scrolls instead of clicking, so long lists scroll like on a phone.

### 12.3 Panels fixed in the room, moved with a grip

- The settings panel opens straight in front of the gaze (by default 0.90 m ahead and 0.15 m below the eyes) and
  then stays fixed in the room. It does not follow the head.
- A grip press that **begins** while that hand's ray is on a panel belongs to the panel until it is released: it
  moves the panel and never reaches the game (no grab, no crouch), and that hand's trigger is masked until it is
  let go (`UpdatePanelGrips` in `src/vr.cpp`).
- Distances are limited, because quad layers have no depth test: a far panel would be drawn over nearer walls.
- The VR inventory is "worn" by an estimated body: a neck pivot that follows steps and crouching with hysteresis,
  and turns only when the body really turns.

### 12.4 Saving settings without hitches

**Problem.** Saving the settings took 55 to 270 ms on the thread that presents frames (problem K2):
`WritePrivateProfileString` rewrites the whole file for each key.

**Fix.** Saving is debounced (0.75 s after the last change). Moving a panel saves only the changed keys, on a
worker thread, at most once per second (`ConfigSaveKeys`); a full save cancels pending batches. Only one writer at
a time.

### 12.5 Two languages

`Tr("English", "Français")` (`src/i18n.h`) returns one of two string literals at each call, so a language switch
shows on the next frame and nothing is allocated. ImGui labels that change with the language but must keep their
state (tabs, windows) end with a fixed `###id`. `[Affichage] language` is `en` for a new install; an `.ini` written
before that key existed is read as French.

---

## 13. Working with other mods

Teardown has a large modding community, and most mods were written for a desktop screen, a mouse and a keyboard.
Teardown VR follows three rules:

- **Never modify another mod's files.** Everything is handled from the DLL and from our own Lua.
- **Prefer generic fixes** (anything that calls `GetCameraTransform`, any interactive window, any mod tool) to
  per-mod code.
- When per-mod knowledge cannot be avoided, keep it in a **small table**, and switch it off automatically when the
  mod behaves differently than expected (13.8).

### 13.1 Their keyboard keys

**Problem.** Many mods use keys (Q, R, F...). A VR player has no keyboard (problem H6).

**Idea.** Find each active mod's keys without running it, and offer them in a panel.

**In Teardown VR** (`src/modkeys.cpp`, on a background thread):

1. **Which mods are active**: when a level loads, the game writes `Active mod: <name> (<id>)` lines to its own log
   (`%LOCALAPPDATA%\Teardown\log.txt`); before that, the enabled mods of the current mod list are used.
2. **Where they are**: workshop mods in Steam's workshop content folder, built-in mods in `<game>\mods\`, local mods
   in the Documents mods folder.
3. **Their keys**: their Lua files are tokenised (never run) to collect `InputPressed("q")`-style calls (also
   through `pcall`), the variables passed to `Input*` calls and the values assigned to them, small key tables, and
   key options saved in `savegame.xml`. Game action names (`interact`, `usetool`...) are resolved to the player's
   own binding in `options.xml`.

The Keys panel (`src/ui.cpp`; right stick click held 0.5 s) lists them, keys that are nowhere on the controllers
first, with Keyboard and Mouse tabs for everything else. Keys are sent by name, so "Q" is the key mods call `q`
wherever it sits on the player's layout. A short left stick click repeats the last key used (the "quick key").

**Check it.** 126 keys found in 46 ms across 21 active mods.

### 13.2 Their mouse

Mods that take over the camera (a plane, a drone, a turret) often read mouse motion. While a mod owns the camera, a
stick becomes a raw mouse (simulated `WM_INPUT` motion with a squared response curve), plus a virtual cursor for
the mod's windows. That stick is then hidden from the Lua mod, so the player does not walk at the same time
(problem H7).

### 13.3 Their aiming

**Problem.** Mods aim with `GetCameraTransform()`, which in VR is the **head**. An air-strike designator aimed
wherever the player looked (problem J1).

**Fix.** `Hook_GetCameraTransform` (`[Controles] modAimHand=1`): when it is called with no argument, on foot, with
no mod camera and the map closed, it returns the player camera, which the Lua mod keeps turned toward the
controller's laser target. Calls with an argument (the VR Lua mod's own) still get the real camera. Nothing is
written to the game.

### 13.4 Their crosshairs

Mods draw crosshairs at the canvas centre, which in VR is in front of the head, not on the gun's target. Every 2D
draw call goes through one "is this rectangle on screen?" test; `Hook_UiRectVisible` moves small quads near the
canvas centre onto the laser target, projected per eye. World markers, interactive windows, mod cameras, the map
and vehicles are left alone (problem J5).

### 13.5 Their cameras, and a zoom that does not distort

**Problem.** In a mod's camera view (the AC130 plane), the player looked at the horizon while the mod looked 50 to
80 degrees down (problem J2).

**Fix.** `ScriptView` (`[Affichage] modCameraVr`): the mod's camera, without roll, becomes the origin of a "room",
and the head looks around from it. A camera within 1.5 m of the player is treated as a first-person camera and left
alone.

**The zoom** is a good lesson in VR optics (problem J3). A desktop mod zooms by narrowing the field of view.
Stretching that narrow rectilinear image over the headset's ~100 degrees turns it into a magnifier: it stays fixed
in the room only at its centre (a point at angle *a* follows the head by sin²(*a*)·(1 − 1/M²) of its rotation),
the runtime's reprojection is wrong by as much, and the relief is flattened M times. Teardown VR instead **moves
the camera** along the mod's axis toward the aimed point by (1 − 1/M)·D, where D is a distance the Lua mod measures
with a raycast (`TDVRZ`). The aimed point looks M times bigger with true perspective, while the projection, the
head rotation and the eye separation stay the headset's. With nothing to aim at (the sky), the magnifier is used.
Setting: `[Affichage] modZoomMode` (0 = move closer, default; 1 = magnifier).

### 13.6 Their menus as floating panels

**Problem.** A weather mod's menu appeared 45 degrees to the left, clipped by one eye, and followed the head
(problem J8). With the HUD's per-eye shift, the same window pixel was a different canvas point in each eye, so
mouse-driven windows were unusable (problem J6).

**Ideas.**

1. **One cursor for both eyes.** During an interactive window, intersect the pointing hand's ray with the canvas
   plane at a chosen distance, and make the engine's "mouse pixel" function (`Hook_MousePixel`) return, in each
   eye, that eye's pixel of the same canvas point.
2. **Put the window on a 3D panel without an extra pass.** Teardown builds each 2D draw's matrix on the CPU, from
   the 2D renderer's projection × view × model, and its 2D shaders use the full 4×4 matrix. So, only for the
   duration of one script's draws, replace the 2D projection with "this eye's panel projection × inverse(view)".
   The window is drawn on a panel fixed in the room, while its layout, its mouse and `UiWorldToPixel` stay flat
   and exact.

**In Teardown VR.** `ModUiPointer`, `ModPanel`, `Hook_ScriptDraw`, `Hook_UiClip` (clip rectangles re-projected),
`Hook_UiBackgroundBlur` (turned off inside the scope). Panels open in front of the gaze, can be moved with a grip,
and their places are saved per window. A guard compares the 2D projection at every quad test; if another pass
overwrote it, the window falls back to the flat canvas for the session.

**Limits.** No depth (the panel is drawn over the scene), and one window at a time.

### 13.7 Their weapons: scaling about the muzzle

**Problem.** Mod weapons are modelled for a desktop screen 0.3 to 3.4 m in front of the eye. In a VR hand they
were 1 to 4.5 m long (problem I3).

**Idea.** Shrink each mod tool by a factor computed once from its own size and desktop distance: s = 0.45 m /
desktop distance, clamped to [0.25, 1], then reduced so the tool is at most 1.1 m long, times
`[Controles] modToolScale`. Teardown shapes own their voxels, so scaling the voxel size and the shapes' positions in
the tool's body changes only that tool.

**The muzzle detail.** Mods compute where their shots start from their own unscaled geometry. Shrinking about the
body origin moved the drawn barrel away from that point, and shots left up to 1 m in front of it (problem I5). So
the tool is scaled **about its muzzle** m (the centre of the front-most solid voxels in the mod's desktop view):
parts are drawn at m + s·(p − m), and any point the mod computes in its unscaled frame lands on the drawn barrel at
the muzzle.

**More details.**

- Mods build their tools over several frames: measure only once the shape count, length and distance have been
  stable for 0.3 s.
- Mods that animate parts with `SetShapeLocalTransform` would un-scale them: the engine function behind that call
  is hooked, and rescales only the shape it wrote (problem I4).
- `GetShapeLocalTransform` and `GetShapeWorldTransform` return unscaled values to mods, so they never scale things
  twice.

**Check it.** Before → after, on the mock: DOOM Chaingun 4.45 m → 0.94 m, GLARE 2.3 m → 0.92 m; shot origins
within about 1 cm of the drawn barrel.

### 13.8 A special case with a profile: dual wield

The Total Overdose weapon draws two pistols as **one** tool and fires from a point beside the eye, alternating
sides. The Lua mod splits the tool (one gun stays in the dominant hand, a copy goes to the other hand); each trigger
press becomes one serialised click; and `Hook_GetPlayerEye` answers the weapon's first "eye" requests after that
click with an eye placed so that the mod's own formula puts the bullet on the drawn muzzle of the hand that
pressed. A small profile table (`kDualProfiles`) holds what the mod is expected to do; after 3 shots in a row that
do not match, the feature switches itself off for the session (problem I15).

### 13.9 Mods and frame B

With the default render-only frame B (section 15.4), mods tick once per pair. A mod that counts ticks instead of
using `dt` animates at half the frame rate, and one-shot effects (a sound, a burst of particles, a camera shake) are
not repeated in frame B. This is the price of synchronised stereo at a reasonable CPU cost; the full list is in
problem G4.

---

## 14. Stereo-consistent stochastic transparency: particles

This section explains a technique **in words only**. The replacement shaders that implement it in Teardown VR are
rebuilt from Teardown's own shaders, which belong to Tuxedo Labs, so **they are not in the repository**. A build
without them (the CMake option `TDVR_GAME_SHADERS` is OFF when `src/generated/particles_p0.h` is missing) keeps the
game's own shaders: stereo particles and the VR anti-aliasing modes are then unavailable, and everything else
works.

### 14.1 How the game draws see-through things

Teardown draws smoke, rain, fire and dust **opaque**, with *stochastic transparency*. For each pixel a particle
covers, it compares a noise value with the particle's opacity: below it, the pixel is drawn; above it, the pixel is
discarded. A particle with 30 % opacity therefore covers about 30 % of its pixels with a scattered dot pattern. The
noise is blue noise read **at the screen pixel**, shifted every frame by the frame number of section 6.8. The
temporal anti-aliasing then averages these on/off patterns over a few frames into smooth transparency. Glass works
the same way.

### 14.2 Why stereo breaks it

The same point of a smoke cloud is about 730 px apart in the two eye images (off-centre projections plus its own
depth). The noise is tied to the screen, so the two eyes see **unrelated** dot patterns at the same 3D point. The
brain cannot fuse them: shimmer, a "fight" between the eyes, eye strain. This is called binocular rivalry, and it
took four rounds to fix.

### 14.3 The four rounds

1. **Same noise frame** (section 6.8). Necessary, but the noise is then equal at the same *pixel*, not at the same
   *point*.
2. **Anchor the noise to the left eye** (problem F2). In the right eye, each particle pixel reads the noise at the
   **left-eye pixel that shows the same 3D point**, instead of at its own pixel. The left eye is unchanged, bit for
   bit. Both eyes now draw the same dots on the same piece of smoke.
3. **The lighting must agree too** (problem F3). The particle shader lights one pixel in two from the back of its
   sphere (a screen checkerboard), and the ambient lighting pass takes its own noise at each eye's pixel. Both are
   anchored to the left eye's pixel in the same way. In a simulation of dense, still smoke after anti-aliasing, the
   difference between the eyes dropped from 1.14 to 0.05 for the checkerboard and from 0.54 to 0.13 for the
   lighting noise.
4. **Motion and texture details** (problems F4, F5). The particle mesh generator remembers each particle's last
   drawn position for its motion vector. In frame B that value had just been overwritten by frame A, so the right
   eye saw no particle motion: `Hook_MeshTask` records it in frame A and puts it back in frame B
   (`[Affichage] stereoParticleMotion`). And the anchored checkerboard had become a patchwork that the
   anti-aliasing could no longer cancel in the right eye: a variant keeps each pixel's own parity for large
   particles (`[Affichage] stereoParity`, automatic).

After round 4, testers reported that all particles looked right.

### 14.4 How the shift is computed and passed

- **The shift.** For a right-eye pixel at clip depth w, the matching left-eye pixel is offset by (cx / w + bx, by)
  in texture units. The three numbers are fitted on the CPU for each frame B, from the view-projection matrices of
  frames A and B (checked from 0.4 m to 300 m; maximum error 0.026 px over 400 random stereo rigs).
- **Passing it without new bindings.** The engine's scene constant buffer holds 0x1F4 bytes, but constant buffers
  are rounded up to multiples of 0x100, so its resource is 0x200 bytes: 12 bytes are spare. A hook on the
  backend's buffer upload, only for the scene-constants call, uploads the full 0x200 bytes with the shift in the
  spare ones: zero in frame A, the fitted values in frame B. With a zero shift, the replacement shader computes
  exactly what the game's shader computes.

### 14.5 How a replacement shader gets in: the pipeline twin

The game compiles no shaders at run time: it loads precompiled blobs and builds each pipeline once, when the
renderer starts. Teardown VR hooks the engine's "create graphics pipeline" function. When the program is one of the
targets **and** its pixel shader is exactly the blob the replacement was made for (checked by container hash and
size), it calls the real function a second time with the replacement pixel shader. That gives a "twin" pipeline
with exactly the game's blend, depth, raster, formats, input layout and root signature. Before each render, the
program object gets either the game's pipeline or the twin, so the setting acts at once.

```cpp
// CPU side only - no shader code here.
static bool Hook_CreateGfxPso(void* backend, const ProgramDesc* desc, void* layout,
                              const ShaderBlobs* blobs, void* rootSig, Pipeline** out)
{
    bool ok = Real_CreateGfxPso(backend, desc, layout, blobs, rootSig, out);
    const Blob* mine = ok ? FindReplacement(desc->name, blobs->ps) : nullptr;   // name + exact hash
    if (mine) {
        ShaderBlobs b = *blobs;
        b.ps = *mine;                                                           // same VS, same states
        Real_CreateGfxPso(backend, desc, layout, &b, rootSig, &g_twin[*out]);
    }
    return ok;
}
// Before each Renderer::render: program->pipeline = vrOn ? twin : original;
```

**Timing pitfall.** Pipelines are built when the renderer starts, before the first `Present`, while the game's
code may still be encrypted (section 3.1). This hook is therefore installed at the first DXGI factory creation, with
retries.

### 14.6 Proving that nothing else changed

The rule was: the left eye must stay **bit-identical** to the game, and the right eye at pixel p + shift must equal
the left eye at pixel p. GPU test harnesses ran the game's own blobs and the replacements on the same random scenes,
for every noise frame and every particle mode (272 checks for round 1, 1105 for round 2, 496 for round 3, all
passed), and negative controls were checked to fail as they should. Without such a harness, "it looks the same" is
not a proof.

### 14.7 Sharper anti-aliasing for a head that never stops moving

**Problem.** Distant detail was blurred, and became sharp only during fast head turns (problem E2). The game's TAA
re-reads its history with a bilinear fetch and keeps 90 to 95 % of it. In a headset the head always moves by a
fraction of a pixel, so the history is re-filtered every frame and fine detail melts away.

**Idea, in words.** Replace the TAA pass with one that reads its history through a sharper filter (Catmull-Rom),
optionally clamps that result to the range of the texels it came from (sharp without dark halos), keeps each
pixel's own current sample, and blends dithered glass and particle pixels with their neighbours so the screen
checkerboard of section 14.3 cancels out. The engine binds the TAA pipeline stored in its own TAA object at every
dispatch, so swapping that one pointer is enough (`src/taa_vr.cpp`, `[Affichage] taaMode`).

**Measured** on the mock with a simulated head tremor: distant sharpness (variance of the Laplacian) went from 278
with the game's TAA to 466 and then 571 with the first two sharp variants; clamping later cut the dark halo of the
sharpest variant by more than half (2.40 → 0.98 on a halo metric measured against a 2× supersampled reference;
problems E2 and E3).

Like the particle shaders, these replacements are not published. In a build without them, `taaMode` keeps the
game's own TAA.

---

## 15. Performance: measure first

Stereo at the same instant doubles the number of frames the game must make. Every millisecond matters, and
guessing was wrong more often than right. Every performance change in Teardown VR followed two rules: **measure
before and after**, and **the image must stay the same**.

### 15.1 Measure first

**In Teardown VR.**

- **`[Debug] perfTrace=1`** (`src/perf.cpp`, `src/prof.h`): per-eye sections (update, fixed-step loop, particles,
  our own code, render, VR submit, present, headset wait), the main thread's waits (jobs, GPU fence, display, shape
  jobs) with their costliest callers, physics steps per frame, the engine's own profiler categories, and CPU time
  per group of threads. `perfTrace=2` adds each mod's tick time and a census of the Lua bindings each mod calls.
  With `perfTrace=0`, nothing is hooked at all.
- **`[Performance] perfDetail`** bits: blocking waits by caller; every job timed by class; GPU busy/idle per frame
  (timestamps around each submission on the game's queue); GPU time per pass (timestamps inserted into the game's
  own command list where the pipeline changes).
- **"Slow frame" lines** for any frame with more than 18 ms of work, with a breakdown and the two costliest mod
  scripts.
- **Missed headset refreshes**, counted from the gaps between predicted display times. This catches small stutters
  that "frames over 20 ms" does not see (two 12 ms frames already miss a refresh at 90 Hz).
- **The main-thread sampler** (`[Debug] debugProfile=1`).

**Pitfalls.**

- **Measuring must not change behaviour.** Check that the median work per pair moves by less than 0.1 ms between
  `perfTrace=0` and `perfTrace=1`.
- **Never open the game's own profiler during a measurement**: it switches the game to one update per frame, capped
  at 60 fps.
- Use interleaved runs (A, B, B, A) and medians. Switch modes live with the control file `TeardownVR_perf.txt`
  rather than restarting the game.
- **Never judge stutter on the mock** (section 16): it paces frames itself.

### 15.2 Let the CPU and the GPU overlap: frame latency

**Problem.** Removing work from frame B gained much less time than expected (problem G5).

**Root cause.** After each `Present`, the engine waits on the swap chain's frame-latency object, with a maximum
latency of 1. So the CPU started frame B only after the GPU had finished the left eye, then the GPU waited for frame
B's commands: they took turns.

**Fix.** During the headset session, `IDXGISwapChain2::SetMaximumFrameLatency(2)` on the game's swap chain
(`[Performance] frameLatency=2`). It is re-checked regularly, because the game restores its own value when it
rebuilds its buffers, and the game's value is put back when the session stops. Same commands, same order, same
queue: only the CPU wait moves.

```cpp
ComPtr<IDXGISwapChain2> sc2;
if (SUCCEEDED(gameSwapChain->QueryInterface(IID_PPV_ARGS(&sc2)))) {   // waitable swap chains only
    sc2->GetMaximumFrameLatency(&g_gameLatency);                        // to restore later
    sc2->SetMaximumFrameLatency(2);
}
```

**Check it.** Mock stress test during explosions, the new settings (render-only frame B + latency 2) against the
old ones: right-eye frame 5.8 → 1.9 ms, missed refreshes 33.9 → 24.4 per second. Latency 2 alone gains little:
the two changes complement each other.

### 15.3 Start simulating before waiting for the GPU: a lazy beginRender

**Problem.** At the peak of an explosion, frame A computed for only about 6 ms but took much longer (problem G6).

**Root cause.** `Game::frame` calls the renderer's `beginRender` **before** `Game::update`, and `beginRender` waits
for the GPU to finish the frame from two frames back. So the simulation could never overlap the GPU's drawing.

**Fix** (`[Performance] lazyBeginFrame=1`). In frame A, `beginRender` is held back until the frame's first real use
of the renderer. The renderer's vtable is redirected so that 57 methods that can be that "first use" go through
small relays, which run the held `beginRender` first. Any unexpected case (a first use from another thread,
`beginRender` still held at the next frame) runs it at once, logs it, and switches the feature off for the session.

```cpp
static Renderer* g_held = nullptr;

static void Lazy_BeginRender(Renderer* r)
{
    if (InFrameA()) { g_held = r; return; }      // the GPU wait is postponed
    Real_BeginRender(r);
}

template <class Real, class... Args>
static auto Relay(Real real, Renderer* r, Args... args)
{
    if (g_held) { Renderer* h = g_held; g_held = nullptr; Real_BeginRender(h); }   // the wait happens here now
    return real(r, args...);
}
```

**Check it.** Frame A 13.2 → 10.6 ms at the peak; images compared live between the modes: identical within capture
noise.

### 15.4 A render-only frame B

**Problem.** Frame B re-ran every mod's tick, the player and the scene update for an instant frame A had already
computed: about 55 % of the main thread's work at rest, and half of every freeze when switching tools (problem G4).

**Idea.** Do not run `Game::update` in frame B at all. But some requests only last one frame: whatever frame A asked
for once, and frame A's render used up, must be asked for again, or the right eye would miss it.

**In Teardown VR** (`src/frameb.cpp`, `[Performance] frameBMode=2`, the default). During frame A's update, hooks
only **record**. In frame B, `FrameB::BReplay` re-issues: lines and sprites (lasers), debug lines, outlines and
highlights, the light lists, depth of field and the low-health blur, camera requests (with the right-eye offset),
sound-loop requests, the music volume, mute, the time scale, and per-frame registry flags (only those written with
the same value in the previous frame A: a key written once is an event and is never repeated). The script events'
double-buffer swap is skipped at the start of a render-only frame B, so mods see what frame A posted, once. Mode 1
is a middle way: frame B only skips the mods whose frame-A tick did nothing but *read* the game. Any unexpected
state falls back to a full update for that frame, with the reason logged; a memory fault disables mode 2 until
restart.

**Check it.** Live A/B comparison of the right-eye image across modes 0, 1 and 2, on scenes chosen to stress each
replayed item (lasers, outlines, sprites, lamps, a mod camera, a slow-motion mod, a HUD fade, smoke): differences
within capture noise. By ear: music fades without buzzing, robots still hear gunshots. An independent review found
7 real bugs before release (problem G4 lists them; for example, `GetLastSound` always returned 0 and robots were
deaf).

### 15.5 Skip GPU passes whose output is provably unused

With bloom removed (factor exactly 0) and depth of field removed (amount exactly 0), the engine still ran both
passes. Per-pass GPU timing showed 0.12 ms and 0.18 ms per eye, and reading the passes proved their results unused
(`composite + bloom × 0`; a depth-of-field pass at amount 0 writes the image back unchanged).
`[Performance] skipDeadPasses` skips them **only** under those exact conditions, writes what the pass would have
left behind (the bloom target cleared to 0), and in mode 2 (the default) compares the real output on the GPU every
few seconds: any difference turns the skips off until restart (problem G7).

### 15.6 Parallelise only what the engine already treats as independent

In frames without a physics step (every frame B), the three particle systems' depth sorts run on three threads at
once instead of one after the other (`[Performance] parallelParticleSort=1`). It is the same function with the same
inputs, which the engine itself already runs in parallel elsewhere. Mode 2 checks each parallel sort against the
serial one (18 003 checks, 0 differences). By contrast, splitting the physics solver's work differently was
rejected: it changes the order in which the solver works, so it changes the physics (problem G8).

### 15.7 Streaming holds: what an app can and cannot do

**Problem.** Periodic hitches, even when standing still in menus (problem G1).

**Root cause.** With a streaming runtime (Virtual Desktop), `xrEndFrame` sometimes blocks for 45 to 100 ms: when
the GPU is saturated, and more often because of streaming back-pressure (Wi-Fi airtime, the headset's decoder, a
high bitrate).

**What the app does.** Count the holds and report them (`[Performance] vdStats=1`: every `xrEndFrame` longer than a
refresh, the codec read **read-only** from the streamer's settings file, the PC's network send rate); keep GPU
headroom; and after a hold, do not jump the game forward (`holdSmooth`, section 6.5). The real remedy is on the
streaming side: in testing, AV1 10-bit at 200 Mbit/s was steadier than H.264+ at 500 Mbit/s.

**Also watched** (`src/gpu_watch.cpp`, `[Interface] gpuWatch=1`): another program using the GPU (in one session a
machine-learning job dropped the headset to 15-20 fps with no visible cause), and overlays hooked into `Present`
(the Steam overlay waited 4 to 7 ms per frame). Both get a log line and a warning in the panel.

### 15.8 What did not pay off

Being honest about failures saves the next person time. Measured and left off by default (problem G9): spinning
idle job threads before they sleep (93 % fell asleep anyway, at 0.6 to 1.3 ms of CPU per frame for a gain within
noise); skipping the shape interpolation jobs in frame B (about 0.02 ms, and those jobs also feed the interpolation
history); blacking out the lens-hidden area for the video encoder (only 0.3 to 2 % of the pixels); a lower GPU
scheduling priority. An asynchronous submit thread would not help either: the streaming runtime blocks all of its
functions during a hold.

---

## 16. Testing without a headset: the mock runtime

### 16.1 Why

Most fixes in Teardown VR were built and checked without a headset. Putting a headset on for every test is slow,
and a person cannot measure "0.1 px" or "1 mm" by eye. A fake runtime makes tests repeatable and automatic.

### 16.2 What it is

`tools/mock_openxr/mock_runtime.cpp` builds `tdvr_mock_openxr.dll`, a real OpenXR runtime (it exports
`xrNegotiateLoaderRuntimeInterface`) that the mod's normal OpenXR code talks to. It is a test tool and is never
shipped to players.

- **What it emulates**: a "Mock Quest 3" with Quest-3-like per-eye fields of view and eye positions, a recommended
  2064 × 2208 per eye, `XR_KHR_D3D12_enable` and `XR_FB_display_refresh_rate`, STAGE, LOCAL and VIEW spaces,
  controllers with haptics, refresh rates of 72, 80, 90 or 120 Hz paced on a high-resolution waitable timer, and a
  head that moves unless told to stay still.
- **What it checks**: every `xrEndFrame` (call order, acquired and released images, view count, rectangles,
  field-of-view sanity, normalised quaternions, the two eyes not sharing one image, the blend mode), with statistics
  in `mock_out\mock.log`.
- **What it produces**: on demand, BMP files of exactly what each eye and each quad layer would show.

### 16.3 Using it

Build it (CMake target `tdvr_mock_openxr`, which also copies its manifests next to the DLL), then point the mod at
it:

```ini
[Debug]
openxrRuntime=<build>\Release\tdvr_mock_openxr.json
```

The manifest is an ordinary OpenXR runtime manifest; the `./` in the library path is required:

```json
{
  "file_format_version": "1.0.0",
  "runtime": {
    "name": "TDVR Mock Headset (test only)",
    "library_path": "./tdvr_mock_openxr.dll"
  }
}
```

The controllers can be driven from the keyboard (while the game window is in front) or, better for scripts, from
`mock_out\controls.txt`, which is re-read whenever it changes. Lines are `name=value`:

```text
rt=1          right trigger fully pulled
aimyaw=10     right hand aimed 10 degrees to the right
aimpitch=-5
headstill=1   head fixed at (0, 1.65, 0)
hjit=0.05     a small head tremor (degrees), for anti-aliasing tests
vdhold=50     simulate streaming holds of 50 ms...
vdholdevery=2 ...every 2 s
dump=3        save the next frame of each eye and quad as BMP files
```

The mock's name contains "Mock": the DLL then allows a few test-only features and tells the Lua mod through a debug
flag. `nohmd=1`, or the second manifest `tdvr_mock_openxr_nohmd.json`, makes the mock answer "no headset", to test
the runtime detection together with `[Debug] xrAutoRuntimes`.

`tdvr_xr_probe.exe [runtime.json]` (CMake target `tdvr_xr_probe`) creates an instance and prints
`xrGetSystem -> <code>`, where 0 means a headset is available. Run it before relaunching the game from a script:
the render size is only chosen at start (section 8.2).

There is also a simpler fake headset that bypasses OpenXR entirely (`[Debug] debugFakeHmd=1`: a swaying, walking
head and keyboard-driven controllers). It is handy for Lua work, but only the mock tests the real path.

### 16.4 Know your test bench's lies

The mock's artefacts were found the hard way (problem M1):

- It paces frames itself and has its own hitches. **Never judge stutter on the mock.**
- Periodic BMP dumps blocked `xrEndFrame` for 55 to 76 ms and looked like hitches. Dumps are on demand now.
- Its default eye geometry put the stereo disparity at infinity (516.6 px) almost exactly on the blue-noise period
  (512 px), which made a stereo-correlation metric give false results. Use the real headset's fields of view and
  eye offsets before running stereo-correlation tests.
- It does not composite layers the way a streaming runtime does: the field-of-view stretch of section 8.5 could
  only be seen in a real headset.
- Its field of view rewrites the saved `vrAspect` in `TeardownVR.ini`.

### 16.5 Test hygiene

Tests share the player's real files (problem M2). Before any automated restart, snapshot `TeardownVR.ini`, the
game's active mod list (in `%LOCALAPPDATA%\Teardown\modlists\`) and the previous log, and check them afterwards.
Never stop a test runner that has already started the game. Do not steal the focus of a PC someone is using: drive
the game through `controls.txt` rather than through desktop automation. And measure what the player sees: a check
that read the player's velocity instead of the actual displacement passed while the bug was still there
(problem M7).

---

## 17. Shipping: installer, logs and builds

### 17.1 Building

```text
cmake -S . -B build -A x64
cmake --build build --config Release --target TeardownVR
```

This produces `build\Release\dsound.dll` (C++20, Visual Studio 2022 build tools). The third-party libraries
(MinHook, Dear ImGui, the OpenXR SDK loader) are fetched as the README explains. Without the unpublished shader
headers, CMake says that it builds **without** the game-derived VR shaders (`TDVR_GAME_SHADERS=OFF`, section 14).
The mock and the probe are the targets `tdvr_mock_openxr` and `tdvr_xr_probe`.

The mod needs three things in the game folder: `dsound.dll` next to `teardown.exe`, and `mods\teardownvr\` with
`info.txt`, `main.lua` and `proxy.vox` (a tiny voxel file used to build the physical tool copy). The player enables
the mod once in the game's mod manager.

### 17.2 The installer

`installer/` builds one Windows program that a player can trust:

- it asks for the language first;
- it finds Teardown through Steam's registry keys and every library listed in `libraryfolders.vdf`, or asks for
  the folder;
- it refuses to run while the game runs;
- it keeps a `dsound.dll` that is not ours as `dsound.dll.original`, and puts it back on uninstall;
- it writes each file under a temporary name, flushes it, then moves it into place (`MoveFileEx` with
  `REPLACE_EXISTING | WRITE_THROUGH`), so an interrupted install never leaves half a file;
- it never overwrites the player's `TeardownVR.ini` (only the language key is written);
- it offers to run again as administrator only if Windows refuses to write in the game folder;
- it has a command line for tests (`/install`, `/uninstall`, `/logs`, `/gamedir`, `/lang`, `/silent`), with exit
  codes.

**Pitfall.** Without `/gamedir`, a test run finds and modifies the **real** game install. Always pass it in tests.

Players who just want to play should use the ready-made installer mentioned in the README rather than build
anything.

### 17.3 Logs that help without slowing the game

- **Never block a game thread for logging** (`src/log.cpp`). Lines are formatted by the caller, appended to a
  buffer, and written by a low-priority thread every 50 ms (or as soon as 64 KB are waiting). The old way, one
  `fprintf` + `fflush` per line on the main thread, cost 10 to 30 µs per line, with rare multi-millisecond stalls
  caused by antivirus scanning.
- **Crashes still get their last lines.** A vectored exception handler flushes on fatal exceptions, but only if the
  locks are free at that instant: it never waits (the exception may be handled later, maybe on the main thread).
  `[Debug] logSync=1` brings back line-by-line writing, for a crash that kills the process at once.
- **Keep previous sessions.** At start, the previous `TeardownVR.log` is moved into `<game>\TeardownVR_journaux\`
  (French for "logs"); the 10 most recent are kept.
- **Make sending logs easy and private.** The installer's "Prepare my logs" button writes a zip on the Desktop with
  the logs, the `.ini` and a short PC summary (Windows version, CPU, RAM, GPU and driver, OpenXR runtime), then
  selects it in Explorer. Nothing is sent anywhere: the player decides what to do with it. File paths in the logs
  (the game folder, the ini) can contain the player's Windows user name, so a player should check them before
  posting them anywhere public.
- **Log what you will need to answer "why?"**: one line per feature found or not recognised, the runtime and
  swapchain sizes, the render-resolution decision, periodic statistics. Many reported "bugs" were not the mod (a
  sleeping controller, another program on the GPU, a game started before the headset; problem M11), and the log
  tells which.

### 17.4 Settings

Every setting and its default live in `src/config.h`, and `TeardownVR.ini` sits next to the game. The defaults are
the configuration validated in the headset, so a new player starts with what was tested. Updates never reset a
player's settings; "Reset all settings" in the panel restores the defaults and keeps the language.

### 17.5 When Teardown updates

A Teardown update can break the mod. By design, this is what to expect:

- features whose patterns are no longer found switch themselves off, each with a "not recognised" line in the log;
- if `Game::update` or its time fields are not found, synchronised stereo becomes alternating eyes;
- a change that no cross-check catches (including an offset kept as a 2.0.1 constant) can still crash the game.

To repair it: run the game once with a level loaded, read `TeardownVR.log`, re-find each failing pattern in the new
version with the method of section 3, update `src/engine.cpp` (and the other files that hold patterns), re-check
the constants listed in [UPDATING §3.3](UPDATING_FOR_A_NEW_GAME_VERSION.md#33-constants-to-re-check-by-hand),
rebuild, and test on the mock before the headset.

---

## 18. Porting these ideas to another game

A checklist, in roughly the order Teardown VR did things. First check that the game's terms allow this kind of mod.

1. **Get in.** Find an import that is not a KnownDLL and proxy it (section 2). Log from the very first second.
2. **See frames.** Hook DXGI (or your graphics API's equivalent) and find the device, the queue and `Present`
   (5.1).
3. **Show something.** Create an OpenXR session on the game's device and copy the back buffer into a quad layer:
   the game on a floating screen. This already proves the whole path, and it stays your fallback for menus.
4. **Find the camera.** Make the game render from the headset's pose, one eye per frame (alternating eyes, 6.2).
   Get the field of view right with a pixel test (7.1).
5. **Fix the render size** (section 8).
6. **Find the frame timing** (the update function, `dt`, the fixed-step accumulators) and build synchronised stereo
   (6.3 to 6.5). This is where most of the reverse engineering goes.
7. **Fix what differs in the second frame**: camera effects (6.6), temporal history (6.7), noise (6.8), anything
   that runs per frame (6.9). Check it with a disparity measurement.
8. **Input** (section 11), then **hands and tools** (section 10): through the game's own scripting API when it has
   one, with a channel to reach it (section 9).
9. **A settings panel** inside the headset (section 12).
10. **Other mods** (section 13), **performance** (section 15), **shipping** (section 17).

All along: find everything by pattern, with cross-checks; degrade instead of crashing; build a mock early; and
prove "no visible change" for every optimisation.

---

## 19. Where to read more

- [PROBLEMS_AND_SOLUTIONS.md](PROBLEMS_AND_SOLUTIONS.md): the problems met, with symptoms, causes, fixes,
  measurements and status, grouped as A (getting in), B (stereo), C (camera, field of view, timing),
  D (resolution), E (sharpness), F (particles), G (performance), H (input), I (tools, weapons and physics),
  J (other mods' cameras and interfaces), K (menus and panels), L (runtime, installer, logs, language) and
  M (development lessons).
- [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md): the engine facts behind each hook, with Teardown 2.0.1
  addresses and offsets.
- The source: `src/engine.cpp` starts with the pattern helpers, and each of its `Init*` functions documents what it
  looks for and what it does when it is not found. `src/config.h` explains every setting.
- Third-party libraries and their licences: MinHook (BSD-2-Clause), Dear ImGui (MIT), OpenXR SDK loader
  (Apache-2.0 or MIT).

This guide describes the project as it was when it was published. It is provided as is, without warranty, and
nothing in it promises support or future changes.
