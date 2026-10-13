// The right-eye frame B made cheaper (see frameb.h, and docs/REVERSE_ENGINEERING.md, "Image B
// allegee"). Mode 1 (skip the frame-B tick of "clean" mods) is in perf.cpp (its census hooks);
// this file holds the mode switch, the control file, mode 2 ("render only": frame A's per-frame
// requests replayed in frame B instead of Game::update) and the counters.
//
// Mode 2, what frame B needs from Game::update (static reverse engineering of 2.0.1, see the doc):
// the camera is set in Game::render (Camera::setTransform, our Hook_CamSet: the right-eye camera),
// the tool body is placed in Game::render from what ToolAnim left in frame A, particles and mods'
// render() / draw() run in Game::render. What frame A's update asked for ONE frame and frame A's
// render / draw / next frame start used up is asked for again:
//   - renderer lists drawn and emptied by Renderer::render: lines and sprites (the entries frame
//     A's update added, copied at its exit, added again), debug lines, outlines and highlights
//     (the calls of frame A's update recorded by hooks, made again);
//   - the two light lists, cleared only by Game::update: cut back to their length at frame A's
//     update exit (frame A's physics steps and draw appended lights after it);
//   - depth of field (SetCameraDof) and the low-health blur threshold, reset by Renderer::render;
//   - camera requests, used up by the camera update in Game::render: SetCameraTransform (and the
//     camera mode 8 it keeps), SaveCameraOverrideTransform, SetCameraOffsetTransform (composed
//     with the eye offset: the right eye, as our Lua's frame-B tick sets it in mode 0),
//     SetCameraFov, AttachCameraTo;
//   - sound requests used up at the start of the next frame (SoundManager::update 0xff420): the
//     loops' (PlayLoop, UiSoundLoop: without them the loops would get quieter at every pair), the
//     mute (UiMute) and the music volume / low-pass keys (game.music.volume / .lowpass, read then
//     cleared); and the last sound heard (GetLastSound: frame A's loudest, moved at frame B's start
//     where no script reads it any more) is put back for the next frame A's start;
//   - registry flags the scripts set at every tick (hud.disable, hud.aimdot, hints... which the
//     HUD's draw resets; game.disablemotionblur / disablepause reset at the frame start): a key a
//     script wrote in this frame A's update AND, with the same value, in the previous one, is set
//     again to its value at frame A's update exit (a key written once is an event - a
//     notification - and is never repeated; a cleared key whose sub-keys were written after the
//     clear is not cleared again);
//   - the time scale asked for (SetTimeScale, game+0x1fc), taken at the next frame start.
// Every script then ticks once per pair, in frame A. The engine's script events are double
// buffered per frame (posted in frame N, readable in frame N+1 only, swapped at each frame start):
// the swap is skipped at the start of a render-only frame B, so what frame A posted is read in the
// next frame A (by the mods too, and once). The scripts that use real time (game+0x208: the HUD's
// and the game mode's ticks, the players' trails) get the real time of the whole pair in frame A.
// The player's own per-frame requests (RequestFirstPerson, SetPlayerHidden, the tool transforms)
// are reset only inside Game::update (0xc4460): skipping it keeps frame A's.
#include "frameb.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "perf.h"
#include "prof.h"
#include "vrmath.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace FrameB {
namespace {

// ---------------------------------------------------------------------------------------------------
// Offsets (Teardown 2.0.1). Each one is inside a pattern engine.cpp matched (FindFrameBTargets).
// ---------------------------------------------------------------------------------------------------
// Game
constexpr uint32_t kGameState = 0x08;                 // 6 = in a level
constexpr uint32_t kGameRenderer = 0x48;
constexpr uint32_t kGameScene = 0x50;
constexpr uint32_t kGameSound = 0x58;
constexpr uint32_t kGameCam = 0x60;
constexpr uint32_t kGameRegistry = 0x100;
constexpr uint32_t kGamePaused = 0x1e8;
constexpr uint32_t kGameTimeScale = 0x1f8, kGameTimeScaleReq = 0x1fc;
constexpr uint32_t kGameRealDt = 0x208;
// Renderer: { int count; int capacity; entry* data (+8) } lists
constexpr uint32_t kRLines = 0x23b0, kRSprites = 0x23c0;
constexpr int kLineSize = 0x3c, kSpriteSize = 0x58;
constexpr uint32_t kRLights[2] = { 0x2428, 0x2f38 };   // two lists of 0xb10 bytes, count at +0
constexpr uint32_t kRDof = 0xa28, kRDofFlag = 0x23f0, kRBlur = 0x23f4;
// Camera
constexpr uint32_t kCamOff = 0x1b0, kCamOffFlag = 0x1cc;                     // SetCameraOffsetTransform
constexpr uint32_t kCamSaveFlag = 0x261;                                    // SaveCameraOverrideTransform
constexpr uint32_t kCamReq = 0x264, kCamReqFov = 0x2c4, kCamReqFlag = 0x2cc;  // SetCameraTransform
constexpr uint32_t kCamFov = 0x2c8;                                         // SetCameraFov
constexpr uint32_t kCamAttach = 0x2d0, kCamAttachFlag = 0x2d8;              // AttachCameraTo
constexpr size_t kPoseSize = 0x1c;                                          // vec3 + quat
// Sound manager: loops { int count (+0x58); Loop** (+0x60) }; a loop's request: volume, 3 floats
constexpr uint32_t kSndLoopN = 0x58, kSndLoops = 0x60, kLoopReq = 0x88;
constexpr size_t kLoopReqSize = 16;
// ... the last sound: { volume, position } GetLastSound reads (+0x7670) and this frame's loudest
// registered sound (+0x7680), moved there and zeroed at each frame start (0xff911)
constexpr uint32_t kSndLast = 0x7670, kSndCur = 0x7680;
constexpr size_t kSndRegSize = 16;
// ... UiMute: all sounds / music (the max of the frame's requests), zeroed at each frame start
constexpr uint32_t kSndMute[2] = { 0x7690, 0x7694 };
// Entities
constexpr uint32_t kEntType = 0x08, kEntHandle = 0x0c;

constexpr int kMaxListCopy = 1 << 16;   // entries copied per list and pair at most (sanity bound)

// ---------------------------------------------------------------------------------------------------
// Engine functions
// ---------------------------------------------------------------------------------------------------
using PFN_LineAdd = void(__fastcall*)(BYTE* r, const float* a, const float* b, const float* c0, const float* c1, uint8_t flag);
using PFN_SpriteAdd = void(__fastcall*)(BYTE* r, const void* entry);
using PFN_DebugLine = void(__fastcall*)(BYTE* r, const float* a, const float* b, const float* color);
using PFN_Outline = void(__fastcall*)(BYTE* r, BYTE* shape, const float* rgba);
using PFN_Highlight = void(__fastcall*)(BYTE* r, BYTE* shape, float alpha);
using PFN_Dof = void(__fastcall*)(BYTE* r, float a, float b, float c);
using PFN_SetString = void(__fastcall*)(BYTE* reg, void* key, void* value, bool sync);
using PFN_ClearKey = void(__fastcall*)(BYTE* reg, void* key);
using PFN_EventSwap = void(__fastcall*)(BYTE* events);
using PFN_StrCtor = void*(__fastcall*)(void* str, const char* text);
using PFN_StrDtor = void(__fastcall*)(void* str);

Targets g_t;
PFN_LineAdd g_lineAdd = nullptr;
PFN_SpriteAdd g_spriteAdd = nullptr;
PFN_Dof g_dof = nullptr;
PFN_StrCtor g_strCtor = nullptr;
PFN_StrDtor g_strDtor = nullptr;
// hooked: the original functions (trampolines)
PFN_DebugLine Real_DebugLine = nullptr;
PFN_Outline Real_Outline = nullptr;
PFN_Highlight Real_Highlight = nullptr;
PFN_SetString Real_SetString = nullptr;
PFN_ClearKey Real_ClearKey = nullptr;
PFN_EventSwap Real_EventSwap = nullptr;

// ---------------------------------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------------------------------
bool g_on = false;            // Init done (hooks wanted)
int g_avail = 0;              // highest mode possible: 2 (all found), 1 (census hooks only), 0
int g_iniMode = 0;            // [Performance] frameBMode
int g_fileMode = -1;          // the control file's frameBMode (-1: no file)
int g_pairMode = 0;           // this stereo pair's mode
int g_loggedMode = -1;
std::wstring g_ctlPath;
ULONGLONG g_pollAt = 0;
FILETIME g_ctlTime = {};
DWORD g_ctlSize = 0;
bool g_broken = false;        // an exception in a replay: mode 2 off until the game restarts

std::wstring CtlPath()
{
    if (g_ctlPath.empty()) g_ctlPath = ModDirectory() + L"TeardownVR_perf.txt";
    return g_ctlPath;
}

// "key=N" (spaces allowed, other lines ignored): N clamped to 0..hi; -1 if not found.
int ParseKey(const char* s, const char* key, int hi)
{
    const size_t len = strlen(key);
    for (const char* p = s; (p = strstr(p, key)) != nullptr; p += len) {
        const char* q = p + len;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q >= '0' && *q <= '9') return std::clamp(*q - '0', 0, hi);
    }
    return -1;
}
int g_fileLatency = -1;       // the control file's "frameLatency=N" (Perf::FrameLatency), -1 = none
int g_fileLazy = -1;          // the control file's "lazyBeginFrame=N" (perf.cpp, lazy beginFrame), -1 = none
int g_fileHidden = -1;        // ... "hiddenAreaFill=N" (vr.cpp, FillHidden)
int g_fileGpuPrio = -1;       // ... "gpuPriority=N" (Perf::GpuPriority)
int g_fileParSort = -1;       // ... "parallelParticleSort=N" (engine.cpp, Hook_PsSort), -1 = none
int g_fileShapeJobs = -1;     // ... "skipShapeJobsB=N" (engine.cpp, Hook_ShapeJobs), -1 = none
int g_fileDeadPasses = -1;    // ... "skipDeadPasses=N" (engine.cpp, Hook_BloomRender / Hook_DofRender), -1 = none
int g_fileSameNoise = -1;     // ... "stereoSameNoise=N" (engine.cpp, SameNoiseBegin), -1 = none
int g_fileStereoParticles = -1;   // ... "stereoParticles=N" (particles_vr.cpp), -1 = none
int g_fileTaaMode = -1;           // ... "taaMode=N" (taa_vr.cpp, test only), -1 = none
int g_fileParticleMotion = -1;    // ... "stereoParticleMotion=N" (engine.cpp, Hook_MeshTask), -1 = none
int g_fileParticleMotionLog = -1; // ... "particleMotionLog=N" (engine.cpp, its counters), -1 = none
int g_fileStereoParity = -1;      // ... "stereoParity=N" (particles_vr.cpp), -1 = none

