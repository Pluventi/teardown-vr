#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// The right-eye frame B of synchronised stereo, made cheaper ([Performance] frameBMode;
// docs/REVERSE_ENGINEERING.md, "Image B allegee"). Frame B shows the instant frame A computed:
//   0 = Game::update runs again in frame B, with no time (as before);
//   1 = the same, but a mod whose tick in the frame A of the same pair only read (Get / Query /
//       Is / Has / Find bindings, no registry write, no other side effect: perf.cpp, the census)
//       is not ticked again in frame B ("clean" mods; ours always is);
//   2 = Game::update does not run in frame B ("render only"): what frame A's update asked for one
//       frame only, and frame A's draw used up, is asked for again (the renderer's lines,
//       sprites, debug lines, outlines and highlights; the light lists cut back to frame A's
//       update; the camera requests; depth of field and low-health blur; sound loop requests,
//       UiMute, the music volume / low-pass keys and the last sound heard (GetLastSound, kept for
//       the next frame A); the registry flags the scripts set at every tick; the time scale). Every script then
//       ticks once per pair, in frame A: the scripts that run on real time (the HUD, the game
//       mode) get the real time of the whole pair there, and the script events (GetEvent) are
//       kept for the next frame A instead of being dropped at frame B's start.
// A control file next to the game, TeardownVR_perf.txt ("frameBMode=N"), read once per second
// while it exists, switches the mode live (image-equivalence tests); without it the ini decides.
namespace FrameB {

// Engine functions and checks, found by engine.cpp (InitPerfTrace). Every pattern also holds the
// offsets frameb.cpp uses (Teardown 2.0.1 values there): one missing -> mode 2 unavailable.
struct Targets
{
    BYTE* lineAdd = nullptr;        // Renderer: add a line (r, a, b, color0, color1, flag)         (+86ca0)
    BYTE* spriteAdd = nullptr;      // Renderer: add a sprite (r, entry 0x58 bytes)                  (+86f50)
    BYTE* debugLineAdd = nullptr;   // Renderer: add a debug line (r, a, b, color)                   (+86a90)
    BYTE* outlineAdd = nullptr;     // Renderer: outline a shape this frame (r, shape, rgba)         (+9d2d0)
    BYTE* highlightAdd = nullptr;   // Renderer: highlight a shape this frame (r, shape, alpha)      (+9d280)
    BYTE* dofSet = nullptr;         // Renderer: depth of field asked for this frame (r, a, b, c)    (+9cbe0)
    bool blurOk = false;            // low-health blur threshold renderer+0x23f4                    (+9cc20)
    bool lightsOk = false;          // light lists renderer+0x2428 / +0x2f38 (cleared only in update) (+87a40)
    bool realTimeOk = false;        // Game::update's real-time readers of game+0x208: HUD and game
                                    // mode ticks, players' trails (+7b149)
    BYTE* eventSwap = nullptr;      // events: Game::frame swaps the two buffers ([game+0x158])      (+157e50)
    bool timeScaleOk = false;       // game+0x1f8 (this frame) / +0x1fc (asked: SetTimeScale)        (+7274c)
    bool soundOk = false;           // sound manager game+0x58: loops +0x58 / +0x60, request +0x88   (+72c77, +ffb80)
    bool musicOk = false;           // SoundManager::update reads then clears game.music.volume /
                                    // game.music.lowpass at every frame start                       (+ff61e, +ff6cf)
    bool lastSoundOk = false;       // GetLastSound: sm+0x7670 (16 bytes), moved there from sm+0x7680
                                    // at every frame start                                         (+ff911, +477e90)
    bool muteOk = false;            // UiMute: sm+0x7690 / +0x7694 (max), zeroed at every frame start (+4c7bdb, +ff9e2)
    bool cameraOk = false;          // camera requests, camera = game+0x60 (Lua bindings checked)
    BYTE* regSetString = nullptr;   // Registry::setString (reg, key, value, sync): every Set* but SetColor (+4626c0)
    BYTE* regClearKey = nullptr;    // Registry::clearKey (reg, key)                                 (+45fdb0)
    BYTE* strCtor = nullptr;        // the engine's string (32 bytes) from a C string                (+5075a0)
    BYTE* strDtor = nullptr;        //                                                                (+507710)
    BYTE* (*entityOfHandle)(BYTE* scene, int handle) = nullptr;   // script handle -> entity (engine.cpp)
    bool (*nextFrameB)() = nullptr; // the coming engine frame is a right-eye frame B (engine.cpp)
    // The right eye from the left one for this pair, as Hook_CamSet builds it: { pos xyz, quat xyzw }
    // (engine.cpp). Frame B's SetCameraOffsetTransform is frame A's composed with it - the right eye,
    // as our Lua's frame-B tick sets it in mode 0. False: not known (frame A's offset kept as is).
    bool (*rightEyeOffset)(float* pose7) = nullptr;
};

// True when frameBMode is 1 or 2 in the ini, or the control file exists at start: the census hooks
// (perf.cpp) and this module's hooks are then installed. Otherwise nothing is hooked (mode 0).
bool HooksWanted();
// Main thread, first Present (after Perf::InstallHooks). Installs the recording hooks; returns
// false (module off: frame B as before) when frame B work was not wanted.
bool Init(const Targets& t);

// Mode of the current stereo pair (latched at its frame A, 0 when the module is off).
int PairMode();
// The control file's "frameLatency=N" (perf.cpp, Perf::FrameLatency), -1 when it has none.
int FileLatency();
// The control file's "lazyBeginFrame=N" (perf.cpp, lazy beginFrame), -1 when it has none.
int FileLazyBegin();
// The control file's "hiddenAreaFill=N" (vr.cpp, FillHidden) and "gpuPriority=N" (perf.cpp, Perf::GpuPriority),
// -1 when it has none.
int FileHiddenArea();
int FileGpuPriority();
// The control file's "parallelParticleSort=N" / "skipShapeJobsB=N" (engine.cpp, the frame-B savings: 0 off, 1 on,
// 2 on and checked), -1 when it has none.
int FileParticleSort();
int FileShapeJobsB();
// The control file's "skipDeadPasses=N" (engine.cpp, the bloom chain / depth of field not drawn when dead: 0 off,
// 1 on, 2 on and checked), -1 when it has none.
int FileDeadPasses();
// The control file's "stereoSameNoise=N" (engine.cpp, SameNoiseBegin: 1 the right eye draws with the left eye's noise
// frame and jitter, 0 its own, one frame ahead, as before), -1 when it has none.
int FileSameNoise();
// The control file's "stereoParticles=N" (particles_vr.cpp: 1 the VR particle and lighting shaders, the right eye taking
// the dither, the front/back lighting and the lighting noise where the left eye sees the same point; 0 the game's;
// tests: 2 the particles only, 3 the lighting only), -1 when it has none.
int FileStereoParticles();
// The control file's "taaMode=N" (taa_vr.cpp; TEST ONLY: 0..4 as the ini, 8 = no anti-aliasing at all, each pixel's own
// current sample - the mock test of stereoParticles measures the per-frame grain on it), -1 when it has none.
int FileTaaMode();
// The control file's "stereoParticleMotion=N" (engine.cpp, Hook_MeshTask: 1 the right eye's particle mesh gets the
// positions the right eye drew at the previous pair as its "last" positions - its own motion vectors; 0 the game's: none),
// -1 when it has none.
int FileParticleMotion();
// The control file's "particleMotionLog=N" (engine.cpp: 1 Hook_MeshTask's counters in the log, measuring only), -1 none.
int FileParticleMotionLog();
// The control file's "stereoParity=N" (particles_vr.cpp: the right eye's front/back checkerboard of the particles - 0 the
// left eye's pixel's parity (round 2), 1 its own pixel's for particles at least 8 px wide, 2 its own for all), -1 none.
int FileStereoParity();
// The module's hooks are in place (frameBMode can change live; otherwise at the next game start).
bool On();

// --- engine.cpp, Hook_Update (main thread) ------------------------------------------------------
// Frame A, right before Game::update (vr: synchronised stereo may follow) / right after it.
void AEnter(BYTE* game, uint64_t frame, bool vr);
void AExit(BYTE* game);
// Frame B, in place of Game::update: true = done (Game::update must not run), false = run it.
bool BReplay(BYTE* game, uint64_t frame);

// --- perf.cpp, the census hooks: a script is in a binding (the dispatcher's: DisableMotionBlur
// writes game.disablemotionblur...) or in a core registry setter (SetBool, SetInt, SetFloat,
// SetString, ClearKey): a registry write meanwhile is the script's ----------------------------------
void ScriptWriteBegin();
void ScriptWriteEnd();

// --- vr.cpp, every "Stereo frames" line ---------------------------------------------------------
// Short summary for that line (" ; frame B ...", "" when off) and the details line after it.
std::string StatsText();
void LogDetails();

} // namespace FrameB
