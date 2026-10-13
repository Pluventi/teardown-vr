#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

struct IDXGISwapChain3;
struct ID3D12CommandQueue;
struct ID3D12CommandList;

// Performance measurement ([Debug] perfTrace, prof.h: g_perfTrace) and the process priority
// while the headset session runs ([Performance] cpuPriority). See
// docs/REVERSE_ENGINEERING.md, "Mesure des performances".
//
// perfTrace 1 (cheap): per-eye averages of the frame sections (prof.h) on the "Stereo frames"
//   line; the engine's own profiler (Profiler::add, hooked: categories per eye); the main
//   thread's waits on jobs (Job::wait, hooked, by calling function) and on the GPU
//   (TFenceD3D12::waitValueOnCpu, hooked); physics steps per frame; the shapes of the scene and
//   those a script moved in the right-eye frame; CPU per thread group (a background thread).
// perfTrace 2: also each mod's tick (the engine's tick of a script entity, hooked), the Lua
//   bindings each mod calls in its left- / right-eye tick (the engine's binding dispatcher and
//   the registry setters, hooked: "census") and the time of each binding.
// [Performance] perfDetail (perfTrace >= 1; bits, default 7; TeardownVR_perf.txt "perfDetail=N" switches
//   them live, among those set up at start): lines "Perf waits / jobs / GPU" after each "Stereo frames"
//   line. 1 = the main thread's blocking Job::wait calls (count, length histogram, the job's state when it
//   blocked, by caller), its GPU fence waits by calling function (beginFrame, the async uploader...) and
//   the async uploader's bytes (TAsyncDataUploaderD3D12::submit, hooked); 2 = every job timed by class
//   (Job::run, hooked, all threads: time, runs, the longest, the longest per frame); 4 = the GPU's busy
//   and idle time per frame (timestamps of ours around each submission on the game's queue); 8 = the GPU time
//   of each pass of the game's frame (implies 4): a timestamp of ours written into the game's own command list where
//   the pass changes (TRendererD3D12's set-program methods, Renderer::render's start / return, its voxel-texture
//   update and ShadowVolume's GPU part, hooked), line "Perf GPU passes" (ms per frame of each eye, by pass).
// Every hook calls the engine's function through, unchanged. perfTrace 0: nothing hooked.
// [Performance] frameBMode 1 or 2 (frameb.h) installs the census hooks too: in mode 1, the
// right-eye tick of a mod whose left-eye tick of the same pair only read is skipped.
namespace Perf {

// Engine functions for the measurement, found by engine.cpp (InitPerfTrace); nullptr = not found.
struct Targets
{
    BYTE* jobWait = nullptr;      // Job::wait(job)
    BYTE* fenceWait = nullptr;    // TFenceD3D12::waitValueOnCpu(fence, value)
    BYTE* profAdd = nullptr;      // Profiler::add(profiler, category, seconds)
    BYTE* displayWait = nullptr;  // TRendererD3D12::endRender's wait on the swap chain's frame-latency object
    BYTE* jobRun = nullptr;       // Job::run(job): its body, then its dependants ([Performance] perfDetail 2)
    BYTE* uploadSubmit = nullptr; // TAsyncDataUploaderD3D12::submit(uploader, mode, queue) (perfDetail 1)
    // perfDetail 8 (GPU time per pass, perf.cpp), searched only when asked for (PassesWanted):
    BYTE* setGfxProgram = nullptr;  // TRendererD3D12::setGraphicsProgram(renderer, program): vtable +190
    BYTE* setCsProgram = nullptr;   // TRendererD3D12::setComputeProgram(renderer, program): vtable +198 (via a jmp)
    BYTE* beginRender = nullptr;    // TRendererD3D12::beginRender: its "in a frame" flag (+cc)
    BYTE* voxelTextures = nullptr;  // Renderer's voxel textures of the frame (renderer): uploads, then GPU mips
    BYTE* shadowVolume = nullptr;   // ShadowVolume's GPU part (volume): uploads or the "shadowmips" compute
    BYTE* modTick = nullptr;      // a script entity's tick (entity, dt, input): server, then client script
    uint32_t entPath = 0;         // script entity -> its script path (the engine's string); 0 = not found
    BYTE* dispatcher = nullptr;   // binding dispatcher (realm, name, impl, script, args, ret)
    struct Core { const char* name; BYTE* fn; };
    Core core[32] = {};           // registry setters and other acting core functions (not through the
                                  // dispatcher): (script, args, ret)
    int coreCount = 0;
    bool frameB = false;          // frame B work wanted (frameb.h): mods' tick, dispatcher and core hooks
                                  // installed even without perfTrace 2 (the census decides mode 1)
    // [Performance] jobSpin (below), found by engine.cpp (FindJobSpinTargets) also without perfTrace
    BYTE* workerWait = nullptr;      // the job semaphore's kernel wait (sem, ms): WaitForSingleObject(*sem, ms) == 0
    BYTE* workerWaitRet = nullptr;   // ... its one caller, the worker loop: the return address of that call
    BYTE** jobManager = nullptr;     // the engine's JobManager pointer (token count +0, running +4, semaphore +100h,
                                     // the extra worker's event +150h)
    DWORD* tlsIndex = nullptr;       // the engine's TLS slot (its block +8: job thread index, -1 = not a job thread)
    BYTE* setEventThunk = nullptr;   // SetEvent(*(HANDLE*)p): Job::wait's wake of the extra worker
};
// Main thread, first Present (EngineInit). Hooks what g_perfTrace (and frameB) asks for, starts the
// CPU sampler.
void InstallHooks(const Targets& t);

// --- Frame B work ([Performance] frameBMode, frameb.h) ------------------------------------------
// The census hooks are installed (UpdateBegin must then be called at every game update).
bool Active();
// Mode 1 possible: mods' tick, the dispatcher and the registry setters hooked.
bool SkipReady();
// The core function of that name (SetBool, ClearKey...) is hooked.
bool CoreHooked(const char* name);
// Our Lua mod is ticking now (bridge.cpp: one of its TDVR messages): its tick is never skipped.
void NoteOurScript();
// Mode 1 since the last call: frames B, mod ticks skipped in them, mods ticked or skipped in them,
// and the frame-A tick time of the skipped ones (ms: what frame B saved, about).
void TakeSkipStats(int& frames, int& skipped, int& ticks, double& ms);

// --- Main thread, from engine.cpp ------------------------------------------------------------------
// Start of Game::update (frozen: the right-eye frame B).
void UpdateBegin(bool frozen);
// Physics steps of this frame's fixed-step loop.
void NoteSteps(int steps);
// FixObjectMotion, right-eye frame: shapes in the scene, shapes moved by a script put back.
void NoteObjectMotion(int shapes, int moved);

// --- vr.cpp, at each Present -----------------------------------------------------------------------
// This frame's sections (g_prof, before they are cleared): eye 0 = A (left), 1 = B (right), -1 other.
void FrameEnd(int engineEye, double frameMs);
// Appended to the "Stereo frames" line every 600 frames (the sums start again).
std::string StatsText();
// Lines logged right after it (perfTrace 2: mods, bindings, census).
void LogDetails();
// Appended to a "Slow frame" line: what its "physics/other" is made of.
std::string SlowFrameText(double other);
// Appended to every "Slow frame" line when the mods' tick is hooked (frame B work, perfTrace 2): the two
// costliest mod ticks of the frame A just finished ("" if none took 0.5 ms).
std::string SlowFrameMods();

// --- Process priority ([Performance] cpuPriority) ---------------------------------------------------
// Every Present: ABOVE_NORMAL while the XR session runs (only from NORMAL), NORMAL again after.
void SessionPriority(bool running);

// --- Desktop swap chain frame latency ([Performance] frameLatency) -----------------------------------
// Every Present: while the headset session runs, the frames the game's swap chain may queue (2: the
// right-eye frame's CPU work runs while the GPU still draws the left eye); the game's own value again after.
void FrameLatency(IDXGISwapChain3* sc, bool session);

// --- Headset stream (Virtual Desktop: [Performance] vdStats, gpuPriority) ----------------------------
// Every Present (vr.cpp PumpXr), with the session's state and the runtime's name: when the session
// starts, Virtual Desktop's encoder settings are logged (StreamerSettings.json, read only) and the
// network send-rate sampler starts; when it stops, the session's hold totals are logged.
void StreamSession(bool running, const std::string& runtime);
// Every Present: the game's GPU scheduling class "below normal" while the session runs ([Performance]
// gpuPriority=1, or TeardownVR_perf.txt "gpuPriority=1"; only from "normal"), "normal" again after.
void GpuPriority(bool running);
// After each xrEndFrame (vr.cpp): its duration and the headset's refresh period, ms.
void NoteEndFrame(double ms, double periodMs);
// Appended to the "Stereo frames" line (" ; VD holds ..."; the window's counts start again); "" with
// vdStats off or no session. fillPct: share of the eye images the hidden-area fill cleared (vr.cpp).
std::string StreamStatsText(double fillPct);

// --- GPU time per frame ([Performance] perfDetail 4; d3d_hooks.cpp, main thread) -----------------------
using PFN_Ecl = void(__stdcall*)(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists);
// Every Present: the swap chain's queue (the first call once the hooks are in sets the timestamps up).
void GpuQueue(ID3D12CommandQueue* q);
// Every ExecuteCommandLists: on that queue, from the main thread, submits the lists between two timestamps
// of ours (ours: made inside the Present - our VR copies, the runtime's) and returns true; false: not
// timed, the caller submits them itself.
bool GpuSubmit(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, bool ours, PFN_Ecl real);
// At the end of every Present (ours, the real one, the headset pacing): this engine frame's submissions are
// all in (its timestamps are read a few frames later).
void GpuFrameEnd();
// [Performance] perfDetail 8 asked for (the ini, or the control file's "perfDetail=N" now): engine.cpp then looks
// for its targets (Targets: setGfxProgram ... shadowVolume).
bool PassesWanted();
// engine.cpp, Hook_Render (main thread): Renderer::render starts (where 1) / has returned (2); backend = its
// TRendererD3D12 (Renderer +9c0). A pass boundary in the game's command list when perfDetail 8 is on, else nothing.
void GpuPassMark(void* backend, int where);

// --- Job-wait latency ([Performance] jobSpin, jobSpinUs, mainSpinUs) ------------------------------------------
// In frame A of the headset session, an idle job thread spins up to jobSpinUs before it sleeps on the job semaphore
// (jobSpin 1 or 3), the main thread up to mainSpinUs in Job::wait before it blocks (2 or 3): the kernel wake-ups of a
// fork or a join are saved when the work comes back that soon. Timing only: the same jobs split the same way, the
// engine's own waits always end it (perf.cpp, "Job-wait latency"). TeardownVR_perf.txt "jobSpin=N", "jobSpinUs=N",
// "mainSpinUs=N" override the ini live.
// The ini or the control file asks for it (decided once): engine.cpp then finds its targets, even without perfTrace.
bool SpinWanted();
// engine.cpp, Hook_Update: frame A starts (vr: synchronised stereo is on).
void SpinFrameA(bool vr);
// vr.cpp, every Present: frame A's job phases are over (every 600 Presents: the "job spin" stats line).
void SpinPresent();
// frameb.cpp: the control file's text, each time it is read (nullptr: removed).
void SpinControl(const char* text);

// --- Overlays hooked into the game's Present ---------------------------------------------------------
// Logs once when the Steam overlay or NVIDIA's capture (overlay, Instant Replay) is loaded.
void CheckOverlays();
// "Steam", "NVIDIA", "Steam + NVIDIA" or nullptr (for the settings panel).
const char* OverlayNames();

// --- Lazy beginFrame ([Performance] lazyBeginFrame) ----------------------------------------------------
// Every engine frame starts with TRendererD3D12::beginRender (Game::frame +71f84, before Game::update),
// which waits until the GPU has finished the frame two engine frames back (the renderer has two frame
// contexts). In synchronised stereo that is the previous pair's frame A: frame A(k+1)'s scripts and
// physics cannot start while the GPU still draws A(k) (2.2-3.1 ms per frame A during the mock's
// explosions, 5.5 ms in its slow frames, the GPU idle meanwhile). lazyBeginFrame holds frame A's
// beginRender back until the first call of another renderer method (normally Game::render's first render
// target bind, after the update, the physics and the particles; perf.cpp): the same calls in the same
// order by the same thread, so the GPU commands are identical.
// engine.cpp (EngineInit), main thread: beginRender / endRender found by pattern (nullptr: not found) and
// the kind of engine frame starting (1 = a stereo pair's frame A, 2 = its frame B, 0 = other). Installs
// nothing unless lazyBeginFrame >= 1 in the ini or the control file TeardownVR_perf.txt has a
// "lazyBeginFrame=N" line at the game's first frame (then N switches it live, 0 included).
void LazyBeginInstall(BYTE* beginRender, BYTE* endRender, int (*frameKind)());
// Appended to the "Stereo frames" line ("" when not installed): frames held back, the delay to the first
// renderer use and its method, beginRender's time there; mode 0: beginRender's time at the frame start.
std::string LazyStatsText();

} // namespace Perf