// Reads the control file when it changed (at most once per second, main thread); its removal gives
// the ini's mode back.
void PollControl(bool now)
{
    const ULONGLONG t = GetTickCount64();
    if (!now && t - g_pollAt < 1000) return;
    g_pollAt = t;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(CtlPath().c_str(), GetFileExInfoStandard, &fa)) {
        if (g_fileMode >= 0) {
            g_fileMode = -1;
            Log("Frame B: control file TeardownVR_perf.txt removed - back to the ini's frameBMode=%d", g_iniMode);
        }
        g_fileHidden = -1;
        g_fileGpuPrio = -1;
        Perf::SpinControl(nullptr);
        g_fileLatency = -1;
        g_fileLazy = -1;
        g_fileParSort = -1;
        g_fileShapeJobs = -1;
        g_fileDeadPasses = -1;
        g_fileSameNoise = -1;
        g_fileStereoParticles = -1;
        g_fileTaaMode = -1;
        g_fileParticleMotion = -1;
        g_fileParticleMotionLog = -1;
        g_fileStereoParity = -1;
        g_ctlTime = {};
        g_ctlSize = 0;
        return;
    }
    if (!now && CompareFileTime(&fa.ftLastWriteTime, &g_ctlTime) == 0 && fa.nFileSizeLow == g_ctlSize) return;
    g_ctlTime = fa.ftLastWriteTime;
    g_ctlSize = fa.nFileSizeLow;
    char buf[2048] = {};
    HANDLE h = CreateFileW(CtlPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        g_ctlTime = {};   // (being written: read again at the next poll)
        return;
    }
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;
    // ("jobSpin=N", "jobSpinUs=N", "mainSpinUs=N": the job-wait spin, perf.h)
    Perf::SpinControl(buf);
    const int lat = ParseKey(buf, "frameLatency", 3);
    if (lat != g_fileLatency) {
        Log("Frame B: control file TeardownVR_perf.txt sets frameLatency=%d (was %d)", lat, g_fileLatency);
        g_fileLatency = lat;
    }
    const int lazy = ParseKey(buf, "lazyBeginFrame", 2);
    if (lazy != g_fileLazy) {
        Log("Frame B: control file TeardownVR_perf.txt sets lazyBeginFrame=%d (was %d)", lazy, g_fileLazy);
        g_fileLazy = lazy;
    }
    // (headset stream A/B tests, see vr.cpp FillHidden and perf.cpp GpuPriority)
    const int fill = ParseKey(buf, "hiddenAreaFill", 3);
    if (fill != g_fileHidden) {
        Log("Frame B: control file TeardownVR_perf.txt sets hiddenAreaFill=%d (was %d)", fill, g_fileHidden);
        g_fileHidden = fill;
    }
    const int gpu = ParseKey(buf, "gpuPriority", 1);
    if (gpu != g_fileGpuPrio) {
        Log("Frame B: control file TeardownVR_perf.txt sets gpuPriority=%d (was %d)", gpu, g_fileGpuPrio);
        g_fileGpuPrio = gpu;
    }
    // (engine.cpp's frame-B savings: 0 off, 1 on, 2 on and checked; read before the frameBMode line's test)
    const int ps = ParseKey(buf, "parallelParticleSort", 2);
    if (ps != g_fileParSort) {
        Log("Frame B: control file TeardownVR_perf.txt sets parallelParticleSort=%d (was %d)", ps, g_fileParSort);
        g_fileParSort = ps;
    }
    const int sj = ParseKey(buf, "skipShapeJobsB", 2);
    if (sj != g_fileShapeJobs) {
        Log("Frame B: control file TeardownVR_perf.txt sets skipShapeJobsB=%d (was %d)", sj, g_fileShapeJobs);
        g_fileShapeJobs = sj;
    }
    const int dp = ParseKey(buf, "skipDeadPasses", 2);
    if (dp != g_fileDeadPasses) {
        Log("Frame B: control file TeardownVR_perf.txt sets skipDeadPasses=%d (was %d)", dp, g_fileDeadPasses);
        g_fileDeadPasses = dp;
    }
    const int sn = ParseKey(buf, "stereoSameNoise", 1);
    if (sn != g_fileSameNoise) {
        Log("Frame B: control file TeardownVR_perf.txt sets stereoSameNoise=%d (was %d)", sn, g_fileSameNoise);
        g_fileSameNoise = sn;
    }
    // (0 off, 1 on; tests: 2 the particle shaders only, 3 the lighting shader only)
    const int sp = ParseKey(buf, "stereoParticles", 3);
    if (sp != g_fileStereoParticles) {
        Log("Frame B: control file TeardownVR_perf.txt sets stereoParticles=%d (was %d)", sp, g_fileStereoParticles);
        g_fileStereoParticles = sp;
    }
    // (TEST ONLY, taa_vr.cpp: 8 = no anti-aliasing, the raw current frame; 0..4 as the ini)
    const int tm = ParseKey(buf, "taaMode", 9);
    if (tm != g_fileTaaMode) {
        Log("Frame B: control file TeardownVR_perf.txt sets taaMode=%d (was %d)%s", tm, g_fileTaaMode,
            tm == 8 ? " - TEST: no anti-aliasing, the raw current frame" : "");
        g_fileTaaMode = tm;
    }
    // (0 the game's: no particle motion in the right eye; 1 frame B gets its own previous particle positions back)
    const int pm = ParseKey(buf, "stereoParticleMotion", 1);
    if (pm != g_fileParticleMotion) {
        Log("Frame B: control file TeardownVR_perf.txt sets stereoParticleMotion=%d (was %d)", pm, g_fileParticleMotion);
        g_fileParticleMotion = pm;
    }
    const int pml = ParseKey(buf, "particleMotionLog", 1);
    if (pml != g_fileParticleMotionLog) {
        Log("Frame B: control file TeardownVR_perf.txt sets particleMotionLog=%d (was %d)", pml, g_fileParticleMotionLog);
        g_fileParticleMotionLog = pml;
    }
    // (0 round 2, 1 own pixel for particles at least 8 px wide, 2 own pixel for all)
    const int par = ParseKey(buf, "stereoParity", 2);
    if (par != g_fileStereoParity) {
        Log("Frame B: control file TeardownVR_perf.txt sets stereoParity=%d (was %d)", par, g_fileStereoParity);
        g_fileStereoParity = par;
    }
    const int m = ParseKey(buf, "frameBMode", 2);
    if (m < 0) {
        static int logs = 0;
        if (logs++ < 5) Log("Frame B: control file TeardownVR_perf.txt has no \"frameBMode=N\" line - frameBMode unchanged (its other keys are read)");
        return;
    }
    if (m != g_fileMode) {
        Log("Frame B: control file TeardownVR_perf.txt sets frameBMode=%d (was %d, ini %d)", m, g_fileMode, g_iniMode);
        g_fileMode = m;
    }
}

