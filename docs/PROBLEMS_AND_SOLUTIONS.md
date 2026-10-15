# Teardown VR: problems and solutions

This document lists the problems met while building Teardown VR, a VR mod for Teardown 2.0.1 on PC (Steam, x64,
D3D12), and how each one was solved, worked around or left open. It was edited from the development notes and
checked against the source code at the time of publication.

It is written for anyone who wants to understand why the mod works the way it does, to study the techniques, or to
repair the mod after a game update. It complements the README and the reverse-engineering notes
(`docs/REVERSE_ENGINEERING.md`).

> **As is.** Teardown VR is provided as is, without warranty and without support (see the README and the LICENSE
> file, GPL-3.0). This document describes the state of the code when it was published. It is not a roadmap and
> promises nothing. A Teardown update can break any of the features described here.

## How to read this document

### The fields of an entry

Each problem has one entry, with the same fields in the same order:

| Field | What it says |
|---|---|
| **Symptom** | What a player saw or felt. |
| **Root cause** | Why it happened. |
| **How it was found** | Measurement, reverse engineering (RE) of the game, a test on the mock OpenXR runtime, or headset testing. |
| **Fix** | What the mod does, with the function names. |
| **Verification** | How the fix was checked, with the numbers measured. |
| **Limits** | Side effects, and what is still not handled. |
| **Status** | Where it stood at publication (see the status words below). |
| **Files / settings** | Where to look in the source, and the `TeardownVR.ini` keys involved. |

A field that the development notes do not document says *Not recorded*.

The entries are grouped in 12 categories, A to L. Their numbers (A1, B2...) are used for cross-references, for
example "(see G2)". Section M is different: it collects lessons from the development process. Those are not
problems a player sees, so they use a shorter format.

### Where to start

| You are looking at... | Read |
|---|---|
| A blurry headset image | D2, D3, then E1-E5 |
| Stutter or hitches | G1, G2, then the rest of G |
| Smoke or rain that looks different in each eye | The introduction of F, then F1-F5 |
| A feature that says "not recognised" after a game update | A2, and M9 |
| Grabbing, tools and weapons | I |
| The campaign's mission computer, or a shaking view | K4-K6, C9 |
| Other mods' cameras, crosshairs and windows | J |
| A problem that turned out not to be the mod | M11 |

### Conventions

- **Addresses.** Every address is an RVA in `teardown.exe` 2.0.1 (Steam, x64, D3D12), written `0x87f40` or
  `+87f40`. The mod finds every engine function at run time with byte patterns (mostly in `src/engine.cpp`), and
  reads most field offsets from the matched instructions, cross-checked. A feature whose pattern is missing is
  switched off, with a line in the log. Some offsets (renderer, shapes, particles, the TAA wrapper) are 2.0.1
  constants, and one debug-only RVA is hard-coded: they must be re-checked by hand after a game update
  (`docs/UPDATING_FOR_A_NEW_GAME_VERSION.md`, section 3.3). The deeper notes are in
  `docs/REVERSE_ENGINEERING.md`, cited below as "RE §n".
- **Frame A and frame B.** In synchronised stereo (B2), each stereo pair is made of two engine frames:
  - frame A renders the left eye and advances the game;
  - frame B renders the right eye at zero game time.
- **The mock.** The mock is `tools/mock_openxr`, a fake OpenXR runtime used for automated tests. It creates a real
  D3D12 session on the game's GPU and fake Touch controllers driven by a `controls.txt` file. It dumps what the
  headset would show as BMP files, and checks every `xrEndFrame`. CMake builds it only on request, and it is never
  part of the player package.
- **Files.**
  - `TeardownVR.ini` holds the settings. It sits next to `teardown.exe`, in the game folder, written `<game>` below.
  - `TeardownVR.log` is the mod's log, in the same folder.
  - `TeardownVR_perf.txt` is an optional live control file, for tests only. It overrides some settings, is re-read
    once per second, and lets A/B tests switch modes without restarting the game.
- **Settings** are written `[Section] key`, as in `TeardownVR.ini`. Some section names are French, because they are
  the real names used in the file: `[Affichage]` (display), `[Controles]` (controls), `[Ecran]` (virtual screen) and
  `[Confort]` (comfort). The others are `[Performance]`, `[Interface]`, `[Mods]` and `[Debug]`. A quoted name such as
  "Sharp image per eye (recommended)" is the label of that setting in the in-headset panel.
- **Measured numbers** come from a high-end desktop PC with a Quest 3 over Virtual Desktop (VDXR runtime 1.0.10),
  unless stated otherwise. Numbers measured on the mock are labelled as such. The mock paces frames itself, so its
  timings are only good for comparisons between modes (M1).
- **Development helpers.** Names such as `stereo_disp2.py` or `taa_metrics2.py` refer to measurement scripts used
  during development. Many of them are in `tools/dev/` (see `docs/DEV_TOOLS.md`), but not all of them are part of
  this repository. The GPU test harnesses that compare the mod's
  shaders with the game's own shader binaries are not published, because they run the game's shaders.

### Status words

| Word | Meaning |
|---|---|
| *Confirmed in the headset* | A tester on a real Quest 3 reported it fixed. |
| *Verified on the mock* | Checked by an automated test on the mock. No headset report recorded. |
| *Deployed* | Part of the published code, on by default unless stated otherwise. |
| *In place* | Part of the code and used ever since; the notes record no separate headset report for it. |
| *Known limit* | Understood, not fixed. |
| *Rejected* | Measured, then not adopted. |

The status is a snapshot of the development notes on 2026-10-13, and on 2026-10-15 for C9 and K4 to K6.

### Game-derived shaders (not in this repository)

Some fixes replace three of the game's shaders with VR versions:

- the temporal anti-aliasing compute shader (`temporalaacs`): E2, E3, E4;
- the particle G-buffer shader (`gbufferparticles`): F2, F3, F5;
- the ambient and directional light shader (`ambientdirectionallight`): F3.

Those VR versions were rebuilt from the game's compiled shaders, so they derive from Tuxedo Labs' work. They are
**not published**: `shaders/*.hlsl` and `src/generated/*.h` are not part of this repository. This document explains
what they change in prose only.

The CMake option `TDVR_GAME_SHADERS` controls this. It defaults to ON only when `src/generated/particles_p0.h`
exists, so a clean clone builds with it OFF. In such a build:

- `[Affichage] taaMode` 1 to 4 (and the test value 8) keep the game's own anti-aliasing, and the log says so;
- `[Affichage] stereoParticles` and `stereoParity` have no effect: the game's particle and lighting shaders are kept;
- the settings panel may still list these options; they then do nothing;
- everything else works the same, including F1 (`stereoSameNoise`), F4 (`stereoParticleMotion`), E1, E5 and G7,
  which do not touch the game's shaders.

The entries concerned say so in their **Files / settings** field.

## Contents