int Effective()
{
    const int want = g_fileMode >= 0 ? g_fileMode : g_iniMode;
    return std::min(want, g_broken ? std::min(g_avail, 1) : g_avail);
}

const char* ModeName(int m)
{
    return m == 2 ? "2 (render only: frame A's per-frame requests replayed)"
                  : m == 1 ? "1 (clean mods not ticked again)" : "0 (full update, as before)";
}

// ---------------------------------------------------------------------------------------------------
// Frame A: what its update asked for (mode 2)
// ---------------------------------------------------------------------------------------------------
DWORD g_mainTid = 0;
bool g_rec = false;           // recording frame A's update (main thread)
int g_writeDepth = 0;         // inside a script's registry setter (perf.cpp's core hooks)

inline bool OnMain() { return GetCurrentThreadId() == g_mainTid; }
inline bool Rec() { return g_rec && OnMain(); }

struct Snap
{
    uint64_t frame = 0;      // engine.cpp's frame counter of frame A (0: none)
    bool ok = false;         // complete (its update exit seen)
    BYTE* game = nullptr;
    BYTE* r = nullptr;
    BYTE* cam = nullptr;
    BYTE* scene = nullptr;
    BYTE* reg = nullptr;
    BYTE* snd = nullptr;
    int line0 = 0, sprite0 = 0;                 // counts at frame A's update entry
    std::vector<BYTE> lines, sprites;           // the entries frame A's update added
    int nLines = 0, nSprites = 0;
    int lights[2] = {};
    bool dof = false;
    float dofV[3] = {};
    float blur = 0;
    bool offSet = false, reqSet = false, saveSet = false;
    BYTE off[kPoseSize] = {}, req[kPoseSize] = {};
    float reqFov = 0, fov = 0;
    BYTE* attach = nullptr;
    BYTE attachFlag = 0;
    struct Loop { int index; BYTE* loop; BYTE req[kLoopReqSize]; };
    std::vector<Loop> loops;
    float mute[2] = {};      // UiMute asked for by frame A's update
} g_snap;

struct DebugLineRec { float a[3], b[3], c[4]; };
struct OutlineRec { BYTE* shape; float rgba[4]; };
struct HighlightRec { BYTE* shape; float alpha; };
std::vector<DebugLineRec> g_dbg;
std::vector<OutlineRec> g_outl;
std::vector<HighlightRec> g_hl;
constexpr size_t kMaxRec = 1 << 14;   // calls recorded per list and update at most

// Registry keys a script wrote in a frame A's update, with their value at its exit (later writes,
// the engine's too, update a key already there; clearKey marks it cleared). Two tables: this frame
// A's and the previous one's (a flag is written at every tick; a key written once is an event).
struct RegRec
{
    std::string key, value;
    uint32_t hash = 0;
    bool cleared = false;
    bool skip = false;   // value too long to keep
};
// Keys per update: kMaxRegKeys, then kRegReserve more for the engine's per-frame flags (hud.*,
// game.*: hud.aimdot / hud.disable, ours, among them), whatever the scripts ticking before wrote.
constexpr size_t kMaxRegKeys = 2048, kRegReserve = 256, kMaxKey = 255, kMaxValue = 4095;
constexpr size_t kRegSlots = 8192;   // open addressing on the hash (index + 1, 0 = empty)
static_assert((kRegSlots & (kRegSlots - 1)) == 0 && kRegSlots >= 2 * (kMaxRegKeys + kRegReserve) &&
                  kMaxRegKeys + kRegReserve < 0xffff,
              "power of two, at most half full (short probes), indexes in 16 bits");
inline bool RegReserved(const char* k, size_t len)
{
    return (len > 4 && memcmp(k, "hud.", 4) == 0) || (len > 5 && memcmp(k, "game.", 5) == 0);
}
struct RegTable
{
    std::vector<RegRec> recs;   // [0, n) in use (the strings' buffers are kept from pair to pair)
    size_t n = 0;
    uint16_t index[kRegSlots] = {};
    uint64_t frame = 0;         // the frame A it was recorded in
    bool done = false;          // its update exit seen

    void Reset(uint64_t f)
    {
        n = 0;
        memset(index, 0, sizeof(index));
        frame = f;
        done = false;
    }
    int Find(uint32_t h, const char* k, size_t len) const
    {
        for (size_t i = h & (kRegSlots - 1), probes = 0; probes < kRegSlots; i = (i + 1) & (kRegSlots - 1), ++probes) {
            const uint16_t v = index[i];
            if (!v) return -1;
            const RegRec& r = recs[v - 1];
            if (r.hash == h && r.key.size() == len && memcmp(r.key.data(), k, len) == 0) return v - 1;
        }
        return -1;
    }
    int Add(uint32_t h, const char* k, size_t len)
    {
        if (n >= kMaxRegKeys + kRegReserve || (n >= kMaxRegKeys && !RegReserved(k, len))) return -1;
        if (recs.size() <= n) recs.emplace_back();
        RegRec& r = recs[n];
        r.key.assign(k, len);
        r.value.clear();
        r.hash = h;
        r.cleared = r.skip = false;
        size_t i = h & (kRegSlots - 1);
        while (index[i]) i = (i + 1) & (kRegSlots - 1);
        index[i] = (uint16_t)(n + 1);
        return (int)n++;
    }
};
RegTable g_regTables[2];
RegTable* g_reg = &g_regTables[0];       // this frame A's
RegTable* g_regPrev = &g_regTables[1];   // the previous frame A's (valid when done and recent)
uint64_t g_regDropped = 0;

// The engine's string (32 bytes; byte +0x1f != 0: the text is on the heap, pointer at +0).
inline const char* EngStr(const void* s)
{
    const BYTE* b = (const BYTE*)s;
    return b[0x1f] ? *(const char* const*)b : (const char*)b;
}

uint32_t Fnv(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ (BYTE)s[i]) * 16777619u;
    return h;
}

// A key never written again in frame B: the HUD's notifications are events (its draw shows each
// one it finds and clears the key).
bool NeverReplayed(const std::string& k) { return k.compare(0, 16, "hud.notification") == 0; }

// Keys the next frame start reads and then clears (SoundManager::update 0xff420, at every frame
// start before Game::update): one-frame requests, like the loops'. Frame A's exit value is set again
// in frame B whatever the previous frame A wrote (a fade changes it at every tick: without it the
// music would get its full volume back at every other frame).
bool FrameStartRequest(const std::string& k) { return k == "game.music.volume" || k == "game.music.lowpass"; }

// A key a script wrote when the table was full: not kept (logged once, then counted).
void RegDropped(const char* k)
{
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("Frame B: more than %zu registry keys written by the scripts in one update ('%.80s'...): the next ones are "
            "not replayed in frame B (hud.* / game.* keep %zu more)", kMaxRegKeys, k, kRegReserve);
    }
    ++g_regDropped;
}

void RecordSet(const void* key, const void* value)
{
    const char* k = EngStr(key);
    const size_t n = strnlen(k, kMaxKey + 1);
    if (!n || n > kMaxKey) return;
    const uint32_t h = Fnv(k, n);
    int i = g_reg->Find(h, k, n);
    if (i < 0) {
        if (g_writeDepth <= 0) return;   // (the engine's own writes: game.player.*, game.tool.*... at each update)
        i = g_reg->Add(h, k, n);
        if (i < 0) {
            RegDropped(k);
            return;
        }
    }
    RegRec& r = g_reg->recs[i];
    r.cleared = false;
    const char* v = value ? EngStr(value) : "";
    const size_t vn = strnlen(v, kMaxValue + 1);
    r.skip = vn > kMaxValue;
    if (!r.skip) r.value.assign(v, vn);
}

void RecordClear(const void* key)
{
    const char* k = EngStr(key);
    const size_t n = strnlen(k, kMaxKey + 1);
    if (!n || n > kMaxKey) return;
    const uint32_t h = Fnv(k, n);
    int i = g_reg->Find(h, k, n);
    if (i < 0 && g_writeDepth > 0) {
        i = g_reg->Add(h, k, n);
        if (i < 0) RegDropped(k);
    }
    if (i >= 0) g_reg->recs[i].cleared = true;
    // (its sub-keys are gone too)
    for (size_t j = 0; j < g_reg->n; ++j) {
        RegRec& r = g_reg->recs[j];
        if (r.key.size() > n && r.key[n] == '.' && memcmp(r.key.data(), k, n) == 0) r.cleared = true;
    }
}

// ---------------------------------------------------------------------------------------------------
// Counters (main thread; the "Stereo frames" line every 600 frames)
// ---------------------------------------------------------------------------------------------------
enum Fallback { F_NoSnap, F_NoStart, F_State, F_Changed, F_SyncTest, F_NoPrev, F_Count };
const char* const kFallbackNames[F_Count] = { "no frame-A record", "frame start not seen", "not playing a level",
                                              "game objects changed", "debugSyncTest", "no previous frame A" };
struct Stats
{
    int bFrames = 0;            // frames B of a mode-2 pair
    int replayed = 0;
    int fallback[F_Count] = {};
    double replayMs = 0, aExitMs = 0;
    double lines = 0, sprites = 0, dbg = 0, outlines = 0, outlinesGone = 0, highlights = 0, regSet = 0, regClear = 0,
           regHeld = 0, regKeepSub = 0, music = 0, cam = 0, camEye = 0, loops = 0, lastSound = 0, mute = 0, lightsCut = 0,
           dof = 0, pairDt = 0;
} g_st;

// ---------------------------------------------------------------------------------------------------
// The start of frame B (Game::frame, before its update): decided there, because the events' swap
// is there (a render-only frame B keeps frame A's events for the next frame A).
// ---------------------------------------------------------------------------------------------------
uint64_t g_latchFrame = 0;    // the frame B it was decided for (engine.cpp's counter)
bool g_latchReplay = false;
Fallback g_latchWhy = F_Count;
float g_bRealDt = 0;          // real time of the last render-only frame B: added to the next frame A's
float g_saved208 = 0;
bool g_restore208 = false;

Fallback Check(BYTE* game)
{
    const Snap& s = g_snap;
    if (!s.ok || !s.frame || !game || game != s.game) return F_NoSnap;
    if (Cfg().debugSyncTest) return F_SyncTest;   // (it needs our Lua's own frame-B tick: one camera per eye)
    if (*(const int*)(game + kGameState) != 6 || game[kGamePaused]) return F_State;
    if (*(BYTE**)(game + kGameRenderer) != s.r || *(BYTE**)(game + kGameCam) != s.cam ||
        *(BYTE**)(game + kGameScene) != s.scene || *(BYTE**)(game + kGameRegistry) != s.reg ||
        *(BYTE**)(game + kGameSound) != s.snd)
        return F_Changed;
    // (the per-frame registry flags are replayed only when the previous frame A wrote them too: with no
    // recent previous frame A - level start, VR resume, a live switch to mode 2 - a render-only frame B
    // would lose them all for one frame; a full update writes them, the next pair has its previous A)
    if (!g_regPrev->done || g_reg->frame <= g_regPrev->frame || g_reg->frame - g_regPrev->frame > 2) return F_NoPrev;
    return F_Count;
}