- [How to read this document](#how-to-read-this-document)
- [A. Getting into the game](#a-getting-into-the-game)
  - [A1. Loading native code into the game: the dsound.dll proxy](#a1-loading-native-code-into-the-game-the-dsounddll-proxy)
  - [A2. The game's code is encrypted on disk, and addresses move between versions](#a2-the-games-code-is-encrypted-on-disk-and-addresses-move-between-versions)
  - [A3. The Lua mod cannot talk to the DLL](#a3-the-lua-mod-cannot-talk-to-the-dll)
  - [A4. Lua requests arriving empty about every 0.4 s (the 30-character limit)](#a4-lua-requests-arriving-empty-about-every-04-s-the-30-character-limit)
  - [A5. An old copy of the Lua mod still talking to the DLL](#a5-an-old-copy-of-the-lua-mod-still-talking-to-the-dll)
- [B. Stereo](#b-stereo)
  - [B1. Two eyes in one engine frame hang the GPU](#b1-two-eyes-in-one-engine-frame-hang-the-gpu)
  - [B2. Alternating eyes show two different game moments: synchronised stereo](#b2-alternating-eyes-show-two-different-game-moments-synchronised-stereo)
  - [B3. The right-eye camera drifted by up to 43 cm and 18 degrees](#b3-the-right-eye-camera-drifted-by-up-to-43-cm-and-18-degrees)
  - [B4. Objects moved by mods at a fixed step per tick: the AC130 plane seen double](#b4-objects-moved-by-mods-at-a-fixed-step-per-tick-the-ac130-plane-seen-double)
  - [B5. Moving objects smeared in the right eye (empty motion vectors)](#b5-moving-objects-smeared-in-the-right-eye-empty-motion-vectors)
  - [B6. Particles computed in the right-eye frame (the engine's "late particles" mode)](#b6-particles-computed-in-the-right-eye-frame-the-engines-late-particles-mode)
- [C. Camera, field of view and timing](#c-camera-field-of-view-and-timing)
  - [C1. Distorted image and no 3D fusion: SetCameraFov is a 16:9 horizontal field](#c1-distorted-image-and-no-3d-fusion-setcamerafov-is-a-169-horizontal-field)
  - [C2. The headset's field is asymmetric (off-centre projection)](#c2-the-headsets-field-is-asymmetric-off-centre-projection)
  - [C3. Taking the camera with SetCameraTransform hides the held tool](#c3-taking-the-camera-with-setcameratransform-hides-the-held-tool)
  - [C4. Camera and tool jerks of up to 39 mm while aiming](#c4-camera-and-tool-jerks-of-up-to-39-mm-while-aiming)
  - [C5. Uneven motion: the game advanced by CPU time, the headset displays at a fixed rate](#c5-uneven-motion-the-game-advanced-by-cpu-time-the-headset-displays-at-a-fixed-rate)
  - [C6. Head bob shaking the whole view](#c6-head-bob-shaking-the-whole-view)
  - [C7. The desktop compositor capped the game at the monitor's refresh rate](#c7-the-desktop-compositor-capped-the-game-at-the-monitors-refresh-rate)
  - [C8. The level map was flat, zoomed and distorted](#c8-the-level-map-was-flat-zoomed-and-distorted)
  - [C9. The view shook when a wrist turned (head offset in the next frame's aim)](#c9-the-view-shook-when-a-wrist-turned-head-offset-in-the-next-frames-aim)
- [D. Resolution and image size](#d-resolution-and-image-size)
  - [D1. Choosing the VR render resolution: the game sizes everything from the monitor at start](#d1-choosing-the-vr-render-resolution-the-game-sizes-everything-from-the-monitor-at-start)
  - [D2. Low-resolution image when the game starts before the headset is connected](#d2-low-resolution-image-when-the-game-starts-before-the-headset-is-connected)
  - [D3. Blurry image after the game's own display mode became "fullscreen"](#d3-blurry-image-after-the-games-own-display-mode-became-fullscreen)
  - [D4. Menu clicks missed when the game drew smaller than its window](#d4-menu-clicks-missed-when-the-game-drew-smaller-than-its-window)
  - [D5. Menu items flickering under the pointer](#d5-menu-items-flickering-under-the-pointer)
  - [D6. The "field of view covered" slider zoomed instead of cropping](#d6-the-field-of-view-covered-slider-zoomed-instead-of-cropping)
- [E. Sharpness: anti-aliasing, blur and haze](#e-sharpness-anti-aliasing-blur-and-haze)
  - [E1. Both eyes shared one temporal history](#e1-both-eyes-shared-one-temporal-history)
  - [E2. Distant blur that cleared up only when turning the head fast](#e2-distant-blur-that-cleared-up-only-when-turning-the-head-fast)
  - [E3. Dark fringes on silhouettes against the sky (over-sharpening)](#e3-dark-fringes-on-silhouettes-against-the-sky-over-sharpening)
  - [E4. A checkerboard on glass and particles in the sharp TAA modes](#e4-a-checkerboard-on-glass-and-particles-in-the-sharp-taa-modes)
  - [E5. Glow, haze and depth of field veiling the distance](#e5-glow-haze-and-depth-of-field-veiling-the-distance)
- [F. Rain, smoke and particles: binocular rivalry](#f-rain-smoke-and-particles-binocular-rivalry)
  - [F1. Round 0: the right eye always drew the next noise frame (stereoSameNoise)](#f1-round-0-the-right-eye-always-drew-the-next-noise-frame-stereosamenoise)
  - [F2. Round 1: the dither is tied to the screen, about 730 px apart between the eyes (stereoParticles)](#f2-round-1-the-dither-is-tied-to-the-screen-about-730-px-apart-between-the-eyes-stereoparticles)
  - [F3. Round 2: each smoke dot lit from the front in one eye and from the back in the other](#f3-round-2-each-smoke-dot-lit-from-the-front-in-one-eye-and-from-the-back-in-the-other)
  - [F4. Round 3, defect 1: no particle motion in the right eye (stereoParticleMotion)](#f4-round-3-defect-1-no-particle-motion-in-the-right-eye-stereoparticlemotion)
  - [F5. Round 3, defect 2: the round-2 parity became a patchwork (stereoParity)](#f5-round-3-defect-2-the-round-2-parity-became-a-patchwork-stereoparity)
- [G. Performance and stutter](#g-performance-and-stutter)
  - [G1. Stutter: the streaming runtime holds xrEndFrame](#g1-stutter-the-streaming-runtime-holds-xrendframe)
  - [G2. After a streaming hold, everything jumped forward (holdSmooth)](#g2-after-a-streaming-hold-everything-jumped-forward-holdsmooth)
  - [G3. Chain stutter after one late frame (double physics steps)](#g3-chain-stutter-after-one-late-frame-double-physics-steps)
  - [G4. Frame B repeated the whole game update (frameBMode)](#g4-frame-b-repeated-the-whole-game-update-framebmode)
  - [G5. CPU and GPU took turns (frameLatency)](#g5-cpu-and-gpu-took-turns-framelatency)
  - [G6. Frame A waited for the GPU before simulating (lazyBeginFrame)](#g6-frame-a-waited-for-the-gpu-before-simulating-lazybeginframe)
  - [G7. GPU passes running for nothing: bloom and depth of field (skipDeadPasses)](#g7-gpu-passes-running-for-nothing-bloom-and-depth-of-field-skipdeadpasses)
  - [G8. Explosion physics: what waits on what (analysis only)](#g8-explosion-physics-what-waits-on-what-analysis-only)
  - [G9. Smaller levers: one adopted, several measured and rejected](#g9-smaller-levers-one-adopted-several-measured-and-rejected)
  - [G10. Measuring without changing anything (perfTrace, perfDetail)](#g10-measuring-without-changing-anything-perftrace-perfdetail)
  - [G11. The mod's own overhead](#g11-the-mods-own-overhead)
  - [G12. Another program on the GPU, and overlays](#g12-another-program-on-the-gpu-and-overlays)
- [H. Input and controls](#h-input-and-controls)
  - [H1. Held buttons released after less than a second (grab, held fire, extinguisher)](#h1-held-buttons-released-after-less-than-a-second-grab-held-fire-extinguisher)
  - [H2. Keys stuck down: the player kept walking after leaving a vehicle](#h2-keys-stuck-down-the-player-kept-walking-after-leaving-a-vehicle)
  - [H3. Auto-walk at full speed](#h3-auto-walk-at-full-speed)
  - [H4. Non-QWERTY keyboards: simulated W/A/S/D did nothing](#h4-non-qwerty-keyboards-simulated-wasd-did-nothing)
  - [H5. Driving vehicles with VR controllers](#h5-driving-vehicles-with-vr-controllers)
  - [H6. Other mods' keyboard keys unreachable from the controllers](#h6-other-mods-keyboard-keys-unreachable-from-the-controllers)
  - [H7. Mods that read the mouse while they own the camera](#h7-mods-that-read-the-mouse-while-they-own-the-camera)
  - [H8. Guns aimed 60 degrees above the laser](#h8-guns-aimed-60-degrees-above-the-laser)
  - [H9. Controls for the free hand](#h9-controls-for-the-free-hand)
  - [H10. The 2D pointer jumped between hands by itself](#h10-the-2d-pointer-jumped-between-hands-by-itself)
- [I. Tools, weapons and physics](#i-tools-weapons-and-physics)
  - [I1. The held tool did not follow the VR hand](#i1-the-held-tool-did-not-follow-the-vr-hand)
  - [I2. Mod tools: the script order decided who placed the tool](#i2-mod-tools-the-script-order-decided-who-placed-the-tool)
  - [I3. Mod weapons were huge in the hand (1 to 4.5 m long)](#i3-mod-weapons-were-huge-in-the-hand-1-to-45-m-long)
  - [I4. A big revolver stayed 1.3 m long, and animated parts popped back to full size](#i4-a-big-revolver-stayed-13-m-long-and-animated-parts-popped-back-to-full-size)
  - [I5. Shots left from the old, unscaled barrel](#i5-shots-left-from-the-old-unscaled-barrel)
  - [I6. Tools passed through objects and pushed nothing (the physical tool)](#i6-tools-passed-through-objects-and-pushed-nothing-the-physical-tool)
  - [I7. The tool trembled in the right eye while walking](#i7-the-tool-trembled-in-the-right-eye-while-walking)
  - [I8. The physical copy lagged and buzzed at low frame rates](#i8-the-physical-copy-lagged-and-buzzed-at-low-frame-rates)
  - [I9. The tool snagged on empty air](#i9-the-tool-snagged-on-empty-air)
  - [I10. The game's own grab and pick took the physical copy](#i10-the-games-own-grab-and-pick-took-the-physical-copy)
  - [I11. Sledgehammer: swing gesture, trembling, and swings that broke nothing](#i11-sledgehammer-swing-gesture-trembling-and-swings-that-broke-nothing)
  - [I12. Grabbed objects were pushed away from the player](#i12-grabbed-objects-were-pushed-away-from-the-player)
  - [I13. No arm for the free hand](#i13-no-arm-for-the-free-hand)
  - [I14. A physical free hand, and far grab with the game's own rules](#i14-a-physical-free-hand-and-far-grab-with-the-games-own-rules)
  - [I15. Dual wield: a two-pistol mod weapon in two hands (Total Overdose)](#i15-dual-wield-a-two-pistol-mod-weapon-in-two-hands-total-overdose)
- [J. Other mods' cameras and 2D interfaces](#j-other-mods-cameras-and-2d-interfaces)
  - [J1. Other mods aimed where the head looked](#j1-other-mods-aimed-where-the-head-looked)
  - [J2. Mod cameras (the AC130 plane view) looked wrong](#j2-mod-cameras-the-ac130-plane-view-looked-wrong)
  - [J3. The mod camera's zoom distorted like a fish-eye](#j3-the-mod-cameras-zoom-distorted-like-a-fish-eye)
  - [J4. The 2D HUD was seen double](#j4-the-2d-hud-was-seen-double)
  - [J5. Mods' crosshairs stayed at the screen centre while the gun aims at the laser](#j5-mods-crosshairs-stayed-at-the-screen-centre-while-the-gun-aims-at-the-laser)
  - [J6. Mods' mouse windows (UiMakeInteractive) were unusable](#j6-mods-mouse-windows-uimakeinteractive-were-unusable)
  - [J7. The revolver's ammo ring moved in every direction, then followed the head](#j7-the-revolvers-ammo-ring-moved-in-every-direction-then-followed-the-head)
  - [J8. Mod menus off to the side, clipped, and following the head (floating panels)](#j8-mod-menus-off-to-the-side-clipped-and-following-the-head-floating-panels)
- [K. Menus, pause screen and the mod's own panels](#k-menus-pause-screen-and-the-mods-own-panels)
  - [K1. The in-level pause screen was square instead of 16:9](#k1-the-in-level-pause-screen-was-square-instead-of-169)
  - [K2. The mod's own panels followed the head, and saving their place froze the game](#k2-the-mods-own-panels-followed-the-head-and-saving-their-place-froze-the-game)
  - [K3. Choosing tools in VR: the inventory](#k3-choosing-tools-in-vr-the-inventory)
  - [K4. The campaign's mission computer: clicking a mission did nothing, even with the real mouse](#k4-the-campaigns-mission-computer-clicking-a-mission-did-nothing-even-with-the-real-mouse)
  - [K5. The campaign's mission computer: the controllers could not move its cursor](#k5-the-campaigns-mission-computer-the-controllers-could-not-move-its-cursor)
  - [K6. Seated at a game computer, the view glided away from the hands](#k6-seated-at-a-game-computer-the-view-glided-away-from-the-hands)
- [L. Headset runtime, installer, logs and language](#l-headset-runtime-installer-logs-and-language)
  - [L1. The wrong OpenXR runtime was used (Virtual Desktop or Quest Link)](#l1-the-wrong-openxr-runtime-was-used-virtual-desktop-or-quest-link)
  - [L2. Installing the mod for players](#l2-installing-the-mod-for-players)
  - [L3. Getting usable logs from players](#l3-getting-usable-logs-from-players)
  - [L4. New players should start with the tested settings, and updates must not reset them](#l4-new-players-should-start-with-the-tested-settings-and-updates-must-not-reset-them)
  - [L5. Localisation: English and French](#l5-localisation-english-and-french)
- [M. Development process: lessons](#m-development-process-lessons)
  - [M1. The mock OpenXR runtime as the test bench, and its artefacts](#m1-the-mock-openxr-runtime-as-the-test-bench-and-its-artefacts)
  - [M2. Tests share the player's real files: snapshot and restore them](#m2-tests-share-the-players-real-files-snapshot-and-restore-them)
  - [M3. Never relaunch the game blindly](#m3-never-relaunch-the-game-blindly)
  - [M4. Do not disturb the person using the PC](#m4-do-not-disturb-the-person-using-the-pc)
  - [M5. Tooling gotchas](#m5-tooling-gotchas)
  - [M6. Patch discipline](#m6-patch-discipline)
  - [M7. Measure what the player sees](#m7-measure-what-the-player-sees)
  - [M8. Proving "no visible change"](#m8-proving-no-visible-change)
  - [M9. Patterns and graceful degradation](#m9-patterns-and-graceful-degradation)
  - [M10. Lua script limits and state](#m10-lua-script-limits-and-state)
  - [M11. Reports that were not the mod](#m11-reports-that-were-not-the-mod)
  - [M12. Hard constraints kept throughout](#m12-hard-constraints-kept-throughout)

---

## A. Getting into the game

### A1. Loading native code into the game: the dsound.dll proxy

- **Symptom.** Teardown has no plugin API for native code, and a VR mod needs D3D12, input and engine hooks.
- **Root cause.** `teardown.exe` imports `DSOUND.dll` (ordinal 1, `DirectSoundCreate`), and `dsound` is not a
  Windows "KnownDLL". A `dsound.dll` placed next to the exe is therefore loaded instead of the system one.
- **How it was found.** Reading the game's import table.
- **Fix.**
  - `src/proxy.cpp` and `src/dsound.def` forward the 12 exports to `System32\dsound.dll`.
  - `DllMain` does nothing unless the host process is `teardown.exe`.
  - D3D12 and DXGI are reached through hooks on `CreateDXGIFactory*`:
    - factory vtable: `CreateSwapChain` (10), `CreateSwapChainForHwnd` (15);
    - swap chain: `Present` (8), `ResizeBuffers` (13), `Present1` (22), `ResizeBuffers1` (39);
    - command queue: `ExecuteCommandLists` (10).
  - The OpenXR session uses `XR_KHR_D3D12_enable` on the game's own device and queue.
- **Verification.** Every later test depends on it.
- **Limits.** Another mod that also ships a `dsound.dll` conflicts with it. The installer keeps such a file as
  `dsound.dll.original` and puts it back on uninstall (L2).
- **Status.** In place since the start.
- **Files / settings.** `src/proxy.cpp`, `src/dsound.def`, `src/main.cpp`, `src/d3d_hooks.cpp`, `src/vr.cpp`.

### A2. The game's code is encrypted on disk, and addresses move between versions

- **Symptom.** Byte patterns found in `teardown.exe` on disk did not match, and fixed addresses break with every
  game update.
- **Root cause.** The `.text` section is encrypted by the SteamStub DRM (a `.bind` section) and decrypted in memory
  at start. When `dsound.dll` loads, the code is still encrypted.
- **How it was found.** Comparing the bytes on disk with the memory of the running process, read with read-only
  tools. All the RE was then done on the process memory.
- **Fix.**
  - The mod does not modify, remove or bypass the DRM. It reads and hooks the code only after the game has
    decrypted itself in memory, and no decrypted game code is distributed.
  - Engine hooks are installed at the first `Present`, when the code is already decrypted.
  - Hooks that must exist before the renderer creates its shaders (the particle pipeline twin, F2) are installed at
    the first DXGI factory creation (`ParticlesVrEarly`). The engine creates a factory (`0x5b5104`) before its device
    and its shaders.
  - Every engine function is found by a byte pattern. The newer features require their pattern to be unique in
    `.text`; the older ones take the first match and rely on cross-checks.
  - On any mismatch, the feature turns itself off and logs which pattern failed. The rest of the mod keeps working.
  - If synchronised stereo cannot be set up, the mod falls back to alternating eyes, and the panel shows "Game
    version not recognized: alternating eyes used."
- **Verification.** The patterns were checked against the running game's memory with development pattern-checking
  helpers.
- **Limits.** A Teardown update can still break features. Most of them switch off instead of crashing, but a
  change that no check catches (a field whose meaning changed, an offset kept as a 2.0.1 constant) can still make
  a feature misbehave or crash the game.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (most patterns), `src/particles_vr.cpp` and `src/d3d_hooks.cpp`
  (`ParticlesVrEarly`), `src/perf.cpp`. See also M9.

### A3. The Lua mod cannot talk to the DLL

- **Symptom.** The Lua part of the mod (camera, aiming, tools, physics) needs head and hand poses and buttons from
  the DLL every frame. Teardown's Lua has no files and no sockets.
- **Root cause.** The script sandbox only offers game bindings.
- **How it was found.** RE of the Lua bindings.
- **Fix.**
  - The channel uses `SetClipboardText` / `GetClipboardText`.
  - The DLL intercepts the clipboard calls in the game's import table: `OpenClipboard`, `EmptyClipboard`,
    `SetClipboardData`, `GetClipboardData`, `CloseClipboard`.
  - A text that starts with `TDVR2` is a request. The reply (eye pose, hands, buttons, settings) comes back through
    `GetClipboardData`.
  - Nothing reaches the real Windows clipboard.
  - These two bindings are reserved to "internal" mods: the binding tests `[ctx+0x420]+0x3c4`.
  - The mod is **not** allowed to patch that check. This is a hard project rule. The Lua mod is installed instead in
    the game's own `mods` folder, where it appears in the "Built-in" list and passes the check legitimately.
- **Verification.** Used by every feature.
- **Limits.**
  - Messages must stay short (A4).
  - The Lua side only runs where the game runs scripts. During a single-player pause no script ticks (K1).
- **Status.** In place. The sandbox check must never be patched.
- **Files / settings.** `src/bridge.cpp`, `mod/TeardownVR/main.lua` (`exchange`), installed as
  `<game>\mods\TeardownVR`.

### A4. Lua requests arriving empty about every 0.4 s (the 30-character limit)

- **Symptom.** Requests from the Lua script sometimes arrived empty, so poses or messages were lost about every
  0.4 s.
- **Root cause.** A bug in the game: a use-after-free in the `SetClipboardText` binding (around `0x4b7120`).
  - The binding copies the Lua string into a temporary `std::string`, destroys it, then reads its buffer.
  - A long string lives on the heap, and the block is sometimes reused in between.
  - A string of at most 30 characters stays in the `std::string`'s inline buffer (small-string optimisation, flag
    byte at `+0x1f`), which the destructor does not touch.
- **How it was found.** Diagnostics of lost requests, then RE of the binding.
- **Fix.**
  - Every request is kept at 30 characters or fewer: `TDVR2 <seq> <flags> <vibL> <vibR> <vignette>`, plus short
    messages such as `TDVRY <deg>`, `TDVRU` and `TDVRT x y z`.
  - Longer data goes in chunks of 30 characters or fewer, for example the tool list for the VR inventory.
  - Diagnostics go in separate `TDVRL ...` requests, which may be lost.
  - The Lua diagnostic `tdvrInputDiag` calls `SetClipboardText` directly, because `debugMsg` is overwritten in the
    same frame.
- **Verification.** Lost-request counters in the log (`[Debug] debugLua=1`).
- **Limits.** Any message longer than 30 characters can still be lost.
- **Status.** In place. This is a hard rule for anyone who extends the protocol.
- **Files / settings.** `mod/TeardownVR/main.lua` (`exchange` and every `SetClipboardText` call), `src/bridge.cpp`;
  `[Debug] debugLua`.

### A5. An old copy of the Lua mod still talking to the DLL

- **Symptom.** After an update, an old first-version copy of the Lua mod could still send requests to the new DLL.
  It had been installed as a "local" mod in the player's Documents mods folder and was enabled in the mod list.
- **Root cause.** The protocol had no version.
- **How it was found.** The mod list and the log.
- **Fix.**
  - The protocol is now `TDVR2`.
  - The DLL answers `TDVR1` requests with "VR inactive" and logs "OLD copy", so an old copy does nothing.
  - The installer recognises its own DLL by the string `TDVR1`, which every version contains.
- **Verification.** On the mock.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/bridge.cpp`, `mod/TeardownVR/main.lua` (`PROTO`), `installer/installer.cpp`,
  `tools/install.ps1`.

---

## B. Stereo

### B1. Two eyes in one engine frame hang the GPU

- **Symptom.** Rendering the scene twice inside one engine frame (one pass per eye) ended in
  `DXGI_ERROR_DEVICE_HUNG` / device removed.
- **Root cause.** Teardown's renderer keeps per-frame GPU data (constant buffers, histories, ring allocations). A
  second render in the same frame overwrites data that the first one still uses.
- **How it was found.** Testing, with the GPU hang detector (`src/gpu_trace.cpp`: the device-removed reason plus
  the main thread's stack).
- **Fix.** Two renders per frame were abandoned in favour of two engine frames per stereo pair (B2).
- **Verification.** Tested: the GPU hung.
- **Limits.** Not applicable.
- **Status.** Rejected approach.
- **Files / settings.** `src/gpu_trace.cpp` (the hang detector, still in the code).

### B2. Alternating eyes show two different game moments: synchronised stereo

- **Symptom.** The first approach rendered each engine frame for the other eye in turn ("alternating eyes"). Each
  eye then showed a different moment of the game. This was rejected in testing: native VR games show both eyes at
  the same instant.
- **Root cause.** Between two engine frames the simulation advances: physics, scripts, the player's movement.
  Anything that moves is at a different place in each eye, which gives false depth.
  - This was never measured in this project. It is the stated reason for the choice.
- **How it was found.** A design requirement, and headset testing.
- **Fix.** Synchronised stereo, `[Affichage] stereoMode=0` (the default; 1 = alternating eyes, kept as a fallback).
  Each pair uses two engine frames that show the same simulation state:
  - **Frame A.** A normal `Game::update` and normal fixed physics steps; the camera is on the left eye.
  - **Frame B.** `Game::update` runs with `dt = 1e-5`. The fixed-step accumulators are restored, so the physics
    loop runs zero steps. The camera is on the right eye.
  - `Game::update` is found through its call in `Game::frame` (pattern `49 8B CC E8 ?? ?? ?? ?? 49 8B CC E8 ...`,
    RVA `0x7a080` in 2.0.1).
  - Time fields:
    - `dt` at `game+0x204`;
    - the fixed-step accumulators at `game+0x214` and `game+0x218`.
  - Frame B's time is set aside and added to the next frame A, with the accumulators capped at 2.99/60 as the engine
    does, so the game runs at real speed.
  - The Lua script knows when it is in frame B (field 81 of the reply) and changes nothing there: no rotation, no
    movement, no aim.
  - Each frame's image is copied into the matching eye swapchain, and the pair is submitted together.
- **Verification.** In the headset, the 3D, the game speed and jumps were confirmed correct. B3 to B6 list what had
  to be fixed on the way.
- **Limits.**
  - The headset gets one stereo frame per two engine frames. The CPU and GPU cost of frame B led to the performance
    work in section G.
  - Some engine and mod behaviour still differs in frame B: see B4, B6 and G4.
- **Status.** Confirmed in the headset. This is the core of the architecture.
- **Files / settings.** `src/engine.cpp` (`Hook_Update` and the frame A / B time handling), `src/vr.cpp`,
  `mod/TeardownVR/main.lua` (`frozen`, reply field 81); `[Affichage] stereoMode`. RE §4.

### B3. The right-eye camera drifted by up to 43 cm and 18 degrees

- **Symptom.** The right eye's view wandered away from the left one, worst during jumps and landings.
- **Root cause.** In the zero-time frame the game still applies per-frame camera effects: height smoothing and
  landing shake.
  - `Game` (around `0x70b20`) builds the render camera with
    `Camera::setTransform(&cam, &player->camPos(+0x1d0), &player->camRot(+0x1dc))`, where player = `game+0x60`.
- **How it was found.** Measuring the rendered camera against the requested one (`debugLua` diagnostic, log line
  "right-eye camera corrected, max drift").
- **Fix.** `Camera::setTransform` is hooked (`Hook_CamSet`, `0x5ec040`). The right eye is rendered from the left
  eye's real camera, moved by the eye offset.
- **Verification.** 0 px of vertical offset between the eyes in the middle of a jump.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (`InitCameraHook`, `Hook_CamSet`); `[Debug] debugLua` for the drift
  measurement.

### B4. Objects moved by mods at a fixed step per tick: the AC130 plane seen double

- **Symptom.** With the AC130 Airstrike mod (workshop 2401575709), the plane looked doubled, with a wrong eye
  separation, as if it were farther away than infinity.
- **Root cause.**
  - In frame B every mod's `tick` runs again.
  - The AC130 mod moves its plane by a fixed step per tick with no `dt` (`AdvanceGameplayPlane`: +0.15 m and
    0.05 degrees per tick).
  - So the right eye saw the plane one tick further on: two different worlds.
  - Even on foot, a few hundred shapes change between the eyes: level parts rotated by scripts, and a mod object
    parked at y = 10 000 in one eye and visible in the other.
- **How it was found.** A mock measurement of the left-to-right offset of the same pair (`stereo_disp2.py`: the NCC
  offset of a region), looking from the plane:

  | Correction | Plane | Relative to the ground (-567 px at 200 m) |
  |---|---|---|
  | none (`syncScriptMotion=0`) | -545 to -548 px | +19 to +22 px: farther than infinity, the doubling |
  | on | -580 to -584 px | -13 to -17 px, about 5 m: correct |

- **Fix.** `[Affichage] syncScriptMotion=1` (`FixObjectMotion` in `src/engine.cpp`).
  - When the left eye renders, each shape of the scene (`scene+0x238`) is recorded with its current render transform
    `+0x13c` and its previous one `+0x158`.
  - When the right eye of the same pair renders, `+0x13c` and `+0x158` are put back on every shape whose `+0x13c`
    changed.
  - These fields are written by ShapeInterpolationJob (`0xd5a50`) for every shape with `flags+0xc8 & 0x60` (0x20
    visible, 0x40 player tool).
    - The jobs are started by Scene::preRender (`0xeb160`) on worker threads.
    - The engine only waits for them at `Renderer::render+0x4a7` (Scene::waitShapeInterpolation, `0xf4150`), which
      comes after the mod's hook, so `FixObjectMotion` first calls that same wait.
  - Guards:
    - a shape is only restored if its frame-B `+0x158` equals the left eye's `+0x13c` (the same shape, not a reused
      address);
    - with a shared history (`perEyeHistory=0`), the "previous" position restored is the left eye's current one
      (zero motion).
  - Shapes created during frame B are not touched.
- **Verification.** On the mock:
  - plane at -12 to -14 px relative to the ground;
  - 25 s of continuous fire from the plane with no error;
  - held tool stable while walking (1 mm per eye on average).
- **Limits.**
  - `SetBodyTransform` (`0x31010`) writes only the world box `+0xa4`, which is left as is. At worst the right eye
    culls for one frame at a screen edge.
  - With `frameBMode=2` (G4) no script ticks in frame B, so this case mostly disappears. The fix stays for modes 0
    and 1.
- **Status.** Verified on the mock; deployed. Later headset sessions in the AC130 view reported no doubling.
- **Files / settings.** `src/engine.cpp` (`FixObjectMotion`); `[Affichage] syncScriptMotion`,
  `[Performance] fastObjectMotion` (G11).

### B5. Moving objects smeared in the right eye (empty motion vectors)

- **Symptom.** Moving objects were less sharp, or smeared, in the right eye only.
- **Root cause.**
  - Each shape keeps its previous render transform (`+0x158`) for motion vectors.
  - ShapeInterpolationJob rewrites it every frame, so in zero-time frame B it equals the current one.
  - The right eye's TAA therefore saw no object motion.
- **How it was found.** RE of the shape interpolation job.
- **Fix.**
  - The DLL records `+0x158` of the moving shapes (`flags+0xc8 & 0x60`) before the left eye renders, and puts it
    back before the right eye renders.
  - Shape list: `*(scene+0x238)`, count at +0, array at +8; scene = `*(game+0x50)`.
  - Merged into `FixObjectMotion` (B4).
- **Verification.** About 2300 shapes restored per pair in the test scene.
- **Limits.** Particles have their own motion data, which needed the same fix later (F4).
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (`FixObjectMotion`); `[Affichage] syncScriptMotion`.

### B6. Particles computed in the right-eye frame (the engine's "late particles" mode)

- **Symptom.**
  - Smoke and sparks in the left eye lagged one display interval behind, about 10 cm for fast smoke.
  - Frame B carried the cost of the particle simulation and its waits.
  - Fast sparks were stretched differently in each eye.
- **Root cause.**
  - `ParticleSystem::update` (`0x42fb50`, ps = `*(*(game+0x50)+0x18)`) runs once per frame inside the draw, after
    the physics loop.
  - If the previous frame took less than 0.8 × the fixed step (`game+0x208` < 13.3 ms; test at
    `0x42fc16`-`0x42fc38`), it uses the step counter, alpha and time saved by the previous call
    (`ps+0x270` / `+0x26c` / `+0x268`) instead of the current ones (`game+0x1f4`, `game+0x21c`, `ps+0x264`). This
    "late particles" mode spreads the load on monitors faster than 75 fps.
  - In synchronised stereo every frame is short, so frame A's step was simulated in frame B.
  - Sparks are stretched by velocity × `game+0x204` (`mubParticleSystemParams.z`, written at `0x8d725`), and
    `game+0x204` is about 0 in frame B.
- **How it was found.** RE of `ParticleSystem::update`, checked read-only in the running game.
- **Fix.** `[Affichage] syncParticles=1`.
  - `Hook_PsUpdate`: before the call, the current values are written into the "previous" slots, so both paths are
    identical. Frame A simulates its step; frame B only sorts and rebuilds the particle mesh.
  - The call is never skipped in frame B: the vertex buffers alternate per frame.
  - During frame B's `Renderer::render`, the DLL puts frame A's frame time back into `game+0x204` for the spark
    stretch.
- **Verification.** Read-only: `ps+0x274` now changes in frame A, and A and B receive the same (step, alpha).
- **Limits.** The right eye's particle motion vectors were still wrong (F4).
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (`Hook_PsUpdate`); `[Affichage] syncParticles`.

---

## C. Camera, field of view and timing

### C1. Distorted image and no 3D fusion: SetCameraFov is a 16:9 horizontal field

- **Symptom.** The image looked squeezed, and the eyes could not fuse it into 3D.
- **Root cause.** The projection builder (around `0x5ebcc0`) computes `tan(vertical/2) = tan(fov/2) × 9/16`, then
  `tan(horizontal/2) = tan(vertical/2) × width/height`. So the parameter is a horizontal field *for a 16:9 image*.
  With a nearly square VR image, the rendered view was about 46 % narrower than the one declared to the headset.
- **How it was found.** Static RE of the projection builder, then a pixel test (`debugFovTest`).
- **Fix.** The mod sends `fov = 2·atan(tan(vertical/2) × 16/9)`.
- **Verification.** Pixel-exact with the FOV test.
- **Limits.** None.
- **Status.** In place.
- **Files / settings.** `src/vr.cpp` (the engine FOV computed from the headset's `angleUp`), `src/engine.cpp`;
  `[Debug] debugFovTest`.

### C2. The headset's field is asymmetric (off-centre projection)

- **Symptom.**
  - The game rendered a symmetric frustum that had to contain the headset's asymmetric field, which wasted pixels.
  - Anything drawn at the image centre (crosshair, HUD) was seen double, because each eye's image centre points
    elsewhere.
- **Root cause.**
  - On a Quest 3 through Virtual Desktop the tangents are left 1.378, right 0.840, up 0.966, down 1.428.
  - The image centre is therefore about 15 degrees outward and 13 degrees down, on opposite sides for the two eyes.
  - The only perspective builder (`0x5ebcc0`) resets m8 and m9 to 0.
- **How it was found.** RE.
  - Everything downstream starts from the full matrix: the copy at `Game+0x240`, `Renderer::render`, the stable and
    jittered view-projection, culling (`0x1a2160` / `0x1a3f20`, in clip space), shader rays through
    `mubProjectionData = (1/m0, 1/m5, m8/m0, m9/m5)`, and the UI.
  - The TAA jitter is added to m8 and m9.
- **Fix.** `[Affichage] offCenter=1`. `Hook_CamSet` writes, for each eye (positive OpenXR tangents):
  - `m0 = 2/(tl+tr)`;
  - `m8 = (tr−tl)/(tl+tr)`;
  - `m5 = 2/(tu+td)`;
  - `m9 = (tu−td)/(tu+td)`.
- **Verification.** Pixel-exact on the mock (FOV test lines).
- **Limits.**
  - The gain is small: the real aspect is 0.925 against 0.964 for a symmetric frustum, so about 4 % of the pixels.
  - It moves the two eyes' pixel grids about 730 px apart, which matters for screen-space noise (F2).
  - The HUD needs a per-eye shift (J4).
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (`Hook_CamSet`); `[Affichage] offCenter`.

### C3. Taking the camera with SetCameraTransform hides the held tool

- **Symptom.** With the camera driven by `SetCameraTransform`, the tool in the player's hand disappeared.
- **Root cause.** `SetCameraTransform` puts the camera in "override" mode (mode 8), and the game hides the
  first-person tool in that mode.
- **How it was found.** Testing, then RE of the camera controller.
- **Fix.**
  - The mod keeps the player's normal camera and moves it to the eye with
    `SetCameraOffsetTransform(local(playerCamera, eye))` plus `SetCameraFov`, and calls `RequestFirstPerson(false)`
    every tick.
  - The offset applies to the player camera *as it will be rendered*. The script turns the player (aim) and moves
    them (roomscale) during the tick, so the offset is computed against the predicted camera: aim rotation applied,
    movement added. Otherwise the left eye turned by that much for one frame.
- **Verification.** Measured with `GetCameraTransform()` in `draw()`: error below 0.03 degrees and 3 mm.
- **Limits.** `SetCameraTransform` must never be called while the map is open: it switches the camera to mode 8 and
  freezes the map (C8).
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua`.

### C4. Camera and tool jerks of up to 39 mm while aiming

- **Symptom.** The view and the tool jerked by up to 39 mm while aiming.
- **Root cause.** Re-orienting the player with `SetPlayerTransformWithPitch` every frame also re-positioned them,
  because the hand always moves a little.
- **How it was found.** Measuring the camera and tool positions frame by frame.
- **Fix.**
  - Pitch goes through `SetPlayerPitch` (`entity+0x700`).
  - Yaw is written by the DLL into `entity+0x704` (radians). The local entity is `game[0xc0][game[0x168]]`; the
    offsets were read in the code of `GetPlayerYaw`.
  - The script sends the yaw in the short message `TDVRY <deg>`.
- **Verification.** No jerk measured afterwards.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua`, `src/bridge.cpp` (`TDVRY`), `src/engine.cpp`.

### C5. Uneven motion: the game advanced by CPU time, the headset displays at a fixed rate

- **Symptom.** Walking looked uneven: the camera moved 72 to 175 mm per pair instead of a steady amount.
- **Root cause.** The simulation followed the irregular CPU frame time, while the headset shows frames at a fixed
  rate.
- **How it was found.** Measuring the camera steps per pair.
- **Fix.** Each frame A advances the game by exactly the gap between two OpenXR `predictedDisplayTime` values.
  `xrWaitFrame` paces the game once per stereo pair.
- **Verification.** A steady 150 mm per pair.
- **Limits.**
  - After a runtime hold, that gap covers the whole hold (G2).
  - `SetTimeScale` (slow-motion mods) has no effect in VR, because the headset interval replaces the frame time
    without the game's time scale (`game+0x1f8`). Not fixed. The fix documented in the notes would multiply the
    interval by `game+0x1f8` (in-level state 6, not paused).
- **Status.** In place. The time-scale issue is a known limit.
- **Files / settings.** `src/vr.cpp`, `src/engine.cpp`.

### C6. Head bob shaking the whole view

- **Symptom.** The game's walking head bob shook the whole headset view.
- **Root cause.** The desktop option `options.input.headbob`.
- **How it was found.** Headset testing.
- **Fix.** `main.lua` sets `options.input.headbob` to 0 while VR runs (`onActivate`), and puts the previous value
  back when VR stops (`onDeactivate`).
- **Verification.** In the headset.
- **Limits.** If the game closes while VR is still active, nothing puts the value back, so the option can stay at 0
  in the game's saved options.
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua`.

### C7. The desktop compositor capped the game at the monitor's refresh rate

- **Symptom.** 12 ms per frame, with the CPU and GPU only a third busy.
- **Root cause.** While the OpenXR session runs, `xrWaitFrame` paces the game, so the PC window must be presented
  without vsync (`SyncInterval 0`). Without `DXGI_PRESENT_ALLOW_TEARING` (the game's swap chain is created with
  `ALLOW_TEARING`), the Windows compositor still limited the game to the monitor rate.
- **How it was found.** Frame time measurement.
- **Fix.** `Present` with `SyncInterval 0` and `DXGI_PRESENT_ALLOW_TEARING` when the swap chain allows it.
- **Verification.** 4 to 6 ms per frame instead of 12 ms.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/vr.cpp`.

### C8. The level map was flat, zoomed and distorted

- **Symptom.** The map view was zoomed, distorted and flat in the headset.
- **Root cause.**
  - The map is mode 7 of the same camera controller (`*(game+0x60)`). It writes the same fields (+0x1d0 position,
    +0x1dc rotation): target +0x128 (on y = 0) + up × (distance +0x188 + 10), smoothed by a spring (+0x110 position,
    +0x11c velocity), with a fixed -90 degree rotation about X (top-down, north up).
  - Its drawing forces a 90 degree field (16:9 convention) through the transition `+0x25c`, with near 1 m and far
    500 m, and ignores `SetCameraFov`.
  - Result in VR: 58.7 degrees vertical instead of about 110, and 6.4 cm between the eyes at 70 m height.
- **How it was found.** RE of the camera controller.
- **Fix.** In `Hook_CamSet`, while in mode 7:
  - the projection takes the VR field;
  - the rotation stays the head's;
  - the camera looks at the target from 50 degrees above the horizon;
  - eye separation and head motion are multiplied by `distance / 1.2 m`, so the level looks like a model at 1.2 m;
  - the opening and closing transitions use the game's curve;
  - the left stick moves the target (+0x128), the right stick zooms (+0x188), and the game clamps both;
  - the right stick's X turns the map (the DLL orbits the map yaw by the change of the anchor yaw).
- **Verification.** Confirmed in the headset.
- **Limits.**
  - `data/ui/map.lua` places its markers with `UiWorldToPixel` without testing whether they are behind the camera,
    so a marker behind the player can appear mirrored in the sky.
  - `SetCameraOffsetTransform` also applies to the map, so the Lua no longer calls it while the map is open.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp` (`Hook_CamSet`, map mode), `mod/TeardownVR/main.lua`. RE §6 bis.

---

### C9. The view shook when a wrist turned (head offset in the next frame's aim)

- **Symptom.** Turning either wrist shook the view, while walking and while seated at the campaign's mission
  computer.
- **Root cause.**
  - The eye position is the game's camera position plus the head offset the Lua mod sets with
    `SetCameraOffsetTransform`. The engine applies that offset in its camera's rotation (`Camera::update`,
    `+3689a`: position + rotate(rotation before the offset, offset)).
  - The Lua mod expressed the offset in the NEW aim of the current tick. But the aim reaches the camera only at the
    next `Player::update` (`0xc2360`, called before the scripts): the yaw goes through `TDVRY` into `+0x704`, and
    `SetPlayerPitch` writes `+0x700`. The camera rendered this frame (`+0x64c`, copied at render time by
    `0xc54a0`) still has the previous tick's aim.
  - So every aim change rotated the head offset by the aim's change: an error of (angle x head offset) for one
    frame. The aim follows the laser's target, which jumps with wrist rotation (laser parallax, steep aims), and
    the free hand's wrist moves the idle hand's aim too.
  - Seated at a screen, the game also glided its camera to the screen camera (see K6): the rotation used for the
    offset was then the screen's, whatever the aim.
- **How it was found.** Reverse engineering of the camera update order and of the camera offset, confirmed by the
  older `camerr` / `badcam` debug lines (0.5 to 5 degree errors), then measured on the mock.
- **Fix.** `main.lua`, `tick`: the offset is built in `pc0.rot`, the camera rotation read at the start of the tick,
  which is the one rendered. Only when `setPlayerAim` used `SetPlayerTransformWithPitch` (a roomscale step, or no
  yaw writer) does the camera turn at once, exactly to `QuatEuler(pitch, yaw, 0)` (`0xc1fc0` writes `+0x64c`
  verbatim): that rotation is used then (`tdvrAimNow`).
- **Verification.** Mock, head still, `heightOffset=0.3`, the right controller's aim stepping 0 / 20 degrees of yaw
  and -10 / -40 degrees of pitch every 0.5 s. Before: the eye jumped 135 mm at each step ("max jerk 270 mm",
  `camerr` 0.135 m / 30.4 degrees). After: 0.0 mm and 0.000 degrees. Confirmed in the headset on 2026-10-15.
- **Limits.** The head offset is still applied by the game; anything else that rotates the game camera between the
  tick and the render would show again. None is known on foot.
- **Status.** Confirmed in the headset.
- **Files / settings.** `mod/TeardownVR/main.lua` (`tick`, `setPlayerAim`, `tdvrAimNow`).

## D. Resolution and image size

### D1. Choosing the VR render resolution: the game sizes everything from the monitor at start

- **Symptom.**
  - The game rendered at the PC monitor's resolution, too low and the wrong shape for the headset.
  - Resizing during play gave a broken image.
- **Root cause.** Teardown sizes its window and render targets from the screen at start, and does not recreate its
  targets on a resize.
- **How it was found.** Testing and RE.
- **Fix.**
  - For a custom resolution, the DLL pretends that the screen has that size: `GetMonitorInfoA`,
    `QueryDisplayConfig`, `GetSystemMetrics(SM_CX/CYSCREEN)`, the popup window sizes (`CreateWindowExW`,
    `SetWindowPos`) and the swap chain.
  - `ChangeDisplaySettingsExA` is blocked, so the real monitor mode is never changed.
  - Presets (`[Affichage] renderPreset`, panel "Image quality (resolution)"), by height in lines:

    | Preset | Lines |
    |---|---|
    | 0 | PC screen |
    | 1 Low | 1280 |
    | 2 Medium | 1600 |
    | 3 High | 1920 |
    | 4 Very high | 2272 |
    | 5 Ultra | 2720 |
    | 6 Headset native (Quest 3) | 3264 |

    The width follows the headset's field (`vrAspect`).
  - `vrAspect` defaults to 0.925 (Quest 3), so the very first launch already gets the right shape.
  - The panel shows "Images sent to the headset: W x H per eye" under the "Resolution sent to the headset" slider
    (`eyeResScale`).
- **Verification.**
  - Native (3024 × 3264) was confirmed sharper in the headset, and holds about 90 Hz on the high-end test PC.
  - Ultra (2624 × 2720) saturated the GPU at times (G1).
- **Limits.** A resolution change applies at the next game start.
- **Status.** Confirmed in the headset. The default is preset 6.
- **Files / settings.** `src/d3d_hooks.cpp` (the screen-size hooks), `src/config.cpp` (`kRenderPresets`,
  `RenderPresetSize`), `src/ui.cpp`; `[Affichage] renderPreset`, `vrAspect`, `eyeResScale`.

### D2. Low-resolution image when the game starts before the headset is connected

- **Symptom.** The headset image was blurry: 1920 × 1080 instead of 3024 × 3264.
- **Root cause.**
  - The render size is chosen once, at start. The DLL asks the OpenXR runtime whether a headset is there
    (`HeadsetInUse`: `xrCreateInstance` + `xrGetSystem` + `xrDestroyInstance`).
  - With the streaming app not connected yet, it falls back to the monitor size.
  - The log then says "Render resolution: headset not connected when the game started".
- **How it was found.** A development relaunch while the headset was off (M3); the low resolution was noticed in the
  headset.
- **Fix.**
  - An in-headset banner (`VrRenderSizeLate`, `src/ui.cpp`): "Low resolution image (W x H)" / "Low resolution:
    restart the game with the headset connected."
  - Development scripts first run `tdvr_xr_probe.exe` (`tools/mock_openxr/probe.cpp`) with the game closed, and only
    relaunch when it prints `xrGetSystem -> 0`.
- **Verification.** Restarted with the headset connected: back to 3024 × 3264. The banner was reviewed.
- **Limits.** The image cannot be resized live (D1). The player must restart the game.
- **Status.** In place.
- **Files / settings.** `src/vr.cpp` (`HeadsetInUse`, `VrRenderSizeLate`), `src/ui.cpp`,
  `tools/mock_openxr/probe.cpp`.

### D3. Blurry image after the game's own display mode became "fullscreen"

- **Symptom.** A sudden blur in the headset, in a session where nothing in the mod had changed.
- **Root cause.**
  - The game's `options.display.mode` (in `%LOCALAPPDATA%\Teardown\options.xml`) had become 0, fullscreen.
  - The game then drew at the monitor's mode (1920 × 1080), and the XR swapchain followed.
- **How it was found.** The log line "XR: swapchain WxH". Check it first whenever blur is reported.
- **Fix.** `FixFullscreenOption` (`src/d3d_hooks.cpp`, called from the `src/main.cpp` init) rewrites mode 0 to 2
  (borderless) in `options.xml` at every start.
- **Verification.** Swapchain back to 3024 × 3264.
- **Limits.** The mod writes one value in the game's options file.
- **Status.** In place.
- **Files / settings.** `src/d3d_hooks.cpp` (`FixFullscreenOption`), `src/main.cpp`.

### D4. Menu clicks missed when the game drew smaller than its window

- **Symptom.** In menus shown on the virtual screen, clicks landed in the wrong place.
- **Root cause.** With the streaming app off, the game drew at 1920 × 1080 inside a 3320 × 3264 window. The pointer
  was mapped to the window, not to the game's drawing size.
- **How it was found.** A mock test.
- **Fix.** The menu cursor uses the game's own size: `EngineGameSize`, `Game+0` / `+4`.
- **Verification.** On the mock.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (`EngineGameSize`), `src/input.cpp`.

### D5. Menu items flickering under the pointer

- **Symptom.** Menu hover highlights flickered.
- **Root cause.**
  - The game window is larger than the desktop.
  - The real Windows cursor (clamped to the desktop, or moved by Virtual Desktop) alternated with the VR pointer's
    position in the mouse messages.
- **How it was found.** Testing, reading the mouse messages.
- **Fix.** While the VR pointer is on the virtual screen, every mouse message (`WM_MOUSEMOVE`, button up and down)
  gets the pointer's position, in the window procedure hook.
- **Verification.** On the mock.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/input.cpp`.

### D6. The "field of view covered" slider zoomed instead of cropping

- **Symptom.** Lowering "Field of view covered" (`fovCoverage`) made the image zoomed and fish-eyed. It still filled
  the whole view, and nothing got faster.
- **Root cause.** Two causes:
  1. The DLL rendered a narrower field and told the runtime so (`pv.fov`). Virtual Desktop's stream does not honour
     a narrower projection field: it showed the image stretched over the whole eye field. VDXR forwards the field
     (`layer.EyeFov.Fov`), so the stretch happens later in Virtual Desktop's pipeline. This was seen in the headset;
     it cannot be seen in VDXR's code.
  2. The render size never shrank with the coverage, so there was no performance gain.
- **How it was found.** Reading the code path, the VDXR source (MIT licence) and headset observation. The mock does
  not composite images, so it could not show this.
- **Fix.** `[Affichage] fovCrop=1` (default).
  - The submitted image always covers the eye's whole field (the 100 % `pv.fov`, which is known to work).
  - The game draws only the covered part. `CropRect` places it on the same z = -1 plane (a linear mapping, same
    aspect, square pixels), and `ClearOutside` paints the rest black, whatever the runtime.
  - The render size becomes the preset × c in each direction: the same sharpness, c² of the pixels (49 % at 70 %).
  - The size is latched at the first swapchain query (`StartCoverage`).
  - c is clamped to [0.5, 1]: beyond 100 % nothing more can be seen.
  - Moving the slider moves the black border at once. The pixel gain comes at the next start.
- **Verification.** On the mock: 70 % gave a 2120 × 2288 render, a black frame and no zoom.
- **Limits.**
  - Going back to 100 % after starting at 70 % upscales the 70 % render until the next restart.
  - `fovCrop=0` gives the old behaviour (coverage 30 to 150 %).
- **Status.** Verified on the mock; deployed. No headset report recorded.
- **Files / settings.** `src/vr.cpp` (`CropRect`, `ClearOutside`, `StartCoverage`), `src/config.cpp`
  (`FovCoverage`, `RenderCoverage`); `[Affichage] fovCoverage`, `fovCrop`.

---

## E. Sharpness: anti-aliasing, blur and haze

### E1. Both eyes shared one temporal history

- **Symptom.** The image was soft, worst up close. Edges got alternating halos.
- **Root cause.**
  - Teardown's temporal passes blend each frame with the previous one: TAA, the disocclusion mask, linear depth, and
    the light, reflection and fog denoisers.
  - In stereo, the "previous frame" was the other eye's, seen from 6 cm away.
- **How it was found.** Static RE of `Renderer::render` (`0x87f40`, a single call site), cross-checked by an
  independent review. All its history sits in a few fields:
  - 10 FBOs, `+0xa50` to `+0xb00`;
  - 5 TAA textures, `*(r+0xa30)+0x50` to `+0x70`;
  - the ping-pong indices `+0x2210` / `+0x2214`;
  - the previous matrices `+0x22ec` (stable view-projection) and `+0x2258` (view);
  - the jitter counter `taa+0`;
  - the reset flag `+0x23f1`.
- **Fix.** `[Affichage] perEyeHistory=1`, panel "Sharp image per eye (recommended)".
  - The mod keeps one set of these fields per eye and swaps them at `render` entry.
  - The second set is created with the engine's own allocators: FBOs through `backend->vtbl[0x138]` with a copy of the
    0x38-byte descriptor that starts each FBO; textures through `TemporalAa::createResources` (`0xa5c90`).
  - Auto-exposure stays shared.
- **Verification.** Sharpness measured as the variance of the Laplacian: × 2.5 in the distance, × 6 to 7 up close.
- **Limits.** The per-eye TAA counters started one step apart, so the screen noise differed between the eyes (F1).
- **Status.** In place.
- **Files / settings.** `src/engine.cpp`; `[Affichage] perEyeHistory`. Does not depend on the game-derived shaders.
  RE §5 ter.

### E2. Distant blur that cleared up only when turning the head fast

- **Symptom.** Distant detail (trees, buildings) looked blurred. It became sharp only during fast head turns.
- **Root cause.**
  - In D3D12 the game's TAA is the compute shader `temporalaacs` (record #274 of `data/shader_cache.cache`,
    precompiled, 0x2b38 bytes). The game compiles no shaders at run time.
  - It reads the previous image bilinearly at `uv - motion` and keeps 95 % of it (90 % when the history was clamped),
    dropping to 0.5 only at `width/64` pixels of motion per frame.
  - In a headset the head always moves by a fraction of a pixel, so the history is re-filtered every frame and fine
    distant detail melts.
  - The engine's own sharpening pass never runs in 2.0.1: the flag `exe+0x1c6fdf1` is always false. The history is
    the raw TAA output (R11G11B10).
- **How it was found.** RE of the shader and of the TAA objects, then mock measurements with simulated head tremor
  (`hjit`).
- **Fix.** The game's TAA pipeline is replaced by a VR version (`src/taa_vr.cpp`).
  - The VR shader rebuilds the game's TAA exactly: its root signature (RTS0) and validation data (PSV0) are
    byte-identical. It then changes how the history (and, in some modes, the current image) is sampled. It is
    compiled with the same DXC version as the game's shader cache.
  - The engine binds, at each `Dispatch`, the pipeline stored in `*(TemporalAa+0x40)` (name at +0x10,
    `ID3D12PipelineState*` at +0x60, root signature at +0x68).
  - The DLL creates its pipelines with the same root signature and swaps the pointer before `Renderer::render`. It
    keeps the original.
  - Setting `[Affichage] taaMode`, panel "Distance sharpness":

    | Value | Name | Definition |
    |---|---|---|
    | 0 | Game's own TAA | |
    | 1 | Sharp (smooth) | |
    | 2 | Natural | `SHARP_CURRENT=2` (each pixel keeps its own sample: no resampling of the current image) + `HIST_CLAMP=1` (Catmull-Rom history clamped to the min/max of its 4 texels: sharp without overshoot) + `GLASS_FIX=1` (E4); blend 0.95 / 0.9 |
    | 3 | Very sharp | Natural with blend 0.93 / 0.9 |
    | 4 | Maximum | No `HIST_CLAMP`; blend 0.9 / 0.88; the current default |
    | 8 | Raw current image | Test only, control file |
    | 9 | Test file | The shader is loaded from `<game>\TeardownVR_taa_test.cso`, to compare variants without rebuilding the DLL |

- **Verification.**
  - Distant sharpness (variance of the Laplacian, mock, slight head tremor): 278 (game) → 466 (Sharp) → 571 (early
    "Very sharp").
  - A reference metric was added: the same view rendered at 2× resolution and downscaled serves as the truth. Each
    mode is aligned sub-pixel per region and scored for detail (high-frequency energy / reference) and halo
    (overshoot past the reference's 3×3 min/max on strong edges). Development helpers: `cap.ps1`,
    `taa_metrics2.py`, `sweep.ps1`.

    | Mode | Detail: forest / buildings / middle | Forest halo |
    |---|---|---|
    | Sharp | 0.40 / 0.39 / 0.40 | 1.13 |
    | Early Very sharp | 0.66 / 0.67 / 0.65 | 2.40 |
    | Natural | 0.59 / 0.66 / 0.67 | 0.98 |
    | New Very sharp | 0.70 / 0.86 / 0.86 | 1.50 |

  - In the headset, testers reported a sharp image; a slight distant smear was judged acceptable.
- **Limits.**
  - A little blur stays on distant forests. It is not a level of detail: the `gbuffervox` raymarch starts at mip 2 to
    skip empty space but always ends at mip 0. It is not depth of field either: `options.gfx.dof` = 0, and
    `SetCameraDof(0, 0)` had no measurable effect. It is the temporal averaging of sub-pixel detail, made worse by
    the bilinear resampling of the jittered current image.
  - "Maximum" can make distant edges flicker (panel help text).
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/taa_vr.cpp`; `[Affichage] taaMode`. **Needs the game-derived shaders**: in a build
  with `TDVR_GAME_SHADERS=OFF`, values 1 to 4 and 8 keep the game's TAA (value 9 still loads a local test file).
  RE §6 ter.

### E3. Dark fringes on silhouettes against the sky (over-sharpening)

- **Symptom.** With the early "Very sharp" mode, dark outlines appeared along silhouettes against the sky. Some
  testers disliked the over-sharpened look.
- **Root cause.** The negative lobes of the Catmull-Rom filter, applied every frame to the history and to the current
  image.
  - Count of pixels below 12/255 at the sky edge: game 0, Sharp 37, early Very sharp 61.
- **How it was found.** Headset feedback, then the halo metric (E2).
- **Fix.** `HIST_CLAMP=1`: the Catmull-Rom result is clamped to the min/max of the 4 texels it comes from. This is
  the "Natural" mode, and "Very sharp" was rebuilt on it. The VR blit's own sharpening slider (`[Affichage] sharpen`,
  panel "Sharpening", 0 to 1) stays separate.
- **Verification.** Halo metric 2.40 → 0.98 (Natural) / 1.50 (new Very sharp).
- **Limits.** The `sharpen` slider multiplies the highest frequencies (gain 1 + 2s at the pixel-checkerboard
  frequency). Its best value is a matter of taste.
- **Status.** In place.
- **Files / settings.** `src/taa_vr.cpp`, `src/render.cpp` (the VR blit and its sharpening); `[Affichage] taaMode`,
  `sharpen`. The TAA part **needs the game-derived shaders**; the `sharpen` slider does not.

### E4. A checkerboard on glass and particles in the sharp TAA modes

- **Symptom.** In the sharpest mode a fine checkerboard showed on glass, and on smoke.
- **Root cause.** The game draws glass and particles as dithered pixels: one pixel in two, plus a lighting
  checkerboard (F3). The game's soft TAA blurred that pattern away; the sharp modes kept it.
- **How it was found.** Mock captures.
- **Fix.** `GLASS_FIX=1` in the TAA replacement (modes 2 to 4). Dithered glass and particle pixels are mixed 50/50
  with their 4 neighbours (0.5·c + 0.125·Σ4), which cancels a screen checkerboard exactly.
- **Verification.** Mock captures.
- **Limits.** It only cancels a *pure* screen checkerboard. A pattern broken per particle layer survives (F5).
- **Status.** In place.
- **Files / settings.** `src/taa_vr.cpp`; `[Affichage] taaMode` 2 to 4. **Needs the game-derived shaders.**

### E5. Glow, haze and depth of field veiling the distance

- **Symptom.** A light veil over the distance.
- **Root cause.** Bloom, atmospheric fog and, potentially, depth of field.
- **How it was found.** Mock A/B measurements of distant sharpness.
- **Fix.**
  - `[Affichage] removeBloom=1` (default): `SetPostProcessingProperty("bloom", 0)`, +23 % measured distant
    sharpness.
  - `[Affichage] removeFog` (default **0**, panel "Remove distant haze"):
    - `main.lua` `updateFog` zeroes `fogParams`, `fogscale` and `sunFogScale`, and checks every 2 s whether the
      level put fog back;
    - the fog itself changed sharpness little, and weather mods use it for rain and snow, so it stays on by
      default.
  - `[Affichage] removeDof=1`: `SetCameraDof(0, 0)`.
- **Verification.** Mock measurements.
- **Limits.**
  - The fog check used to rewrite the fog every 2 s, which made the fog flicker in mod camera views (the AC130 plane).
    It now writes only when the level re-adds fog, and skips while a mod camera is far from the player.
  - `removeDof=1` itself makes the engine run the depth-of-field pass at amount 0. That cost was removed later (G7).
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua` (`updateFog`); `[Affichage] removeBloom`, `removeFog`,
  `removeDof`.

---

## F. Rain, smoke and particles: binocular rivalry

Rain, extinguisher smoke, fire and dust looked different in each eye. The result was shimmer, a "fight" between the
eyes, and eye strain. The problem was present from the first version and took four rounds of work (rounds 0 to 3).

How the game draws particles: `gbufferparticles` draws each particle **opaque** into the G-buffer with stochastic
transparency. A pixel is kept when `frac(blue noise + 0.618 × FrameRnd)` is below the particle's alpha (minus 0.01),
and discarded otherwise. The TAA then averages these 0/1 patterns into transparency.

- The noise texel is taken at the **screen pixel** (pixel position / 512), plus a per-particle offset from the
  particle's slot index.
- `FrameRnd` (`mubFrameParams.w` of the scene constants) animates the noise of the whole engine: particles, glass,
  ambient occlusion, sun, lamps, fog, reflections, water, wet surfaces and denoisers.

### F1. Round 0: the right eye always drew the next noise frame (stereoSameNoise)

- **Symptom.** Thin smoke and rain fought between the eyes.
- **Root cause.**
  - `FrameRnd` is written in the scene constants (`0x9e130`, called at `Renderer::render+0x75e`). Its value comes from
    the TAA counter (`taa+0`), stepped `(n + 1) % 8` by `TemporalAa::update` (`0xa5950`), which also sets the
    sub-pixel jitter and the TAA ping-pong parity.
  - The per-eye history (E1) gives each eye its own counter. The right eye's set was created one step ahead, so
    frame B always drew `FrameRnd + 1`.
  - For particle alpha ≤ 0.38 (thin smoke, rain), the two eyes' dithered pixel sets were **disjoint** at the same
    screen pixel: no pixel lit in both eyes.
  - Before the per-eye history, A and B were consecutive engine frames, with the same +1.
- **How it was found.**
  - RE of the scene constants and of the TAA update.
  - A frame-by-frame model of the counter (`sim_counter.py`) and an overlap table (`mask_corr.py`).
  - Other suspects were ruled out:
    - rain is spawned with `rand()` once per *physics step* (`0xf2af0`, from Scene::update `0xf2640`), never in
      frame B;
    - the per-eye sort (`0x437df0`) only culls what is behind the camera;
    - the time inputs are equal in A and B.
- **Fix.** `[Affichage] stereoSameNoise=1` (`SameNoiseBegin` in `src/engine.cpp`).
  - At the start of frame B's render, the TAA counter is set back to the value frame A started with. B then gets A's
    noise frame, jitter and ping-pong parity.
  - The left eye is bit-identical to before. Each eye still steps through the 8 noise frames once per pair.
  - The fix checks itself after every render (`SameNoiseEnd`) and logs a summary every minute ("N pairs, the same
    noise frame in both eyes in N").
- **Verification.**
  - The self-check log, on the mock and in the headset.
  - With the control file at `stereoSameNoise=0`, the log shows "in 0 of N": a live measurement of the bug.
- **Limits.** The noise is now identical at the same **pixel**, not at the same 3D point. With off-centre
  projections the two eyes' pixel grids are about 730 px apart, plus each point's own disparity. So this fix is
  necessary, but not sufficient (F2).
- **Status.** In place (default 1).
- **Files / settings.** `src/engine.cpp` (`SameNoiseBegin`, `SameNoiseEnd`); `[Affichage] stereoSameNoise`
  (needs `perEyeHistory`). Does not depend on the game-derived shaders.

### F2. Round 1: the dither is tied to the screen, about 730 px apart between the eyes (stereoParticles)

- **Symptom.** Still shimmer and strain on smoke and rain.
- **Root cause.** The same point of a smoke cloud is about 730 px apart in the two images, from the off-centre fields
  plus its disparity. Screen-space noise is therefore uncorrelated between the eyes, whatever `FrameRnd` is. A
  critical review proved that an equal `FrameRnd` cannot help.
- **How it was found.** RE of the shader, plus a stereo dither simulation.
- **Fix.** `[Affichage] stereoParticles=1`, panel "Same particles in both eyes (rain, smoke)".
  - Pipeline twin (`src/particles_vr.cpp`):
    - the game builds its programs once, at renderer start: shader loader `0x93f00` → `createGraphicsProgram`
      (vtable +0x180 = `0x5afd50`) → `createGfxPSO` (`0x5ae850`);
    - the DLL hooks `createGfxPSO` and, for `gbufferparticles`, calls it a second time with its own pixel shader. It
      gets a twin PSO with the game's exact pipeline state;
    - before each `Renderer::render`, the program object (PSO at +0x70) gets the VR or the game's pipeline, so the
      setting acts at once;
    - renderer program slots: `+0xdc0` / `+0xdc8` (CPU mesh), `+0xdd0` / `+0xdd8` (GPU mesh).
  - The VR particle shader (both permutations) runs the game's own noise formula on the centre of the **left-eye
    pixel that shows the same point**:
    - the left eye's shift is 0, so it is bit-identical to the game;
    - the right eye reads exactly the left eye's texel.
  - Passing the shift without new bindings:
    - the scene constant buffer is 0x1f4 bytes in a 0x200-byte resource and view (constant buffers are rounded up to
      0x100);
    - the hook on `updateBuffer` (vtable +0x128 = `0x5b9e00`), only for the scene-constants call site, uploads 0x200
      bytes;
    - the 12 spare bytes carry `shift(w) = (cx / w + bx, by)`, fitted on the CPU from frame A's and frame B's
      view-projection matrices.
  - The hook must be installed before the renderer builds its programs, while the code may still be encrypted (A2):
    it is installed at the first DXGI factory creation.
- **Verification.**
  - GPU harness against the game's own particle shaders: 272 checks, all passed. The reconstruction equals the
    original; the VR shader with shift 0 equals the original on every bit of the 4 render targets, for every
    `FrameRnd` and every particle-system mode; the right eye at p with shift s equals the left eye at p + s.
  - The shift fit: 0.026 px maximum error over 400 random stereo rigs.
  - Mock: 351/351 pairs reprojected, model error 0.1 px.
  - Headset: better, but a slight discomfort remained, moving from one eye to the other.
- **Limits.**
  - A grain-correlation test on the mock could not show the gain: with a still head, the TAA resolves the dither.
  - The lighting checkerboard and the lighting noise were still tied to each eye's pixels (F3).
- **Status.** Superseded and extended by F3 to F5.
- **Files / settings.** `src/particles_vr.cpp`, `src/d3d_hooks.cpp` (`ParticlesVrEarly`); `[Affichage]
  stereoParticles`. **Needs the game-derived shaders.** RE §6 ter bis.

### F3. Round 2: each smoke dot lit from the front in one eye and from the back in the other

- **Symptom.** After round 1, a slight discomfort remained, moving from one eye to the other.
- **Root cause.**
  - **Lighting checkerboard.** `gbufferparticles` flips the normal of one pixel in two (`(x + y) mod 2`), so that dot
    is lit from the back of the sphere. With the dither anchored, a right-eye dot's parity is the left one's XOR the
    parity of the rounded disparity. For one particle in two (depending on its depth), every dot was front-lit in one
    eye and back-lit in the other.
  - **Lighting noise.** The `ambientdirectionallight` pass (ambient-occlusion cosine rays, screen-space occlusion
    step, ray-start jitter, sun direction jitter) takes its noise at each eye's own pixel. Its denoiser rejects other
    depths and normals, so most of that noise reaches the particle dots.
- **How it was found.** A survey of the remaining screen-tied noise, plus a residual simulation (`residual_sim.py`).
  - Binocular difference in dense still smoke, after TAA: 1.14 → 0.05 for the checkerboard, and 0.54 → 0.13 for the
    ±50 % lighting noise.
- **Fix.** Still under `stereoParticles`.
  - The particle shader takes the parity at the left eye's pixel (`noisePixel`).
  - A VR version of the ambient/directional light shader rebuilds all 16 permutations from the engine's own shader
    helpers. With `STEREO_ANCHOR=0` it compiles to the game's 16 binaries byte for byte. The VR change shifts the
    noise texture coordinate by the integer number of pixels to the left-eye pixel that shows the same point, using
    the same depth as `linearizeDepth`.
  - Test-only control-file values: `stereoParticles=2` (particles only), `3` (lighting only); `taaMode=8` (raw frames,
    never in the panel).
- **Verification.**
  - GPU harness: 1105 checks, all passed. The left eye is bit-identical (16 permutations × 8 `FrameRnd` values × 2
    random scenes). The right eye at p + shift equals the left eye at p.
  - Negative controls fail as they should.
- **Limits.**
  - A mock metric artefact: the mock's disparity at infinity (516.6 px) was almost the blue-noise period (512).
    Screen-locked dots then correlated at exactly -512 px, which made the analyser's verdict misleading (M1).
  - Still tied to pixels: lamps (`diffuselight`, 16-bit depth only), volumetric fog, rain on `wet` surfaces,
    reflections, and the denoiser kernel rotation.
  - A further cause was found in round 3.
- **Status.** In place, as part of `stereoParticles`.
- **Files / settings.** `src/particles_vr.cpp`; `[Affichage] stereoParticles`. **Needs the game-derived shaders.**

### F4. Round 3, defect 1: no particle motion in the right eye (stereoParticleMotion)

- **Symptom.** With the extinguisher, closing one eye then the other showed slightly different smoke. Moving smoke
  trailed, and was smoothed differently, in the right eye.
- **Root cause.**
  - The mesh generators (`MeshGenerationTask::run` `0x434160`, vtable slot 1 at .rdata `0xa291e0` → GPU path
    `0x4341d0` / CPU path `0x434830`) read each particle's "last drawn position" (`+0xC`) as `lastPos` for the motion
    vector, then overwrite it with the position just drawn.
  - Frame A reads what B(n−1) wrote, which is correct. Frame B reads what A has just written, so `lastPos == pos`:
    camera motion only.
  - The right eye's TAA (with its own history) and the light denoiser fetched history at the wrong place.
  - This defect predates rounds 1 and 2, and existed with the game's own TAA.
- **How it was found.** RE of the mesh generator, of the vertex shader's use of `lastPos`, and of the frame-time
  code, in an investigation round with independent critical reviews.
- **Fix.** `[Affichage] stereoParticleMotion=1`, panel "Moving smoke: same smoothing in both eyes".
  - `Hook_MeshTask` (`src/engine.cpp`), on worker threads, under SEH.
  - In frame A it records `+0xC` per particle slot, *before* the real work, and writes nothing in the game.
  - In frame B (same physics step, right eye with its own history) it puts the record back. A particle born in this
    pair (record zero) is skipped.
  - The generator then rewrites `+0xC` as before, so the next frame A is unchanged.
  - Counters in the log with `particleMotionLog=1`.
- **Verification.**
  - Mock motion log: in frame B, 99.9 % of the drawn particles moved (median 0.87 px, p90 4.1 px), 0 writes in A, and
    no "NONE" phases.
  - See F5 for the combined image test.
- **Limits.** If a frame B is skipped, the next B restores one pair of motion instead of two (partial, never worse).
- **Status.** Confirmed in the headset, together with F5.
- **Files / settings.** `src/engine.cpp` (`Hook_MeshTask`); `[Affichage] stereoParticleMotion`,
  `[Debug] particleMotionLog`. Does not depend on the game-derived shaders.

### F5. Round 3, defect 2: the round-2 parity became a patchwork (stereoParity)

- **Symptom.** Extra fine grain, or mottling, in the right eye in dense smoke.
- **Root cause.**
  - Round 2 took the front/back parity at the left eye's pixel, layer by layer.
  - In the right eye, neighbouring pixels that show different particle layers get unrelated parities.
  - That is no longer a screen checkerboard, so `GLASS_FIX` (E4) cannot cancel it in the right eye.
- **How it was found.** Raw-frame forensics (checkerboard energy and spectra, right/left) and a `GLASS_FIX`
  simulation on real frames.
- **Fix.** `[Affichage] stereoParity=-1` (auto).
  - `STEREO_PARITY` has 3 pipeline variants in the particle shader:
    - 0 = round 2;
    - 1 = the pixel's own parity for particles of at least 8 px projected radius;
    - 2 = the pixel's own parity for all (= round 1).
  - Auto picks 1 when the bound TAA has `GLASS_FIX` (`taaMode` 2 to 4, `TaaVrBoundMode()`), 0 otherwise.
  - The left eye is identical in all variants.
- **Verification.** GPU harness: 496 checks, all passed. Mock test with the extinguisher, TAA on (`taaMode` 4,
  `sharpen` 1):

  | Config | Grain R/L | Mid R/L | Mean R/L | Residual |
  |---|---|---|---|---|
  | C0 (round 2) | 0.848 | 1.064 | 0.988 | 0.0225 |
  | C1 (motion) | 1.057 | 1.101 | 0.996 | 0.0165 |
  | C2 (parity) | 0.850 | 1.005 | 0.992 | 0.0202 |
  | C3 (both) | 1.004 | 1.032 | 0.997 | 0.0158 |

  The scene's mean R/L is 1.000, and the left eye is unchanged. In the headset, the tester reported that all
  particles were fine.
- **Limits.**
  - Residual from F3: normals are oriented about each eye's own camera, so the right eye's smoke is 0.3 to 1.7 %
    darker. Not addressed.
  - Nearest-particle rivalry: particles at different depths each get their own shift, so each eye can see a
    different nearest "winning" particle. This is a comfort issue with both eyes open. A shared-shift design (band
    quantisation) was considered and not implemented.
- **Status.** Confirmed in the headset. The particle work was declared done.
- **Files / settings.** `src/particles_vr.cpp`, `src/taa_vr.cpp` (`TaaVrBoundMode`); `[Affichage] stereoParity`.
  **Needs the game-derived shaders.**

---

## G. Performance and stutter

### G1. Stutter: the streaming runtime holds xrEndFrame

- **Symptom.** Periodic hitches, even in menus or standing still. They were worse at high resolution and with some
  codec settings.
- **Root cause.** Two different kinds:
  1. **GPU saturation.** Virtual Desktop's `xrEndFrame` waits for its send queue to drain
     (`SleepConditionVariableSRW`, `virtualdesktop-openxr.dll+0x52390`) when the GPU is saturated (93 to 98 % in
     bursts of 60 to 110 ms; Ultra averaged 83 %).
  2. **Streaming back-pressure** (the majority, later on).
     - VDXR's `xrEndFrame` first calls `waitForAsyncSubmissionIdle` (VDXR is MIT-licensed source), which waits on
       Virtual Desktop's own submission.
     - During holds the encoder is almost idle (one sample: encoder 2 %, GPU 13 %), which points at Wi-Fi airtime or
       the headset's decoder.
     - In one session there was a burst of holds every 18 s on average, even in menus. The first frame always waited
       about 90 ms (+10 ms of `xrWaitFrame`), the next ones 45 to 75 ms. About 85 % of the frames over 20 ms were
       holds.
     - H.264+ at 500 Mbit/s is about 5.6 Mbit per frame at 90 Hz (above H.264 level 5.2), against 2.2 Mbit at
       200 Mbit/s. Peaks, such as a level start or smoke, do not fit in 11.1 ms of airtime.
     - Holds cluster in the 25 s or so after a level loads.
- **How it was found.**
  - The main-thread sampler (`[Debug] debugProfile=1`, `src/sampler.cpp`, 1000 samples per second).
  - Per-section timing of slow frames.
  - GPU and encoder readings, and the VDXR source.
- **Fix.**
  - App side: GPU headroom (resolution), and `[Performance] vdStats=1`. Every `xrEndFrame` longer than a refresh is
    counted on the "Stereo frames" log line ("runtime holds N in M bursts"), together with the codec and 2-pass
    setting, read **read-only** from `C:\ProgramData\Virtual Desktop\StreamerSettings.json`, and the PC's network
    send rate.
  - The "Stereo frames" line also counts the headset refreshes missed (gaps between `predictedDisplayTime` values).
    That catches small hitches that "frames > 20 ms" does not.
  - G2 limits the damage after a hold.
  - The real lever is on the streaming side. In testing, AV1 10-bit at 200 Mbit/s was steadier than H.264+ at
    500 Mbit/s. The RE notes recommend AV1 or HEVC 10-bit at 200 Mbit/s or less. Other levers: Wi-Fi quality and
    headset placement.
- **Verification.** Log statistics across sessions.
- **Limits.**
  - The app cannot remove network or decoder holds.
  - An asynchronous submit thread would not help: VDXR blocks all its functions during the wait.
  - The rest of the 20 to 30 ms hitches are engine waits: particle jobs (`Renderer::finish`, `0x91830`), fire,
    physics, the first rasterisation of a font, and decoding a `UiSound` or an image in menus. The Lua mod and its
    garbage collector were ruled out.
- **Status.** Measured. Partly mitigated (G2). Streaming-side settings remain the main remedy.
- **Files / settings.** `src/vr.cpp` (stream statistics), `src/sampler.cpp`, `src/perf.cpp`;
  `[Performance] vdStats`, `[Debug] debugProfile`. RE §6 quater.

### G2. After a streaming hold, everything jumped forward (holdSmooth)

- **Symptom.** After each hitch the world teleported forward. The next frames were heavier, which caused chains of
  holds.
- **Root cause.**
  - Frame A's time step is the gap between two `predictedDisplayTime` values (C5). After a hold that gap covers the
    whole hold (20 to 90 ms), so the game jumped by that much.
  - `limitCatchUp` (G3) still left 2 physics steps in the next frame A.
- **How it was found.** Log analysis. The mock runtime then gained simulated holds (`controls.txt`: `vdhold=50
  vdholdevery=2`).
- **Fix.** `[Performance] holdSmooth=1`, panel "No jump after a streaming hitch (recommended)". After a hold (the
  last `xrEndFrame` longer than a refresh), the pair advances by one refresh only. The game resumes where it
  stopped: it stays a few ms behind the wall clock, never ahead.
- **Verification.** On the mock with simulated holds:
  - about 55 ms of jump avoided per hold;
  - during explosions: dropped catch-up steps 4.0 → 1.9 per second, physics loop 3.6 → 2.8 ms, hitches 74 → 62.
- **Limits.** The time lost during the hold is not played.
- **Status.** Verified on the mock; deployed. No headset confirmation recorded.
- **Files / settings.** `src/vr.cpp`; `[Performance] holdSmooth`; mock `controls.txt` keys `vdhold`, `vdholdevery`.

### G3. Chain stutter after one late frame (double physics steps)

- **Symptom.** In explosions, one slow frame was followed by more slow frames.
- **Root cause.** After a late frame, the next display interval covers two physics steps. The fixed-step loop then
  runs two steps, which makes that frame slow too.
- **How it was found.** Frame timing in the log.
- **Fix.** `[Affichage] limitCatchUp=1`. The accumulators are capped at `max(1.98, interval + 0.98)` steps (and never
  above the engine's own 2.99), so there is at most one step per frame when the headset is faster than the physics.
  The extra time is dropped and counted (log: "physics catch-ups dropped").
- **Verification.** Log counters.
- **Limits.** Under sustained overload the game runs slower than real time.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp`; `[Affichage] limitCatchUp`.

### G4. Frame B repeated the whole game update (frameBMode)

- **Symptom.**
  - CPU freezes in heavy destruction.
  - Double spikes when switching tools: for example frame A 81 ms, then frame B 76.5 ms, because the tick ran again
    in frame B.
- **Root cause.**
  - Frame B re-ran every mod's tick (this mod's included), the player, the tool and Scene::update, for an instant
    that frame A had already computed. That was about 55 % of the main thread's work per pair at rest.
  - In mode 0 it also caused one-tick muzzle flashes and point lights from frame B, and shells spawned in frame B
    shown one frame early in the right eye.
- **How it was found.** Per-eye section timing (`perfTrace`), and a census of the Lua bindings each mod calls in A
  and in B.
  - One hook on the binding dispatcher `0x4a9050` sees 486 bindings.
  - The "core" bindings (registry, input, time...) bypass it. They form a static `{fn, name, next}` list in `.data`
    and were hooked one by one.
- **Fix.** `[Performance] frameBMode` (`src/frameb.cpp`; `Hook_Update` and `FindFrameBTargets` in
  `src/engine.cpp`).
  - **0**: as before.
  - **1**: in frame B, skip the tick of the mods whose frame-A tick of the same pair called only read bindings.
    - A list of exceptions keeps the mods that must tick in B: `GetLastSound`, events, `UiSoundLoop` / `UiMute`,
      `PauseMenuButton` and others.
    - `[Performance] frameBKeepMods` forces mods to keep their B tick.
  - **2** (default, "render only"): frame B runs no `Game::update` at all. `FrameB::BReplay` re-issues what frame A's
    update asked for one frame and that A's render consumed:
    - lines (`DrawLine`, lasers; renderer list `+0x23b0`) and sprites (`+0x23c0`), debug lines, outlines, highlights;
    - point-light counts, depth of field, low-health blur, camera requests (the camera offset composed with the
      right-eye offset);
    - sound loop requests, music volume and low-pass, the "last sound heard", mute, time scale;
    - per-frame registry flags (rewritten only if they had the same value at the previous frame A's exit).
  - The script event double-buffer swap is skipped at the start of a render-only frame B, so mods still see what A
    posted, once.
  - HUD and game-mode ticks get the real time of the whole pair.
  - Any unexpected state falls back to a full update for that frame B, with a logged reason ("no frame-A record",
    "frame start not seen"...). A memory fault in a replay disables mode 2 until restart.
- **Verification.**
  - An independent review found 7 real bugs, all fixed before release:
    - a replayed `ClearKey` wiped sub-keys written after the clear;
    - registry keys past 512 per update were dropped silently;
    - a fallback reason was mislabelled;
    - the engine's camera pose in B was the left eye's;
    - the music volume buzzed at about 90 Hz, because the per-frame volume key was consumed every frame;
    - `GetLastSound` always returned 0, so robots were deaf;
    - `UiSoundLoop` / `UiMute` called from `tick` were not seen as side effects.
  - Live A/B of the right-eye image across modes 0, 1 and 2 on the mock: the differences are within capture noise.
    Scenes: lasers and outlines, a mod drawing lines, sprites, lamps, Physics Gun outlines, the AC130 camera, a
    slow-motion mod, a HUD notification fade, smoke.
  - By ear: music fade without buzz, robots hearing gunshots.
  - Stress tests (G5).
  - Headset log: frame B work 1.1 to 1.5 ms.
- **Limits** (modes 1 and 2):
  - a mod that counts ticks without `dt` animates at about 45 fps instead of twice per pair;
  - one-shot effects (`PlaySound`, `SpawnParticle`, `ShakeCamera`) are not repeated in B;
  - real key edges falling in a frame B are not seen by any update (the mod's own simulated keys are already delayed
    to frame A);
  - a vibration requested in frame A leaves one frame later;
  - the game's internal scripts that read body hits were not checked.
- **Status.** Default 2. It is part of the "CPU optimization (recommended)" checkbox, which sets `frameBMode` 2/0 and
  `frameLatency` 2/0 together. Verified on the mock; headset sessions measured.
- **Files / settings.** `src/frameb.cpp`, `src/frameb.h`, `src/engine.cpp`; `[Performance] frameBMode`,
  `frameBKeepMods`. RE §6 quater ter.

### G5. CPU and GPU took turns (frameLatency)

- **Symptom.** Removing frame B's update (G4) gained much less real time than expected.
- **Root cause.**
  - After each `Present`, the engine waits on the swap chain's frame-latency object (`TRendererD3D12::endRender`
    `0x5b6e50` → `0x5bc180`; `SetMaximumFrameLatency(1)` at `0x5bba4b`).
  - With latency 1, the CPU starts frame B only after the GPU has finished the left eye; then the GPU waits for B's
    commands.
  - Measured: 4.4 ms of "display" wait per frame B under destruction, with the GPU at 55 to 65 % and the main thread
    at about 40 % of a core.
- **How it was found.** A measuring hook on `0x5bc180` (pattern `48 8B 49 60 48 85 C9 74 0C BA FF FF FF FF 48 FF 25`)
  that adds the time to "display" in the per-eye waits.
- **Fix.** `[Performance] frameLatency=2`.
  - During the headset session, `IDXGISwapChain2::SetMaximumFrameLatency(2)` on the game's chain
    (`Perf::FrameLatency`). It is re-checked every 30 frames, because the game restores its value when it rebuilds
    its buffers.
  - The game's value is put back when the session stops.
  - The drawing is the same: same commands, same order, same queue. The engine's two frames in flight still protect
    its resources.
- **Verification.** Mock stress test on the Lee Chemicals level, mode 2 + latency 2 against mode 0 + latency 1:

  | Phase | Pair (ms) | Right eye (ms) | Missed refreshes per s | Hitches per 600 |
  |---|---|---|---|---|
  | Calm | 6.8 → 4.3 | 3.5 → 1.0 | 0 → 0 | 0 → 0 |
  | Explosions | 15.8 → 13.9 | 5.8 → 1.9 | 33.9 → 24.4 | 105 → 63 |
  | After | 9.9 → 7.6 | 4.1 → 1.6 | 11.1 → 5.1 | 0 → 0 |

  Latency 2 alone gains little: B's update moves onto the critical path. The two settings complement each other.
- **Limits.** Under the heaviest load, frame A now waits on the GPU: the GPU becomes the limit.
- **Status.** Default 2. Verified on the mock; deployed.
- **Files / settings.** `src/perf.cpp` (`Perf::FrameLatency`); `[Performance] frameLatency`. RE §6 quater quater.

### G6. Frame A waited for the GPU before simulating (lazyBeginFrame)

- **Symptom.** At the explosion peak, frame A computed only about 6 ms but took much longer.
- **Root cause.** `Game::frame` (`0x71ce0`) calls `TRendererD3D12::beginRender` (`0x5b35a0`, call at `0x71f84`)
  *before* `Game::update`. It waits for the GPU fence of frame N-2, about 3 ms at the peak. So the next simulation
  could never overlap the GPU drawing.
- **How it was found.** Fence-wait instrumentation by calling function (`perfDetail` bit 1), and a full RE of what
  `beginRender` does (it submits nothing).
- **Fix.** `[Performance] lazyBeginFrame=1`.
  - Frame A's `beginRender` is held back until the frame's first real use of the renderer.
  - The `TRendererD3D12` vtable (`0xa81d80`, 74 entries, checked by pattern and RTTI name) is redirected:
    - +0x10 → `LazyBegin`;
    - 57 methods → small generated relays (one `cmp` + `jmp` when nothing is held; otherwise `LazyTouch` runs the
      original `beginRender` with its wait, then the method);
    - 16 entries are left alone: empty functions, accessors, and the asynchronous uploader's 3 creation methods
      (their bodies are hash-checked).
  - The commands, their order and the queue are the same; only the CPU wait moves.
  - An unexpected case (a first use from another thread, or `beginRender` still held at the next one) runs it at
    once, logs, and switches the feature off for the session.
- **Verification.** Mock, explosions, 2 + 2 interleaved runs:
  - frame A 13.2 → 10.6 ms;
  - pair 15.0 → 14.2 ms;
  - missed refreshes 29.5 → 26.7 per second;
  - hitches 88 → 72;
  - just after the explosions: missed refreshes 7.2 → 5.7 per second, pair p90 14.8 → 10.9 ms;
  - images compared live with lazy 0 and 1: the difference is noise (0.088 against 0.084).
- **Limits.** At the peak, part of the GPU wait moves to frame B, where the GPU is the limit.
- **Status.** Default 1 (part of "CPU optimization"). With all the defaults: 21.9 missed refreshes per second and 57
  hitches, against 34 to 40 per second and 105 to 154 hitches before this work.
- **Files / settings.** `src/perf.cpp` (`LazyBegin`, `LazyTouch`), `src/frameb.cpp`; `[Performance] lazyBeginFrame`
  (2 = frames B too, test only).

### G7. GPU passes running for nothing: bloom and depth of field (skipDeadPasses)

- **Symptom.** About 0.3 ms per eye of GPU time spent on effects that were switched off.
- **Root cause.**
  - **Bloom.** With `removeBloom`, the post bloom factor (`scene+0xa4c`) is 0, and `post` computes
    `composite + bloom × factor`. The `Bloom::render` chain (`0x7ea80` via `0x7f050`: 20 draws) still ran, and its
    result × 0 changes nothing.
  - **Depth of field.**
    - `removeDof` calls `SetCameraDof(0, 0)`. `Renderer::setDof` (`0x9cbe0`) sets the one-shot flag
      `renderer+0x23f0`, which makes `Renderer::render` run `Dof::render` (`0x80ea0`) **even though the game's
      depth-of-field option is off**.
    - It runs with amount 0, and its last pass writes the composite back unchanged.
    - So the mod itself caused that cost.
- **How it was found.** Per-pass GPU timing (`perfDetail` bit 8: timestamps written into the game's command list
  where the pass changes). This showed that no frame-B pass can be skipped in general, but that bloom (0.12 ms per
  eye) and depth of field (0.18 ms per eye) still ran. RE then proved both outputs dead.
- **Fix.** `[Performance] skipDeadPasses` (`Hook_BloomRender`, `Hook_DofRender`).
  - The bloom is skipped only when its factor is exactly 0. The result target is cleared to 0 instead, as the chain
    itself does first.
  - The depth of field is skipped only when its amount is exactly 0. The noise-angle global (`0x1c6ecb0`) is written
    as the pass would write it, and the focus pass keeps running.
  - Never in photo mode, never with a debug view (whose normal value is `final`).
  - Value 2 (default) also checks on the GPU every 5 s or so, with an exact comparison. Any difference turns the
    skips off until restart.
- **Verification.**
  - GPU harness with the game's own shaders at three resolutions: the post image is bit-identical with factor 0; the
    5 depth-of-field passes at amount 0 rewrite the composite bit for bit.
  - Mock during explosions: 77 exact GPU comparisons, 0 different, 0 Inf/NaN texels. Bloom and depth of field at
    0.00 ms.
- **Limits.** The proof assumes a finite HDR image (no Inf or NaN), which mode 2 counts.
- **Status.** Default 2. Value 1 (no check) would be enough once headset sessions show only "identical" checks.
- **Files / settings.** `src/engine.cpp` (`Hook_BloomRender`, `Hook_DofRender`); `[Performance] skipDeadPasses`.
  Does not depend on the game-derived shaders.

### G8. Explosion physics: what waits on what (analysis only)

- **Symptom.** Heavy destruction still gave frames A of 14 to 18 ms.
- **Root cause.** Measured at the peak (`perfTrace=1`, `perfDetail=7`):
  - frame A computes only about 6 ms;
  - 4 to 7 ms is job waiting, mostly the solver (`TSolver::TSolverJob`: about 65 ms of CPU per frame A across the
    workers, 8 Gauss-Seidel iterations);
  - one "island" (a pile of touching debris) is solved in **one** job, 3.5 to 6 ms;
  - the GPU is busy about 6.3 ms per eye at the peak, about 12.8 ms per pair, above the 11.1 ms budget at 90 Hz.
- **How it was found.** `Job::run` timed per RTTI class (`perfDetail` bit 2), and GPU timestamps around each
  submission.
- **Fix.** None that keeps the game identical:
  - splitting islands changes the Gauss-Seidel order, so the physics changes;
  - the **number** of jobs also sets how the near-phase pairs are split (`min(lists, 2 × workers)`), so the number of
    workers must never change;
  - GPU physics is not feasible: the voxel collision, solver and destruction are closed code; the results would not
    be identical; there would be CPU↔GPU round trips inside the step; and the GPU is already the limit.
  - The levers that remain change the image: resolution, particle density, number of lamps.
- **Verification.** Not applicable.
- **Limits.** Heavy destruction stays the heaviest case.
- **Status.** Analysis only.
- **Files / settings.** `[Debug] perfTrace`, `[Performance] perfDetail`. RE §6 quater quinquies.

### G9. Smaller levers: one adopted, several measured and rejected

- **Symptom.** Remaining CPU and GPU time worth trying to recover, after G4 to G7.
- **Root cause.** Various; see each item.
- **How it was found.** Measurements with `perfTrace` and `perfDetail` (G10).
- **Fix.**
  - **Adopted: `parallelParticleSort=1`.**
    - In frames without a physics step (every frame B), the depth sort of the 3 particle systems (`0x437df0`) runs on
      3 threads at once.
    - It is the same function with the same inputs, and the engine already runs it in parallel from its
      post-simulation jobs.
    - The main thread takes over if a helper thread has not started.
  - **Rejected after measurement** (left as options, default 0):
    - `jobSpin`: idle engine threads spin a few µs before sleeping. 93 % fall asleep anyway, it costs 0.6 to 1.3 ms of
      CPU per frame A, and the gain is within noise.
    - `skipShapeJobsB`: a gain of about 0.02 ms, and the job also updates the interpolation history of dynamic
      shapes.
    - `hiddenAreaFill`: paint the area hidden by the lenses black (`XR_KHR_visibility_mask`), so the encoder spends no
      bits there. It covers only 0.3 to 2 % of a Quest 3's pixels with a 2-degree margin, has no airtime effect with
      constant-bitrate encoding, and risks black arcs during long reprojections.
    - `gpuPriority`: GPU scheduling class "below normal". It could only help GPU-saturated holds, and other apps could
      pre-empt the game.
  - **Not done:**
    - an asynchronous submit thread (G1);
    - a pool for job objects (it would need hooking the engine's global allocator);
    - forcing a higher Luau optimisation level for mod scripts (the investigation stopped unfinished).
- **Verification.** `parallelParticleSort=2` checks each sort against the serial one: 18 003 checks, 0 differences.
- **Limits.** As listed for each rejected lever.
- **Status.** As listed.
- **Files / settings.** `src/engine.cpp` (`Hook_PsSort`, `Hook_ShapeJobs`), `src/perf.cpp`, `src/vr.cpp`;
  `[Performance] parallelParticleSort`, `jobSpin` (`jobSpinUs`, `mainSpinUs`), `skipShapeJobsB`, `hiddenAreaFill`
  (`hiddenAreaGuard`), `gpuPriority`.

### G10. Measuring without changing anything (perfTrace, perfDetail)

- **Symptom.** No optimisation could be judged without per-eye, per-section numbers, and measuring must not change
  the behaviour.
- **Root cause.** The game provides no per-eye, per-section timing, and opening its own profiler changes the frame
  loop (see Limits).
- **How it was found.** Not applicable: this is tooling.
- **Fix.**
  - `[Debug] perfTrace`:
    - 0: no hook installed, nothing timed;
    - 1: per-eye sections (update, fixed loop, particles, the mod's own work, render, VR submit, present, headset
      wait), main-thread waits (jobs, GPU fence, display, shapes), physics steps per frame, the engine's own profiler
      categories, and CPU per thread group;
    - 2: adds each mod's tick time and a census of Lua bindings per mod.
  - Hooks call the engine function unchanged. Every pattern must be unique, otherwise that measurement is left out
    ("NOT hooked").
  - `[Performance] perfDetail` (bits, only with `perfTrace` ≥ 1): 1 blocking job waits and GPU fence waits by
    caller; 2 every job timed by class; 4 GPU busy/idle per frame; 8 GPU time per pass.
  - "Slow frame" lines (more than 18 ms of work) break the frame down and name the 2 costliest mod scripts.
    - In one session, AC130 Airstrike cost about 1.2 to 1.5 ms of Lua per frame A even when unused, and Meteor
      Strike about 0.8 ms after explosions.
- **Verification.** With `perfTrace` 0 then 1, the median work per pair must not move by more than 0.1 ms.
- **Limits.** Gotcha: never open the game's own profiler (SHIFT+ALT+F3) during a measurement. It switches
  `Game::frame` to one update per frame, limited to 60 fps.
- **Status.** In place, off by default.
- **Files / settings.** `src/perf.cpp`, `src/perf.h`, `src/prof.h`; `[Debug] perfTrace`, `[Performance] perfDetail`.
  RE §6 quater bis.

### G11. The mod's own overhead

- **Symptom.** The mod's own work cost main-thread time: logging, script work, and very large mod weapons.
- **Root cause.** See each item below.
- **How it was found.** Per-item measurements on the mock.
- **Fix.**
  - **Logging on the main thread.** Each line was `fprintf` + `fflush` (10 to 30 µs, with rare multi-ms stalls from
    antivirus scanning).
    - The log is now asynchronous (`src/log.cpp`). Lines are formatted by the caller, buffered, and written every
      50 ms (or at 64 KB) by a writer thread.
    - Hang reports, stacks, DLL unload and severe exceptions are written at once. A vectored handler never waits on a
      lock.
    - Lines beyond 8 MB pending are counted as dropped.
    - `[Debug] logSync=1` restores line-by-line writing.
  - **Priority.** `[Performance] cpuPriority=1` sets `ABOVE_NORMAL` during the headset session, only if the game is at
    `NORMAL` (a priority chosen by the player is not touched). It is never HIGH, REALTIME or MMCSS, which would pass
    ahead of the streaming threads.
  - **Fast paths** (`fastToolShapes`, `fastToolScaleB`, `fastObjectMotion`, `fastLua`):
    - find a scaled mod tool's part by handle, instead of rebuilding a table of about 1000 shapes per frame;
    - reuse frame A's tool scale in frame B;
    - an open-addressing shape index with memory prefetch for `FixObjectMotion`;
    - cached and spread-out work in `main.lua`.
  - **Diagnostics left on.** `debugLua=1` costs CPU. It must be off for normal play.
  - **Equipping a huge mod weapon** (THE EVERYTHING REVOLVER, about 1000 shapes): its own script respawns its shapes
    in batches (about 77 ms over the two frames of a pair).
    - The mod's physical copy (`physBuild`, I6) also read every voxel of its first 16 shapes: about 52 000 reads, with
      0.5 mm voxels.
    - A 10 cm cell's read now stops at its first solid voxel (114 reads). The result is cached per tool shape and
      checked at the next equip.
    - Hidden "visible" copy shapes are no longer created when the copy is not shown.
- **Verification.** Measured per item on the mock.
- **Limits.** The mod weapon's own respawn cost remains; it belongs to that mod.
- **Status.** In place.
- **Files / settings.** `src/log.cpp`, `src/engine.cpp`, `mod/TeardownVR/main.lua`; `[Debug] logSync`, `debugLua`;
  `[Performance] cpuPriority`, `fastToolShapes`, `fastToolScaleB`, `fastObjectMotion`, `fastLua`.

### G12. Another program on the GPU, and overlays

- **Symptom.** A session ran at 15 to 20 fps. Separately, small hitches showed when overlays were present.
- **Root cause.**
  - Another GPU-heavy program, using most of the video memory at 100 % GPU, was sharing the card. With it paused,
    the headset held 90 fps at native resolution.
  - The Steam overlay waited 4 to 7 ms inside its `Present` hook.
- **How it was found.** Frame timing, plus system tools.
- **Fix.**
  - `src/gpu_watch.cpp` (`[Interface] gpuWatch=1`, panel "Warn if another program is using the graphics card") reads
    the PDH counter `\GPU Process Memory(*)\Local Usage`.
    - Not "Dedicated Usage", which also counts surfaces shared with other programs: the desktop compositor and an
      overlay process showed 17.5 GB and 11.1 GB there.
    - A head-locked banner shows for 10 s.
  - Overlays (`gameoverlayrenderer64.dll`, `nvspcap64.dll`) are detected at the first `Present` and later. They get a
    log warning and the panel line "%s overlay active: may cause stutters."
- **Verification.** On the test PC.
- **Limits.** The mod only warns; it cannot stop another program.
- **Status.** In place.
- **Files / settings.** `src/gpu_watch.cpp`; `[Interface] gpuWatch`.

---

## H. Input and controls

### H1. Held buttons released after less than a second (grab, held fire, extinguisher)

- **Symptom.** Holding the right grip (grab), the trigger (automatic fire) or the extinguisher stopped working after
  less than a second, although the button was still held.
- **Root cause.**
  - The game's window procedure (`+4f7f60`) keeps an "active" flag (window+0x18) from `WM_ACTIVATE` only;
    `IsFocused` (`+4f9450`) reads it.
  - While it is 0, every frame (`+a6866` / `+a74f3`) the game shows the cursor, releases the capture, and clears all
    the key and button "down / pressed / released" arrays (`+4f1fd0`, 3 × 0x121 bytes).
  - Mouse buttons come only from the legacy `WM_xBUTTONxxx` messages (`+4f84c6`); `WM_INPUT` gives only motion. The
    mod posts a button message once per press.
  - So whenever another window was in front (the streaming app, a notification, any program), a held button lasted
    one frame.
- **How it was found.** A mock test with Notepad in front of the game, then static RE of the window procedure.
- **Fix.** In `src/input.cpp` (`g_vrFocus`), while VR runs:
  - every `WM_ACTIVATE` reaches the game as "active";
  - `WM_ACTIVATEAPP(FALSE)` and `WM_KILLFOCUS` are swallowed;
  - `UpdateGameFocus` posts an activation if the game believes it is inactive;
  - Alt, Tab and Windows are released when a deactivation is hidden;
  - when VR stops, the game is told the truth;
  - `SetCursorPos` is refused while the window is not really in front (the "active" game recentres the cursor on
    every raw mouse move);
  - the real keyboard and mouse still go only to the window really in front (raw mouse without `RIDEV_INPUTSINK`,
    legacy keyboard).
  - An early version also hooked `GetForegroundWindow`. That proved useless and was removed: the game uses it only to
    bring itself forward and to open links.
- **Verification.** On the mock with Notepad in front: trigger held 3 s, and a 750 kg crate grabbed for 3 s.
- **Limits.** None known.
- **Status.** Verified on the mock; deployed. No later report of the problem in the notes.
- **Files / settings.** `src/input.cpp` (`g_vrFocus`, `UpdateGameFocus`). RE §7 quater.

### H2. Keys stuck down: the player kept walking after leaving a vehicle

- **Symptom.** After leaving a vehicle, or after using the settings panel, the player kept walking on their own.
- **Root cause.** Two ordering bugs in simulated input:
  - Messages that arrive while a frame B is pending are held back and replayed before the next frame A, so the game's
    "just pressed" logic sees them on a normal frame. A message posted meanwhile could overtake the held ones. A
    key-up arriving before its key-down left the key down for the game.
  - While the settings panel was open, `WM_KEYUP` was swallowed. The game's keyboard is legacy-only (not raw), so the
    key stayed down.
- **How it was found.** Message logging in the window procedure hook.
- **Fix.**
  - `PostInput` queues new messages behind the held ones while any are waiting.
  - On foot, the DLL also sends "release" for W/A/S/D once per second.
  - Key-ups are no longer lost while the panel is open.
- **Verification.** On the mock.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/input.cpp` (`PostInput`).

### H3. Auto-walk at full speed

- **Symptom.** The player ran by themselves at 6.5 to 6.9 m/s (the game's own walk speed), above the mod's stick
  locomotion cap of 4.5 m/s, and the stick could not stop it.
- **Root cause.** The game saw a movement key held: a lost key-up, or a virtual gamepad created by the streaming app.
  The DLL itself held no movement key.
- **How it was found.** Speed measurement and the input state in the log.
- **Fix.**
  - `main.lua` `neutralizeGameWalk`: `SetPlayerWalkingSpeed(0)` while the game's own movement input is down. The mod
    moves the player with `SetPlayerVelocity`.
    - A first version set 0 on every tick, which broke stick walking.
  - If `InputDown` of up/down/left/right lasts 0.5 s, the Lua sends `TDVRU`. The DLL then releases W/A/S/D/Z/Q and
    the arrows, and logs "auto-walk".
- **Verification.**
  - Mock: a W key hammered by `PostMessage` leaves the player at 0 m/s, and the stick still gives 4.5 m/s.
  - The first mock check was wrong, because it read `GetPlayerVelocity` rather than the actual displacement (M7).
- **Limits.** None known.
- **Status.** Verified on the mock; deployed.
- **Files / settings.** `mod/TeardownVR/main.lua` (`neutralizeGameWalk`), `src/bridge.cpp` (`TDVRU`),
  `src/input.cpp`.

### H4. Non-QWERTY keyboards: simulated W/A/S/D did nothing

- **Symptom.** On an AZERTY keyboard, the simulated movement keys did not move the vehicle.
- **Root cause.** The game binds movement to "w a s d" (`options.xml`). On AZERTY, the letter W is not at the W
  position.
- **How it was found.** Not recorded.
- **Fix.** Each simulated key is sent with the US letter's virtual key **and** the US scan code (0x11, 0x1E, 0x1F,
  0x20 for W, A, S, D).
- **Verification.** On the mock and in the headset.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/input.cpp` (`SendKeyPos`).

### H5. Driving vehicles with VR controllers

- **Symptom.** Half-pressed controls did nothing, and the view did not follow the vehicle.
- **Root cause.**
  - The game reads gamepads through GameInput (not XInput), which the mod does not simulate. It has to send keys.
  - The accelerator is all-or-nothing: a pulsed key did not move the vehicle.
- **How it was found.** Not recorded.
- **Fix.**
  - Right and left triggers = W / S, pressed beyond 40 % (released below 25 %).
  - Left stick = A / D, pulsed over a 0.25 s cycle for progressive steering.
  - Y = E (get in and out); grips = mouse buttons.
  - In a vehicle, the VR "room" turns with the vehicle's yaw every frame, and the right stick turns the view relative
    to it.
- **Verification.** Mock: forward 2.8 m/s, reverse and steering. Driving was confirmed in the headset.
- **Limits.** None known.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/input.cpp`. RE §6 sexies bis.

### H6. Other mods' keyboard keys unreachable from the controllers

- **Symptom.** Many mods use keyboard keys (Q, R, F...). A VR player has no keyboard.
- **Root cause.** The keys exist only in each mod's Lua code and saved options.
- **How it was found.** RE of the game's log and mod lists:
  - the game writes `Active mod: <name> (<id>)` to `%LOCALAPPDATA%\Teardown\log.txt` for each global mod when a level
    loads;
  - before that, `modlists\<id>.xml`;
  - folders: `steam-<n>` → the workshop content folder, `builtin-<x>` → `<game>\mods\<x>`, `local-<x>` → the
    Documents mods folder.
- **Fix.** A keys panel (right stick click held), with keyboard and mouse tabs.
  - It scans the active mods' Lua files without running them, on a separate thread, for:
    - `InputPressed` / `Down` / `Released` / `Value("q")`, also through `pcall`;
    - variables passed to those functions, and the values the scripts assign to them;
    - keys chosen in the mod's options (`savegame.xml`).
  - Game actions (`interact`, `usetool`...) map to the player's binding in `options.xml`.
  - Long key lists (key-binding menus) are ignored.
  - A short left stick click repeats the last key used ("quick key").
  - Simulated keys pass through even while the panel is open: the window recognises its own posted messages (same
    message and parameters, under 2 s) and still holds back the real keyboard and mouse.
  - Alt is sent as `WM_KEYDOWN`. A lone `WM_SYSKEYUP` of Alt opens the window's system menu and freezes the message
    loop.
  - F8 and F10 are not offered: they are the mod's own shortcuts.
- **Verification.** 126 keys found in 46 ms across 21 active mods.
- **Limits.** Keys that a mod builds in ways the scan does not follow are not found.
- **Status.** In place.
- **Files / settings.** `src/modkeys.cpp`, `src/pckeys.cpp`; `[Mods] quickKey`. RE §6 sexies ter.

### H7. Mods that read the mouse while they own the camera

- **Symptom.** In a mod camera (AC130 plane, drone, turret), aiming needs the mouse.
- **Root cause.**
  - Mods read `InputValue("camerax" / "cameray")` or `"mousedx" / "mousedy"`. The game registers the mouse as raw
    input (page 1, usage 2, without `NOLEGACY`).
  - A script camera is camera mode 8, detected by the flag at `+0x2cc` of the camera object.
- **How it was found.** RE of the game's raw input registration and of the camera object.
- **Fix.**
  - While a mod owns the camera, a stick becomes that mouse: a simulated `WM_INPUT` with `lLastX` / `lLastY`, 1500
    points per second at full deflection, squared curve.
  - A `WM_MOUSEMOVE` follows a virtual cursor for the mods' windows.
  - That stick is no longer sent to the Lua (no walking or turning at the same time).
- **Verification.** On the mock and in the headset (AC130).
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/input.cpp`; `[Mods] modMouse`, `modMouseStick`, `modMouseSpeed`.

### H8. Guns aimed 60 degrees above the laser

- **Symptom.** Guns pointed upward instead of along the controller's aim.
- **Root cause.** The Lua passed OpenXR's "grip" pose. On Virtual Desktop, `aim = grip × {(0, -0.067, -0.074),
  Rx(-60°)}`, so the grip's -Z axis is 60 degrees above the aim ray.
- **How it was found.** Comparing the two poses.
- **Fix.** Guns take the rotation of the "aim" pose; the position stays the hand's. The sledge keeps the grip pose.
- **Verification.** Confirmed in the headset.
- **Limits.** None known.
- **Status.** Confirmed in the headset.
- **Files / settings.** `mod/TeardownVR/main.lua`.

### H9. Controls for the free hand

- **Symptom.** The free hand (left, or right in left-handed mode) had to grab, crouch and interact, and its grip
  first chose between grabbing and crouching depending on whether the Lua had reported an object in reach.
- **Root cause.** One grip served two actions, and the choice depended on the Lua's report of an object in reach.
- **How it was found.** Not recorded.
- **Fix.**
  - The left grip always grabs (with `offGrab`; otherwise it crouches).
  - L2 = crouch while held.
  - Y = interact (E).
  - In a vehicle, L2 still reverses.
  - The Lua "ghost" gesture now uses `InputDown("interact")`. `TDVRH` is kept for the log and for panel grabs.
- **Verification.** Mock: the grip held a 29 kg body with crouch 0.00. L2 gave crouch 1.00, back to 0.00 on release.
- **Limits.** None known.
- **Status.** Verified on the mock.
- **Files / settings.** `src/input.cpp`, `mod/TeardownVR/main.lua`; `[Controles] offGrab`, `offHandPhysics`,
  `offArm`.

### H10. The 2D pointer jumped between hands by itself

- **Symptom.** In menus, the pause screen and panels, the pointer sometimes jumped to the other hand when the hands
  came close or crossed. The other hand's trigger also clicked at the pointer hand's cursor.
- **Root cause.**
  - `Input::UpdatePointerHand` switched hands whenever only the other hand's ray was on an interface. That happens
    when the ray sweeps past an edge, drops for a frame, or the controller loses tracking for a frame, which is likely
    when the hands are close.
  - `ModUiPointer` (mod windows) had the same rule.
  - On the virtual screen, either trigger clicked, and a press made beside the screen clicked at the real Windows
    cursor.
- **How it was found.** The log line "pointer hand -> left (only it points at a panel)", then code review.
- **Fix.** One shared "last clicked hand" (`Input::PointerHand`, `Input::TakePointer`):
  - the other hand takes the pointer **only** by pulling its trigger with its ray on an interface, and that press is
    its first click, at its own ray;
  - a press in progress keeps its hand;
  - a press beside the screen does not click;
  - the other hand's ray shows as a small grey square on the virtual screen;
  - with `offHandPointer`, mod windows use the same pointer hand.
- **Verification.** Mock test steps A to G passed. Steps H and I were inconclusive because of test aiming.
- **Limits.** Not fully tested on the mock (steps H and I).
- **Status.** Verified on the mock; deployed.
- **Files / settings.** `src/input.cpp`, `src/input.h` (`PointerHand`, `TakePointer`, `UpdatePointerHand`),
  `src/engine.cpp` (`ModUiPointer`); `[Controles] offHandPointer`.

---

## I. Tools, weapons and physics

### I1. The held tool did not follow the VR hand

- **Symptom.** In first person, the tool stayed where the desktop game puts it.
- **Root cause.** In first person the engine ignores `SetToolTransformOverride`.
- **How it was found.** RE of `ToolAnim::update` (`0x452f90` in 2.0.1). It was located by its "override → final" copy
  (`movups xmm0,[rcx+0A0Ch] ; movups [rcx+844h],xmm0`), preceded by `cmp byte [rcx+0A38h],0 ; jne`.
- **Fix.** `Hook_ToolAnim`. After the normal computation, the final pose becomes
  `hand × inverse(grip) × inverse(rest) × animatedPose`, so the tool follows the hand and keeps its animations (recoil,
  sledge swing).
  - `animatedPose` = `ToolAnim+0x68`, the animator's pose before the desktop offsets (`SetToolOffset`, the
    `options.gfx.fov` offset, the lowering at `+0x8a8`).
  - `rest` = the tool prefab's `fp_action` location and `grip` = its `tp_action_rh` location, read from
    `data/prefab/tool/<name>.xml` (Teardown Euler angles = Ry·Rz·Rx, in degrees). The sledge has no `fp_action`; it
    uses a fixed rest position.
  - Tool name: `*(ToolAnim+0x30)+0x18`, an engine string of 32 bytes, inline while byte 0x1f is 0. Any printable name
    of up to 31 characters is read: mods' IDs often have capitals.
- **Verification.** Confirmed in the headset.
- **Limits.** None known.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp` (`Hook_ToolAnim`). RE §6.

### I2. Mod tools: the script order decided who placed the tool

- **Symptom.** Mod weapons sometimes jumped between the mod's pose and the VR hand.
- **Root cause.**
  - `SetToolTransform` (binding `0x497330`) and `SetToolTransformOverride` (`0x497480`) write the **same** field,
    `player+0xa0c`. Only the override raises `+0xa38`.
  - The field is reset every frame by `0xc4460` (called by `Game::update` before ToolAnim), so whichever script ran
    last won.
  - Mod tools ("script tools", tool number 0) take another branch in ToolAnim (`0x453afc`): their pose is the smoothed
    `SetToolTransform` pose.
- **How it was found.** RE of the two bindings and of ToolAnim.
- **Fix.**
  - The DLL hooks the `SetToolTransform` binding (`Hook_SetToolTransform`). It keeps the mod's pose aside and puts the
    VR hand back in the field.
  - Mod tools are placed at `hand × inverse(H) × screenPose`. H is chosen once per tool by the Lua:
    `fp_action × tp_action_rh` if the tool has them; otherwise the point the mod's own hand holds, if within 10 cm of
    the tool's shapes; otherwise a point taken from the tool's shape box.
  - The mod's hand pose (`SetToolHandPoseLocalTransform`, `0x496fa0`) is read from `+0xa70` (right) and `+0xa90`
    (left).
- **Verification.** On the mock, with a dozen mod tools.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (`Hook_SetToolTransform`), `mod/TeardownVR/main.lua`.

### I3. Mod weapons were huge in the hand (1 to 4.5 m long)

- **Symptom.** Mod tools held in the hand were giant.
- **Root cause.** Mods place their tool for a desktop screen, 0.3 to 3.4 m in front of the eye, and draw it to match.
- **How it was found.** Measuring the held tools on the mock (table below).
- **Fix.** `ScaleModTool` in `Hook_ToolAnim` ("kind 2"). Each mod tool is shrunk by a factor computed once per tool
  name, from its unscaled shapes and screen pose:
  - s = 0.45 m (eye → VR hand) / screen distance of the box centre, clamped to [0.25, 1];
  - then reduced so that the tool is at most 1.1 m long (the game's shotgun is 1.1 m, the sledge 1.2 m), never below
    0.2;
  - times `[Controles] modToolScale`.
  - Layout used:
    - tool body `player+0x820`, entity type u16 at +8 (1 body, 2 shape), first child +0x28, next sibling +0x18;
    - shape local transform +0x44;
    - voxel data +0xf0: sizes (3 ints) at +0, voxel size (float) at +0x20.
  - The voxel size and the shapes' positions in the body are multiplied by s. Voxels belong to each shape, so only the
    tool changes.
  - The measurement waits until the shape count, length and screen distance have been stable for 0.3 s (3 s at most),
    because mods build their tool over several frames.
  - A tool whose shape count changes a lot is measured again.
  - The Lua receives s (reply field 101).
- **Verification.** On the mock, before → after:

  | Tool | Before | After |
  |---|---|---|
  | Revolver | 1.2 m | 0.43 m |
  | DOOM Chaingun | 4.45 m | 0.94 m |
  | GLARE | 2.3 m | 0.92 m |
  | GPD | 2.6 m | 0.75 m |
  | Physgun | 0.93 m | 0.39 m |
  | Magnetizer | 0.8 m | 0.31 m |
  | Thermite | 2.15 m | 0.73 m |
  | Vacuum | 2.05 m | 1.1 m |

- **Limits.** See I4 and I5 for what the first version missed.
- **Status.** Confirmed in the headset (the round after the revolver fix, I4).
- **Files / settings.** `src/engine.cpp` (`ScaleModTool`, `Hook_ToolAnim`); `[Controles] modToolScale`.

### I4. A big revolver stayed 1.3 m long, and animated parts popped back to full size

- **Symptom.** THE EVERYTHING REVOLVER stayed a 1.3 m revolver against the face. Animated parts of other mod weapons
  (the DOOM Chaingun's barrels) were full size during the scripts' tick.
- **Root cause.**
  - The shape walk was capped at 64 children. That revolver has 1028 shapes (266 + 762), so 964 were left at screen
    size.
  - Mods move parts with `SetShapeLocalTransform`, which rewrote them unscaled. The Lua physical copy then measured
    2.33 m instead of 0.94 m.
- **How it was found.** Dumping the held tool's shapes, read-only (`tool_box.py`).
- **Fix.**
  - All the shapes of the body are walked, up to 4096. Voxels smaller than 1 mm (engravings down to 0.35 mm) are
    scaled too.
  - The engine function behind `SetShapeLocalTransform` (`+0x496550`; it writes `shape+0x44`) is hooked
    (`Hook_ShapeLocalImpl`). After the call, only that shape is rescaled, if its parent is the held tool's body.
    - An earlier version hooked the binding and rescaled the whole tool on each call: too slow for the revolver,
      which moves about 125 of its 1028 shapes every tick.
    - If the function is not recognised, the binding fallback is used, only for tools of up to 128 shapes.
- **Verification.** Mock: the revolver at 534 shapes and 0.42 m; +0.3 ms of cost.
- **Limits.** Without the engine function, animated parts follow only on tools of 128 shapes at most.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp` (`Hook_ShapeLocalImpl`, `kMaxToolShapes`).

### I5. Shots left from the old, unscaled barrel

- **Symptom.** Bullets, flashes, beams and lights of mod weapons started up to 1 m in front of the drawn barrel.
- **Root cause.** Mods compute the muzzle in their tool's own frame (bones, offsets) or from the camera. Shrinking the
  tool about its body origin moved the drawn barrel away from that point. Most bullets are simulated in Lua each
  tick, so correcting `SpawnParticle` / `PointLight` positions would fix only the first frame.
- **How it was found.** A survey of the installed mods' muzzle computations (DOOM Chaingun bone `mainbarrelend`,
  physgun `nozzle`, revolver `main.shoot`, GPD and Omnicutter offsets, and so on).
- **Fix.** Each tool is scaled **about its muzzle** m (`ToolMuzzle`):
  - m is the centre of the frontmost solid voxels in the mod's screen view, within a slice of max(2.5 voxels, 3 % of
    depth), at most 20 cm, set on their front face;
  - shapes are drawn at `m + s (p − m)`, so any point the mod computes in its unscaled tool frame lands on the drawn
    tool at the muzzle;
  - the body is placed at `hand × (q, s·p − (1 − s)·q·m)`, so the image equals the screen image shrunk toward the eye,
    recoil included;
  - `GetShapeLocalTransform` (`+0x47cdd0`) and `GetShapeWorldTransform` (`+0x47da60`) are hooked: mods reading a part
    of the held tool get it where they put it (unscaled), so they do not scale things twice. The VR mod passes a
    second argument to read the drawn size.
- **Verification.** Mock: shot origin within about 1 cm of the drawn barrel (DOOM, GPD, Omnicutter). Confirmed in the
  headset.
- **Limits.** The tool body's origin is now |m| behind the drawn barrel, often behind the head. A ray or sound that a
  mod places at the body origin (the GPD's ray) starts behind the player; with the player's back less than about 1 m
  from a wall, the GPD ray hits that wall.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp` (`ToolMuzzle`).

### I6. Tools passed through objects and pushed nothing (the physical tool)

- **Symptom.** The held tool went through crates and walls, and pushed nothing.
- **Root cause.** The game's tool is a static, non-colliding body placed by the engine every frame.
- **How it was found.** Testing.
- **Fix**, in two steps:
  1. **Swept spheres** (`toolCollision`; `toolBlock`, default off).
     - Spheres derived from the tool's shapes are swept from the last shown pose to the hand.
     - On contact the tool stops 1 cm before the surface and slides; moving bodies get an impulse (at most 1500 N)
       using the real hand velocity.
     - Right after a tool is equipped, `GetBodyTransform` of the tool has a zero rotation: it must be validated before
       use.
  2. **Physical copy, like Bonelab** (`[Controles] toolPhysics=1`, default).
     - The Lua `Spawn`s a dynamic copy (tag `tdvr_proxy`): for each tool shape, an exact visible shape and an
       invisible 10 cm physics shape. The game's tool gets the `invisible` tag.
     - The copy is driven in `update()` (60 Hz) by `ConstrainVelocity` and `ConstrainAngularVelocity` on 3 axes:
       target velocity = hand velocity (player included) + 0.8 × error / dt, with impulses capped (2000 N,
       120 N·m). The engine resolves contacts at the same time, so the copy goes through nothing.
     - Collision layer 128, which the player ignores.
     - Engine gotchas found on the way:
       - `CreateShape(body, t, refShape)` takes the reference's palette but always makes 10 cm voxels (tools use
         5 cm); `CopyShapeContent` into it doubles the size.
       - A `<vox scale=...>` shape made by `Spawn` has the right voxel size, but after `CopyShapeContent` and
         `CopyShapePalette` its mass stays zero and the body turns static.
       - So: visible shape = `<vox file="MOD/proxy.vox" scale=...>` plus copied content and palette, collision
         filter (0, 0); physics shape = `CreateShape` with the tool's palette, downsampled to 10 cm, tagged
         `invisible`. A wooden `voxbox` keeps the body dynamic during creation.
       - `SetShapeDensity` multiplies the material density (a factor, not kg/m³).
       - The `invisible` tag hides a shape at run time, its shadow included.
- **Verification.**
  - Mock: a 50 kg crate pushed; error 1.5 cm walking at 4 m/s; no visible offset rotating at 150°/s.
  - Testers liked the physics in the headset.
- **Limits.** See I7 to I10 for the problems found afterwards.
- **Status.** Confirmed in the headset.
- **Files / settings.** `mod/TeardownVR/main.lua`, `mod/TeardownVR/proxy.vox`; `[Controles] toolCollision`,
  `toolBlock`, `toolPhysics`. RE §6 sexies.

### I7. The tool trembled in the right eye while walking

- **Symptom.** While walking, the held tool shook in the right eye only.
- **Root cause.** Frame B reused the tool's world-space pose, but the player's body moves one physics step between
  the A and B ticks.
- **How it was found.** Not recorded.
- **Fix.** Reuse the body-relative pose instead (`lastShownLocal` in `main.lua`).
- **Verification.** Measured jitter: 3 mm in both eyes.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua` (`lastShownLocal`).

### I8. The physical copy lagged and buzzed at low frame rates

- **Symptom.** At 15 to 20 fps (two physics steps per frame), the copy trailed about 10 cm behind and the controller
  vibrated non-stop.
- **Root cause.** The hand velocity was computed over the frame time (55 ms), while the game simulated only 33 ms.
- **How it was found.** Not recorded.
- **Fix.**
  - Each physics step targets the hand of the tick, plus the player's real displacement since the tick
    (`GetPlayerTransform` read in `update`), plus the hand's own motion.
  - The game's tool is drawn at the hand plus the copy's physical error at the same simulation instant
    (`PHYS_SHOW_COPY = false`, the game's animations kept).
  - Haptics fire only when the copy's velocity departs from the command near another shape, that is on real contact.
- **Verification.** Mock: 3 mm of shown error walking at 4.5 m/s, at 72 and at 16 fps; no vibration in the air; one
  vibration on hitting a 50 kg crate.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua` (`PHYS_SHOW_COPY`).

### I9. The tool snagged on empty air

- **Symptom.** The physical copy snagged on nothing visible near the player.
- **Root cause.** The player character is made of 26 animated bodies whose shapes accepted all collision layers,
  including 128.
- **How it was found.** Not recorded.
- **Fix.** The mod removes layer 128 from their masks, and re-checks every second.
- **Verification.** Not recorded.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua`.

### I10. The game's own grab and pick took the physical copy

- **Symptom.** Grabbing or firing sometimes caught the invisible copy in the player's hand.
- **Root cause.**
  - The game's queries (shots, strikes, grab pick) skip the game's tool, but not the copy.
  - The game fires before the Lua tick, so switching layers on the trigger press came too late.
- **How it was found.** Not recorded.
- **Fix.**
  - The copy moves to layer 0 (skipped by those queries) from 25 % of trigger travel, and while an object is carried.
  - It also goes "ghost" when the grip is above 0.25 **and** its box is on the camera's pick ray.
  - If the game's grab still takes the copy, `ReleasePlayerGrab()` is called.
- **Verification.** Not recorded.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `mod/TeardownVR/main.lua`.

### I11. Sledgehammer: swing gesture, trembling, and swings that broke nothing

- **Symptom.**
  1. In VR, a sledgehammer should strike when it is swung, not only on a button.
  2. Hits made objects tremble.
  3. A light trigger press did nothing, and sometimes the swing animation played but the object did not break.
- **Root cause.**
  1. The game's sledge strikes only on its "use tool" button.
  2. The strike impulse went to everything touched, and hits counted for as long as the head stayed in contact.
  3. Two causes:
     - the DLL pressed "use tool" only above 55 % of trigger travel;
     - the game's sledge (`Sledge::update`, `0x450690`) starts on the **press** only, then looks for its target once,
       **0.256 s later** (the swing timer crosses 0.47 at 1.8 per second). The search is a 0.3 m sphere cast up to
       3 m from the player camera, with no layer filter, so it hits layer 128. If the trigger was released before
       then, `main.lua` had given the copy its collisions back, and the sphere hit the unbreakable copy in front of
       the camera instead of the target.
- **How it was found.** Static RE of `Sledge::update`: the cooldown at `+0x138` (0.3 s), action 9 "pressed this
  frame" at `input+0xdad`, the timer at `+0x130`, the cast `0x193910` with 3.0 and 0.3, and the hole `0x138260`.
- **Fix.**
  1. Swing gesture (`[Controles] swingGesture`, panel "Physical sledge"). The Lua makes the sledge's head a physical
     striker: three spheres of 7.5 cm radius, covering the 15 × 35 × 15 cm head, are swept between frames with
     `QueryRaycast`.
     - A hit counts when the head approaches the surface at 2 m/s or more (relative to the player), with a 0.25 s
       cooldown.
     - The strike strength k goes from 0 at 2.5 m/s to 1 at 6.5 m/s (the game's own strike).
     - From 3 m/s it makes a hole with `MakeHole(P, 0.25 + 0.75·k, 0, 0)`, which, like the game's sledge, breaks only
       soft materials (not metal or concrete). It plays the material's sound and spawns debris.
     - An earlier version had the Lua send `TDVRS` above 2.8 m/s downward or forward, so that the DLL pressed "use
       tool" for 0.12 s. The DLL still accepts that message.
  2. The strike impulse goes to the struck body only (at most 200 N·s, scaled by 0.3 + 0.7·k, and never pushing the
     hit point beyond 5 m/s), and a hit counts only on entry: a head that is already inside an object makes no new
     hit.
  3. `[Controles] sledgeTap=1` (default): with the sledge, the trigger presses from 20 % (`sledgeTrigger`, released at
     half of it) for at least 0.15 s (`SledgeUse`). `main.lua` keeps the copy, and the free hand when it is near the
     strike's axis, "ghost" for 0.4 s of game time after each press (`phys.strikeT`).
- **Verification.**
  - The hammer was confirmed in the headset.
  - For (3), on the mock: a 0.3 s / 0.08 s tap broke 90/90 voxels; a 15 % press gave no strike; a held trigger gave
    one strike. The reach is the game's own.
- **Limits.** None known.
- **Status.** (1) and (2) confirmed in the headset; (3) verified on the mock and deployed.
- **Files / settings.** `mod/TeardownVR/main.lua` (sledge striker, `phys.strikeT`), `src/input.cpp`
  (`SledgeUse`, `RequestSwing`), `src/bridge.cpp` (`TDVRS`); `[Controles] swingGesture`, `sledgeTap`,
  `sledgeTrigger`.

### I12. Grabbed objects were pushed away from the player

- **Symptom.** An object grabbed close was pushed away to 1.5 m, and every grabbed object moved 20 cm away from where
  it was taken. With the free hand the minimum was 1 m, and the stick changed tools instead of moving the object.
- **Root cause.** The game's grab rules (static RE, RE §7 ter):
  - the wanted distance = max(1.5, d0 − 0.2), set at `Player::grab` `0xba130` (`+ba1e4`);
  - clamped to [1.5, 3] **every frame** in `Player::update` (`+c2c28`);
  - eased at half the gap per frame (`0xc56b0`).
  - The free hand's Lua far grab copied the same rule.
- **How it was found.** Static RE of the game's grab.
- **Fix.** `[Controles] grabKeepDistance=1` (default), panel "Grabbed object: keeps its distance".
  - **Right hand (the game's grab).** `InitGrabDistance` finds two unique patterns at `+ba1e4` and `+c2c28`, checks the
    constants they read (0.2, 1.5, 1.5, and a maximum between 2 and 10), and redirects only the rip-relative operands
    of three SSE instructions to two floats of the DLL, allocated within 2 GB of the image.
    - In VR those floats are 0 and `grabMinDistance` (0.5 m from the eyes); otherwise the game's 0.2 and 1.5.
    - `GrabControls` then moves the wanted distance (`player+0x7b0`) with the right stick (`grabStickSpeed`,
      1.5 m/s), within [minimum, 3 m].
  - **Free hand (Lua `grabFar`).** Keeps max(0.15 m from the hand, d0). The stick moves it between 0.15 and 3 m, and
    the tool does not change while that hand holds at a distance (Lua flag 32).
- **Verification.** Mock: the left hand held an object at 1.35 m, where it was taken. The right-hand check failed on
  the mock, because the test aim hit the slab or the ground.
- **Limits.** The right-hand behaviour has not been confirmed in a headset.
- **Status.** Deployed. The right hand is not confirmed.
- **Files / settings.** `src/engine.cpp` (`InitGrabDistance`, `GrabControls`), `mod/TeardownVR/main.lua` (`grabFar`);
  `[Controles] grabKeepDistance`, `grabMinDistance`, `grabStickSpeed`.

### I13. No arm for the free hand

- **Symptom.** In first person the player character is not drawn, so the free hand was invisible.
- **Root cause.** The game draws no character body in first person.
- **How it was found.** Not recorded.
- **Fix.** `tdvrOffArmTick` (`offArm=1`):
  - the Lua spawns two fixed bodies with the character's `hand_l` and `arm_lower_l` voxels from
    `player/player_<character>.vox`, placed as in the character prefab;
  - the OpenXR grip pose maps onto the character's `ik_hand_l` frame;
  - the forearm uses two-segment IK toward a guessed elbow.
  - **Late fix.** The Lua places the arm for the camera it requests, but the rendered camera has moved since the
    tick (physics step, height smoothing).
    - In `Renderer::render`, after the shape interpolation wait and `FixObjectMotion`, `OffArmLateFix` shifts the
      arm shapes' `+0x13c` by that difference. The Lua sends `TDVRE x y z` (camera minus hand) every normal frame.
    - The right eye gets the same values.
- **Verification.** On the mock.
- **Limits.** One report of a missing left arm and no walking turned out to be a sleeping left controller (M11).
- **Status.** Deployed.
- **Files / settings.** `mod/TeardownVR/main.lua` (`tdvrOffArmTick`), `src/engine.cpp` (`OffArmLateFix`);
  `[Controles] offArm`. RE §7 bis.

### I14. A physical free hand, and far grab with the game's own rules

- **Symptom.** Testers wanted the free hand to push and grab like the tool, and to grab at a distance like the right
  hand, with the hand icon.
- **Root cause.** The free hand had no physical presence and no far grab of its own.
- **How it was found.** Headset testing.
- **Fix.**
  - **Physical hand** (`tdvrOffHandTick` / `tdvrOffHandStep`, `offHandPhysics=1`):
    - a 10 cm invisible `voxbox` (dirt, density scaled to 6 kg, unbreakable) driven like the tool copy: 2000 N,
      120 N·m, 25 m/s, 40 rad/s. It was 1.5 kg / 600 N / 20 N·m before, which was judged too weak;
    - layer 128, mask 127. It goes "ghost" on the camera axis (12 cm) or the other hand's aim (25 cm) during shots,
      near a carried object, or when the game's pick ray chooses it;
    - the drawn hand is placed at the controller plus the body's error.
  - **Near grab.** The grip takes a dynamic object of at most 300 kg and 32 shapes, not a vehicle and not under the
    feet. Held objects are moved to layer 128, and their original filters are kept in a `tdvr_lm` tag.
  - **Laser** (`tdvrOffLaserTick`, `offLaser`). It stops on the mod's panels: the DLL adds `LAS <set> <dist>` to its reply.
  - **Far grab** (`grabFar`) copies the game's native grab rules, read statically:
    - range 3 m; a dynamic body with 0 < mass ≤ 10 t;
    - target velocity = stick tip velocity + 0.1 × error / dt (capped at 50);
    - impulse clamp(mass, 10, 200) × (1 + |error| / 0.8, at most 2) per step;
    - dropped beyond 3 m from its place;
    - icon `LoadSprite("ui/hud/crosshair-hand.png")` with a dark shadow, kept for frame B.
- **Verification.**
  - Mock: a 750 kg crate held at 1.74 m and lifted 1.16 m; a 3 kg crate held at 1 m; the icon in both eyes.
  - A headset session showed a far grab of a 358 kg object.
- **Limits.** None known.
- **Status.** Deployed; used in the headset.
- **Files / settings.** `mod/TeardownVR/main.lua` (`tdvrOffHandTick`, `tdvrOffHandStep`, `tdvrOffLaserTick`,
  `grabFar`); `[Controles] offHandPhysics`, `offGrab`, `offLaser`. RE §7 bis.

### I15. Dual wield: a two-pistol mod weapon in two hands (Total Overdose)

- **Symptom.** The Total Overdose weapon (workshop 3449723355) draws both pistols as **one** tool. Its shots left from
  an invisible point beside the eye (alternating sides), never from a drawn gun. Testers wanted one gun per hand, each
  fired by its own trigger.
- **Root cause** (read from the mod, which is not modified):
  - one body with two 8-voxel pistols about 44 cm apart;
  - each shot = `GetPlayerEyeTransform()` + rot × (±0.25, 0, −0.5), alternating by a hidden flag.
- **How it was found.** Reading the mod's script.
- **Fix** (`[Controles] dualWield=1`, panel "Dual wield: one weapon in each hand (Total Overdose)"):
  - **Split** (`tdvrDualTick`). The Lua finds the largest left-right gap in the tool's voxels and erases the other
    pistol's voxels with `DrawShapeBox`, which accepts the tool's negative handles. It keeps them to restore later, and
    reads them back to check that the erase took effect.
  - **Copy in the other hand.** A fixed body copies the kept shapes, placed at hand × H⁻¹ × rest pose, with its own
    recoil.
  - **Each shot from its own gun** (`DualEye`, `GetPlayerEyeTransform` hooked at `+0x4791b0`). For the shot caused by
    the VR mod's simulated click, the first eye calls of the weapon's script in the first normal update after the click return an
    "eye" chosen so that the mod's formula puts the bullet at that gun's muzzle, aimed at that hand's laser target.
  - **Clicks serialised** (`DualClicks`). Each trigger press = one left click, one after the other, each waiting until
    the game has seen it. About one shot every two stereo pairs.
  - **Controls.** R2 = right gun, L2 = left gun, left grip = crouch. The right grip triggered the mod's slow-motion
    dodge, so the grab action is off while dual wielding.
  - **Quicksave.** The global state table `tdvrDual` carries `nonserializable = true`: the game's quicksave serializer
    (`+0x636cd0`) skips such tables, so F9 does not restore stale handles.
- **Verification.** Mock: 10 alternating shots, each from its own muzzle. Headset: 88 shots, each from its own gun.
- **Limits.**
  - One shot per game update.
  - The shot's light stays at the given "eye", about 0.5 m behind the muzzle.
  - The mod's dodge, dash and parachute are unchanged.
  - On the mock, Total Overdose fires only its first shot per level, in every mode: a mock artefact.
- **Status.** Confirmed in the headset.
- **Files / settings.** `mod/TeardownVR/main.lua` (`tdvrDualTick`), `src/engine.cpp` (`DualEye`), `src/input.cpp`
  (`DualClicks`); `[Controles] dualWield`. RE §6 octies.

---

## J. Other mods' cameras and 2D interfaces

### J1. Other mods aimed where the head looked

- **Symptom.** Mods that aim with `GetCameraTransform()` (an air strike designator, for example) aimed at the gaze,
  not along the controller.
- **Root cause.** The binding (`0x475f80`) returns the rendered camera, which follows the head in VR and alternates
  between the eyes.
- **How it was found.** Not recorded.
- **Fix.** `[Controles] modAimHand=1`. The DLL hooks the binding and returns `GetPlayerCameraTransform` (`0x478c10`,
  `player+0x640`) instead. That axis passes through the laser target, which the Lua turns the player toward.
  - Only on foot, with no mod camera (mode 8) and the map closed, and only for calls without arguments.
  - The VR mod itself calls `GetCameraTransform(1)` to get the real camera.
- **Verification.** Mock: 1 mm from the laser target.
- **Limits.** Only `GetCameraTransform()` calls without arguments are redirected. Mods that aim from another source
  (for example `GetPlayerEyeTransform`, see I15) are not.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp`; `[Controles] modAimHand`.

### J2. Mod cameras (the AC130 plane view) looked wrong

- **Symptom.** In the AC130 view, the player looked at the horizon while the target was 50 to 80 degrees below, with
  no head position.
- **Root cause.**
  - `SetCameraTransform(t, fov)` (binding `0x48e9d0`) writes the pose at `+0x264` / `+0x270`, the field of view at
    `+0x2c4` and the flag at `+0x2cc`.
  - The camera update (`0x36d00`) then switches to mode 8 and copies the script's pose. The DLL had kept the mod's
    camera position with the room's head rotation.
- **How it was found.** RE of the binding and of the camera update.
- **Fix** (`ScriptView`, `modCameraVr`):
  - the mod's camera, without roll, becomes the origin of a "room": the head's initial direction is the mod's view,
    and the player can look around and lean;
  - normal eye separation (`modCameraDepth` > 1 gives a model-like relief);
  - if the previous camera is within 1.5 m of the player's (Lua bit 16), it is a first-person mod camera, and the
    normal head view is kept.
  - The HUD is shifted per eye so that 2D elements converge (J4).
- **Verification.** On the mock; used in the headset.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp` (`ScriptView`); `[Affichage] modCameraVr`, `modCameraDepth`. RE §6 septies.

### J3. The mod camera's zoom distorted like a fish-eye

- **Symptom.** The AC130 view's zoom (a narrower field of view) was lost at first, because the headset projection
  replaces the mod's. Rebuilt as a magnifier, it then distorted the view: the curvature changed toward the edges.
- **Root cause.** A narrow rectilinear image spread over the headset's field of about 100 degrees stays fixed in the
  room only at its centre.
  - A point at angle a follows the head by sin²(a)(1 − 1/M²) of its rotation.
  - The runtime's reprojection is wrong by as much.
  - The centre grows faster than the edges.
  - The relief is flattened M times.
- **How it was found.** Headset feedback on the magnifier, then geometry.
- **Fix.**
  - First version, the magnifier (`ModZoom`, `modZoomMode=1`):
    - M = tan(30°) / tan(fov/2), clamped to [1, 16], read from the mod's projection before it is replaced;
    - applied as m0, m5 × M;
    - smoothed (τ 0.12 s, in log space);
    - the head turns the camera by 1/M only.
  - Now the dolly zoom (`DollyTravel`, `modZoomMode=0`, default):
    - the camera moves along the mod's axis by (1 − 1/M)·D toward the aimed point, so that point looks M times bigger
      with true perspective, while the projection, head rotation and eye separation stay the headset's;
    - D comes from a Lua raycast (`TDVRZ <m>`), which ignores small and fast objects and the plane's own body;
    - the DLL tracks D quickly (0.1 s), steps at once for nearer surfaces, and moves toward farther ones at most 20 %
      of D per second;
    - with no known surface (sky), or under 6 to 8 m, the magnifier is used.
    - Example: AC130 at × 13.2, D ≈ 212 m, the camera advances about 196 m.
  - The right stick sends one mouse-wheel notch, then one every 0.12 s after 0.3 s.
- **Verification.** Mock: zoom × 1 to × 2.6 (magnifier). The dolly was built after headset feedback on the
  magnifier.
- **Limits.** Over the sky or at short range, the magnifier and its distortion come back.
- **Status.** Deployed.
- **Files / settings.** `src/engine.cpp` (`ModZoom`, `DollyTravel`), `src/input.cpp`, `mod/TeardownVR/main.lua`
  (`TDVRZ`); `[Affichage] modCameraZoom`, `modZoomMode`.

### J4. The 2D HUD was seen double

- **Symptom.** Crosshairs and HUD text at the canvas centre were doubled.
- **Root cause.** With off-centre projections (C2), each eye's image centre points about 15 degrees outward and
  about 13 degrees down, on opposite sides.
- **How it was found.** Follows from C2.
- **Fix.** `[Affichage] hudFix=1`. The DLL adds a per-eye translation to the UI camera's view (`game+0x2c8`, view at
  +0x308, canvas → pixels), so the canvas centre is seen in the intended direction: straight ahead, or along a mod
  camera's axis. The distance is set by `hudDistance` (0 = infinity). `UiWorldToPixel` stays exact (same inverted
  matrix).
- **Verification.** Not recorded.
- **Limits.** The canvas keeps its size, so once shifted it leaves the image: about 10 % at the top in both eyes and
  about 12 % on the nose side. A full-screen rectangle (fade, damage flash) leaves a band of about 12 % on the outer
  side and 10 % at the bottom.
- **Status.** In place.
- **Files / settings.** `src/engine.cpp`; `[Affichage] hudFix`, `hudDistance`.

### J5. Mods' crosshairs stayed at the screen centre while the gun aims at the laser

- **Symptom.** A mod crosshair drawn at `UiCenter()` / `UiMiddle()` (a portal gun logo, a chaingun sight) stayed in
  front of the head, not on the gun's target.
- **Root cause.** In VR the gun aims along the controller (J1, H8), not along the screen centre.
- **How it was found.** Not recorded.
- **Fix.** `[Affichage] modCrosshairAim=1`.
  - Every UI draw call builds its quad matrix and calls the "is this rectangle on screen" test (`0x4d1f90`).
  - The DLL shifts the matrix in that test for small quads (≤ 360 canvas units) whose centre is within 160 units of
    the canvas centre. They move to the laser target (`TDVRT x y z`), projected per eye.
  - Excluded: inside a `UiWorldToPixel` scope, during interactive windows, mod cameras, the map, vehicles, and head
    aiming.
  - The few callers that compute their constants before the test are recognised by return address and never shifted.
- **Verification.** Mock: the portal crosshair converges at the laser target.
- **Limits.** Large crosshairs, or crosshairs drawn far from the canvas centre, are not moved.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp`, `mod/TeardownVR/main.lua` (`TDVRT`); `[Affichage] modCrosshairAim`.

### J6. Mods' mouse windows (UiMakeInteractive) were unusable

- **Symptom.**
  - Mod windows driven by the mouse could not be used.
  - With the revolver's ammo menu, the ring was mirrored, at a different place in each eye, with negative-size
    chambers. A dragged bullet was drawn twice.
- **Root cause.**
  - There is no mouse in VR.
  - The same window pixel maps to a different canvas point in each eye: the per-eye UI shift, m8 = ∓0.242, about 465
    canvas units, 25 degrees.
  - The revolver places its ring with `UiWorldToPixel` on the **unscaled** tool's bone, while the DLL draws the tool
    scaled about its muzzle (I5). The point was 0.76 m off (measured 0.761 m live), behind the eyes.
- **How it was found.** Live measurement of the projected points.
- **Fix** (`ModUiPointer`, `Hook_MousePixel` on `MousePixel` `0x4f27a0`, `WorldPixelView`):
  - during an interactive window, the canvas converges at `modUiDistance` (1 m);
  - the aiming controller's ray is intersected with that plane in the left eye's frame, and `MousePixel` returns, in
    each eye, the pixel of that canvas point;
  - the cursor is drawn by the DLL, per eye;
  - trigger = left button, grip = right button;
  - points of the unscaled tool box are projected where they are drawn.
  - The window handling is limited to stereo gameplay. Without that limit, the game's own menus broke.
- **Verification.** On the mock and in the headset.
- **Limits.** See J7 and J8 for the extensions.
- **Status.** In place; extended by J7 and J8.
- **Files / settings.** `src/engine.cpp` (`ModUiPointer`, `Hook_MousePixel`, `WorldPixelView`), `src/input.cpp`;
  `[Affichage] modUiPointer`, `modUiDistance`.

### J7. The revolver's ammo ring moved in every direction, then followed the head

- **Symptom.** The ring followed the hand and wrist (turning, squashing, flipping). Later, once centred in the view,
  it stayed in front of the face even when looking down at the gun.
- **Root cause.**
  - Its markers are the gun's bones, read at `draw()` time where the gun is drawn, that is in the moving hand.
  - The first fix (below) centred the ring on a head-locked canvas.
- **How it was found.** Headset testing.
- **Fix.**
  - **First fix** (`ToolRing`, `modUiCenterTool=1`). During the tool script's interactive window, held-tool markers
    are projected as on the desktop screen: desktop camera = tool body × inverse(screen pose), with that camera
    written into `game+0x240` for the duration of the call. The same canvas point appears in both eyes, and the ring's
    centroid is anchored at the canvas centre.
    - The game's barrel-distortion flag (`game+0x388`, `options.gfx.barrel`) is cleared during each "flat" call;
      otherwise each eye would bend the point differently.
  - **Second fix** (`modUiToolFixed=1`, `PanelToolPlace`). The ring gets its own panel, fixed in the room:
    - opened at the gaze heading (`HeadingOf`, valid with the head pitched down), at eye height, at `modUiDistance`;
    - the scale keeps the validated size;
    - it never turns with the head, follows translations (it glides after 0.25 m of movement), and is never grabbed.
- **Verification.** On the mock:
  - first fix: centroid at 960,944 = the canvas centre, in both eyes, unchanged when the hand moves;
  - second fix: placement correct at pitch −40°, at 0° and at yaw 30°.
- **Limits.** The first pair of a window is still drawn in the hand: the revolver projects its markers before calling
  `UiMakeInteractive`. Anything a mod draws outside the canvas shows on the borderless panel.
- **Status.** First fix deployed; second fix deployed, no headset report recorded.
- **Files / settings.** `src/engine.cpp` (`ToolRing`, `PanelToolPlace`, `HeadingOf`); `[Affichage] modUiCenterTool`,
  `modUiToolFixed`, `modUiDistance`.

### J8. Mod menus off to the side, clipped, and following the head (floating panels)

- **Symptom.** Natural Phenomena's weather menu (workshop 3261134951) appeared about 45 degrees to the left, was
  clipped by the right eye's image, and followed the head.
- **Root cause.** The UI canvas is head-locked and spans the per-eye images.
- **How it was found.** RE of the 2D renderer:
  - every `Ui*` draw builds its MVP on the CPU at call time from `R+8` (projection) × `R+0x48` (view) × M, with
    `R = *(game+0x30)`, and copies it as is into its constant buffer;
  - the UI shaders use the full 4×4 matrix (w kept), so a perspective matrix draws correct quads.
- **Fix.** `[Affichage] modUiPanel=1` (`ModPanel`).
  - For the duration of ONE script's draws, `R+8` is replaced by `Q[eye] × inverse(R+0x48)`, which puts that window
    on a 3D panel with no extra pass. Layout, mouse and `UiWorldToPixel` stay flat and exact.
  - Panel: 0.8 mm per canvas unit × `modUiPanelScale`, opened straight ahead of the gaze, fixed in the room. It
    follows when the gaze heading stays more than 10 degrees away for 0.5 s.
  - It can be moved with a grip, and its place is saved per window signature (`modUiPanelPlaces`, at most 16, written
    off the game thread).
  - Clip rectangles (`0x4cf4e0`) are re-projected; the background blur (`ui+0x3d0`) is turned off inside the scope.
  - The right button is detected by watching the window's own `InputPressed` / `Down` / `Released` reads.
  - A guard compares `R+8` with Q at every quad test. If another pass overwrote it, the camera is restored and the
    window falls back for that session; after 3 failures, panels are off until restart.
  - The first pair of a session is drawn nowhere (the anchor is known only after measuring the content box). Without
    that, the window would flash 0.8 m to the left.
  - Panels are also allowed in mod-camera views.
- **Verification.**
  - Mock: anchored, fixed in the room at a 15-degree head turn, moved by the left grip, place saved.
  - Headset: the weather panel was grabbed and moved several times.
- **Limits.**
  - No depth: the panel is drawn over the scene.
  - One window at a time: two interactive windows shown together fall back to the converged canvas.
  - A marker computed before `UiMakeInteractive` keeps each eye's projection.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp` (`ModPanel`), `src/vr.cpp`; `[Affichage] modUiPanel`, `modUiPanelScale`,
  `modUiPanelPlaces`.

---

## K. Menus, pause screen and the mod's own panels

### K1. The in-level pause screen was square instead of 16:9

- **Symptom.** Pausing in a level showed a nearly square 2D screen, not a full widescreen one.
- **Root cause.**
  - The game's UI canvas follows the window's shape: `Game::frame` builds the UI camera every frame (`+71e94`) with a
    view scale of W/1920. In VR the window is the headset render size (3024 × 3264), so the canvas is 1920 × 2072.
  - The main menu draws inside `UiSafeMargins` (1920 × 1080) and looks 16:9. The pause menu (`hud.lua`) uses the
    whole canvas.
  - During a single-player pause no script ticks (`Game::update`, `+7acec`). The Lua sends no eye request, so the
    frame goes to the virtual screen, whose quad copied the window's shape (3.00 × 3.24 m).
- **How it was found.** RE, plus the log ("Stereo frames: left=0 right=0 other=600" while paused).
- **Fix.** `[Ecran] pauseWide=1`, panel "16:9 pause menu (like on the PC screen)".
  - `WideDecide` (after frame A's update, when paused, in level state 6, and with no eye request) sets the UI view to
    (W/1920, H/1080): a 1920 × 1080 canvas stretched over the window.
  - `Hook_CamSet` gives the 3D view m0 = m5 × 9/16.
  - `vr.cpp` blits the back buffer into the top 16:9 band of the screen image and shows a 3.00 × 1.69 m quad.
  - The TAA history is reset at each switch.
  - The mouse mapping stays exact.
  - Multiplayer (where scripts keep ticking), cinema mode and alternating eyes are unchanged.
- **Verification.** Mock: the layout is correct, and the Natural Phenomena button click works.
- **Limits.** None known.
- **Status.** Verified on the mock; deployed.
- **Files / settings.** `src/engine.cpp` (`WideDecide`, `Hook_CamSet`), `src/vr.cpp`; `[Ecran] pauseWide`.

### K2. The mod's own panels followed the head, and saving their place froze the game

- **Symptom.**
  - The VR inventory followed the head's heading within 0.4 s.
  - The settings panels could not be moved.
  - Saving the settings took 55 to 270 ms on the `Present` thread.
  - The settings panel's Display tab overflowed under the "Close" button.
- **Root cause.** The panels were placed relative to the head's heading instead of being fixed in the room, and every
  save rewrote the whole ini file on the calling thread (`ConfigSave`).
- **How it was found.** Headset use, and timing of the save.
- **Fix** ("floating panels" in `src/vr.cpp`: `UpdatePanels`, `UpdatePanelGrips`):
  - the VR inventory and the settings/keys panel are OpenXR quad layers in room space, composited by the runtime with
    the latest head pose, the same for both eyes;
  - the inventory is anchored to an estimated body (a neck pivot with hysteresis). It turns only on a real turn
    (`inventoryFollow`: 55 degrees for 1.2 s), and is recalled by R3 when far away;
  - the settings panel opens straight ahead of the gaze and stays fixed;
  - a grip on a panel moves it. That grip and its trigger are kept away from the game until released;
  - only the changed keys are saved, on a worker thread, at most once per second (`ConfigSaveKeys`). A full
    `ConfigSave` cancels pending batches;
  - tab contents scroll above the bottom bar.
- **Verification.** Mock: the panel stays fixed at ±40-degree head turns, and is grabbed and saved in 4 ms off the
  main thread.
- **Limits.** None known.
- **Status.** Deployed.
- **Files / settings.** `src/vr.cpp` (`UpdatePanels`, `UpdatePanelGrips`), `src/ui.cpp`, `src/config.cpp`
  (`ConfigSaveKeys`); `[Controles] inventoryFollow`, `inventoryFollowAngle`, `inventoryFollowDelay`,
  `inventoryDown`, `inventoryForward`, `inventoryYaw`; `[Interface] uiPanelDistance`, `uiPanelDown`.

### K3. Choosing tools in VR: the inventory

- **Symptom.** The game's tool selection is a desktop HUD.
- **Root cause.** The game has no VR tool selection.
- **How it was found.** Not applicable: a missing feature.
- **Fix.** `src/inventory.cpp`: a game-styled VR inventory, shown with a short right stick click (R3), with columns
  per tool group and drag scrolling.
  - The tool list comes from the Lua in chunks of 30 characters or fewer (A4).
  - `inventoryOnSwitch` (default off) also flashes it when the stick changes tool.
  - On the inventory, the pointer hand's trigger is captured (no shot, no E).
- **Verification.** Mock: pick and R3.
- **Limits.** None known.
- **Status.** In place.
- **Files / settings.** `src/inventory.cpp`, `mod/TeardownVR/main.lua` (`TDVRI` messages); `[Controles]
  inventoryOnSwitch`.

---

### K4. The campaign's mission computer: clicking a mission did nothing, even with the real mouse

- **Symptom.** At the hub's mission computer, a place could be opened but a mission could not be selected: its
  PLAY panel never opened, with the controllers or with the real desktop mouse.
- **Root cause.**
  - The game's in-world screens draw their script in `Screen::render` (`0x467dd0`), from `Game::render`, so in both
    frames of a stereo pair. Their input is copied from the player's only in `Screen::update` (`0x4695d0`), in
  `Game::update`.
  - With `[Performance] frameBMode=2`, frame B runs no `Game::update` (G4) but still renders. The screen's script
    (`data/ui/terminal.lua`, which reads the clicks in `draw()`) saw frame A's "left button pressed" again.
  - A mission row is a toggle (`terminal.lua`: selected if not, else unselected): selected in frame A, unselected in
    frame B. A place's dot is not a toggle, which is why places opened.
- **How it was found.** Reverse engineering of the screen loop (`0xeb160`) and of the input binding, and the
  player's log (Interact while "frame B mode 2: 300 render-only of 300").
- **Fix.** `Hook_ScreenRender`: in a render-only frame B, the screen the local player uses (`player+0x9a0`) is not
  drawn again. Its texture keeps frame A's image, cursor included, the same in both eyes. Every other screen is
  drawn as before, so their sound loops are still renewed.
- **Verification.** Confirmed in the headset: mission rows select and the PLAY panel opens, with the controllers and
  with the real mouse.
- **Limits.** A mod's own interactive screen whose `draw()` plays a sound loop gets that loop renewed every other
  engine frame while the player uses it. The game's computer plays none. A mod's `draw()` that toggles on a click
  outside such screens is not covered.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp` (`Hook_ScreenRender`, `InitScreens`); `[Controles] screenOncePerPair`.

### K5. The campaign's mission computer: the controllers could not move its cursor

- **Symptom.** Seated at the mission computer (Y), only the real mouse moved its cursor.
- **Root cause.** `Screen::update` moves the cursor by the player's RAW mouse motion only (input `+0x3fc/+0x400`),
  clamped to the resolution, and gives the result as an input event of type 4 (`Input::handleEvent`
  `0x4f3e30`, called at `0x469a48`). The controllers make no raw mouse motion. The mod's other pointers
  (menus, mods' windows) work differently.
- **How it was found.** Reverse engineering of `Screen::update` and of the input class.
- **Fix.**
  - `ScreenPointerUpdate`, in each frame A (from `Hook_CamSet`): the local player's screen (`player+0x9a0`, the
    screen pointing back at the player at `+0x2258`, enabled and interactive), its world transform (the one the
    renderer draws it with, `0x4682c0`), size and resolution.
  - Each hand's aim ray goes from the recentred room to the world as `main.lua`'s `toWorld` does, from the left
    eye's room pose and its drawn camera. It is intersected with the screen's mesh (Moller-Trumbore): the hit's UV
    gives the pixel, `u x width`, `(1 - v) x height`, as in the engine's touch hit test (`0x467900`). Beside the
    mesh, or if the mesh does not pass its checks, the flat rectangle is used (within 2% of its edges).
  - `Hook_ScreenCursor` (on `Input::handleEvent`, only for the call returning to `0x469a4d`) gives that pixel to
    the cursor event.
  - The pointing hand: the dominant one when the player sits down; the other hand takes it by pulling its trigger
    with its ray on the screen, and that pull is already a click where it points. A pull blocked by the pointing
    hand's held trigger waits until that trigger is released.
  - `input.cpp`: the pointing hand's trigger is the left mouse button; the other trigger does nothing (the left
    trigger no longer crouches there); a pull begun with the ray beside the screen does not click, also when the
    ray reaches the screen, until released; when the pointer changes hands with the button down, the button is
    released and pressed again for the new hand. No sledge tap, wrist swing or dual wield there.
  - The real mouse still works: its motion (2 counts or more) takes the cursor; the laser takes it back with a
    click on the screen, or by moving 24 pixels within 0.25 s once the mouse has rested 0.75 s.
- **Verification.** Confirmed in the headset on 2026-10-15: the cursor followed the laser, five controller clicks
  counted, then the real mouse took the cursor.
- **Limits.** One frame of latency, like the rest of the cursor. A screen on a moving body is hit where it is drawn.
- **Status.** Confirmed in the headset.
- **Files / settings.** `src/engine.cpp` (`ScreenPointerUpdate`, `ScreenRayPixel`, `ScreenMeshHit`,
  `Hook_ScreenCursor`, `InitScreens`), `src/input.cpp`; `[Controles] screenPointer`.

### K6. Seated at a game computer, the view glided away from the hands

- **Symptom.** After Y at the mission computer, the view moved in front of the screen while the hands and the lasers
  stayed behind, and the view shook with the wrists (C9).
- **Root cause.** While the local player uses a screen without the `nocam` tag, `Game::frame` raises a glide value
  (`game+0x358`, 1.2 per second) and the camera update moves the camera toward a point in front of the screen
  (0.43 m away for the hub's computer) and turns it to the screen. The Lua mod's room stays at the player.
- **How it was found.** Reverse engineering of `Game::frame` (`0x72eb7`-`0x72f59`) and of the camera update.
- **Fix.**
  - `main.lua` (`tdvrScreenTick`): while `GetPlayerScreen()` is not 0, the screen gets the tags `nocam` and
    `tdvrnocam` (the second marks it as ours); they are removed when the player leaves, when VR stops, and at the
    next script start for any screen still marked (a quick save keeps tags).
  - `nocam` only freezes the glide where it is (`0x72f06` skips both its rise and its fall), so a glide begun before
    the tag would stay. In VR, `ScreenPointerUpdate` also writes 0 to `game+0x358` while the player uses a screen
    (offset read from `0x72f1a`).
- **Verification.** Confirmed in the headset: the view stays at the head, with the hands and lasers.
- **Limits.** The view is not moved closer to the screen: lean in to read small text.
- **Status.** Confirmed in the headset.
- **Files / settings.** `mod/TeardownVR/main.lua` (`tdvrScreenTick`), `src/engine.cpp` (`ScreenPointerUpdate`).

## L. Headset runtime, installer, logs and language

### L1. The wrong OpenXR runtime was used (Virtual Desktop or Quest Link)

- **Symptom.** A Quest Link session found no headset ("headset 0 fps"), because the game connected to Virtual
  Desktop's runtime.
- **Root cause.**
  - With no choice set, the OpenXR loader uses Windows' active runtime (HKLM `...\OpenXR\1` `ActiveRuntime`).
  - Virtual Desktop's streamer makes itself the active runtime whenever it runs.
  - The first fix was a manual choice (`xrRuntime`), which meant switching back and forth.
- **How it was found.** Session logs, the bundled loader's source (OpenXR SDK 1.1.49), and read-only RE of the
  installed runtimes.
- **Fix.** `[Affichage] headsetRuntime=0` (automatic, the default; panel "Headset used").
  - At start (`RuntimeStartup` in `src/vr.cpp`), each candidate runtime is asked whether it has a headset: its
    `.json` is put in `XR_RUNTIME_JSON`, then `xrCreateInstance` + `xrGetSystem` + `xrDestroyInstance`.
    - The loader re-reads the variable at each create and unloads the runtime at destroy.
    - Only one instance can exist at a time.
    - `xrEnumerateInstanceExtensionProperties` loads a runtime without unloading it, so it is never called between
      probes.
  - Order: Windows' active runtime, then Virtual Desktop, then Meta Quest Link. SteamVR only if it is Windows'
    runtime, and last, because asking it starts SteamVR.
  - Meta's runtime is asked only while the named event `OculusHMDConnected` is signalled. Without it, Meta's
    `xrGetSystem` can only answer `XR_ERROR_FORM_FACTOR_UNAVAILABLE`, so nothing is lost, and Meta's DLL is never
    loaded when no Link headset is there. Virtual Desktop also uses that name (Oculus emulation), so it is asked
    first.
  - With no headset, `RuntimeRedetect` asks again when the signal stays up for two checks in a row (6 s), at most
    every 15 s.
  - Values 1, 2 and 3 force Virtual Desktop, Link and SteamVR; 4 = Windows' runtime without detection.
  - The old `xrRuntime` key is removed at load and **not** carried over, so every player gets "automatic".
- **Verification.** Mock with test runtime lists (`[Debug] xrAutoRuntimes`, with a fake "no headset" runtime).
- **Limits.**
  - An elevated game process ignores `XR_RUNTIME_JSON`.
  - The render size is still decided at start (D2).
- **Status.** Verified on the mock; deployed.
- **Files / settings.** `src/vr.cpp` (`RuntimeStartup`, `RuntimeRedetect`), `src/config.cpp`
  (`ConfigFindXrRuntime`, `ConfigActiveXrRuntime`), `tools/mock_openxr/tdvr_mock_openxr_nohmd.json`;
  `[Affichage] headsetRuntime`, `[Debug] xrAutoRuntimes`, `openxrRuntime`. RE §2 bis.

### L2. Installing the mod for players

- **Symptom.** Players needed a safe install that would not break another mod's `dsound.dll`, would survive game
  updates, and would keep their settings.
- **Root cause.** A manual copy into the game folder can overwrite another mod's `dsound.dll` and the player's
  settings.
- **How it was found.** Not applicable: a release requirement.
- **Fix.**
  - A release script builds the DLL, generates `installer/gen/payload.h` and the `.rc` file, and builds `installer/`
    (CMake, static CRT, Windows task dialog).
  - One `.exe` that:
    - asks for the language first;
    - finds Teardown through Steam (registry `SteamPath`, then every library in `libraryfolders.vdf`), or asks for the
      folder;
    - refuses to run while the game runs;
    - keeps another mod's `dsound.dll` (one without "TDVR1") as `dsound.dll.original`;
    - writes each file under a temporary name, then moves it into place;
    - writes only `[Affichage] language` into `TeardownVR.ini`;
    - offers to run again as administrator if Windows refuses writing in the game folder.
  - Uninstall removes the mod's files and puts the other mod's DLL back. The settings are kept.
  - Command line for tests: `/install`, `/uninstall`, `/logs` with `/out`, `/gamedir`, `/lang en|fr`, `/silent`.
    Exit codes: 0 done, 2 game running, 3 no game folder, 5 access denied.
  - The player package holds the installer, a readme and the third-party licences (MinHook, Dear ImGui, the OpenXR
    loader under Apache 2.0, JsonCpp).
- **Verification.** On a fake game folder: install, update, uninstall, another mod's DLL kept, game running → 2, bad
  folder → 3. An antivirus scan of the installer found nothing.
- **Limits.** Gotcha for testers: an installer run without `/gamedir` finds the **real** game and installs there.
  Test runs must pass `/gamedir`. Nested PowerShell quoting dropped it once, so test runs are best started with .NET
  `ProcessStartInfo`.
- **Status.** In place.
- **Files / settings.** `installer/` (`installer.cpp`, `logs.cpp`, `CMakeLists.txt`), `tools/install.ps1`,
  `tools/install.bat`, `tools/uninstall.bat`.

### L3. Getting usable logs from players

- **Symptom.** Each new session overwrote `TeardownVR.log`, and players could not easily send logs.
- **Root cause.** A single log file, and no tool to collect it.
- **How it was found.** Not applicable: a tooling need (reading what happened in a session from its log).
- **Fix.**
  - `ArchivePreviousLog` (`src/log.cpp`), run in `DllMain` with plain file calls, **moves** the previous
    `TeardownVR.log` into `<game>\TeardownVR_journaux\TeardownVR_<last write yyyy-MM-dd_HH-mm-ss>[_N].log` and keeps
    the newest 10. Empty logs are skipped. The current session stays at `<game>\TeardownVR.log`.
  - The installer's "Prepare my logs" button (`installer/logs.cpp`) writes a store-only zip
    `TeardownVR_journaux_<date>.zip` on the Desktop, then selects it in Explorer. It contains:
    - `TeardownVR.log` and the archived logs;
    - `TeardownVR.ini`;
    - `infos_PC.txt`: Windows version (Windows 11 detected by build ≥ 22000), CPU, RAM, GPUs and driver, OpenXR
      runtime, file sizes and dates.
  - The game may be running: the files are only read.
  - Uninstall also removes the archive folder.
- **Verification.** A test harness for the archive (10 kept, collision suffix, empty log skipped). The zip CRCs were
  checked with Python's `testzip`.
- **Limits.** The zip describes the player's PC (hardware, Windows version, runtime). Players decide whether to share
  it.
- **Status.** In place.
- **Files / settings.** `src/log.cpp` (`ArchivePreviousLog`), `installer/logs.cpp`.

### L4. New players should start with the tested settings, and updates must not reset them

- **Symptom.** A new player would otherwise start with untested settings, and an update could overwrite a player's
  own settings.
- **Root cause.** Defaults and the ini file had to be managed explicitly.
- **How it was found.** Not applicable: a release requirement.
- **Fix.**
  - The defaults in `src/config.h` are the configuration validated in the headset:
    - `renderPreset=6`, `vrAspect=0.925`, `refreshRate=0`;
    - `taaMode=4`, `sharpen=1.0`;
    - `frameBMode=2`, `frameLatency=2`, `lazyBeginFrame=1`, `holdSmooth=1`;
    - the inventory place;
    - two default floating-window places.
  - The installer never overwrites `TeardownVR.ini` (only its language key). "Reset all settings" restores the
    defaults and keeps the language.
- **Verification.** Not recorded.
- **Limits.** In a build without the game-derived shaders, the default `taaMode=4` keeps the game's anti-aliasing.
- **Status.** In place.
- **Files / settings.** `src/config.h`, `src/config.cpp` (`ConfigReset`), `installer/installer.cpp`.

### L5. Localisation: English and French

- **Symptom.** The mod was written in French only.
- **Root cause.** The texts were written directly in French in the code, with no translation layer.
- **How it was found.** Not applicable.
- **Fix.**
  - `src/i18n.h`: `Tr("English", "Français")` picks the text of the language in use at each call (string literals,
    nothing allocated), so a language switch shows on the next frame. `LangFr()` tells which language is active.
  - 316 `Tr` pairs in `src/ui.cpp` at publication. `RenderPreset` has `nameEn` / `nameFr`. The Lua messages and `info.txt` are
    bilingual.
  - `[Affichage] language` defaults to `"en"`. An ini from before this key belongs to a French-speaking player, so it
    gets `"fr"`, which is written back.
  - The panel has a "Language / Langue" combo.
  - The virtual keyboard is QWERTY in English and AZERTY in French.
  - The installer asks for the language first: English is pre-selected, or French when the ini says so.
  - Gotcha: ImGui labels that change with the language and must keep their state (tabs, windows) end with a fixed
    `###id`.
- **Verification.** An automated UI test rendered the English panel: the texts fit, the combo is at the top of the
  Display tab, and the settings file was restored byte-identical. The live language switch and the QWERTY keyboard
  were not exercised.
- **Limits.** Only English and French.
- **Status.** Partly verified.
- **Files / settings.** `src/i18n.h`, `src/ui.cpp`, `src/config.cpp`, `mod/TeardownVR/info.txt`;
  `[Affichage] language`.

---

## M. Development process: lessons

These are not problems a player sees. They are what the development of this mod taught about working on a closed
game, and they are written as general engineering advice.

### M1. The mock OpenXR runtime as the test bench, and its artefacts

- **Why.** Most fixes were built and checked without a headset. `tools/mock_openxr` runs the mod's real OpenXR path:
  - controllers driven by `controls.txt`;
  - head controls (`headyaw`, `headpitch`, `headstill`, tremor `hjit`);
  - hand offsets, simulated runtime holds (`vdhold`, `vdholdevery`), "no headset" (`nohmd`);
  - on-demand BMP dumps of both eyes.
  - Its runtime manifest must reference the DLL with a relative path of the form `./<name>.dll`.
- **Artefacts found, and fixed or worked around:**
  - Periodic BMP dumps (`autodump`) blocked `xrEndFrame` for 55 to 76 ms, which looked like hitches. They are now off
    by default; dumps are on demand.
  - `xrWaitFrame` spun for its last 2 ms, inflating the main thread's CPU (up to about 18 % of a core at 90 Hz). It now
    waits on a high-resolution waitable timer.
  - The mock paces frames itself and has its own hitches: **never judge stutter on the mock.**
  - The mock's field of view gave a disparity at infinity (516.6 px) close to the blue-noise period (512).
    Screen-locked noise then correlated at exactly −512 px, a false positive. Use the real headset's field of view
    and eye offsets in the mock before stereo-correlation tests.
  - The mock does not composite layers the way Virtual Desktop does. D6's stretch could only be seen in the headset.
  - The mock's field-of-view aspect (1.155) overwrote the saved `vrAspect`. Back up the settings and restore them
    around mock runs (M2).

### M2. Tests share the player's real files: snapshot and restore them

- **Incidents.**
  - Blind re-clicks in a menu-automation script emptied the active mod list
    (`%LOCALAPPDATA%\Teardown\modlists\1.xml`).
  - Closing the game too early, or restarting a test twice in quick succession, dropped the Workshop mods from the
    list.
  - Stopping a background test runner mid-run skipped its cleanup: the game was closed early, and the mod list dropped
    to one mod.
  - A mock run overwrote the previous session's `TeardownVR.log`.
  - A mock session changed settings in the live `TeardownVR.ini`.
- **Lessons.**
  - Snapshot `modlists\1.xml`, `TeardownVR.ini` and the log before any restart, and check them after.
  - Never stop a runner that has already started the game: let it finish, or clean up by hand.
  - Back up the **live** ini, not an old copy.
  - Restore the ini before a script that relaunches the game.
  - Keep a copy of each session log, from the headset and from the mock, outside the game folder.
  - For players, the log archive (L3) came from the same need.

### M3. Never relaunch the game blindly

- The render size is chosen at start (D2). A development relaunch while the headset was off produced a
  low-resolution session.
- The relaunch script now probes the runtime with `tdvr_xr_probe.exe` (game closed) and launches only on
  `xrGetSystem -> 0`.
- Never test the automatic runtime detection without `[Debug] xrAutoRuntimes`: it would probe the real Virtual
  Desktop and Meta runtimes.

### M4. Do not disturb the person using the PC

- Test scripts must not take the focus: the PC may be in use for something else at the same time.
- Stress tests abort if the PC was used within the last 45 s.
- Drive the game through the mock's `controls.txt` rather than through desktop automation.

### M5. Tooling gotchas

- PowerShell can mangle UTF-8 JSON. Extract patches with Python.
- Bash heredocs mangle backslashes. Write files with an editor or a Python patch script.
- Copying `dsound.dll` fails while the game is exiting: retry.
- The game shows a start-up warning whose first click is swallowed. Menu clicks are flaky: click Play, then Sandbox
  1.5 s later.
- Paths with spaces must be quoted in helper invocations.

### M6. Patch discipline

- Each change is delivered as an anchored patch script:
  - every anchor is checked before anything is written;
  - a second run is refused;
  - CRLF line endings are kept.
- Each patch is dry-run on a copy and syntax-checked (`cl /Zs /std:c++20 /W4`, plus `luacheck` for Lua).
- Independent reviews re-check the RE and the code before integration.
- Back up the sources before each integration round.
- Give reviewers **copies**, or scripts that take the source root as a parameter: once, a reviewer's patch script with
  a hard-coded working directory was applied to the real `main.lua`.

### M7. Measure what the player sees

- A check that read `GetPlayerVelocity` instead of the real displacement wrongly passed (H3).
- The mock's settings must match the configuration being judged (`taaMode`, `sharpen`, refresh rate). `sharpen` is
  not a control-file key.
- Never open the game's profiler during a measurement (G10).
- Use interleaved (ABBA) runs and medians. Use the live control file `TeardownVR_perf.txt` to switch modes without a
  restart; for some keys, the file must exist when the game starts.

### M8. Proving "no visible change"

- For every performance change, the rule was: the same image.
- Tools used:
  - bit-exact GPU harnesses running the game's own shader binaries against the mod's (F2, F3, F5, G7);
  - "check" modes that compare at run time against the engine's own computation and log any difference
    (`parallelParticleSort=2`, `skipDeadPasses=2`, `skipShapeJobsB=2`);
  - live image comparisons between modes on the mock.

### M9. Patterns and graceful degradation

- Every engine function comes from a byte pattern (unique in `.text` for the newer features), and most field
  offsets are read from the matched instructions. The offsets kept as 2.0.1 constants, and the one debug-only RVA,
  must be re-checked by hand after an update (`docs/UPDATING_FOR_A_NEW_GAME_VERSION.md`, section 3.3).
- A missing pattern turns off only its feature, with a log line. Synchronised stereo falls back to alternating eyes.
- Memory faults in replay code (frame B, particle motion) are caught with SEH and turn the feature off until restart.
- Engine fields are written only from the thread, and at the moment, where the engine itself would write them.

### M10. Lua script limits and state

- Lua 5.1 limits matter in a large script (`main.lua`, about 4600 lines): 200 locals per function, 60 upvalues per
  closure. `tick` sits at 55/60 upvalues and the main chunk at 181/200 locals. New state goes into global tables or
  global functions.
- A global state table holding handles must carry `nonserializable = true`, or a quickload restores stale handles
  (I15).

### M11. Reports that were not the mod

- Green spheres in the world came from another workshop mod (Waypointer) using a tool slot.
- A missing left arm and no walking: the left controller was asleep (no left inputs in the log).
- Every key opening the Start menu: the Windows key was logically stuck on the PC (released with a synthetic key-up).
  The game was not even running.
- A session at 15 to 20 fps: another program was using the GPU (G12).
- Low resolution: the game had started before the headset (D2), or its display mode had become fullscreen (D3).
- Lesson: check the log, the active mod list and the system state before changing code.

### M12. Hard constraints kept throughout

- Never patch the game's "internal usage only" Lua sandbox check (A3).
- Never modify other mods' files. Total Overdose, the revolver and Natural Phenomena are handled from the DLL and from
  the mod's own Lua.
- Read the streaming app's files read-only (`StreamerSettings.json`). Never attach to other processes.
- Keep synchronised stereo: both eyes show the same moment.
- Do not distribute game code: the game-derived shaders stay out of the public source (see "How to read this
  document").