// ---------------------------------------------------------------------------------------------------
// Hooks (each calls the engine's function through, unchanged; they only record during frame A's
// update in mode 2 - and the events' swap is skipped at the start of a render-only frame B)
// ---------------------------------------------------------------------------------------------------
void __fastcall Hook_DebugLine(BYTE* r, const float* a, const float* b, const float* color)
{
    if (Rec() && r == g_snap.r && a && b && color && g_dbg.size() < kMaxRec) {
        DebugLineRec d;
        memcpy(d.a, a, sizeof(d.a));
        memcpy(d.b, b, sizeof(d.b));
        memcpy(d.c, color, sizeof(d.c));
        g_dbg.push_back(d);
    }
    Real_DebugLine(r, a, b, color);
}

void __fastcall Hook_Outline(BYTE* r, BYTE* shape, const float* rgba)
{
    if (Rec() && r == g_snap.r && shape && rgba && g_outl.size() < kMaxRec) {
        OutlineRec o;
        o.shape = shape;
        memcpy(o.rgba, rgba, sizeof(o.rgba));
        g_outl.push_back(o);
    }
    Real_Outline(r, shape, rgba);
}

void __fastcall Hook_Highlight(BYTE* r, BYTE* shape, float alpha)
{
    if (Rec() && r == g_snap.r && shape && g_hl.size() < kMaxRec) g_hl.push_back({ shape, alpha });
    Real_Highlight(r, shape, alpha);
}

void __fastcall Hook_SetString(BYTE* reg, void* key, void* value, bool sync)
{
    if (Rec() && reg == g_snap.reg && key) RecordSet(key, value);
    Real_SetString(reg, key, value, sync);
}

void __fastcall Hook_ClearKey(BYTE* reg, void* key)
{
    if (Rec() && reg == g_snap.reg && key) RecordClear(key);
    Real_ClearKey(reg, key);
}

void __fastcall Hook_EventSwap(BYTE* events)
{
    g_latchFrame = 0;
    g_latchReplay = false;
    if (g_on && g_pairMode == 2 && g_snap.ok && g_t.nextFrameB && g_t.nextFrameB() && OnMain()) {
        // The frame starting is the frame B of the pair whose frame A was just recorded.
        g_latchFrame = g_snap.frame + 1;
        g_latchWhy = Check(g_snap.game);
        g_latchReplay = g_latchWhy == F_Count;
        if (g_latchReplay) return;   // (render only: frame A's events stay readable for the next frame A)
    }
    Real_EventSwap(events);
}

// ---------------------------------------------------------------------------------------------------
// Frame B: the replay. No C++ object with a destructor in these functions (structured exception
// handling around them: a fault means mode 2 off, frame B as before).
// ---------------------------------------------------------------------------------------------------
// A shape (outline / highlight) or entity (camera attachment) still in the scene.
bool EntityAlive(BYTE* scene, BYTE* e, int type)
{
    int h = 0;
    uint16_t t = 0;
    __try {
        h = *(const int*)(e + kEntHandle);
        t = *(const uint16_t*)(e + kEntType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return (type < 0 || t == type) && g_t.entityOfHandle && g_t.entityOfHandle(scene, h) == e;
}

void ReplayLists(BYTE* r, BYTE* scene)
{
    const BYTE* p = g_snap.lines.data();
    for (int i = 0; i < g_snap.nLines; ++i, p += kLineSize)
        g_lineAdd(r, (const float*)p, (const float*)(p + 0xc), (const float*)(p + 0x18), (const float*)(p + 0x28), p[0x38]);
    p = g_snap.sprites.data();
    for (int i = 0; i < g_snap.nSprites; ++i, p += kSpriteSize) g_spriteAdd(r, p);
    for (size_t i = 0; i < g_dbg.size(); ++i) Real_DebugLine(r, g_dbg[i].a, g_dbg[i].b, g_dbg[i].c);
    int gone = 0;
    for (size_t i = 0; i < g_outl.size(); ++i) {
        const OutlineRec& o = g_outl[i];
        if (EntityAlive(scene, o.shape, 2)) Real_Outline(r, o.shape, o.rgba);
        else ++gone;
    }
    for (size_t i = 0; i < g_hl.size(); ++i) {
        const HighlightRec& h = g_hl[i];
        if (EntityAlive(scene, h.shape, 2)) Real_Highlight(r, h.shape, h.alpha);
        else ++gone;
    }
    g_st.lines += g_snap.nLines;
    g_st.sprites += g_snap.nSprites;
    g_st.dbg += (double)g_dbg.size();
    g_st.outlines += (double)g_outl.size();
    g_st.highlights += (double)g_hl.size();
    g_st.outlinesGone += gone;
}

// A sub-key of k written after k's last clear in this frame A (so still there at its update exit).
bool LiveBelow(const RegTable& t, const std::string& k)
{
    const size_t n = k.size();
    for (size_t j = 0; j < t.n; ++j) {
        const RegRec& r = t.recs[j];
        if (!r.cleared && r.key.size() > n && r.key[n] == '.' && memcmp(r.key.data(), k.data(), n) == 0) return true;
    }
    return false;
}

// The keys of this frame A also written by the previous frame A, with the same value (or cleared
// in both): set again (frame A's draw or this frame's start may have reset them). The music
// requests the frame start uses up: set again whatever the previous frame A wrote. A cleared key
// with a sub-key written after the clear is not cleared again: its subtree was not empty at frame
// A's exit (a mod clearing "a.b" at the end of its tick while another writes "a.b.c" after it -
// clearing would wipe a.b.c, and the sub-keys the engine wrote, until the next frame A).
void ReplayRegistry(BYTE* reg)
{
    const RegTable& cur = *g_reg;
    const RegTable& prev = *g_regPrev;
    const bool prevOk = prev.done && cur.frame > prev.frame && cur.frame - prev.frame <= 2;
    alignas(16) BYTE k[64], v[64];
    for (size_t i = 0; i < cur.n; ++i) {
        const RegRec& rr = cur.recs[i];
        bool replay = false;
        if (rr.skip) {
            replay = false;
        } else if (FrameStartRequest(rr.key)) {
            replay = !rr.cleared;   // (cleared by a script: nothing asked for)
            if (replay) g_st.music += 1;
        } else if (rr.cleared && LiveBelow(cur, rr.key)) {
            g_st.regKeepSub += 1;
        } else if (prevOk && !NeverReplayed(rr.key)) {
            const int j = prev.Find(rr.hash, rr.key.data(), rr.key.size());
            replay = j >= 0 && !prev.recs[j].skip && prev.recs[j].cleared == rr.cleared &&
                     (rr.cleared || prev.recs[j].value == rr.value);
        }
        if (!replay) {
            g_st.regHeld += 1;   // (first written now, changed, an event, or a subtree kept: not repeated)
            continue;
        }
        g_strCtor(k, rr.key.c_str());
        if (rr.cleared) {
            Real_ClearKey(reg, k);
            g_st.regClear += 1;
        } else {
            g_strCtor(v, rr.value.c_str());
            Real_SetString(reg, k, v, false);
            g_strDtor(v);
            g_st.regSet += 1;
        }
        g_strDtor(k);
    }
}

// A pose as the engine stores it ({ pos xyz, quat xyzw }, kPoseSize bytes).
Pose PoseOf(const float* f) { return { { f[3], f[4], f[5], f[6] }, { f[0], f[1], f[2] } }; }
void StorePose(float* f, const Pose& v)
{
    f[0] = v.p.x; f[1] = v.p.y; f[2] = v.p.z;
    f[3] = v.q.x; f[4] = v.q.y; f[5] = v.q.z; f[6] = v.q.w;
}

void ReplayCamera(BYTE* cam, BYTE* scene)
{
    int n = 0;
    if (g_snap.offSet) {
        // Frame A's offset is the left eye's (our Lua's: the player camera moved to the eye). In mode 0
        // our frame-B tick sets the right eye's: the same, composed with the eye offset (cam = pc x
        // offset, right = left x eye offset). The image does not depend on it (Hook_CamSet puts the
        // right-eye camera in), but the camera pose the engine keeps does: GetCameraTransform in
        // frame B's draw(), the debugLua drift check, a frame B without Hook_CamSet's correction.
        float off[7], eye[7];
        static_assert(sizeof(off) == kPoseSize, "pose layout");
        memcpy(off, g_snap.off, kPoseSize);
        if (g_t.rightEyeOffset && g_t.rightEyeOffset(eye)) {
            StorePose(off, Mul(PoseOf(off), PoseOf(eye)));
            g_st.camEye += 1;
        }
        memcpy(cam + kCamOff, off, kPoseSize);
        cam[kCamOffFlag] = 1;
        ++n;
    }
    if (g_snap.reqSet) {
        memcpy(cam + kCamReq, g_snap.req, kPoseSize);
        *(float*)(cam + kCamReqFov) = g_snap.reqFov;
        cam[kCamReqFlag] = 1;
        ++n;
    }
    if (g_snap.saveSet) {
        cam[kCamSaveFlag] = 1;
        ++n;
    }
    if (g_snap.fov != 0) {
        *(float*)(cam + kCamFov) = g_snap.fov;
        ++n;
    }
    if (g_snap.attach && EntityAlive(scene, g_snap.attach, -1)) {
        *(BYTE**)(cam + kCamAttach) = g_snap.attach;
        cam[kCamAttachFlag] = g_snap.attachFlag;
        ++n;
    }
    g_st.cam += n;
}

void ReplaySound(BYTE* snd)
{
    // GetLastSound: this frame's start moved frame A's loudest registered sound (its update, physics
    // and draw: gunshots, explosions, loops) to +0x7670, where no script reads it any more, and
    // zeroed +0x7680. Put back as this frame's, the next frame A's start moves it again: that
    // frame's ticks (choppers) and updates (robots' hearing) hear it, once.
    float lastVol, curVol;
    memcpy(&lastVol, snd + kSndLast, 4);
    memcpy(&curVol, snd + kSndCur, 4);
    if (lastVol > curVol) {
        memcpy(snd + kSndCur, snd + kSndLast, kSndRegSize);
        g_st.lastSound += 1;
    }
    // UiMute from frame A's update (a tick), zeroed by this frame's start: asked again (the engine
    // keeps the largest request of a frame, as UiMute does).
    for (int k = 0; k < 2; ++k) {
        float m;
        memcpy(&m, snd + kSndMute[k], 4);
        if (g_snap.mute[k] > m) {
            memcpy(snd + kSndMute[k], &g_snap.mute[k], 4);
            g_st.mute += 1;
        }
    }
    const int n = *(const int*)(snd + kSndLoopN);
    BYTE** loops = *(BYTE***)(snd + kSndLoops);
    if (!loops || n <= 0) return;
    for (size_t k = 0; k < g_snap.loops.size(); ++k) {
        const Snap::Loop& s = g_snap.loops[k];
        BYTE* L = nullptr;
        if (s.index < n && loops[s.index] == s.loop) {
            L = s.loop;
        } else {
            for (int i = 0; i < n && !L; ++i)
                if (loops[i] == s.loop) L = s.loop;   // (the list changed: a loop added or removed)
        }
        if (!L) continue;
        float cur, want;
        memcpy(&cur, L + kLoopReq, 4);
        memcpy(&want, s.req, 4);
        if (want > cur) {   // (PlayLoop keeps the loudest request)
            memcpy(L + kLoopReq, s.req, kLoopReqSize);
            g_st.loops += 1;
        }
    }
}

void ReplayAll(BYTE* g)
{
    BYTE* r = g_snap.r;
    BYTE* scene = g_snap.scene;
    // Light lists: frame A's update entries only (its physics steps, preRender and draw appended more).
    for (int k = 0; k < 2; ++k) {
        int* c = (int*)(r + kRLights[k]);
        if (*c > g_snap.lights[k] && g_snap.lights[k] >= 0) {
            g_st.lightsCut += *c - g_snap.lights[k];
            *c = g_snap.lights[k];
        }
    }
    ReplayRegistry(g_snap.reg);
    ReplayLists(r, scene);
    if (g_snap.dof) {
        g_dof(r, g_snap.dofV[0], g_snap.dofV[1], g_snap.dofV[2]);
        g_st.dof += 1;
    }
    *(float*)(r + kRBlur) = g_snap.blur;
    ReplayCamera(g_snap.cam, scene);
    ReplaySound(g_snap.snd);
    // SetTimeScale: game+0x1fc is taken into +0x1f8 at the start of each frame (then 1 again):
    // frame A's request, already taken by this frame's start, is kept for the next one.
    *(float*)(g + kGameTimeScaleReq) = *(const float*)(g + kGameTimeScale);
}

bool ReplaySeh(BYTE* g)
{
    __try {
        ReplayAll(g);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void AppendF(std::string& s, const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(b, (size_t)std::min(n, (int)sizeof(b) - 1));
}

} // namespace

// ---------------------------------------------------------------------------------------------------
bool HooksWanted()
{
    static int wanted = -1;
    if (wanted < 0) {
        g_iniMode = std::clamp(Cfg().frameBMode, 0, 2);
        const bool file = GetFileAttributesW(CtlPath().c_str()) != INVALID_FILE_ATTRIBUTES;
        wanted = g_iniMode >= 1 || file ? 1 : 0;
    }
    return wanted == 1;
}

bool Init(const Targets& t)
{
    if (!HooksWanted()) return false;
    g_t = t;
    PollControl(true);
    // Mode 2: every piece found, and the registry setters' hooks of the census (perf.cpp) in place
    // (they tell a script's registry writes from the engine's).
    std::string missing;
    auto need = [&](bool ok, const char* what) {
        if (ok) return;
        missing += missing.empty() ? "" : ", ";
        missing += what;
    };
    need(t.lineAdd, "lines");
    need(t.spriteAdd, "sprites");
    need(t.debugLineAdd, "debug lines");
    need(t.outlineAdd && t.highlightAdd, "outlines/highlights");
    need(t.dofSet && t.blurOk, "depth of field");
    need(t.lightsOk, "light lists");
    need(t.realTimeOk, "real time");
    need(t.eventSwap, "events");
    need(t.timeScaleOk, "time scale");
    need(t.soundOk, "sound loops");
    need(t.musicOk, "music requests");
    need(t.lastSoundOk, "last sound (GetLastSound)");
    need(t.muteOk, "UiMute");
    need(t.cameraOk, "camera requests");
    need(t.regSetString && t.regClearKey && t.strCtor && t.strDtor, "registry");
    need(t.entityOfHandle != nullptr && t.nextFrameB != nullptr, "handle lookup");
    need(Perf::Active() && Perf::CoreHooked("SetBool") && Perf::CoreHooked("SetInt") && Perf::CoreHooked("SetFloat") &&
             Perf::CoreHooked("SetString") && Perf::CoreHooked("ClearKey"),
         "census hooks (dispatcher, registry setters)");
    bool hooks = false;
    if (missing.empty()) {
        g_lineAdd = (PFN_LineAdd)t.lineAdd;
        g_spriteAdd = (PFN_SpriteAdd)t.spriteAdd;
        g_dof = (PFN_Dof)t.dofSet;
        g_strCtor = (PFN_StrCtor)t.strCtor;
        g_strDtor = (PFN_StrDtor)t.strDtor;
        hooks = HookFunction(t.debugLineAdd, (void*)Hook_DebugLine, (void**)&Real_DebugLine) &&
                HookFunction(t.outlineAdd, (void*)Hook_Outline, (void**)&Real_Outline) &&
                HookFunction(t.highlightAdd, (void*)Hook_Highlight, (void**)&Real_Highlight) &&
                HookFunction(t.regSetString, (void*)Hook_SetString, (void**)&Real_SetString) &&
                HookFunction(t.regClearKey, (void*)Hook_ClearKey, (void**)&Real_ClearKey) &&
                HookFunction(t.eventSwap, (void*)Hook_EventSwap, (void**)&Real_EventSwap);
        if (!hooks) missing = "a hook could not be installed";
    }
    g_avail = hooks ? 2 : Perf::SkipReady() ? 1 : 0;
    g_on = true;
    Log("Frame B: frameBMode=%d in the ini%s; mode 1 %s; mode 2 %s%s", g_iniMode,
        g_fileMode >= 0 ? " (the control file TeardownVR_perf.txt overrides it)" : "",
        Perf::SkipReady() ? "ready" : "NOT available (census hooks missing)", hooks ? "ready" : "NOT available: ",
        hooks ? "" : missing.c_str());
    Log("Frame B: mode %s", ModeName(Effective()));
    g_loggedMode = Effective();
    return true;
}

int PairMode() { return g_on ? g_pairMode : 0; }
int FileLatency() { return g_fileLatency; }
int FileLazyBegin() { return g_fileLazy; }
int FileHiddenArea() { return g_fileHidden; }
int FileGpuPriority() { return g_fileGpuPrio; }
int FileParticleSort() { return g_fileParSort; }
int FileShapeJobsB() { return g_fileShapeJobs; }
int FileDeadPasses() { return g_fileDeadPasses; }
int FileSameNoise() { return g_fileSameNoise; }
int FileStereoParticles() { return g_fileStereoParticles; }
int FileTaaMode() { return g_fileTaaMode; }
int FileParticleMotion() { return g_fileParticleMotion; }
int FileParticleMotionLog() { return g_fileParticleMotionLog; }
int FileStereoParity() { return g_fileStereoParity; }
bool On() { return g_on; }

void AEnter(BYTE* game, uint64_t frame, bool vr)
{
    if (!g_on || !game) return;
    g_mainTid = GetCurrentThreadId();
    // The last frame B was render only (no update, no script tick): its real time goes to the
    // scripts that use real time in this update (HUD and game mode ticks, players' trails).
    if (g_bRealDt > 0) {
        float* rdt = (float*)(game + kGameRealDt);
        g_saved208 = *rdt;
        *rdt += g_bRealDt;
        g_st.pairDt += *rdt;
        g_restore208 = true;
        g_bRealDt = 0;
    }
    PollControl(false);
    g_iniMode = std::clamp(Cfg().frameBMode, 0, 2);   // (the settings panel changes it live)
    const int mode = Effective();
    if (mode != g_loggedMode) {
        Log("Frame B: mode %s", ModeName(mode));
        g_loggedMode = mode;
    }
    g_pairMode = mode;
    g_rec = false;
    g_snap.ok = false;
    g_snap.frame = 0;
    if (mode != 2 || !vr) return;
    BYTE* r = *(BYTE**)(game + kGameRenderer);
    BYTE* cam = *(BYTE**)(game + kGameCam);
    BYTE* scene = *(BYTE**)(game + kGameScene);
    BYTE* reg = *(BYTE**)(game + kGameRegistry);
    BYTE* snd = *(BYTE**)(game + kGameSound);
    if (!r || !cam || !scene || !reg || !snd || *(const int*)(game + kGameState) != 6) return;
    g_snap.game = game;
    g_snap.r = r;
    g_snap.cam = cam;
    g_snap.scene = scene;
    g_snap.reg = reg;
    g_snap.snd = snd;
    g_snap.line0 = *(const int*)(r + kRLines);
    g_snap.sprite0 = *(const int*)(r + kRSprites);
    g_dbg.clear();
    g_outl.clear();
    g_hl.clear();
    // (the previous frame A's registry record becomes the reference this pair's keys are compared to)
    std::swap(g_reg, g_regPrev);
    g_reg->Reset(frame);
    g_writeDepth = 0;
    g_snap.frame = frame;
    g_rec = true;
}

void AExit(BYTE* game)
{
    if (g_restore208) {
        *(float*)(game + kGameRealDt) = g_saved208;   // (the draw's GetTimeStep: this frame's own time)
        g_restore208 = false;
    }
    if (!g_rec) return;
    g_rec = false;
    const double t0 = ProfNow();
    Snap& s = g_snap;
    BYTE* r = s.r;
    if (game != s.game || *(BYTE**)(game + kGameRenderer) != r) return;
    const int nl = *(const int*)(r + kRLines), ns = *(const int*)(r + kRSprites);
    if (nl < s.line0 || ns < s.sprite0 || nl - s.line0 > kMaxListCopy || ns - s.sprite0 > kMaxListCopy) return;   // (a level reset)
    const BYTE* ld = *(const BYTE* const*)(r + kRLines + 8);
    const BYTE* sd = *(const BYTE* const*)(r + kRSprites + 8);
    s.nLines = ld ? nl - s.line0 : 0;
    s.nSprites = sd ? ns - s.sprite0 : 0;
    s.lines.assign(ld ? ld + (size_t)s.line0 * kLineSize : nullptr, ld ? ld + (size_t)nl * kLineSize : nullptr);
    s.sprites.assign(sd ? sd + (size_t)s.sprite0 * kSpriteSize : nullptr, sd ? sd + (size_t)ns * kSpriteSize : nullptr);
    for (int k = 0; k < 2; ++k) s.lights[k] = *(const int*)(r + kRLights[k]);
    s.dof = r[kRDofFlag] != 0;
    if (const float* d = *(const float* const*)(r + kRDof)) memcpy(s.dofV, d, sizeof(s.dofV));
    else s.dof = false;
    memcpy(&s.blur, r + kRBlur, 4);
    BYTE* cam = s.cam;
    s.offSet = cam[kCamOffFlag] != 0;
    memcpy(s.off, cam + kCamOff, kPoseSize);
    s.reqSet = cam[kCamReqFlag] != 0;
    memcpy(s.req, cam + kCamReq, kPoseSize);
    memcpy(&s.reqFov, cam + kCamReqFov, 4);
    s.saveSet = cam[kCamSaveFlag] != 0;
    memcpy(&s.fov, cam + kCamFov, 4);
    s.attach = *(BYTE**)(cam + kCamAttach);
    s.attachFlag = cam[kCamAttachFlag];
    s.loops.clear();
    const int n = *(const int*)(s.snd + kSndLoopN);
    BYTE** loops = *(BYTE***)(s.snd + kSndLoops);
    for (int i = 0; loops && i < n && i < 100000; ++i) {
        BYTE* L = loops[i];
        float v;
        if (!L) continue;
        memcpy(&v, L + kLoopReq, 4);
        if (!(v > 0)) continue;
        Snap::Loop l;
        l.index = i;
        l.loop = L;
        memcpy(l.req, L + kLoopReq, kLoopReqSize);
        s.loops.push_back(l);
    }
    for (int k = 0; k < 2; ++k) memcpy(&s.mute[k], s.snd + kSndMute[k], 4);   // (zeroed at this frame's start: the update's)
    g_reg->done = true;
    s.ok = true;
    g_st.aExitMs += ProfNow() - t0;
}

bool BReplay(BYTE* game, uint64_t frame)
{
    if (!g_on || g_pairMode != 2) return false;
    ++g_st.bFrames;
    // (decided at this frame's start, where frame A's events were kept or not: Hook_EventSwap; it
    // only decides when frame A was recorded - the first VR pair, a frame A out of VR or whose
    // update exit returned early have none: said so, not "frame start not seen")
    const bool kept = g_latchFrame == frame && g_latchReplay;
    Fallback why = !g_snap.ok || g_snap.frame + 1 != frame ? F_NoSnap
                   : g_latchFrame != frame                 ? F_NoStart
                   : g_latchReplay                         ? Check(game)
                                                           : g_latchWhy;
    g_latchFrame = 0;
    g_latchReplay = false;
    if (why != F_Count) {
        ++g_st.fallback[why < F_Count ? why : F_NoSnap];
        static int logs = 0;
        if (logs < 10) {
            ++logs;
            Log("Frame B: full update in this frame B (%s%s)", kFallbackNames[why < F_Count ? why : F_NoSnap],
                kept ? "; frame A's events are read a second time" : "");
        }
        return false;
    }
    const double t0 = ProfNow();
    if (!ReplaySeh(game)) {
        g_broken = true;
        Log("Frame B: ERROR - memory fault while replaying frame A's requests: mode 2 turned off until the game "
            "restarts (frame B runs its full update again)");
        return false;
    }
    g_bRealDt = std::max(0.0f, *(const float*)(game + kGameRealDt));
    ++g_st.replayed;
    g_st.replayMs += ProfNow() - t0;
    static bool first = true;
    if (first) {
        first = false;
        Log("Frame B: first render-only right-eye frame (replayed: %d lines, %d sprites, %zu outlines, %zu registry keys "
            "written, %zu loop requests)", g_snap.nLines, g_snap.nSprites, g_outl.size(), g_reg->n, g_snap.loops.size());
    }
    return true;
}

void ScriptWriteBegin() { ++g_writeDepth; }
void ScriptWriteEnd() { --g_writeDepth; }

std::string StatsText()
{
    std::string s;
    if (!g_on) return s;
    int frames = 0, mods = 0, ticks = 0;
    double ms = 0;
    Perf::TakeSkipStats(frames, mods, ticks, ms);
    if (frames > 0)
        AppendF(s, " ; frame B mode 1: B ticks skipped: %.1f mods (of %.1f), %.2f ms saved per pair", (double)mods / frames,
                (double)ticks / frames, ms / frames);
    if (g_st.bFrames > 0) {
        int fb = 0;
        for (int k = 0; k < F_Count; ++k) fb += g_st.fallback[k];
        AppendF(s, " ; frame B mode 2: %d render-only of %d, frame-B work %.3f ms per pair (replay), frame-A record "
                   "%.3f ms, real time of the pair for the scripts %.1f ms",
                g_st.replayed, g_st.bFrames, g_st.replayed ? g_st.replayMs / g_st.replayed : 0.0,
                g_st.aExitMs / g_st.bFrames, g_st.replayed ? g_st.pairDt * 1000.0 / g_st.replayed : 0.0);
        if (fb) AppendF(s, ", %d full updates", fb);
    }
    if (s.empty()) AppendF(s, " ; frame B mode %d", g_pairMode);
    return s;
}

void LogDetails()
{
    if (!g_on) return;
    if (g_st.replayed > 0 || g_st.bFrames > 0) {
        const double n = std::max(1, g_st.replayed);
        std::string s = "Frame B: replayed per pair:";
        AppendF(s, " lines %.1f, sprites %.1f, debug lines %.1f, outlines %.1f, highlights %.1f (%.1f dropped: shape gone), "
                   "registry keys %.1f set (%.1f music requests) / %.1f cleared (%.1f written once or changed: not repeated, "
                   "%.1f of them clears kept back: sub-keys written after), camera requests %.1f (%.1f offsets moved to "
                   "the right eye), loop requests %.1f, last sound kept %.1f, mute %.1f, depth of field %.1f, light "
                   "entries cut %.1f",
                g_st.lines / n, g_st.sprites / n, g_st.dbg / n, g_st.outlines / n, g_st.highlights / n,
                g_st.outlinesGone / n, g_st.regSet / n, g_st.music / n, g_st.regClear / n, g_st.regHeld / n,
                g_st.regKeepSub / n, g_st.cam / n, g_st.camEye / n, g_st.loops / n, g_st.lastSound / n, g_st.mute / n,
                g_st.dof / n, g_st.lightsCut / n);
        if (g_regDropped)
            AppendF(s, "; registry keys not kept (over %zu, or %zu for hud.* / game.*): %llu", kMaxRegKeys,
                    kMaxRegKeys + kRegReserve, (unsigned long long)g_regDropped);
        bool any = false;
        for (int k = 0; k < F_Count; ++k)
            if (g_st.fallback[k]) {
                AppendF(s, "%s%s %d", any ? ", " : "; full updates: ", kFallbackNames[k], g_st.fallback[k]);
                any = true;
            }
        Log("%s", s.c_str());
    }
    g_st = Stats{};
    g_regDropped = 0;
}

} // namespace FrameB
